// Checks that a cancelled request of the in-process sourcekitd leaves nothing behind that a later request
// reuses. The test file has a global whose type is only known once a closure of many statements has been
// type-checked, a long function body, a function with a deliberate type error, and two completion points.
// sourcekitd answers a cancelled request at once while the cancelled work may go on in the background and
// delay the next request: the time of the request after each cancel shows that.
//
//   diagnostics: editor.open, one uncancelled source.request.diagnostics (the reference), then rounds of:
//     edit the document (new snapshot), send diagnostics asynchronously, cancel it after a fraction of the
//     reference time, then request diagnostics again; the result must equal the reference.
//   completion: one uncancelled completion at "table." with an extra compiler argument (a compiler instance of
//     its own; the reference), then rounds of: a completion elsewhere in the file (which leaves a cached compiler
//     instance that has not type-checked the global), a completion at "table." cancelled after a fraction of
//     the reference time (it reuses that instance and type-checks the global), then the same completion
//     again; its results must equal the reference.
//
// A cancel that comes too late (the request completed) is reported and does not count as a failure, but at
// least one cancel of each kind must have taken effect. Output lines start with "diag", "complete" or
// "RESULT". Exit status 0 when every check passed.
//
//   skd-cancel --sdk <wasm32-unknown-wasip1-threads dir> --mc <module cache dir> --workdir <dir>
//              [--statements N] [--closure-statements N] [--rounds N]
// --statements: statements of the long function body (default 400); --closure-statements: statements of the
// closure that gives the global its type (default 30); --rounds: cancels of each kind (default 3).
#include <sourcekitd/sourcekitd.h>

#include <dispatch/dispatch.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static sourcekitd_uid_t K(const char *s) { return sourcekitd_uid_get_from_cstr(s); }

static uint64_t fnv(uint64_t h, const char *s) {
  for (; s && *s; s++)
    h = (h ^ (unsigned char)*s) * 1099511628211ull;
  return (h ^ '\n') * 1099511628211ull;
}

static const char *SDK, *MC;
static char *TEXT, *PATH_;
static size_t TEXT_LEN;

static sourcekitd_object_t compiler_args(int baseline) {
  const char *args[] = {
      PATH_, "-module-name", "main", "-target", "wasm32-unknown-wasip1-threads", "-sdk", NULL, "-resource-dir",
      NULL, "-static-stdlib", "-module-cache-path", MC, "-parse-as-library", "-Xcc", "-matomics", "-Xcc",
      "-mbulk-memory", "-Xcc", "-mthread-model", "-Xcc", "posix", "-Xcc", "-pthread", "-Xcc",
      "-ftls-model=local-exec", "-Xcc", "-mtail-call"};
  char sdk[4096], res[4096];
  snprintf(sdk, sizeof sdk, "%s/WASI.sdk", SDK);
  snprintf(res, sizeof res, "%s/swift.xctoolchain/usr/lib/swift_static", SDK);
  args[6] = sdk;
  args[8] = res;
  sourcekitd_object_t arr = sourcekitd_request_array_create(NULL, 0);
  for (size_t i = 0; i < sizeof args / sizeof *args; i++)
    sourcekitd_request_array_set_string(arr, SOURCEKITD_ARRAY_APPEND, args[i]);
  if (baseline) // Same meaning, different arguments: a compiler instance of its own.
    sourcekitd_request_array_set_string(arr, SOURCEKITD_ARRAY_APPEND, "-DSKD_CANCEL_REFERENCE");
  return arr;
}

static sourcekitd_object_t request(const char *kind) {
  sourcekitd_object_t req = sourcekitd_request_dictionary_create(NULL, NULL, 0);
  sourcekitd_request_dictionary_set_uid(req, K("key.request"), K(kind));
  return req;
}

// Sends req (consumed); with cancel_after_ms >= 0 asynchronously, cancelling it after that many ms.
// *ms is the time to the response, *cancel_ms the time from the cancel to the response.
static sourcekitd_response_t send(sourcekitd_object_t req, double cancel_after_ms, double *ms, double *cancel_ms) {
  double t0 = now_ms();
  if (cancel_after_ms < 0) {
    sourcekitd_response_t resp = sourcekitd_send_request_sync(req);
    *ms = now_ms() - t0;
    sourcekitd_request_release(req);
    return resp;
  }
  __block sourcekitd_response_t resp = NULL;
  __block double t_resp = 0;
  dispatch_semaphore_t sem = dispatch_semaphore_create(0);
  sourcekitd_request_handle_t handle = NULL;
  sourcekitd_send_request(req, &handle, ^(sourcekitd_response_t r) {
    resp = r;
    t_resp = now_ms();
    dispatch_semaphore_signal(sem);
  });
  usleep((useconds_t)(cancel_after_ms * 1000));
  double t_cancel = now_ms();
  sourcekitd_cancel_request(handle);
  if (dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 600ll * NSEC_PER_SEC))) {
    printf("RESULT FAIL no response within 600 s after a cancel\n");
    exit(1);
  }
  sourcekitd_request_handle_dispose(handle);
  sourcekitd_request_release(req);
  *ms = t_resp - t0;
  *cancel_ms = t_resp - t_cancel;
  return resp;
}

static int is_cancelled(sourcekitd_response_t resp) {
  return sourcekitd_response_is_error(resp) &&
         sourcekitd_response_error_get_kind(resp) == SOURCEKITD_ERROR_REQUEST_CANCELLED;
}

static const char *error_text(sourcekitd_response_t resp) {
  const char *d = sourcekitd_response_error_get_description(resp);
  return d ? d : "?";
}

// ---------------------------------------------------------------- diagnostics

typedef struct {
  size_t count, errors;
  uint64_t hash; // severity, line, column and description of every diagnostic, in order
  char first_error[300];
} diags_t;

static diags_t summarize_diags(sourcekitd_response_t resp) {
  diags_t d = {0, 0, 1469598103934665603ull, ""};
  sourcekitd_variant_t arr =
      sourcekitd_variant_dictionary_get_value(sourcekitd_response_get_value(resp), K("key.diagnostics"));
  if (sourcekitd_variant_get_type(arr) != SOURCEKITD_VARIANT_TYPE_ARRAY)
    return d;
  d.count = sourcekitd_variant_array_get_count(arr);
  for (size_t i = 0; i < d.count; i++) {
    sourcekitd_variant_t e = sourcekitd_variant_array_get_value(arr, i);
    sourcekitd_uid_t sev = sourcekitd_variant_dictionary_get_uid(e, K("key.severity"));
    const char *sevname = sev ? sourcekitd_uid_get_string_ptr(sev) : "";
    const char *desc = sourcekitd_variant_dictionary_get_string(e, K("key.description"));
    char pos[64];
    snprintf(pos, sizeof pos, "%lld:%lld", (long long)sourcekitd_variant_dictionary_get_int64(e, K("key.line")),
             (long long)sourcekitd_variant_dictionary_get_int64(e, K("key.column")));
    d.hash = fnv(fnv(fnv(d.hash, sevname), pos), desc);
    if (strstr(sevname, ".error")) {
      if (!d.errors++)
        snprintf(d.first_error, sizeof d.first_error, "%s %s", pos, desc ? desc : "");
    }
  }
  return d;
}

static sourcekitd_response_t diagnostics(double cancel_after_ms, double *ms, double *cancel_ms) {
  sourcekitd_object_t req = request("source.request.diagnostics");
  sourcekitd_request_dictionary_set_string(req, K("key.sourcefile"), PATH_);
  sourcekitd_object_t args = compiler_args(0);
  sourcekitd_request_dictionary_set_value(req, K("key.compilerargs"), args);
  sourcekitd_request_release(args);
  return send(req, cancel_after_ms, ms, cancel_ms);
}

// Appends a comment line at the end of the open document: a new snapshot with the same diagnostics.
static int edit(int round) {
  char line[64];
  snprintf(line, sizeof line, "// edit %d\n", round);
  sourcekitd_object_t req = request("source.request.editor.replacetext");
  sourcekitd_request_dictionary_set_string(req, K("key.name"), PATH_);
  sourcekitd_request_dictionary_set_int64(req, K("key.offset"), (int64_t)TEXT_LEN);
  sourcekitd_request_dictionary_set_int64(req, K("key.length"), 0);
  sourcekitd_request_dictionary_set_string(req, K("key.sourcetext"), line);
  double ms, unused;
  sourcekitd_response_t resp = send(req, -1, &ms, &unused);
  int ok = !sourcekitd_response_is_error(resp);
  if (!ok)
    printf("diag edit %d failed: %s\n", round, error_text(resp));
  sourcekitd_response_dispose(resp);
  TEXT_LEN += strlen(line);
  return ok;
}

// ---------------------------------------------------------------- completion

typedef struct {
  size_t count;
  uint64_t hash;    // key.name and key.typename of every result, sorted (the order may depend on caching)
  uint64_t ordered; // the same in response order, for information
  int found;        // a result named "count" exists
} results_t;

static int compare_strings(const void *a, const void *b) {
  return strcmp(*(char *const *)a, *(char *const *)b);
}

static results_t summarize_results(sourcekitd_response_t resp, const char *dump_tag) {
  results_t r = {0, 1469598103934665603ull, 1469598103934665603ull, 0};
  sourcekitd_variant_t arr =
      sourcekitd_variant_dictionary_get_value(sourcekitd_response_get_value(resp), K("key.results"));
  if (sourcekitd_variant_get_type(arr) != SOURCEKITD_VARIANT_TYPE_ARRAY)
    return r;
  r.count = sourcekitd_variant_array_get_count(arr);
  char **lines = calloc(r.count ? r.count : 1, sizeof *lines);
  for (size_t i = 0; i < r.count; i++) {
    sourcekitd_variant_t item = sourcekitd_variant_array_get_value(arr, i);
    const char *name = sourcekitd_variant_dictionary_get_string(item, K("key.name"));
    const char *type = sourcekitd_variant_dictionary_get_string(item, K("key.typename"));
    size_t n = strlen(name ? name : "") + strlen(type ? type : "") + 2;
    lines[i] = malloc(n);
    snprintf(lines[i], n, "%s\t%s", name ? name : "", type ? type : "");
    r.ordered = fnv(r.ordered, lines[i]);
    if (name && !strcmp(name, "count"))
      r.found = 1;
  }
  qsort(lines, r.count, sizeof *lines, compare_strings);
  for (size_t i = 0; i < r.count; i++) {
    r.hash = fnv(r.hash, lines[i]);
    if (dump_tag && getenv("SKD_CANCEL_DUMP"))
      printf("dump %s %s\n", dump_tag, lines[i]);
    free(lines[i]);
  }
  free(lines);
  return r;
}

// key.reusingastcontext: the completion reused the compiler instance cached by the one before (fast completion);
// sourcekitd leaves the key out when it did not.
static int reused_ast(sourcekitd_response_t resp) {
  return sourcekitd_variant_dictionary_get_bool(sourcekitd_response_get_value(resp), K("key.reusingastcontext"));
}

static long offset_after(const char *pattern) {
  const char *p = strstr(TEXT, pattern);
  return p ? (long)(p - TEXT + strlen(pattern)) : -1;
}

static sourcekitd_response_t complete(const char *after, int baseline, double cancel_after_ms, double *ms,
                                      double *cancel_ms) {
  sourcekitd_object_t req = request("source.request.codecomplete");
  sourcekitd_request_dictionary_set_string(req, K("key.sourcefile"), PATH_);
  sourcekitd_request_dictionary_set_string(req, K("key.sourcetext"), TEXT);
  sourcekitd_request_dictionary_set_int64(req, K("key.offset"), offset_after(after));
  sourcekitd_object_t args = compiler_args(baseline);
  sourcekitd_request_dictionary_set_value(req, K("key.compilerargs"), args);
  sourcekitd_request_release(args);
  return send(req, cancel_after_ms, ms, cancel_ms);
}

// ---------------------------------------------------------------- main

static void write_source(const char *path, int statements, int closure_statements) {
  FILE *f = fopen(path, "w");
  if (!f) {
    perror(path);
    exit(2);
  }
  // The type of 'table' is inferred from the closure's return statement, so finding it type-checks every
  // statement of the closure (which the constraint solver does as one system: keep it short).
  fputs("let table = { () in\n  var t: [Int] = []\n", f);
  for (int i = 0; i < closure_statements; i++)
    fprintf(f, "  t.append([%d, %d, %d].map { $0 &* 3 }.filter { $0 > 1 }.reduce(0, &+))\n", i, i + 1, i + 2);
  fputs("  return t\n}()\n\n", f);
  // A long function body, where most of the time of the diagnostics goes.
  fputs("func work() -> Int {\n  var total = 0\n", f);
  for (int i = 0; i < statements; i++)
    fprintf(f, "  total &+= [%d, %d, %d].map { $0 &* 3 }.filter { $0 > 1 }.reduce(0, &+)\n", i, i + 1, i + 2);
  fputs("  return total\n}\n\n"
        "func probe() -> Int {\n  let s = \"abc\"\n  return s.count\n}\n\n"
        "func use() -> Int {\n  return table.count\n}\n\n"
        "func broken() -> Int {\n  let x: String = 42\n  return x.count\n}\n",
        f);
  fclose(f);
}

static char *read_file(const char *path, size_t *len) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *buf = malloc((size_t)n + 1);
  *len = fread(buf, 1, (size_t)n, f);
  buf[*len] = 0;
  fclose(f);
  return buf;
}

int main(int argc, char **argv) {
  const char *work = NULL;
  int statements = 400, closure_statements = 30, rounds = 3;
  for (int i = 1; i + 1 < argc; i += 2) {
    if (!strcmp(argv[i], "--sdk")) SDK = argv[i + 1];
    else if (!strcmp(argv[i], "--mc")) MC = argv[i + 1];
    else if (!strcmp(argv[i], "--workdir")) work = argv[i + 1];
    else if (!strcmp(argv[i], "--statements")) statements = atoi(argv[i + 1]);
    else if (!strcmp(argv[i], "--closure-statements")) closure_statements = atoi(argv[i + 1]);
    else if (!strcmp(argv[i], "--rounds")) rounds = atoi(argv[i + 1]);
  }
  if (!SDK || !MC || !work) {
    fprintf(stderr, "usage: skd-cancel --sdk DIR --mc DIR --workdir DIR [--statements N] [--closure-statements N]"
                    " [--rounds N]\n");
    return 2;
  }
  size_t plen = strlen(work) + 32;
  PATH_ = malloc(plen);
  snprintf(PATH_, plen, "%s/cancel.swift", work);
  write_source(PATH_, statements, closure_statements);
  TEXT = read_file(PATH_, &TEXT_LEN);
  setvbuf(stdout, NULL, _IOLBF, 0);
  sourcekitd_initialize();
  int failures = 0, diag_cancelled = 0, complete_cancelled = 0;
  double ms, cms;

  // --- diagnostics
  sourcekitd_object_t open = request("source.request.editor.open");
  sourcekitd_request_dictionary_set_string(open, K("key.name"), PATH_);
  sourcekitd_request_dictionary_set_string(open, K("key.sourcefile"), PATH_);
  sourcekitd_request_dictionary_set_string(open, K("key.sourcetext"), TEXT);
  sourcekitd_object_t oargs = compiler_args(0);
  sourcekitd_request_dictionary_set_value(open, K("key.compilerargs"), oargs);
  sourcekitd_request_release(oargs);
  sourcekitd_response_t resp = send(open, -1, &ms, &cms);
  if (sourcekitd_response_is_error(resp)) {
    printf("RESULT FAIL editor.open: %s\n", error_text(resp));
    return 1;
  }
  sourcekitd_response_dispose(resp);

  resp = diagnostics(-1, &ms, &cms);
  if (sourcekitd_response_is_error(resp)) {
    printf("RESULT FAIL reference diagnostics: %s\n", error_text(resp));
    return 1;
  }
  diags_t ref = summarize_diags(resp);
  sourcekitd_response_dispose(resp);
  double ref_ms = ms;
  printf("diag reference: %.0f ms, %zu diagnostics, %zu errors, hash %016llx, first error: %s\n", ref_ms, ref.count,
         ref.errors, (unsigned long long)ref.hash, ref.first_error);
  if (ref.errors != 1 || !strstr(ref.first_error, "String")) {
    printf("diag FAIL reference: expected exactly the deliberate type error\n");
    ++failures;
  }
  for (int r = 1; r <= rounds; r++) {
    if (!edit(r)) {
      ++failures;
      continue;
    }
    double delay = ref_ms * r / (rounds + 1);
    resp = diagnostics(delay, &ms, &cms);
    int cancelled = is_cancelled(resp);
    diag_cancelled += cancelled;
    printf("diag round %d: cancel at %.0f ms: %s, cancel to response %.1f ms\n", r, delay,
           cancelled ? "cancelled" : (sourcekitd_response_is_error(resp) ? error_text(resp) : "completed first"),
           cms);
    if (sourcekitd_response_is_error(resp) && !cancelled)
      ++failures;
    sourcekitd_response_dispose(resp);
    resp = diagnostics(-1, &ms, &cms);
    if (sourcekitd_response_is_error(resp)) {
      printf("diag FAIL round %d: diagnostics after the cancel: %s\n", r, error_text(resp));
      ++failures;
    } else {
      diags_t d = summarize_diags(resp);
      int same = d.count == ref.count && d.hash == ref.hash;
      printf("diag round %d: after the cancel: %.0f ms, %zu diagnostics, hash %016llx: %s\n", r, ms, d.count,
             (unsigned long long)d.hash, same ? "same as the reference" : "DIFFERENT");
      if (!same) {
        printf("  first error: %s\n", d.first_error);
        ++failures;
      }
    }
    sourcekitd_response_dispose(resp);
  }

  // --- completion
  resp = complete("return table.", 1, -1, &ms, &cms);
  if (sourcekitd_response_is_error(resp)) {
    printf("RESULT FAIL reference completion: %s\n", error_text(resp));
    return 1;
  }
  results_t cref = summarize_results(resp, "reference");
  sourcekitd_response_dispose(resp);
  double cref_ms = ms;
  printf("complete reference: %.0f ms, %zu results, hash %016llx, 'count' %s\n", cref_ms, cref.count,
         (unsigned long long)cref.hash, cref.found ? "found" : "MISSING");
  if (!cref.found) {
    printf("complete FAIL reference: no 'count' member of [Int]\n");
    ++failures;
  }
  // Control: the same sequence without a cancel must give the reference results too.
  resp = complete("return s.", 0, -1, &ms, &cms);
  sourcekitd_response_dispose(resp);
  resp = complete("return table.", 0, -1, &ms, &cms);
  if (sourcekitd_response_is_error(resp)) {
    printf("complete FAIL control: %s\n", error_text(resp));
    ++failures;
  } else {
    results_t c = summarize_results(resp, "control");
    int same = c.count == cref.count && c.hash == cref.hash, reused = reused_ast(resp);
    printf("complete control (no cancel): %.0f ms, %zu results, hash %016llx: %s, %s\n", ms, c.count,
           (unsigned long long)c.hash, same ? "same as the reference" : "DIFFERENT",
           reused ? "reused the cached AST" : "DID NOT REUSE the cached AST");
    failures += !same;
    // The warm repeat in the same file reuses the instance of the completion before; the rounds below rely on it.
    failures += !reused;
  }
  sourcekitd_response_dispose(resp);
  for (int r = 1; r <= rounds; r++) {
    // A cached compiler instance that has not looked at 'table' yet: replace the cached one (which has, after
    // the control) by one for other arguments, then by one for these arguments.
    resp = complete("return s.", 1, -1, &ms, &cms);
    sourcekitd_response_dispose(resp);
    resp = complete("return s.", 0, -1, &ms, &cms);
    double warm_ms = ms;
    if (sourcekitd_response_is_error(resp)) {
      printf("complete FAIL round %d: completion in probe(): %s\n", r, error_text(resp));
      ++failures;
    }
    sourcekitd_response_dispose(resp);
    // The completion at "table." reuses it and spends its time type-checking the closure.
    double delay = (cref_ms > warm_ms ? cref_ms - warm_ms : cref_ms) * r / (rounds + 1);
    resp = complete("return table.", 0, delay, &ms, &cms);
    int cancelled = is_cancelled(resp);
    complete_cancelled += cancelled;
    printf("complete round %d: cancel at %.0f ms: %s, cancel to response %.1f ms\n", r, delay,
           cancelled ? "cancelled" : (sourcekitd_response_is_error(resp) ? error_text(resp) : "completed first"),
           cms);
    if (sourcekitd_response_is_error(resp) && !cancelled)
      ++failures;
    sourcekitd_response_dispose(resp);
    resp = complete("return table.", 0, -1, &ms, &cms);
    if (sourcekitd_response_is_error(resp)) {
      printf("complete FAIL round %d: completion after the cancel: %s\n", r, error_text(resp));
      ++failures;
    } else {
      results_t c = summarize_results(resp, "after-cancel");
      int same = c.count == cref.count && c.hash == cref.hash, reused = reused_ast(resp);
      printf("complete round %d: after the %s: %.0f ms, %zu results, hash %016llx: %s, %s\n", r,
             cancelled ? "cancel" : "uncancelled completion", ms, c.count, (unsigned long long)c.hash,
             same ? "same as the reference" : "DIFFERENT", reused ? "reused the cached AST" : "new AST");
      failures += !same;
      // A cancelled completion leaves nothing for reuse: the next one builds a new AST.
      if (cancelled && reused) {
        printf("complete FAIL round %d: the completion after a cancelled one reused its AST\n", r);
        ++failures;
      }
    }
    sourcekitd_response_dispose(resp);
  }

  if (!diag_cancelled) {
    printf("diag FAIL no cancel took effect (all requests completed first)\n");
    ++failures;
  }
  if (!complete_cancelled) {
    printf("complete FAIL no cancel took effect (all requests completed first)\n");
    ++failures;
  }
  sourcekitd_object_t close = request("source.request.editor.close");
  sourcekitd_request_dictionary_set_string(close, K("key.name"), PATH_);
  resp = send(close, -1, &ms, &cms);
  sourcekitd_response_dispose(resp);
  sourcekitd_shutdown();
  printf("RESULT %s failures=%d diag_cancelled=%d/%d complete_cancelled=%d/%d\n", failures ? "FAIL" : "PASS",
         failures, diag_cancelled, rounds, complete_cancelled, rounds);
  return failures ? 1 : 0;
}
