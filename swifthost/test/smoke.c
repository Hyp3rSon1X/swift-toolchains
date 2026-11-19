/*
 * C2 smoke test for SwiftHostCompiler: uses only the public header.
 *
 *   smoke <sdk-target-dir> <work-dir> <iwasm|-> <test-sources-dir> [mode]
 *
 * <sdk-target-dir> is .../swift-6.4.2-RELEASE_wasm/wasm32-unknown-wasip1.
 * Without a mode, runs every library call on one worker thread with a
 * SWIFTHOST_MIN_STACK_SIZE stack, checks results and outputs, checks that no
 * signal disposition or alternate signal stack changed, and ends with the
 * crash/poison check. The other modes each end with the library poisoned, so
 * each needs a process of its own:
 *   overflow       a stack overflow on a calling thread with a 512 KiB stack
 *   helper-crash   a crash in a clang module build (on the compiler's helper
 *                  thread)
 *   xllvm-version  -Xllvm -version, which ends in LLVM's exit path with 0
 *   deep-reply     a plugin reply of 4 Mi nested JSON arrays
 * and these modes need a process of their own for other reasons:
 *   plugin-handler macros through swifthost_set_plugin_message_handler (it is
 *                  process-wide and can be set only once)
 *   exec-plugin    every way to name an executable plugin; run-smoke.sh runs
 *                  it with an interposer that aborts on any process creation
 * Macros use test/plugin-server.c, built next to smoke as
 * libswifthost-test-plugin-server.dylib and also linked into smoke itself.
 * Exit status 0 = all checks passed.
 */
#include "SwiftHostCompiler.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

static const char *SDK, *WORK, *IWASM, *TESTS;
static char SW[4096], CACHE[4096], PLUGIN_SERVER[PATH_MAX];

/* test/plugin-server.c, linked into smoke too (the handler of the
   plugin-handler mode); the dynamic library has its own copy. */
bool swift_inproc_plugins_handle_message(const char *message, size_t length, char **response,
                                         size_t *responseLength);
int swifthost_test_plugin_count(const char *kind);
const char *swifthost_test_plugin_last_library(void);
static int failures;

#define CHECK(cond, ...)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      ++failures;                                                              \
      printf("FAIL %s:%d: ", __FILE__, __LINE__);                              \
      printf(__VA_ARGS__);                                                     \
      printf("\n");                                                            \
    }                                                                          \
  } while (0)

static double nowMs(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static char *fmt(const char *f, ...) {
  char *s = NULL;
  va_list ap;
  va_start(ap, f);
  if (vasprintf(&s, f, ap) < 0)
    abort();
  va_end(ap);
  return s;
}

static char *readFile(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return strdup("");
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *buf = malloc((size_t)n + 1);
  size_t got = fread(buf, 1, (size_t)n, f);
  buf[got] = 0;
  fclose(f);
  return buf;
}

static void writeFile(const char *path, const char *text) {
  FILE *f = fopen(path, "wb");
  fputs(text, f);
  fclose(f);
}

/* argv builder */
typedef struct {
  const char *v[512];
  int n;
} Args;
static void add(Args *a, const char *s) { a->v[a->n++] = s; }

typedef int (*Entry)(int, const char **, int, int);

/* Calls entry with the given descriptors. */
static int callFds(Entry entry, Args *a, int outFd, int errFd) {
  a->v[a->n] = NULL;
  return entry(a->n, a->v, outFd, errFd);
}

/* Calls entry with err_fd and out_fd redirected to files; returns rc and the
   diagnostics text in *diag (caller frees). */
static int call(Entry entry, Args *a, const char *tag, char **diag) {
  char *errPath = fmt("%s/%s.err", WORK, tag);
  char *outPath = fmt("%s/%s.out", WORK, tag);
  int errFd = open(errPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  int outFd = open(outPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  a->v[a->n] = NULL;
  int rc = entry(a->n, a->v, outFd, errFd);
  close(errFd);
  close(outFd);
  if (diag)
    *diag = readFile(errPath);
  free(errPath);
  free(outPath);
  return rc;
}

static void addFrontendCommon(Args *a, const char *obj) {
  add(a, "-target"); add(a, "wasm32-unknown-wasip1");
  add(a, "-disable-objc-interop");
  add(a, "-sdk"); add(a, fmt("%s/WASI.sdk", SDK));
  add(a, "-resource-dir"); add(a, SW);
  add(a, "-use-static-resource-dir");
  add(a, "-module-cache-path"); add(a, CACHE);
  add(a, "-no-color-diagnostics");
  add(a, "-Xcc"); add(a, "-fno-color-diagnostics");
  add(a, "-empty-abi-descriptor");
  add(a, "-no-auto-bridging-header-chaining");
  add(a, "-module-name"); add(a, "main");
  add(a, "-o"); add(a, obj);
}

typedef struct {
  int rc;
  int stage; /* 1 frontend, 2 autolink, 3 wasm-ld */
  double fe, al, ld;
  char *diag;
} Build;

/* Full build of src with the job recipe. extra = frontend extras (NULL-terminated). */
static Build build(const char *src, const char *tag, const char **extra) {
  Build b = {0};
  char *obj = fmt("%s/%s.o", WORK, tag);
  char *al = fmt("%s/%s.autolink", WORK, tag);
  char *wasm = fmt("%s/%s.wasm", WORK, tag);
  unlink(wasm);

  Args fe = {0};
  add(&fe, "swift-frontend");
  add(&fe, "-c");
  add(&fe, src);
  addFrontendCommon(&fe, obj);
  for (; extra && *extra; ++extra)
    add(&fe, *extra);
  double t0 = nowMs();
  b.rc = call(swifthost_swift_frontend, &fe, fmt("%s-fe", tag), &b.diag);
  b.fe = nowMs() - t0;
  b.stage = 1;
  if (b.rc)
    return b;

  Args ax = {0};
  add(&ax, "swift-autolink-extract");
  add(&ax, obj);
  add(&ax, "-o");
  add(&ax, al);
  t0 = nowMs();
  b.rc = call(swifthost_swift_autolink_extract, &ax, fmt("%s-al", tag), &b.diag);
  b.al = nowMs() - t0;
  b.stage = 2;
  if (b.rc)
    return b;

  Args ld = {0};
  add(&ld, "wasm-ld");
  add(&ld, "-m"); add(&ld, "wasm32");
  add(&ld, fmt("-L%s/wasi", SW));
  add(&ld, fmt("-L%s/WASI.sdk/lib/wasm32-wasip1", SDK));
  add(&ld, fmt("%s/WASI.sdk/lib/wasm32-wasip1/crt1-command.o", SDK));
  add(&ld, fmt("%s/wasi/wasm32/swiftrt.o", SW));
  add(&ld, obj);
  char *lines = readFile(al);
  for (char *save = NULL, *l = strtok_r(lines, "\n", &save); l;
       l = strtok_r(NULL, "\n", &save))
    if (*l)
      add(&ld, l);
  const char *tail[] = {"-lswiftSwiftOnoneSupport", "-ldl", "-lc++", "-lc++abi", "-lm",
                        "-lwasi-emulated-mman", "-lwasi-emulated-signal",
                        "-lwasi-emulated-process-clocks", "--error-limit=0", "--threads=1",
                        "--global-base=4096", "--table-base=4096", "-z", "stack-size=131072",
                        "-lc", NULL};
  for (const char **t = tail; *t; ++t)
    add(&ld, *t);
  add(&ld, fmt("%s/clang/lib/wasip1/libclang_rt.builtins-wasm32.a", SW));
  add(&ld, "-o");
  add(&ld, wasm);
  t0 = nowMs();
  b.rc = call(swifthost_wasm_ld, &ld, fmt("%s-ld", tag), &b.diag);
  b.ld = nowMs() - t0;
  b.stage = 3;
  return b;
}

/* Runs the module in WAMR, returns its stdout (caller frees). With iwasm "-"
   (simulator runs: a simulator process cannot run a macOS binary) nothing is
   run and NULL is returned; run-smoke.sh runs the modules on the host. */
static char *runWasm(const char *tag) {
  if (!strcmp(IWASM, "-"))
    return NULL;
  char *wasm = fmt("%s/%s.wasm", WORK, tag);
  char *outPath = fmt("%s/%s.run", WORK, tag);
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_addopen(&fa, 1, outPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  const char *argv[] = {IWASM, wasm, NULL};
  pid_t pid;
  int status = -1;
  if (posix_spawn(&pid, IWASM, &fa, NULL, (char *const *)argv, environ) == 0)
    waitpid(pid, &status, 0);
  posix_spawn_file_actions_destroy(&fa);
  CHECK(status == 0, "%s: iwasm exit status %d", tag, status);
  return readFile(outPath);
}

typedef struct {
  struct sigaction sa[NSIG];
} SigSnap;
static void snap(SigSnap *s) {
  memset(s, 0, sizeof(*s));
  for (int i = 1; i < NSIG; ++i)
    if (i != SIGKILL && i != SIGSTOP)
      sigaction(i, NULL, &s->sa[i]);
}
static int snapDiff(const SigSnap *a, const SigSnap *b, const char *when) {
  int diffs = 0;
  for (int i = 1; i < NSIG; ++i)
    if (a->sa[i].sa_handler != b->sa[i].sa_handler ||
        a->sa[i].sa_flags != b->sa[i].sa_flags) {
      printf("FAIL signal %d (%s) disposition changed %s\n", i, strsignal(i), when);
      ++diffs;
    }
  failures += diffs;
  return diffs;
}

/* Swift modules rebuilt from .swiftinterface would land in the module cache;
   the SDK's binary modules must load directly. */
static int rebuiltModules;
static int countSwiftModule(const char *path, const struct stat *st, int type, struct FTW *ftw) {
  size_t n = strlen(path);
  if (type == FTW_F && n > 12 && !strcmp(path + n - 12, ".swiftmodule")) {
    printf("FAIL rebuilt Swift module in the module cache: %s\n", path);
    ++rebuiltModules;
  }
  return 0;
}

/* Cancels the frontend call that is running after delayMs. */
typedef struct {
  int delayMs;
  double requestedAt;
} CancelCtx;
static void *canceller(void *p) {
  CancelCtx *c = p;
  usleep((useconds_t)c->delayMs * 1000);
  c->requestedAt = nowMs();
  swifthost_request_cancel();
  return NULL;
}

static const char *HELLO_EXPECT = "hi\n";
static const char *FOUNDATION_EXPECT =
    "hi from Foundation 1970-01-01T00:00:00Z {\"n\":42} example.org\n";

static double peakRssMiB(void) {
  struct rusage ru;
  getrusage(RUSAGE_SELF, &ru);
  return (double)ru.ru_maxrss / (1024.0 * 1024.0); /* bytes on Darwin */
}

static void report(const char *tag, Build *b) {
  printf("%-24s rc %3d  frontend %6.0f ms  autolink %4.0f ms  wasm-ld %5.0f ms  peak rss %4.0f MiB\n",
         tag, b->rc, b->fe, b->al, b->ld, peakRssMiB());
  if (b->rc && b->diag)
    printf("  diagnostics: %.600s\n", b->diag);
}

/* The calling thread's alternate signal stack must be the same after every
   call, including one that recovered from a crash (the library installs its
   own for the duration of a call). */
static stack_t altStack;
static void installAltStack(void) {
  altStack.ss_size = 4 * SIGSTKSZ;
  altStack.ss_sp = malloc(altStack.ss_size);
  altStack.ss_flags = 0;
  if (sigaltstack(&altStack, NULL) != 0)
    perror("sigaltstack");
}
static void checkAltStack(const char *when) {
  stack_t now;
  sigaltstack(NULL, &now);
  CHECK(now.ss_sp == altStack.ss_sp && now.ss_size == altStack.ss_size && now.ss_flags == 0,
        "alternate signal stack %s: sp %p size %zu flags %d (expected %p %zu 0)", when,
        now.ss_sp, (size_t)now.ss_size, now.ss_flags, altStack.ss_sp, (size_t)altStack.ss_size);
}

/* A source file of n function bodies of four statements each. Even with the
   constraint solver giving up at once after a cancel, checking all of them
   takes seconds. */
static char *writeFunctions(const char *name, int n) {
  char *path = fmt("%s/%s", WORK, name);
  FILE *f = fopen(path, "w");
  for (int i = 0; i < n; ++i) {
    fprintf(f, "func f%d() -> Int {\n", i);
    for (int k = 0; k < 4; ++k)
      fprintf(f, "  let a%d = [1, 2, 3].map { $0 * 2 + %d }.filter { $0 > 1 }.reduce(0, +) + "
                 "Int(Double(%d) * 1.5)\n", k, i, k);
    fputs("  return a0 + a1 + a2 + a3\n}\n", f);
  }
  fclose(f);
  return path;
}

/* A freestanding expression macro implemented by test/plugin-server.c. */
static const char *MACRO_SOURCE =
    "@freestanding(expression) macro answer() -> Int = "
    "#externalMacro(module: \"M\", type: \"AnswerMacro\")\n"
    "print(#answer)\n";
static const char *MACRO_EXPECT = "42\n";

/* The macro source file, and the (empty) library file that names module M:
   the compiler only passes its resolved path to the plugin server. */
static char *macroSource(void) {
  char *src = fmt("%s/answer.swift", WORK);
  writeFile(src, MACRO_SOURCE);
  writeFile(fmt("%s/M.wasm", WORK), "");
  return src;
}

/* Builds the macro source through the in-process plugin server at
   serverPath; reply selects the server's reply to the expansion request. */
static Build buildMacro(const char *tag, const char *serverPath, const char *reply) {
  char *src = macroSource();
  if (reply)
    setenv("SWIFTHOST_TEST_PLUGIN_REPLY", reply, 1);
  else
    unsetenv("SWIFTHOST_TEST_PLUGIN_REPLY");
  setenv("SWIFTHOST_TEST_PLUGIN_FILE", src, 1);
  const char *extra[] = {"-in-process-plugin-server-path", serverPath, "-load-resolved-plugin",
                         fmt("%s/M.wasm##M", WORK), NULL};
  Build b = build(src, tag, extra);
  unsetenv("SWIFTHOST_TEST_PLUGIN_REPLY");
  return b;
}

typedef int (*CountFn)(const char *);

/* The message counters of the plugin server library loaded by the compiler. */
static CountFn serverCounter(void) {
  void *h = dlopen(PLUGIN_SERVER, RTLD_LAZY | RTLD_NOLOAD);
  return h ? (CountFn)dlsym(h, "swifthost_test_plugin_count") : NULL;
}

static void checkMacroBuild(const char *tag, Build *b, CountFn count) {
  report(tag, b);
  CHECK(b->rc == 0, "%s failed", tag);
  CHECK(count != NULL, "%s: plugin server counters not found", tag);
  if (count) {
    printf("  plugin messages: getCapability %d, loadPluginLibrary %d, expandFreestandingMacro %d, "
           "other %d\n", count("getCapability"), count("loadPluginLibrary"),
           count("expandFreestandingMacro"), count("other"));
    CHECK(count("getCapability") > 0, "%s: no getCapability message", tag);
    CHECK(count("loadPluginLibrary") > 0, "%s: no loadPluginLibrary message", tag);
    CHECK(count("expandFreestandingMacro") > 0, "%s: no expansion request", tag);
    CHECK(count("other") == 0, "%s: unexpected plugin messages", tag);
  }
  if (!b->rc) {
    char *out = runWasm(tag);
    CHECK(!out || !strcmp(out, MACRO_EXPECT), "%s output %s", tag, out);
  }
}

/* Malformed and hostile replies from the plugin server are macro errors: rc
   1, a diagnostic, no poison. */
static void checkBadReplies(void) {
  static const struct {
    const char *mode, *expect;
  } cases[] = {
      {"truncated", "(from macro 'answer')"},
      {"garbage", "(from macro 'answer')"},
      {"wrong-kind", "plugin returned invalid result"},
      {"empty", "(from macro 'answer')"},
      {"bad-positions", "hostile diagnostic"},
      {"error", "failed to send request to plugin"},
  };
  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
    char *tag = fmt("macro-reply-%s", cases[i].mode);
    Build b = buildMacro(tag, PLUGIN_SERVER, cases[i].mode);
    report(tag, &b);
    CHECK(b.rc == 1 && b.stage == 1, "%s: rc %d stage %d", tag, b.rc, b.stage);
    CHECK(b.diag && strstr(b.diag, cases[i].expect), "%s: diagnostics lack \"%s\": %s", tag,
          cases[i].expect, b.diag ? b.diag : "(null)");
  }
}

/* Executable plugins are rejected with a diagnostic and never launched. The
   executable exists (/bin/true does not on macOS 27): the compiler resolves
   the path before it gets to the plugin. */
static void checkExecutablePlugins(void) {
  char *src = macroSource();
  char *dir = fmt("%s/external-plugins", WORK);
  mkdir(dir, 0755);
  writeFile(fmt("%s/libM.dylib", dir), "");
  const char *extras[][3] = {
      {"-load-plugin-executable", "/usr/bin/true#M", NULL},
      {"-external-plugin-path", fmt("%s#/usr/bin/true", dir), NULL},
      {"-load-resolved-plugin", fmt("%s/M.wasm#/usr/bin/true#M", WORK), NULL},
  };
  const char *tags[] = {"exec-plugin-executable", "exec-plugin-external-path",
                        "exec-plugin-resolved"};
  for (int i = 0; i < 3; ++i) {
    Build b = build(src, tags[i], extras[i]);
    report(tags[i], &b);
    CHECK(b.rc == 1 && b.stage == 1, "%s: rc %d stage %d", tags[i], b.rc, b.stage);
    CHECK(b.diag && strstr(b.diag, "executable plugins are not supported in this build"),
          "%s: diagnostics: %s", tags[i], b.diag ? b.diag : "(null)");
  }
}

static void *worker(void *unused) {
  (void)unused;
  SigSnap before, after;
  snap(&before);
  installAltStack();
  const char *O[] = {"-O", "-enable-default-cmo", NULL};
  char *hello = fmt("%s/hello.swift", TESTS);
  char *found = fmt("%s/hello_foundation.swift", TESTS);

  const char *v = swifthost_version();
  printf("version: %s\n", v);
  CHECK(v && strstr(v, "6.4.2"), "version string lacks 6.4.2");

  for (int i = 0; i < 3; ++i) {
    char *tag = fmt("hello-onone-%d", i);
    Build b = build(hello, tag, NULL);
    report(tag, &b);
    CHECK(b.rc == 0, "%s failed", tag);
    if (!b.rc) {
      char *out = runWasm(tag);
      CHECK(!out || !strcmp(out, HELLO_EXPECT), "%s output %s", tag, out);
    }
  }
  for (int i = 0; i < 2; ++i) {
    char *tag = fmt("foundation-onone-%d", i);
    Build b = build(found, tag, NULL);
    report(tag, &b);
    CHECK(b.rc == 0, "%s failed", tag);
    if (!b.rc) {
      char *out = runWasm(tag);
      CHECK(!out || !strcmp(out, FOUNDATION_EXPECT), "%s output %s", tag, out);
    }
  }
  for (int i = 0; i < 2; ++i) {
    char *tag = fmt("foundation-O-%d", i);
    Build b = build(found, tag, O);
    report(tag, &b);
    CHECK(b.rc == 0, "%s failed", tag);
    if (!b.rc) {
      char *out = runWasm(tag);
      CHECK(!out || !strcmp(out, FOUNDATION_EXPECT), "%s output %s", tag, out);
    }
  }

  nftw(CACHE, countSwiftModule, 16, FTW_PHYS);
  failures += rebuiltModules;
  printf("module cache: %d rebuilt Swift module(s)\n", rebuiltModules);

  /* A compile error is an ordinary failure: rc 1, diagnostics, no poison. */
  char *bad = fmt("%s/bad.swift", WORK);
  writeFile(bad, "let x: Int = \"a\"\nprint(x)\n");
  Build e = build(bad, "compile-error", NULL);
  report("compile-error", &e);
  CHECK(e.rc == 1 && e.stage == 1, "compile error: rc %d stage %d", e.rc, e.stage);
  CHECK(e.diag && strstr(e.diag, "error:") && strstr(e.diag, "bad.swift:1:"),
        "compile error diagnostics missing: %s", e.diag ? e.diag : "(null)");

  /* A macro without a plugin server for its module: a clean error, no poison. */
  char *mac = fmt("%s/macro.swift", WORK);
  writeFile(mac, "import Observation\n@Observable final class M { var a = 1 }\nprint(M().a)\n");
  Build m = build(mac, "macro", NULL);
  report("macro", &m);
  CHECK(m.rc == 1 && m.stage == 1, "macro: rc %d stage %d", m.rc, m.stage);

  /* Macros through the in-process plugin server library. */
  Build md = buildMacro("macro-dylib", PLUGIN_SERVER, NULL);
  checkMacroBuild("macro-dylib", &md, serverCounter());
  checkBadReplies();
  checkExecutablePlugins();
  Build ma = buildMacro("macro-after-bad-replies", PLUGIN_SERVER, NULL);
  checkMacroBuild("macro-after-bad-replies", &ma, serverCounter());

  /* A link error (missing library) is an ordinary failure too. */
  {
    Args ld = {0};
    char *diag = NULL;
    add(&ld, "wasm-ld");
    add(&ld, "-m"); add(&ld, "wasm32");
    add(&ld, fmt("%s/hello-onone-0.o", WORK));
    add(&ld, "-lnosuchlibrary");
    add(&ld, "-o"); add(&ld, fmt("%s/link-error.wasm", WORK));
    int rc = call(swifthost_wasm_ld, &ld, "link-error", &diag);
    printf("%-24s rc %3d  %.200s", "link-error", rc, diag);
    CHECK(rc == 1, "link error rc %d", rc);
    CHECK(strstr(diag, "nosuchlibrary") != NULL, "link error diagnostics missing");
  }

  /* A descriptor that cannot be written loses that output and nothing else:
     -1, a closed descriptor, and a pipe without a reader (with SIGPIPE
     ignored, as the header requires). wasm-ld --version covers the buffered
     out_fd stream. */
  {
    Args fe = {0};
    add(&fe, "swift-frontend");
    add(&fe, "-c");
    add(&fe, bad);
    addFrontendCommon(&fe, fmt("%s/unwritable.o", WORK));
    Args ver = {0};
    add(&ver, "wasm-ld");
    add(&ver, "--version");
    int rc = callFds(swifthost_swift_frontend, &fe, -1, -1);
    printf("%-24s rc %3d\n", "err_fd -1", rc);
    CHECK(rc == 1, "compile error with err_fd -1: rc %d", rc);
    rc = callFds(swifthost_wasm_ld, &ver, -1, -1);
    printf("%-24s rc %3d\n", "wasm-ld out_fd -1", rc);
    CHECK(rc == 0, "wasm-ld --version with out_fd -1: rc %d", rc);

    int file = open(fmt("%s/closed.err", WORK), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    int closedFd = fcntl(file, F_DUPFD, 200); /* above anything the compiler opens */
    close(file);
    close(closedFd);
    rc = callFds(swifthost_swift_frontend, &fe, closedFd, closedFd);
    printf("%-24s rc %3d\n", "err_fd closed", rc);
    CHECK(rc == 1, "compile error with a closed err_fd: rc %d", rc);
    rc = callFds(swifthost_wasm_ld, &ver, closedFd, closedFd);
    printf("%-24s rc %3d\n", "wasm-ld out_fd closed", rc);
    CHECK(rc == 0, "wasm-ld --version with a closed out_fd: rc %d", rc);

    int pipeFds[2];
    pipe(pipeFds);
    close(pipeFds[0]);
    struct sigaction ignore = {0}, previous;
    ignore.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &ignore, &previous);
    rc = callFds(swifthost_swift_frontend, &fe, pipeFds[1], pipeFds[1]);
    printf("%-24s rc %3d\n", "err_fd broken pipe", rc);
    CHECK(rc == 1, "compile error with err_fd a pipe without a reader: rc %d", rc);
    rc = callFds(swifthost_wasm_ld, &ver, pipeFds[1], pipeFds[1]);
    printf("%-24s rc %3d\n", "wasm-ld broken pipe", rc);
    CHECK(rc == 0, "wasm-ld --version with out_fd a pipe without a reader: rc %d", rc);
    sigaction(SIGPIPE, &previous, NULL);
    close(pipeFds[1]);
  }

  /* Still healthy after the failures. */
  Build h = build(hello, "hello-after-errors", NULL);
  report("hello-after-errors", &h);
  CHECK(h.rc == 0, "build after errors failed");

  /* Cancellation. A request while nothing runs has no effect. */
  swifthost_request_cancel();
  Build idle = build(hello, "cancel-idle", NULL);
  report("cancel-idle", &idle);
  CHECK(idle.rc == 0, "build after an idle cancel request: rc %d", idle.rc);

  /* A long type check, cancelled 300 ms into the frontend call. */
  char *big = fmt("%s/big.swift", WORK);
  FILE *bf = fopen(big, "w");
  for (int i = 0; i < 6000; ++i)
    fprintf(bf, "let v%d = [1, 2, 3].map { $0 * 2 + %d }.filter { $0 > 1 }.reduce(0, +) + "
                "Int(Double(%d) * 1.5)\n", i, i, i);
  fclose(bf);
  CancelCtx cc = {300, 0};
  pthread_t ct;
  pthread_create(&ct, NULL, canceller, &cc);
  Build k = build(big, "cancelled", NULL);
  double latency = nowMs() - cc.requestedAt;
  pthread_join(ct, NULL);
  report("cancelled", &k);
  printf("  cancel latency %.0f ms (frontend ran %.0f ms)\n", latency, k.fe);
  CHECK(k.rc == SWIFTHOST_ECANCELED && k.stage == 1,
        "cancelled build: rc %d stage %d (expected %d in the frontend)", k.rc, k.stage,
        SWIFTHOST_ECANCELED);
  CHECK(latency < 2000, "cancel took %.0f ms", latency);

  /* Function bodies are type checked after the top-level declarations; a
     cancel lands among them. */
  char *funcs = writeFunctions("funcs.swift", 3000);
  CancelCtx fc = {300, 0};
  pthread_create(&ct, NULL, canceller, &fc);
  Build kf = build(funcs, "cancelled-bodies", NULL);
  latency = nowMs() - fc.requestedAt;
  pthread_join(ct, NULL);
  report("cancelled-bodies", &kf);
  printf("  cancel latency %.0f ms (frontend ran %.0f ms)\n", latency, kf.fe);
  CHECK(kf.rc == SWIFTHOST_ECANCELED && kf.stage == 1,
        "cancelled function bodies: rc %d stage %d (expected %d in the frontend)", kf.rc,
        kf.stage, SWIFTHOST_ECANCELED);
  CHECK(latency < 2000, "cancel in function bodies took %.0f ms", latency);

  Build ak = build(hello, "after-cancel", NULL);
  report("after-cancel", &ak);
  CHECK(ak.rc == 0, "build after a cancelled build: rc %d", ak.rc);
  if (!ak.rc) {
    char *out = runWasm("after-cancel");
    CHECK(!out || !strcmp(out, HELLO_EXPECT), "after-cancel output %s", out);
  }

  snap(&after);
  snapDiff(&before, &after, "after the builds");
  checkAltStack("after the builds");

  /* Crash recovery: the compiler traps, the call returns 128 + SIGTRAP and
     every later call returns SWIFTHOST_EPOISONED. */
  const char *crash[] = {"-debug-crash-after-parse", NULL};
  Build c = build(hello, "crash", crash);
  report("crash", &c);
  CHECK(c.rc == 128 + SIGTRAP || c.rc == 128 + SIGILL,
        "crash: rc %d (expected %d or %d)", c.rc, 128 + SIGTRAP, 128 + SIGILL);
  Build p = build(hello, "after-crash", NULL);
  report("after-crash", &p);
  CHECK(p.rc == SWIFTHOST_EPOISONED, "after crash: rc %d", p.rc);
  CHECK(p.diag && strstr(p.diag, "restart"), "poison message missing");
  SigSnap afterCrash;
  snap(&afterCrash);
  snapDiff(&before, &afterCrash, "after the crash");
  checkAltStack("after the crash");
  return NULL;
}

/* Runs a call that poisons the library, then checks that the process is
   intact: the next call returns SWIFTHOST_EPOISONED, the signal dispositions
   and the thread's alternate signal stack are unchanged. */
static void afterPoison(const char *tag, const SigSnap *before) {
  char *hello = fmt("%s/hello.swift", TESTS);
  Build p = build(hello, tag, NULL);
  report(tag, &p);
  CHECK(p.rc == SWIFTHOST_EPOISONED, "%s: rc %d", tag, p.rc);
  SigSnap after;
  snap(&after);
  snapDiff(before, &after, "after the poisoning call");
  checkAltStack("after the poisoning call");
}

/* A stack overflow on the calling thread is recovered (the crash handler runs
   on the library's alternate signal stack). 250 nested functions (the parser
   allows 256 levels of nesting) overflow a 512 KiB stack while parsing. */
static void *overflowWorker(void *unused) {
  (void)unused;
  SigSnap before;
  snap(&before);
  installAltStack();
  char *deep = fmt("%s/deep.swift", WORK);
  FILE *f = fopen(deep, "w");
  for (int i = 0; i < 250; ++i)
    fprintf(f, "func f%d() -> Int {\n", i);
  fputs("return 1\n", f);
  for (int i = 0; i < 250; ++i)
    fputs("}\n", f);
  fputs("print(f0())\n", f);
  fclose(f);
  Build o = build(deep, "overflow", NULL);
  report("overflow", &o);
  CHECK(o.rc == 128 + SIGSEGV || o.rc == 128 + SIGBUS, "overflow: rc %d (expected %d or %d)",
        o.rc, 128 + SIGSEGV, 128 + SIGBUS);
  checkAltStack("after the overflow");
  afterPoison("after-overflow", &before);
  return NULL;
}

/* A crash in a clang module build, which clang runs on a helper thread under
   its own crash recovery and reports as an ordinary error. */
static void *helperCrashWorker(void *unused) {
  (void)unused;
  SigSnap before;
  snap(&before);
  installAltStack();
  char *dir = fmt("%s/crash-module", WORK);
  mkdir(dir, 0755);
  writeFile(fmt("%s/module.modulemap", dir), "module CrashOnBuild {\n  header \"crash.h\"\n}\n");
  writeFile(fmt("%s/crash.h", dir), "#pragma clang __debug crash\n");
  char *src = fmt("%s/import-crash.swift", WORK);
  writeFile(src, "import CrashOnBuild\nprint(1)\n");
  const char *extra[] = {"-I", dir, NULL};
  Build c = build(src, "helper-crash", extra);
  report("helper-crash", &c);
  CHECK(c.rc != 0 && c.stage == 1, "helper-thread crash: rc %d stage %d", c.rc, c.stage);
  CHECK(c.diag && strstr(c.diag, "CrashOnBuild"), "helper-thread crash diagnostics: %s",
        c.diag ? c.diag : "(null)");
  afterPoison("after-helper-crash", &before);
  return NULL;
}

/* -Xllvm -version prints and ends in LLVM's exit path with status 0: the call
   poisons the library, so it must not report success. */
static void *xllvmVersionWorker(void *unused) {
  (void)unused;
  SigSnap before;
  snap(&before);
  installAltStack();
  char *hello = fmt("%s/hello.swift", TESTS);
  const char *extra[] = {"-Xllvm", "-version", NULL};
  Build v = build(hello, "xllvm-version", extra);
  report("xllvm-version", &v);
  CHECK(v.rc == SWIFTHOST_EPOISONED && v.stage == 1, "-Xllvm -version: rc %d stage %d", v.rc,
        v.stage);
  afterPoison("after-xllvm-version", &before);
  return NULL;
}

/* A reply nested deeper than the JSON scanner's recursion can go on the
   calling thread's stack: a stack overflow, recovered, which poisons. */
static void *deepReplyWorker(void *unused) {
  (void)unused;
  SigSnap before;
  snap(&before);
  installAltStack();
  Build d = buildMacro("deep-reply", PLUGIN_SERVER, "deep");
  report("deep-reply", &d);
  CHECK(d.rc == 128 + SIGSEGV || d.rc == 128 + SIGBUS, "deep reply: rc %d (expected %d or %d)",
        d.rc, 128 + SIGSEGV, 128 + SIGBUS);
  checkAltStack("after the deep reply");
  afterPoison("after-deep-reply", &before);
  return NULL;
}

static int unexpectedHandlerCalls;
static bool unexpectedHandler(const char *message, size_t length, char **response,
                              size_t *responseLength) {
  (void)message;
  (void)length;
  ++unexpectedHandlerCalls;
  *response = strdup("this handler must never be used");
  *responseLength = strlen(*response);
  return true;
}

/* The app's own message handler: the server path does not exist and is never
   opened. It is set once; later attempts change nothing. */
static void *pluginHandlerWorker(void *unused) {
  (void)unused;
  SigSnap before, after;
  snap(&before);
  installAltStack();
  swifthost_set_plugin_message_handler(swift_inproc_plugins_handle_message);
  swifthost_set_plugin_message_handler(unexpectedHandler);
  char *noServer = fmt("%s/no-such-dir/no-such-plugin-server.dylib", WORK);
  Build b = buildMacro("macro-handler", noServer, NULL);
  checkMacroBuild("macro-handler", &b, swifthost_test_plugin_count);
  CHECK(strstr(swifthost_test_plugin_last_library(), "/M.wasm") != NULL,
        "loadPluginLibrary path: %s", swifthost_test_plugin_last_library());
  swifthost_set_plugin_message_handler(unexpectedHandler);
  Build again = buildMacro("macro-handler-again", noServer, "wrong-kind");
  report("macro-handler-again", &again);
  CHECK(again.rc == 1 && again.diag && strstr(again.diag, "plugin returned invalid result"),
        "macro-handler-again: rc %d: %s", again.rc, again.diag ? again.diag : "(null)");
  Build last = buildMacro("macro-handler-last", noServer, NULL);
  checkMacroBuild("macro-handler-last", &last, swifthost_test_plugin_count);
  CHECK(unexpectedHandlerCalls == 0, "a second handler was used %d time(s)",
        unexpectedHandlerCalls);
  snap(&after);
  snapDiff(&before, &after, "after the macro builds");
  checkAltStack("after the macro builds");
  return NULL;
}

static void *execPluginWorker(void *unused) {
  (void)unused;
  installAltStack();
  checkExecutablePlugins();
  /* The library is still healthy. */
  Build h = build(fmt("%s/hello.swift", TESTS), "hello-after-exec-plugins", NULL);
  report("hello-after-exec-plugins", &h);
  CHECK(h.rc == 0, "build after the executable plugin attempts: rc %d", h.rc);
  return NULL;
}

static int runMode(const char *mode) {
  void *(*fn)(void *) = NULL;
  size_t stackSize = SWIFTHOST_MIN_STACK_SIZE;
  if (!strcmp(mode, "overflow")) {
    fn = overflowWorker;
    stackSize = 512 * 1024;
  } else if (!strcmp(mode, "helper-crash")) {
    fn = helperCrashWorker;
  } else if (!strcmp(mode, "xllvm-version")) {
    fn = xllvmVersionWorker;
  } else if (!strcmp(mode, "deep-reply")) {
    fn = deepReplyWorker;
  } else if (!strcmp(mode, "plugin-handler")) {
    fn = pluginHandlerWorker;
  } else if (!strcmp(mode, "exec-plugin")) {
    fn = execPluginWorker;
  } else {
    fprintf(stderr, "unknown mode %s\n", mode);
    return 2;
  }
  /* pthread allocates the stack, with a guard page below it. */
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, stackSize);
  pthread_t th;
  pthread_create(&th, &attr, fn, NULL);
  pthread_join(th, NULL);
  printf("%s %s: %d failure(s)\n", failures ? "FAILED" : "PASSED", mode, failures);
  return failures ? 1 : 0;
}

int main(int argc, char **argv) {
  if (argc != 5 && argc != 6) {
    fprintf(stderr, "usage: %s <sdk-target-dir> <work-dir> <iwasm> <tests-dir> [mode]\n",
            argv[0]);
    return 2;
  }
  SDK = argv[1];
  WORK = argv[2];
  IWASM = argv[3];
  TESTS = argv[4];
  snprintf(SW, sizeof SW, "%s/swift.xctoolchain/usr/lib/swift_static", SDK);
  snprintf(CACHE, sizeof CACHE, "%s/module-cache", WORK);
  char self[PATH_MAX];
  if (!realpath(argv[0], self)) {
    perror(argv[0]);
    return 2;
  }
  *strrchr(self, '/') = 0;
  snprintf(PLUGIN_SERVER, sizeof PLUGIN_SERVER, "%s/libswifthost-test-plugin-server.dylib", self);
  mkdir(WORK, 0755);
  setvbuf(stdout, NULL, _IOLBF, 0);
  if (argc == 6)
    return runMode(argv[5]);

  /* The worker's stack is our own mapping (with a guard page below it),
     filled with a pattern, so that its high-water mark can be measured. */
  size_t page = (size_t)getpagesize(), size = SWIFTHOST_MIN_STACK_SIZE;
  unsigned char *map = mmap(NULL, size + page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (map == MAP_FAILED || mprotect(map, page, PROT_NONE) != 0) {
    perror("stack");
    return 2;
  }
  unsigned char *stack = map + page;
  memset(stack, 0xA5, size);
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstack(&attr, stack, size);
  pthread_t th;
  pthread_create(&th, &attr, worker, NULL);
  pthread_join(th, NULL);
  size_t untouched = 0;
  while (untouched < size && stack[untouched] == 0xA5)
    ++untouched;
  printf("worker stack high-water mark: %zu KiB of %zu KiB\n", (size - untouched) / 1024, size / 1024);
  printf("peak rss: %.0f MiB (includes the %zu MiB test stack, filled to measure stack use)\n",
         peakRssMiB(), size >> 20);
  printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
  return failures ? 1 : 0;
}
