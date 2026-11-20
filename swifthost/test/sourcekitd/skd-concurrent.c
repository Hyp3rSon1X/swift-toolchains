// Finds out what happens when the in-process sourcekitd and swifthost_swift_frontend run at the same
// time in one process (it does not try to fix anything):
//   phase 1: the probe alone (editor.open, cursor info, 20 completions, diagnostics, editor.close);
//   phase 2: the frontend compile alone (return code, stderr, object hash);
//   phase 3: the compile on one thread while another thread repeats the probe iteration until the
//            compile ends; every iteration's results are compared with phase 1, the compile's with phase 2.
// Output lines start with "A:" (compile), "B:" (probe) or "RESULT". Exit status 0 when nothing differed.
//
//   skd-concurrent <probe options as skd-probe-main> --frontend-args FILE --object PATH --workdir DIR
// FILE has one frontend argument per line (argv[0] = swift-frontend first); PATH is the -o output.
#include "skd-probe.h"
#include "SwiftHostCompiler.h"

#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static pthread_mutex_t out_lock = PTHREAD_MUTEX_INITIALIZER;
static void say(const char *who, const char *line) {
  pthread_mutex_lock(&out_lock);
  printf("%s: %s\n", who, line);
  fflush(stdout);
  pthread_mutex_unlock(&out_lock);
}

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static uint64_t fnv_bytes(uint64_t h, const char *p, size_t n) {
  for (size_t i = 0; i < n; i++)
    h = (h ^ (unsigned char)p[i]) * 1099511628211ull;
  return h;
}

static uint64_t file_hash(const char *path, long *size) {
  FILE *f = fopen(path, "rb");
  uint64_t h = 1469598103934665603ull;
  *size = -1;
  if (!f)
    return 0;
  char chunk[1 << 16];
  size_t n;
  *size = 0;
  while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) {
    h = fnv_bytes(h, chunk, n);
    *size += (long)n;
  }
  fclose(f);
  return h;
}

// ---- probe side: signature = hash of every step's deterministic result text

typedef struct {
  const skd_probe_config *cfg;
  uint64_t sig;
  int steps;
  int failed_status; // a step reported status=error or check=fail
  char first_bad[512];
  int verbose;
} iter_t;

static void collect(const char *line, void *ctx) {
  iter_t *it = ctx;
  if (strncmp(line, "step=", 5))
    return;
  const char *r = strstr(line, " result=\"");
  const char *e = strstr(line, "\" fp=");
  if (!r || !e)
    return;
  it->sig = fnv_bytes(it->sig, r, (size_t)(e - r));
  it->steps++;
  if ((strstr(line, "status=error") || strstr(line, "check=fail")) && !it->first_bad[0])
    snprintf(it->first_bad, sizeof it->first_bad, "%.400s", line);
  if (it->verbose)
    say("B", line);
}

static uint64_t run_iteration(const skd_probe_config *base, const char *steps, int verbose, iter_t *out) {
  skd_probe_config cfg = *base;
  cfg.steps = steps;
  cfg.sample_interval_ms = 0;
  iter_t it = {.cfg = &cfg, .sig = 1469598103934665603ull, .verbose = verbose};
  skd_probe_run(&cfg, collect, &it);
  *out = it;
  return it.sig;
}

// ---- compile side

typedef struct {
  char **argv;
  int argc;
  const char *err_path, *object;
  int rc;
  double ms;
  uint64_t obj_hash, err_hash;
  long obj_size, err_size;
} compile_t;

static void *compile_main(void *arg) {
  compile_t *c = arg;
  int err = open(c->err_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  unlink(c->object);
  double t0 = now_ms();
  c->rc = swifthost_swift_frontend(c->argc, (const char **)c->argv, -1, err);
  c->ms = now_ms() - t0;
  close(err);
  c->obj_hash = file_hash(c->object, &c->obj_size);
  c->err_hash = file_hash(c->err_path, &c->err_size);
  return NULL;
}

static void run_compile_thread(compile_t *c) {
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 64u << 20);
  pthread_t t;
  pthread_create(&t, &attr, compile_main, c);
  pthread_join(t, NULL);
}

typedef struct {
  const skd_probe_config *cfg;
  const char *steps;
  uint64_t baseline;
  atomic_int *stop;
  int iterations, mismatches, bad_steps;
  char first_mismatch[600];
} prober_t;

static void *prober_main(void *arg) {
  prober_t *p = arg;
  do {
    iter_t it;
    uint64_t sig = run_iteration(p->cfg, p->steps, 0, &it);
    p->iterations++;
    if (!p->baseline) p->baseline = sig; // no phase 1: the first iteration is the reference
    char line[700];
    snprintf(line, sizeof line, "iteration %d: %d steps, signature %016llx (%s)%s%s", p->iterations, it.steps,
             (unsigned long long)sig, sig == p->baseline ? "same as alone" : "DIFFERENT from alone",
             it.first_bad[0] ? " first failing step: " : "", it.first_bad);
    say("B", line);
    if (sig != p->baseline) {
      p->mismatches++;
      snprintf(p->first_mismatch, sizeof p->first_mismatch, "%s", line);
    }
    if (it.first_bad[0])
      p->bad_steps++;
  } while (!atomic_load(p->stop));
  return NULL;
}

static int ends_with(const char *s, const char *suffix) {
  size_t a = strlen(s), b = strlen(suffix);
  return a >= b && !strcmp(s + a - b, suffix);
}

int main(int argc, char **argv) {
  skd_probe_config cfg;
  skd_probe_config_init(&cfg);
  const char *phases = "pcx", *files[4096], *fe_args_file = NULL, *object = NULL, *workdir = NULL, *module_dir = NULL;
  size_t nfiles = 0;
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
#define OPT(name) (!strcmp(a, name) && v && (i++, 1))
    if (OPT("--yaml-dir")) cfg.yaml_dir = v;
    else if (OPT("--sdk")) cfg.sdk = v;
    else if (OPT("--mc")) cfg.module_cache = v;
    else if (OPT("--module-dir")) module_dir = v;
    else if (OPT("--file")) files[nfiles++] = v;
    else if (OPT("--primary")) cfg.primary = v;
    else if (OPT("--complete-after")) { cfg.complete_pattern = v; cfg.complete_delta = (int)strlen(v); }
    else if (OPT("--complete-expect")) cfg.complete_expect = v;
    else if (OPT("--cursor-at")) cfg.cursor_pattern = v;
    else if (OPT("--cursor-expect")) cfg.cursor_expect = v;
    else if (OPT("--frontend-args")) fe_args_file = v;
    else if (OPT("--object")) object = v;
    else if (OPT("--workdir")) workdir = v;
    else if (OPT("--phases")) phases = v;
    else { fprintf(stderr, "unknown or incomplete option %s\n", a); return 2; }
  }
  if (module_dir) {
    DIR *d = opendir(module_dir);
    struct dirent *e;
    while (d && (e = readdir(d)) && nfiles < 4096)
      if (ends_with(e->d_name, ".swift")) {
        char *p;
        if (asprintf(&p, "%s/%s", module_dir, e->d_name) > 0)
          files[nfiles++] = p;
      }
    if (d) closedir(d);
  }
  if (!cfg.primary && nfiles) cfg.primary = files[0];
  cfg.files = files;
  cfg.file_count = nfiles;
  if (!cfg.yaml_dir || !fe_args_file || !object || !workdir) {
    fprintf(stderr, "--yaml-dir, --frontend-args, --object and --workdir are required\n");
    return 2;
  }

  // frontend argv
  char *text = NULL;
  {
    FILE *f = fopen(fe_args_file, "rb");
    if (!f) { perror(fe_args_file); return 2; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    text = calloc((size_t)n + 1, 1);
    if (fread(text, 1, (size_t)n, f) != (size_t)n) return 2;
    fclose(f);
  }
  char **fe_argv = calloc(65536, sizeof(char *));
  int fe_argc = 0;
  for (char *save, *l = strtok_r(text, "\n", &save); l; l = strtok_r(NULL, "\n", &save))
    if (*l) fe_argv[fe_argc++] = l;

  // init/shutdown are reference counted: phase 1 keeps one reference, so the iterations never tear sourcekitd down.
  const char *iter_steps = "init,open,cursor,complete*20,diag,close,shutdown";
  char err_path[4096];
  snprintf(err_path, sizeof err_path, "%s/compile.err", workdir);
  say("RESULT", "version");
  printf("%s\n", swifthost_version());

  // phase 1: probe alone
  iter_t alone = {0};
  uint64_t baseline = 0;
  char line[700];
  if (strchr(phases, 'p')) {
    skd_probe_config c = cfg;
    c.steps = "init";
    iter_t it;
    run_iteration(&c, "init", 0, &it);
  double t0 = now_ms();
  baseline = run_iteration(&cfg, iter_steps, 1, &alone);
  snprintf(line, sizeof line, "probe alone: %d steps in %.0f ms, signature %016llx%s%s", alone.steps, now_ms() - t0,
           (unsigned long long)baseline, alone.first_bad[0] ? " FAILING: " : "", alone.first_bad);
  say("B", line);
  }

  // phase 2: compile alone
  compile_t c1 = {.argv = fe_argv, .argc = fe_argc, .err_path = err_path, .object = object};
  if (strchr(phases, 'c'))
  run_compile_thread(&c1);
  if (strchr(phases, 'c')) {
  snprintf(line, sizeof line, "compile alone: rc=%d %.0f ms object=%ld bytes hash=%016llx stderr=%ld bytes hash=%016llx",
           c1.rc, c1.ms, c1.obj_size, (unsigned long long)c1.obj_hash, c1.err_size, (unsigned long long)c1.err_hash);
  say("A", line);
  }
  if (!strchr(phases, 'x'))
    return 0;

  // phase 3: both
  atomic_int stop = 0;
  prober_t pr = {.cfg = &cfg, .steps = iter_steps, .baseline = baseline, .stop = &stop};
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 16u << 20);
  pthread_t tp;
  compile_t c2 = {.argv = fe_argv, .argc = fe_argc, .err_path = err_path, .object = object};
  pthread_t tc;
  pthread_attr_t cattr;
  pthread_attr_init(&cattr);
  pthread_attr_setstacksize(&cattr, 64u << 20);
  say("RESULT", "concurrent phase starts");
  pthread_create(&tp, &attr, prober_main, &pr);
  pthread_create(&tc, &cattr, compile_main, &c2);
  pthread_join(tc, NULL);
  snprintf(line, sizeof line, "compile concurrent: rc=%d %.0f ms object=%ld bytes hash=%016llx stderr=%ld bytes hash=%016llx",
           c2.rc, c2.ms, c2.obj_size, (unsigned long long)c2.obj_hash, c2.err_size, (unsigned long long)c2.err_hash);
  say("A", line);
  atomic_store(&stop, 1);
  pthread_join(tp, NULL);

  int compile_same = c1.rc == c2.rc && c1.obj_hash == c2.obj_hash && c1.err_hash == c2.err_hash;
  snprintf(line, sizeof line,
           "probe iterations during/after compile: %d, mismatches vs alone: %d, iterations with a failing step: %d; compile result %s alone",
           pr.iterations, pr.mismatches, pr.bad_steps, compile_same ? "identical to" : "DIFFERENT from");
  say("RESULT", line);
  return (pr.mismatches || pr.bad_steps || !compile_same || c1.rc != 0) ? 1 : 0;
}
