// Debug probe for the in-process sourcekitd inside SwiftHostCompiler; see skd-probe.h.
//
// Requests are the probe-*.yaml templates of cfg->yaml_dir. Before parsing, ${NAME} placeholders are
// replaced: ${FILE} ${TEXT} (YAML-escaped file text) ${OFFSET} ${LENGTH} ${INSERT} ${FILTER} ${LIMIT}
// ${SYNTAXMAP} ${SUBSTRUCTURE} ${CONFIGKEYS} per request, ${ARGS} (the quoted files and compiler
// arguments, for key.compilerargs), ${SDK} and ${MC} (also inside the compiler arguments file).

#include "skd-probe.h"

#include "sourcekitd/sourcekitd.h"

#include <dispatch/dispatch.h>
#include <mach/mach.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <TargetConditionals.h>
#if TARGET_OS_IOS
#include <os/proc.h>
#endif

// ---------------------------------------------------------------- small helpers

typedef struct {
  char *p;
  size_t n, cap;
} buf_t;

static void buf_add(buf_t *b, const char *s, size_t n) {
  if (b->n + n + 1 > b->cap) {
    b->cap = (b->n + n + 1) * 2;
    b->p = realloc(b->p, b->cap);
  }
  if (n)
    memcpy(b->p + b->n, s, n);
  b->n += n;
  b->p[b->n] = 0;
}

static void buf_str(buf_t *b, const char *s) { buf_add(b, s, strlen(s)); }

static void buf_printf(buf_t *b, const char *fmt, ...) {
  char tmp[1024];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
  va_end(ap);
  if (n < 0)
    return;
  if ((size_t)n >= sizeof tmp)
    n = sizeof tmp - 1;
  buf_add(b, tmp, (size_t)n);
}

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static char *read_path(const char *path, size_t *len) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  buf_t b = {0};
  buf_add(&b, "", 0);
  char chunk[1 << 16];
  size_t n;
  while ((n = fread(chunk, 1, sizeof chunk, f)) > 0)
    buf_add(&b, chunk, n);
  fclose(f);
  if (len)
    *len = b.n;
  return b.p;
}

// Appends s as the inside of a YAML double-quoted scalar.
static void yaml_escape(buf_t *out, const char *s, size_t n) {
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)s[i];
    switch (c) {
    case '"': buf_str(out, "\\\""); break;
    case '\\': buf_str(out, "\\\\"); break;
    case '\n': buf_str(out, "\\n"); break;
    case '\r': buf_str(out, "\\r"); break;
    case '\t': buf_str(out, "\\t"); break;
    default:
      if (c < 0x20 || c == 0x7f)
        buf_printf(out, "\\x%02X", c);
      else
        buf_add(out, (const char *)&c, 1);
    }
  }
}

// Variable names per request: {name, raw value}; ${TEXT}, ${INSERT}, ${FILE} and ${FILTER} are escaped on expansion.
typedef struct {
  const char *name;
  const char *value;
  size_t len;
  int escape;
} var_t;

// ---------------------------------------------------------------- memory

static double mb(uint64_t bytes) { return (double)bytes / (1024.0 * 1024.0); }

static uint64_t footprint(uint64_t *ledger_peak) {
  task_vm_info_data_t info;
  mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
  if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count) != KERN_SUCCESS)
    return 0;
  if (ledger_peak)
    *ledger_peak = count >= TASK_VM_INFO_REV2_COUNT ? (uint64_t)info.ledger_phys_footprint_peak : 0;
  return info.phys_footprint;
}

static uint64_t available_memory(void) {
#if TARGET_OS_IOS
  return (uint64_t)os_proc_available_memory();
#else
  return 0;
#endif
}

// ---------------------------------------------------------------- run state

typedef struct {
  const skd_probe_config *cfg;
  skd_probe_report_fn report;
  void *report_ctx;

  char *args_json; // quoted files and arguments, comma separated
  char *primary_text, *complete_text;
  size_t primary_len, complete_len;
  const char *complete_file;
  long complete_off, cursor_off;

  int initialized, opened, session_open, config_done;
  int last_errors; // errors of the last diag step, -1 before the first
  int failures;
  int steps_run;

  // memory sampler
  pthread_t sampler;
  int sampler_running;
  atomic_int sampler_stop;
  atomic_ullong step_peak, run_peak;
  int sample_ms;
} probe_t;

static void sample_once(probe_t *p) {
  uint64_t fp = footprint(NULL);
  unsigned long long old = atomic_load(&p->step_peak);
  while (fp > old && !atomic_compare_exchange_weak(&p->step_peak, &old, fp)) {
  }
  old = atomic_load(&p->run_peak);
  while (fp > old && !atomic_compare_exchange_weak(&p->run_peak, &old, fp)) {
  }
}

static void *sampler_main(void *arg) {
  probe_t *p = arg;
  while (!atomic_load(&p->sampler_stop)) {
    sample_once(p);
    usleep((useconds_t)p->sample_ms * 1000);
  }
  return NULL;
}

// One report line; extra is "check=..." or "" and result the quoted summary.
static void emit(probe_t *p, const char *step, int index, double ms, const char *status, int check,
                 const char *result) {
  sample_once(p);
  uint64_t ledger = 0;
  uint64_t fp = footprint(&ledger);
  buf_t line = {0};
  buf_add(&line, "", 0);
  buf_printf(&line, "step=%s#%d ms=%.1f status=%s", step, index, ms, status);
  if (check >= 0)
    buf_printf(&line, " check=%s", check ? "pass" : "fail");
  buf_str(&line, " result=\"");
  for (const char *s = result; *s; s++) {
    char c = *s == '"' ? '\'' : (*s == '\n' || *s == '\r' ? ' ' : *s);
    buf_add(&line, &c, 1);
  }
  buf_printf(&line, "\" fp=%.1f step_peak=%.1f peak=%.1f ledger_peak=%.1f avail=%.1f", mb(fp),
             mb(atomic_load(&p->step_peak)), mb(atomic_load(&p->run_peak)), mb(ledger),
             mb(available_memory()));
  p->report(line.p, p->report_ctx);
  free(line.p);
  atomic_store(&p->step_peak, fp);
  p->steps_run++;
  if (!strcmp(status, "error") || (check == 0))
    p->failures++;
}

// ---------------------------------------------------------------- request templates

static char *expand_text(const char *in, const var_t *vars, size_t nvars, const probe_t *p) {
  buf_t out = {0};
  buf_add(&out, "", 0);
  while (*in) {
    const char *open = strstr(in, "${");
    const char *close = open ? strchr(open, '}') : NULL;
    if (!close) {
      buf_str(&out, in);
      break;
    }
    buf_add(&out, in, (size_t)(open - in));
    const char *name = open + 2;
    size_t len = (size_t)(close - name);
    int done = 0;
    for (size_t i = 0; i < nvars && !done; i++) {
      if (strlen(vars[i].name) == len && !strncmp(vars[i].name, name, len)) {
        if (vars[i].escape)
          yaml_escape(&out, vars[i].value, vars[i].len);
        else
          buf_add(&out, vars[i].value, vars[i].len);
        done = 1;
      }
    }
    if (!done && len == 4 && !strncmp(name, "ARGS", 4)) {
      buf_str(&out, p->args_json);
      done = 1;
    }
    if (!done && len == 3 && !strncmp(name, "SDK", 3) && p->cfg->sdk) {
      buf_str(&out, p->cfg->sdk);
      done = 1;
    }
    if (!done && len == 2 && !strncmp(name, "MC", 2) && p->cfg->module_cache) {
      buf_str(&out, p->cfg->module_cache);
      done = 1;
    }
    if (!done)
      buf_add(&out, open, (size_t)(close - open + 1));
    in = close + 1;
  }
  return out.p;
}

// Template file without its comment lines, placeholders replaced.
static char *render(probe_t *p, const char *name, const var_t *vars, size_t nvars) {
  char path[4096];
  snprintf(path, sizeof path, "%s/%s", p->cfg->yaml_dir, name);
  char *raw = read_path(path, NULL);
  if (!raw)
    return NULL;
  buf_t stripped = {0};
  buf_add(&stripped, "", 0);
  for (char *line = raw; *line;) {
    char *nl = strchr(line, '\n');
    size_t n = nl ? (size_t)(nl - line) + 1 : strlen(line);
    if (line[0] != '#')
      buf_add(&stripped, line, n);
    line += n;
  }
  free(raw);
  char *out = expand_text(stripped.p, vars, nvars, p);
  free(stripped.p);
  return out;
}

static char *build_args(const skd_probe_config *cfg) {
  buf_t out = {0};
  buf_add(&out, "", 0);
  int first = 1;
  probe_t tmp = {.cfg = cfg};
  tmp.args_json = (char *)"";
#define ADD_ARG(s, n, esc)                                                                         \
  do {                                                                                             \
    if (!first)                                                                                    \
      buf_str(&out, ", ");                                                                         \
    first = 0;                                                                                     \
    buf_str(&out, "\"");                                                                           \
    if (esc)                                                                                       \
      yaml_escape(&out, (s), (n));                                                                 \
    else                                                                                           \
      buf_add(&out, (s), (n));                                                                     \
    buf_str(&out, "\"");                                                                           \
  } while (0)
  for (size_t i = 0; i < cfg->file_count; i++)
    ADD_ARG(cfg->files[i], strlen(cfg->files[i]), 1);
  char path[4096];
  if (cfg->compiler_args_file)
    snprintf(path, sizeof path, "%s", cfg->compiler_args_file);
  else
    snprintf(path, sizeof path, "%s/compilerargs.txt", cfg->yaml_dir);
  char *args = read_path(path, NULL);
  if (!args) {
    free(out.p);
    return NULL;
  }
  for (char *line = args; *line;) {
    char *nl = strchr(line, '\n');
    size_t n = nl ? (size_t)(nl - line) : strlen(line);
    char *l = strndup(line, n);
    line += n + (nl ? 1 : 0);
    if (n) {
      char *arg = expand_text(l, NULL, 0, &tmp);
      ADD_ARG(arg, strlen(arg), 1);
      free(arg);
    }
    free(l);
  }
  free(args);
  for (size_t i = 0; i < cfg->extra_arg_count; i++)
    ADD_ARG(cfg->extra_args[i], strlen(cfg->extra_args[i]), 1);
#undef ADD_ARG
  return out.p;
}

// ---------------------------------------------------------------- response access

static sourcekitd_uid_t K(const char *s) { return sourcekitd_uid_get_from_cstr(s); }

static sourcekitd_variant_t V(sourcekitd_variant_t dict, const char *key) {
  return sourcekitd_variant_dictionary_get_value(dict, K(key));
}

static const char *S(sourcekitd_variant_t dict, const char *key) {
  const char *s = sourcekitd_variant_dictionary_get_string(dict, K(key));
  return s ? s : "";
}

static size_t array_count(sourcekitd_variant_t v) {
  return sourcekitd_variant_get_type(v) == SOURCEKITD_VARIANT_TYPE_ARRAY ? sourcekitd_variant_array_get_count(v) : 0;
}

static const char *error_kind_name(sourcekitd_error_t kind) {
  switch (kind) {
  case SOURCEKITD_ERROR_CONNECTION_INTERRUPTED: return "CONNECTION_INTERRUPTED";
  case SOURCEKITD_ERROR_REQUEST_INVALID: return "REQUEST_INVALID";
  case SOURCEKITD_ERROR_REQUEST_FAILED: return "REQUEST_FAILED";
  case SOURCEKITD_ERROR_REQUEST_CANCELLED: return "REQUEST_CANCELLED";
  default: return "UNKNOWN";
  }
}

static void describe_error(sourcekitd_response_t resp, buf_t *out) {
  const char *d = sourcekitd_response_error_get_description(resp);
  buf_printf(out, "error kind=%s desc=%.300s", error_kind_name(sourcekitd_response_error_get_kind(resp)),
             d ? d : "");
}

// Sends the rendered YAML synchronously. On a request error fills summary and returns NULL.
static sourcekitd_response_t send_yaml(probe_t *p, const char *yaml_name, const var_t *vars, size_t nvars,
                                       double *ms, buf_t *summary) {
  char *yaml = render(p, yaml_name, vars, nvars);
  if (!yaml) {
    buf_printf(summary, "cannot read template %s", yaml_name);
    return NULL;
  }
  char *error = NULL;
  sourcekitd_object_t req = sourcekitd_request_create_from_yaml(yaml, &error);
  free(yaml);
  if (!req) {
    buf_printf(summary, "invalid request %s: %.200s", yaml_name, error ? error : "?");
    free(error);
    return NULL;
  }
  double t0 = now_ms();
  sourcekitd_response_t resp = sourcekitd_send_request_sync(req);
  *ms = now_ms() - t0;
  sourcekitd_request_release(req);
  if (sourcekitd_response_is_error(resp)) {
    describe_error(resp, summary);
    sourcekitd_response_dispose(resp);
    return NULL;
  }
  return resp;
}

// ---------------------------------------------------------------- result summaries

static uint64_t fnv(uint64_t h, const char *s) {
  for (; *s; s++)
    h = (h ^ (unsigned char)*s) * 1099511628211ull;
  return (h ^ '\n') * 1099511628211ull;
}

// Returns 1 when `expect` (or, for NULL, any result) is present. hash covers every key.name in order.
static int summarize_results(sourcekitd_variant_t results, const char *expect, buf_t *out) {
  size_t count = array_count(results);
  uint64_t h = 1469598103934665603ull;
  int found = expect == NULL && count > 0;
  buf_printf(out, "%zu results", count);
  for (size_t i = 0; i < count; i++) {
    sourcekitd_variant_t item = sourcekitd_variant_array_get_value(results, i);
    const char *name = S(item, "key.name");
    h = fnv(h, name);
    if (expect && !strncmp(name, expect, strlen(expect)))
      found = 1;
    if (i < 3)
      buf_printf(out, "%s%.60s", i ? " | " : ": ", S(item, "key.description"));
  }
  buf_printf(out, " hash=%016llx expect=%s", (unsigned long long)h, expect ? (found ? "found" : "MISSING") : "-");
  return found;
}

// Counts errors; prints up to two diagnostics. *matched is set when expect occurs in a description.
static size_t summarize_diagnostics(sourcekitd_variant_t diags, const char *expect, int *matched, buf_t *out) {
  size_t count = array_count(diags), errors = 0;
  for (size_t i = 0; i < count; i++) {
    sourcekitd_variant_t d = sourcekitd_variant_array_get_value(diags, i);
    sourcekitd_uid_t sev = sourcekitd_variant_dictionary_get_uid(d, K("key.severity"));
    const char *sevname = sev ? sourcekitd_uid_get_string_ptr(sev) : "";
    int is_error = strstr(sevname, ".error") != NULL;
    errors += is_error;
    const char *desc = S(d, "key.description");
    if (expect && strstr(desc, expect))
      *matched = 1;
    if (i < 2 && is_error)
      buf_printf(out, " [%lld:%lld %.100s]", (long long)sourcekitd_variant_dictionary_get_int64(d, K("key.line")),
                 (long long)sourcekitd_variant_dictionary_get_int64(d, K("key.column")), desc);
  }
  return errors;
}

// ---------------------------------------------------------------- steps

typedef struct {
  const char *name;
  int count;
} step_t;

static long find_offset(const char *text, size_t len, const char *pattern, int delta, long explicit_off) {
  if (explicit_off > 0)
    return explicit_off;
  if (!pattern)
    return -1;
  size_t plen = strlen(pattern);
  for (size_t i = 0; i + plen <= len; i++)
    if (!memcmp(text + i, pattern, plen))
      return (long)i + delta;
  return -1;
}

static void run_step(probe_t *p, const char *name, int index, int *skip_rest_of_dependent) {
  const skd_probe_config *cfg = p->cfg;
  buf_t sum = {0};
  buf_add(&sum, "", 0);
  double ms = 0;
  char offset_s[32], length_s[32], limit_s[32], syntaxmap_s[8], substructure_s[8];
  snprintf(limit_s, sizeof limit_s, "%d", cfg->request_limit);
  snprintf(syntaxmap_s, sizeof syntaxmap_s, "%d", cfg->enable_syntax_map ? 1 : 0);
  snprintf(substructure_s, sizeof substructure_s, "%d", cfg->enable_substructure ? 1 : 0);
  const char *primary = cfg->primary;

#define VAR(n, v) {.name = n, .value = v, .len = strlen(v), .escape = 0}
#define VARE(n, v, l) {.name = n, .value = v, .len = l, .escape = 1}
#define FAIL_STEP() do { emit(p, name, index, ms, "error", -1, sum.p); free(sum.p); return; } while (0)
#define SKIP(why) do { emit(p, name, index, 0, "skipped", -1, why); free(sum.p); return; } while (0)

  if (!strcmp(name, "init")) {
    double t0 = now_ms();
    sourcekitd_initialize();
    p->initialized = 1;
    emit(p, name, index, now_ms() - t0, "ok", -1, "sourcekitd_initialize");
    free(sum.p);
    return;
  }
  if (!p->initialized && strcmp(name, "shutdown")) {
    sourcekitd_initialize();
    p->initialized = 1;
    emit(p, "init(implicit)", index, 0, "ok", -1, "sourcekitd_initialize");
  }

  if (!strcmp(name, "shutdown")) {
    double t0 = now_ms();
    if (p->initialized)
      sourcekitd_shutdown();
    p->initialized = p->opened = p->session_open = 0;
    emit(p, name, index, now_ms() - t0, "ok", -1, "sourcekitd_shutdown");
    free(sum.p);
    return;
  }

  if (!strcmp(name, "config")) {
    buf_t keys = {0};
    buf_add(&keys, "", 0);
    if (cfg->config_reuse_count >= 0)
      buf_printf(&keys, ", key.completion_max_astcontext_reuse_count: %d", cfg->config_reuse_count);
    if (cfg->config_check_interval >= 0)
      buf_printf(&keys, ", key.completion_check_dependency_interval: %d", cfg->config_check_interval);
    var_t v[] = {VAR("CONFIGKEYS", keys.p)};
    sourcekitd_response_t r = send_yaml(p, "probe-config.yaml", v, 1, &ms, &sum);
    free(keys.p);
    if (!r)
      FAIL_STEP();
    sourcekitd_variant_t d = sourcekitd_response_get_value(r);
    buf_printf(&sum, "%s: reuse_count=%lld check_interval=%lld", keys.n ? "set" : "read",
               (long long)sourcekitd_variant_dictionary_get_int64(d, K("key.completion_max_astcontext_reuse_count")),
               (long long)sourcekitd_variant_dictionary_get_int64(d, K("key.completion_check_dependency_interval")));
    sourcekitd_response_dispose(r);
    emit(p, name, index, ms, "ok", -1, sum.p);
    free(sum.p);
    return;
  }

  if (!strcmp(name, "open")) {
    var_t v[] = {VARE("FILE", primary, strlen(primary)), VARE("TEXT", p->primary_text, p->primary_len),
                 VAR("SYNTAXMAP", syntaxmap_s), VAR("SUBSTRUCTURE", substructure_s)};
    sourcekitd_response_t r = send_yaml(p, "probe-open.yaml", v, 4, &ms, &sum);
    if (!r)
      FAIL_STEP();
    sourcekitd_variant_t d = sourcekitd_response_get_value(r);
    buf_printf(&sum, "%zu bytes, %zu syntactic diagnostics, %zu syntaxmap entries, %zu substructure entries",
               p->primary_len, array_count(V(d, "key.diagnostics")), array_count(V(d, "key.syntaxmap")),
               array_count(V(d, "key.substructure")));
    sourcekitd_response_dispose(r);
    p->opened = 1;
    emit(p, name, index, ms, "ok", -1, sum.p);
    free(sum.p);
    return;
  }

  if (!strcmp(name, "diag") || !strcmp(name, "tokens") || !strcmp(name, "cursor") || !strcmp(name, "related") ||
      !strcmp(name, "replace") || !strcmp(name, "close")) {
    if (!p->opened)
      SKIP("editor document is not open");
  }

  if (!strcmp(name, "diag")) {
    var_t v[] = {VARE("FILE", primary, strlen(primary))};
    sourcekitd_response_t r = send_yaml(p, "probe-diagnostics.yaml", v, 1, &ms, &sum);
    if (!r)
      FAIL_STEP();
    int matched = 0;
    buf_t d = {0};
    buf_add(&d, "", 0);
    size_t errors = summarize_diagnostics(V(sourcekitd_response_get_value(r), "key.diagnostics"), cfg->diag_expect,
                                          &matched, &d);
    size_t total = array_count(V(sourcekitd_response_get_value(r), "key.diagnostics"));
    buf_printf(&sum, "%zu diagnostics, %zu errors%s", total, errors, d.p);
    int check;
    if (cfg->diag_expect)
      check = matched;
    else if (p->last_errors >= 0 && (int)errors > p->last_errors) {
      check = 1; // an edit (replace step) introduced errors: expected, so the check is "more errors than before"
      buf_printf(&sum, " (was %d errors before the edit)", p->last_errors);
    } else
      check = errors == 0;
    p->last_errors = (int)errors;
    free(d.p);
    sourcekitd_response_dispose(r);
    emit(p, name, index, ms, "ok", check, sum.p);
    free(sum.p);
    return;
  }

  if (!strcmp(name, "tokens")) {
    var_t v[] = {VARE("FILE", primary, strlen(primary))};
    sourcekitd_response_t r = send_yaml(p, "probe-tokens.yaml", v, 1, &ms, &sum);
    if (!r)
      FAIL_STEP();
    sourcekitd_variant_t t = V(sourcekitd_response_get_value(r), "key.semantic_tokens");
    if (sourcekitd_variant_get_type(t) == SOURCEKITD_VARIANT_TYPE_DATA)
      buf_printf(&sum, "%zu bytes of semantic tokens", sourcekitd_variant_data_get_size(t));
    else if (sourcekitd_variant_get_type(t) == SOURCEKITD_VARIANT_TYPE_ARRAY)
      buf_printf(&sum, "%zu semantic tokens", array_count(t));
    else
      buf_printf(&sum, "semantic tokens variant type %d", (int)sourcekitd_variant_get_type(t));
    sourcekitd_response_dispose(r);
    emit(p, name, index, ms, "ok", -1, sum.p);
    free(sum.p);
    return;
  }

  if (!strcmp(name, "cursor") || !strcmp(name, "related")) {
    if (p->cursor_off < 0)
      SKIP("cursor position not found (cursor_pattern / cursor_offset)");
    snprintf(offset_s, sizeof offset_s, "%ld", p->cursor_off);
    var_t v[] = {VARE("FILE", primary, strlen(primary)), VAR("OFFSET", offset_s)};
    int related = !strcmp(name, "related");
    sourcekitd_response_t r = send_yaml(p, related ? "probe-related.yaml" : "probe-cursor.yaml", v, 2, &ms, &sum);
    if (!r)
      FAIL_STEP();
    sourcekitd_variant_t d = sourcekitd_response_get_value(r);
    int check = -1;
    if (related) {
      buf_printf(&sum, "%zu related identifiers", array_count(V(d, "key.results")));
    } else {
      const char *nm = S(d, "key.name"), *usr = S(d, "key.usr");
      buf_printf(&sum, "name=%.80s usr=%.100s kind=", nm, usr);
      sourcekitd_uid_t kind = sourcekitd_variant_dictionary_get_uid(d, K("key.kind"));
      buf_printf(&sum, "%s", kind ? sourcekitd_uid_get_string_ptr(kind) : "?");
      if (cfg->cursor_expect)
        check = !strncmp(nm, cfg->cursor_expect, strlen(cfg->cursor_expect)) && *usr;
    }
    sourcekitd_response_dispose(r);
    emit(p, name, index, ms, "ok", check, sum.p);
    free(sum.p);
    return;
  }

  if (!strcmp(name, "complete") || !strcmp(name, "ccopen") || !strcmp(name, "ccupdate") || !strcmp(name, "ccclose") ||
      !strcmp(name, "cancel")) {
    if (p->complete_off < 0)
      SKIP("completion position not found (complete_pattern / complete_offset)");
  }
  if (!strcmp(name, "ccupdate") || !strcmp(name, "ccclose")) {
    if (!p->session_open)
      SKIP("no completion session is open");
  }

  if (!strcmp(name, "complete") || !strcmp(name, "ccopen")) {
    int session = !strcmp(name, "ccopen");
    snprintf(offset_s, sizeof offset_s, "%ld", p->complete_off);
    var_t v[] = {VARE("FILE", p->complete_file, strlen(p->complete_file)),
                 VARE("TEXT", p->complete_text, p->complete_len), VAR("OFFSET", offset_s), VAR("LIMIT", limit_s)};
    sourcekitd_response_t r = send_yaml(p, session ? "probe-ccopen.yaml" : "probe-complete.yaml", v, 4, &ms, &sum);
    if (!r)
      FAIL_STEP();
    int ok = summarize_results(V(sourcekitd_response_get_value(r), "key.results"), cfg->complete_expect, &sum);
    sourcekitd_response_dispose(r);
    if (session)
      p->session_open = 1;
    emit(p, name, index, ms, "ok", ok, sum.p);
    free(sum.p);
    return;
  }

  if (!strcmp(name, "ccupdate")) {
    snprintf(offset_s, sizeof offset_s, "%ld", p->complete_off);
    var_t v[] = {VARE("FILE", p->complete_file, strlen(p->complete_file)), VAR("OFFSET", offset_s),
                 VARE("FILTER", cfg->update_filter, strlen(cfg->update_filter)), VAR("LIMIT", limit_s)};
    sourcekitd_response_t r = send_yaml(p, "probe-ccupdate.yaml", v, 4, &ms, &sum);
    if (!r)
      FAIL_STEP();
    buf_printf(&sum, "filter '%s': ", cfg->update_filter);
    summarize_results(V(sourcekitd_response_get_value(r), "key.results"), NULL, &sum);
    sourcekitd_response_dispose(r);
    emit(p, name, index, ms, "ok", -1, sum.p);
    free(sum.p);
    return;
  }

  if (!strcmp(name, "ccclose")) {
    snprintf(offset_s, sizeof offset_s, "%ld", p->complete_off);
    var_t v[] = {VARE("FILE", p->complete_file, strlen(p->complete_file)), VAR("OFFSET", offset_s)};
    sourcekitd_response_t r = send_yaml(p, "probe-ccclose.yaml", v, 2, &ms, &sum);
    if (!r)
      FAIL_STEP();
    sourcekitd_response_dispose(r);
    p->session_open = 0;
    emit(p, name, index, ms, "ok", -1, "session closed");
    free(sum.p);
    return;
  }

  if (!strcmp(name, "replace")) {
    // Appends a type error, so the next diag step must show one more error than the last one.
    static const char insert[] = "\nlet __skd_probe_error: Int = \"not an Int\"\n";
    snprintf(offset_s, sizeof offset_s, "%zu", p->primary_len);
    snprintf(length_s, sizeof length_s, "0");
    var_t v[] = {VARE("FILE", primary, strlen(primary)), VAR("OFFSET", offset_s), VAR("LENGTH", length_s),
                 VARE("INSERT", insert, sizeof insert - 1), VAR("SYNTAXMAP", syntaxmap_s),
                 VAR("SUBSTRUCTURE", substructure_s)};
    sourcekitd_response_t r = send_yaml(p, "probe-replace.yaml", v, 6, &ms, &sum);
    if (!r)
      FAIL_STEP();
    buf_printf(&sum, "appended %zu bytes, %zu syntactic diagnostics", sizeof insert - 1,
               array_count(V(sourcekitd_response_get_value(r), "key.diagnostics")));
    sourcekitd_response_dispose(r);
    emit(p, name, index, ms, "ok", -1, sum.p);
    free(sum.p);
    return;
  }

  if (!strcmp(name, "close")) {
    var_t v[] = {VARE("FILE", primary, strlen(primary))};
    sourcekitd_response_t r = send_yaml(p, "probe-close.yaml", v, 1, &ms, &sum);
    if (!r)
      FAIL_STEP();
    sourcekitd_response_dispose(r);
    p->opened = 0;
    emit(p, name, index, ms, "ok", -1, "editor.close");
    free(sum.p);
    return;
  }

  if (!strcmp(name, "cancel")) {
    // Async one-shot completion of the (biggest) completion file, cancelled right after sending.
    snprintf(offset_s, sizeof offset_s, "%ld", p->complete_off);
    var_t v[] = {VARE("FILE", p->complete_file, strlen(p->complete_file)),
                 VARE("TEXT", p->complete_text, p->complete_len), VAR("OFFSET", offset_s)};
    char *yaml = render(p, "probe-complete.yaml", v, 3);
    char *error = NULL;
    sourcekitd_object_t req = yaml ? sourcekitd_request_create_from_yaml(yaml, &error) : NULL;
    free(yaml);
    if (!req) {
      buf_printf(&sum, "invalid request: %.200s", error ? error : "?");
      free(error);
      FAIL_STEP();
    }
    __block sourcekitd_response_t resp = NULL;
    __block double t_resp = 0;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    sourcekitd_request_handle_t handle = NULL;
    double t0 = now_ms();
    sourcekitd_send_request(req, &handle, ^(sourcekitd_response_t r) {
      resp = r;
      t_resp = now_ms();
      dispatch_semaphore_signal(sem);
    });
    if (cfg->cancel_delay_ms > 0)
      usleep((useconds_t)cfg->cancel_delay_ms * 1000);
    double t_cancel = now_ms();
    sourcekitd_cancel_request(handle);
    long timed_out = dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 600ll * NSEC_PER_SEC));
    sourcekitd_request_release(req);
    if (timed_out) {
      buf_str(&sum, "no response within 600 s after cancel");
      FAIL_STEP();
    }
    ms = t_resp - t0;
    int cancelled = 0;
    if (sourcekitd_response_is_error(resp)) {
      cancelled = sourcekitd_response_error_get_kind(resp) == SOURCEKITD_ERROR_REQUEST_CANCELLED;
      describe_error(resp, &sum);
    } else {
      buf_str(&sum, "completed normally (cancel came too late): ");
      summarize_results(V(sourcekitd_response_get_value(resp), "key.results"), NULL, &sum);
    }
    buf_printf(&sum, "; cancelled=%s cancel_to_response_ms=%.1f cancel_sent_after_ms=%.1f", cancelled ? "yes" : "no",
               t_resp - t_cancel, t_cancel - t0);
    sourcekitd_response_dispose(resp);
    sourcekitd_request_handle_dispose(handle);
    emit(p, name, index, ms, "ok", cancelled, sum.p);
    free(sum.p);
    return;
  }

  buf_printf(&sum, "unknown step '%s'", name);
  emit(p, name, index, 0, "error", -1, sum.p);
  free(sum.p);
  (void)skip_rest_of_dependent;
#undef VAR
#undef VARE
#undef FAIL_STEP
#undef SKIP
}

// ---------------------------------------------------------------- entry points

void skd_probe_config_init(skd_probe_config *cfg) {
  memset(cfg, 0, sizeof *cfg);
  cfg->config_reuse_count = -1;
  cfg->config_check_interval = -1;
  cfg->request_limit = 50;
  cfg->update_filter = "a";
  cfg->sample_interval_ms = 10;
}

static const char *default_steps =
    "init,config,open,diag,tokens,cursor,related,complete*2,ccopen,ccupdate,ccclose,replace,diag,close,cancel,shutdown";

int skd_probe_run(const skd_probe_config *cfg, skd_probe_report_fn report, void *ctx) {
  probe_t p = {.cfg = cfg, .report = report, .report_ctx = ctx, .last_errors = -1};
  if (!cfg->yaml_dir || !cfg->primary || !cfg->files || !cfg->file_count || !report) {
    if (report)
      report("summary status=error result=\"yaml_dir, primary and files are required\"", ctx);
    return -1;
  }
  p.args_json = build_args(cfg);
  p.primary_text = read_path(cfg->primary, &p.primary_len);
  p.complete_file = cfg->complete_file ? cfg->complete_file : cfg->primary;
  if (!p.args_json || !p.primary_text) {
    report("summary status=error result=\"cannot read compilerargs.txt or the primary file\"", ctx);
    free(p.args_json);
    free(p.primary_text);
    return -1;
  }
  if (strcmp(p.complete_file, cfg->primary)) {
    p.complete_text = read_path(p.complete_file, &p.complete_len);
    if (!p.complete_text) {
      report("summary status=error result=\"cannot read complete_file\"", ctx);
      free(p.args_json);
      free(p.primary_text);
      return -1;
    }
  } else {
    p.complete_text = p.primary_text;
    p.complete_len = p.primary_len;
  }
  p.complete_off = find_offset(p.complete_text, p.complete_len, cfg->complete_pattern, cfg->complete_delta,
                               cfg->complete_offset);
  p.cursor_off = find_offset(p.primary_text, p.primary_len, cfg->cursor_pattern, cfg->cursor_delta, cfg->cursor_offset);

  p.sample_ms = cfg->sample_interval_ms;
  uint64_t fp0 = footprint(NULL);
  atomic_store(&p.step_peak, fp0);
  atomic_store(&p.run_peak, fp0);
  if (p.sample_ms > 0 && pthread_create(&p.sampler, NULL, sampler_main, &p) == 0)
    p.sampler_running = 1;

  {
    char line[512];
    snprintf(line, sizeof line, "start fp=%.1f avail=%.1f primary_bytes=%zu complete_offset=%ld cursor_offset=%ld files=%zu",
             mb(fp0), mb(available_memory()), p.primary_len, p.complete_off, p.cursor_off, cfg->file_count);
    report(line, ctx);
  }

  char *steps = strdup(cfg->steps ? cfg->steps : default_steps);
  int counts[64] = {0};
  const char *seen[64];
  int nseen = 0;
  for (char *save = NULL, *tok = strtok_r(steps, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
    char *star = strchr(tok, '*');
    int repeat = star ? atoi(star + 1) : 1;
    if (star)
      *star = 0;
    for (int r = 0; r < repeat; r++) {
      int slot = -1;
      for (int i = 0; i < nseen; i++)
        if (!strcmp(seen[i], tok))
          slot = i;
      if (slot < 0 && nseen < 64) {
        seen[nseen] = strdup(tok);
        slot = nseen++;
      }
      run_step(&p, tok, slot >= 0 ? ++counts[slot] : 1, NULL);
    }
  }
  for (int i = 0; i < nseen; i++)
    free((void *)seen[i]);
  free(steps);

  if (p.sampler_running) {
    atomic_store(&p.sampler_stop, 1);
    pthread_join(p.sampler, NULL);
  }
  sample_once(&p);
  uint64_t ledger = 0, fp = footprint(&ledger);
  char line[512];
  snprintf(line, sizeof line, "summary status=%s steps=%d failures=%d fp=%.1f peak=%.1f ledger_peak=%.1f avail=%.1f",
           p.failures ? "FAIL" : "PASS", p.steps_run, p.failures, mb(fp), mb(atomic_load(&p.run_peak)), mb(ledger),
           mb(available_memory()));
  report(line, ctx);

  free(p.args_json);
  if (p.complete_text != p.primary_text)
    free(p.complete_text);
  free(p.primary_text);
  return p.failures;
}
