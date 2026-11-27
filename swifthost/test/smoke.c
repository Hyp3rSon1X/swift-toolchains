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

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <mach/mach.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdatomic.h>
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

/* 1 when both files exist and have the same bytes. */
static int sameBytes(const char *a, const char *b) {
  FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
  int same = fa && fb;
  while (same) {
    int ca = fgetc(fa), cb = fgetc(fb);
    if (ca != cb)
      same = 0;
    else if (ca == EOF)
      break;
  }
  if (fa)
    fclose(fa);
  if (fb)
    fclose(fb);
  return same;
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

/* Whether diag holds exactly one diagnostic, "error: compilation cancelled", as a cancelled call must (see the
   header). A diagnostic with a location and source excerpt repeats its kind in the excerpt, so it counts twice. */
static int onlyCancellation(const char *diag) {
  static const char *const kinds[] = {"error: ", "warning: ", "note: ", "remark: "};
  int n = 0;
  for (size_t i = 0; diag && i < sizeof kinds / sizeof kinds[0]; ++i)
    for (const char *e = diag; (e = strstr(e, kinds[i])); e += strlen(kinds[i]))
      ++n;
  return n == 1 && strstr(diag, "error: compilation cancelled");
}

typedef struct {
  int rc;
  int stage; /* 1 frontend, 2 autolink, 3 wasm-ld */
  double fe, al, ld;
  char *diag;
} Build;

/* Full build of src with the job recipe. extra = frontend extras, objs =
   further objects for the link (both NULL-terminated or NULL). */
static Build buildObjs(const char *src, const char *tag, const char **extra, const char **objs) {
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
  for (; objs && *objs; ++objs)
    add(&ld, *objs);
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

static Build build(const char *src, const char *tag, const char **extra) {
  return buildObjs(src, tag, extra, NULL);
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

static double threadCpuMs(mach_port_t thread) {
  thread_basic_info_data_t bi;
  mach_msg_type_number_t n = THREAD_BASIC_INFO_COUNT;
  if (thread_info(thread, THREAD_BASIC_INFO, (thread_info_t)&bi, &n) != KERN_SUCCESS)
    return -1;
  return bi.user_time.seconds * 1e3 + bi.user_time.microseconds / 1e3 +
         bi.system_time.seconds * 1e3 + bi.system_time.microseconds / 1e3;
}

/* Cancels the frontend call that is running once the calling thread has used
   cpuMs of CPU time since the canceller started. On a loaded machine a wall
   clock delay lands earlier in the compilation (in parsing, which does not
   stop), so the CPU clock keeps the cancel where it lands on an idle device.
   Gives up when done is set (the call finished first). Read requestedAt and
   cpuAtRequest only after joining the canceller; requestedAt stays 0 when it
   gave up. */
typedef struct {
  int cpuMs;
  mach_port_t thread;
  atomic_int done;
  double requestedAt, cpuAtRequest;
} CpuCancelCtx;
static void *cpuCanceller(void *p) {
  CpuCancelCtx *c = p;
  double start = threadCpuMs(c->thread);
  while (!atomic_load(&c->done) && threadCpuMs(c->thread) - start < c->cpuMs)
    usleep(1000);
  if (atomic_load(&c->done))
    return NULL;
  c->requestedAt = nowMs();
  c->cpuAtRequest = threadCpuMs(c->thread);
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
   serverPath (NULL: no -in-process-plugin-server-path); reply selects the
   server's reply to the expansion request. */
static Build buildMacro(const char *tag, const char *serverPath, const char *reply) {
  char *src = macroSource();
  if (reply)
    setenv("SWIFTHOST_TEST_PLUGIN_REPLY", reply, 1);
  else
    unsetenv("SWIFTHOST_TEST_PLUGIN_REPLY");
  setenv("SWIFTHOST_TEST_PLUGIN_FILE", src, 1);
  const char *extra[] = {"-load-resolved-plugin", fmt("%s/M.wasm##M", WORK),
                         "-in-process-plugin-server-path", serverPath, NULL};
  if (!serverPath)
    extra[2] = NULL;
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
      /* The compiler drops the handler's error message. */
      {"error", "failed to send request to plugin"},
      /* Rejected by the library before the compiler parses it. */
      {"deep", "failed to send request to plugin"},
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

/* One C or C++ file through swifthost_clang (the C and C++ targets of a
   package): argv[0] "clang" or "clang++", then extra (NULL-terminated, may be
   NULL) before the common arguments. */
static int clangCompileAs(const char *argv0, const char **extra, const char *src, const char *obj,
                          const char *inc, const char *opt, const char *tag, char **diag) {
  Args a = {0};
  add(&a, argv0);
  for (; extra && *extra; ++extra)
    add(&a, *extra);
  add(&a, "-target"); add(&a, "wasm32-unknown-wasip1");
  add(&a, "--sysroot"); add(&a, fmt("%s/WASI.sdk", SDK));
  add(&a, "-resource-dir"); add(&a, fmt("%s/clang", SW));
  add(&a, opt);
  add(&a, "-I"); add(&a, inc);
  add(&a, "-MD"); add(&a, "-MF"); add(&a, fmt("%s.d", obj));
  add(&a, "-c"); add(&a, src);
  add(&a, "-o"); add(&a, obj);
  return call(swifthost_clang, &a, tag, diag);
}

static int clangCompile(const char *src, const char *obj, const char *inc, const char *opt,
                        const char *tag, char **diag) {
  return clangCompileAs("clang", NULL, src, obj, inc, opt, tag, diag);
}

/* C++ as SwiftPM compiles a package's C++ target for this SDK, with
   -fno-exceptions: the SDK's libc++abi has no exception runtime. */
static int clangCxxCompile(const char *src, const char *obj, const char *inc, const char *opt,
                           const char *tag, char **diag) {
  const char *extra[] = {"-std=c++17", "-fno-exceptions", NULL};
  return clangCompileAs("clang++", extra, src, obj, inc, opt, tag, diag);
}

static const char *CLANG_EXPECT = "c says 42\n";
static const char *CXX_EXPECT = "c++ says 42\n";

/* swifthost_clang: a C module (header, module map, source) compiled
   in-process, imported by a Swift main and linked with it; then a clang call
   cancelled after 100 ms of its CPU time (20,000 functions at -O0 take a few
   hundred ms to parse), which must report only "compilation cancelled" and
   leave the older object alone, and a clang call after it. */
static void checkClang(void) {
  char *dir = fmt("%s/cmod", WORK), *inc = fmt("%s/cmod/include", WORK);
  mkdir(dir, 0755);
  mkdir(inc, 0755);
  writeFile(fmt("%s/cadd.h", inc), "int cadd(int a, int b);\n");
  writeFile(fmt("%s/module.modulemap", inc), "module CAdd {\n  header \"cadd.h\"\n  export *\n}\n");
  char *src = fmt("%s/cadd.c", dir), *obj = fmt("%s/cadd.o", dir);
  writeFile(src, "#include \"cadd.h\"\nint cadd(int a, int b) { return a * 10 + b; }\n");
  char *diag = NULL;
  double t0 = nowMs();
  int rc = clangCompile(src, obj, inc, "-O2", "clang-cadd", &diag);
  printf("%-24s rc %3d  clang %6.0f ms\n", "clang-cadd", rc, nowMs() - t0);
  CHECK(rc == 0 && diag && !*diag, "clang: rc %d, diagnostics: %s", rc, diag ? diag : "(null)");

  char *mainSrc = fmt("%s/cmain.swift", dir);
  writeFile(mainSrc, "import CAdd\nprint(\"c says\", cadd(4, 2))\n");
  const char *incArgs[] = {"-I", inc, NULL};
  const char *objs[] = {obj, NULL};
  Build b = buildObjs(mainSrc, "clang-swift-main", incArgs, objs);
  report("clang-swift-main", &b);
  CHECK(b.rc == 0, "Swift main with the C module failed");
  if (!b.rc) {
    char *out = runWasm("clang-swift-main");
    CHECK(!out || !strcmp(out, CLANG_EXPECT), "clang-swift-main output %s", out);
  }

  /* A target that depends on the C module, as SwiftPM compiles it: -I and
     -fmodule-map-file= for the dependency. With modules off the map is only
     parsed, so C and C++17 objects equal the ones built without it. In C++17
     the header is still included as text, but clang, the stand-alone one too,
     warns that the #include of a header from a module sits in extern "C"
     (-Wmodule-import-in-extern-c). In C++20 the map turns the #include into a
     module import, which fails (as in the stand-alone clang, which has no
     module file to load). */
  char *use = fmt("%s/cuse.c", dir), *useCxx = fmt("%s/cuse.cpp", dir);
  writeFile(use, "#include \"cadd.h\"\nint cuse(int x) { return cadd(x, 1); }\n");
  writeFile(useCxx, "extern \"C\" {\n#include \"cadd.h\"\n}\nint cuse(int x) { return cadd(x, 1); }\n");
  char *mapArg = fmt("-fmodule-map-file=%s/module.modulemap", inc);
  const struct {
    const char *tag, *argv0, *src, *std;
    int map, expectRc;
  } dep[] = {
      {"clang-dep", "clang", NULL, NULL, 0, 0},
      {"clang-dep-modulemap", "clang", NULL, NULL, 1, 0},
      {"clang-dep-cxx17", "clang++", "cxx", "-std=c++17", 0, 0},
      {"clang-dep-cxx17-modulemap", "clang++", "cxx", "-std=c++17", 1, 0},
      {"clang-dep-cxx20-modulemap", "clang++", "cxx", "-std=c++20", 1, 1},
  };
  for (size_t i = 0; i < sizeof dep / sizeof dep[0]; ++i) {
    const char *extra[4] = {NULL};
    int n = 0;
    if (dep[i].std) {
      extra[n++] = dep[i].std;
      extra[n++] = "-fno-exceptions";
    }
    if (dep[i].map)
      extra[n++] = mapArg;
    char *depObj = fmt("%s/%s.o", dir, dep[i].tag);
    rc = clangCompileAs(dep[i].argv0, extra, dep[i].src ? useCxx : use, depObj, inc, "-O2", dep[i].tag, &diag);
    printf("%-24s rc %3d\n", dep[i].tag, rc);
    if (dep[i].expectRc == 0) {
      const char *externC = "warning: import of C++ module 'CAdd' appears within extern \"C\" language linkage "
                            "specification [-Wmodule-import-in-extern-c]";
      int warns = dep[i].src && dep[i].map;
      CHECK(rc == 0 && diag && (warns ? strstr(diag, externC) && !strstr(diag, "error:") : !*diag),
            "%s: rc %d, diagnostics: %s", dep[i].tag, rc, diag ? diag : "(null)");
      if (dep[i].map)
        CHECK(sameBytes(depObj, fmt("%s/%s.o", dir, dep[i - 1].tag)),
              "%s: the object differs from the one built without -fmodule-map-file=", dep[i].tag);
    } else {
      int lines = 0;
      for (const char *c = diag; c && *c; ++c)
        lines += *c == '\n';
      CHECK(rc == 1 && lines == 1 && strstr(diag, "error: swifthost_clang does not support module imports"),
            "%s: rc %d (expected 1, a refused module import), diagnostics: %s", dep[i].tag, rc,
            diag ? diag : "(null)");
      CHECK(access(depObj, F_OK) != 0, "%s: the failed call wrote %s", dep[i].tag, depObj);
    }
  }

  char *many = fmt("%s/many.c", dir), *manyObj = fmt("%s/many.o", dir);
  FILE *f = fopen(many, "w");
  for (int i = 0; i < 20000; ++i)
    fprintf(f, "unsigned f%d(unsigned x) { unsigned y = x * %du; for (unsigned k = 0; k < (x & 7u); ++k) y ^= y << 3; return y; }\n",
            i, 2 * i + 1);
  fclose(f);
  writeFile(manyObj, "OLD OBJECT\n");
  CpuCancelCtx cc = {.cpuMs = 100, .thread = pthread_mach_thread_np(pthread_self())};
  atomic_init(&cc.done, 0);
  pthread_t ct;
  pthread_create(&ct, NULL, cpuCanceller, &cc);
  t0 = nowMs();
  rc = clangCompile(many, manyObj, inc, "-O0", "clang-cancelled", &diag);
  double returnedAt = nowMs(), cpuAtReturn = threadCpuMs(cc.thread);
  atomic_store(&cc.done, 1);
  pthread_join(ct, NULL);
  int fired = cc.requestedAt > 0 && cc.requestedAt <= returnedAt;
  printf("%-24s rc %3d  clang %6.0f ms\n", "clang-cancelled", rc, returnedAt - t0);
  if (fired)
    printf("  cancel latency %.0f ms, %.0f ms of CPU\n", returnedAt - cc.requestedAt, cpuAtReturn - cc.cpuAtRequest);
  CHECK(fired, "cancelled clang call: the call returned before the cancel was requested");
  CHECK(rc == SWIFTHOST_ECANCELED, "cancelled clang call: rc %d (expected %d)", rc, SWIFTHOST_ECANCELED);
  CHECK(rc != SWIFTHOST_ECANCELED || onlyCancellation(diag),
        "cancelled clang call: diagnostics other than \"compilation cancelled\":\n%s", diag);
  char *old = readFile(manyObj);
  CHECK(old && !strcmp(old, "OLD OBJECT\n"), "cancelled clang call changed the object at its -o path");
  CHECK(!fired || returnedAt - cc.requestedAt < 500, "clang cancel took %.0f ms (bound 500 ms)",
        returnedAt - cc.requestedAt);

  rc = clangCompile(src, obj, inc, "-O2", "clang-after-cancel", &diag);
  printf("%-24s rc %3d\n", "clang-after-cancel", rc);
  CHECK(rc == 0, "clang call after a cancelled one: rc %d", rc);
}

/* swifthost_clang with C++ (argv[0] "clang++"): a C++ source behind a C
   header (libc++ containers, a template, a virtual class with dynamic_cast, a
   global constructor), imported by a Swift main and linked with it and the
   SDK's libc++/libc++abi; then a C++ call cancelled after 400 ms of its CPU
   time, while it instantiates templates at the end of the file (parsing
   <map> takes about 200 ms, the 150 std::map specializations several
   seconds), which must return within the same bound as a C call, and a C++
   call after it. */
static void checkClangCxx(void) {
  char *dir = fmt("%s/cxxmod", WORK), *inc = fmt("%s/cxxmod/include", WORK);
  mkdir(dir, 0755);
  mkdir(inc, 0755);
  writeFile(fmt("%s/cxxops.h", inc),
            "#ifdef __cplusplus\nextern \"C\" {\n#endif\nint cxx_answer(void);\n"
            "#ifdef __cplusplus\n}\n#endif\n");
  writeFile(fmt("%s/module.modulemap", inc), "module CxxOps {\n  header \"cxxops.h\"\n  export *\n}\n");
  char *src = fmt("%s/cxxops.cpp", dir), *obj = fmt("%s/cxxops.o", dir);
  writeFile(src,
            "#include \"cxxops.h\"\n#include <map>\n#include <memory>\n#include <numeric>\n"
            "#include <string>\n#include <vector>\n"
            "namespace {\n"
            "struct Base { virtual ~Base() = default; virtual int value() const = 0; };\n"
            "struct Six : Base { int value() const override { return 6; } };\n"
            "struct Counter { int n; Counter() : n(7) {} };\n"
            "Counter counter;\n"
            "template <typename T> T sum(const std::vector<T> &v) { return std::accumulate(v.begin(), v.end(), T()); }\n"
            "}\n"
            "extern \"C\" int cxx_answer(void) {\n"
            "  std::map<std::string, int> m{{\"a\", 1}, {\"b\", 2}};\n"
            "  std::unique_ptr<Base> b(new Six);\n"
            "  int six = dynamic_cast<Six *>(b.get()) ? b->value() : 0;\n"
            "  return sum(std::vector<int>{m[\"a\"], m[\"b\"], six}) + counter.n * 4 + 5;\n"
            "}\n");
  char *diag = NULL;
  double t0 = nowMs();
  int rc = clangCxxCompile(src, obj, inc, "-O2", "cxx-ops", &diag);
  printf("%-24s rc %3d  clang++ %6.0f ms\n", "cxx-ops", rc, nowMs() - t0);
  CHECK(rc == 0 && diag && !*diag, "clang++: rc %d, diagnostics: %s", rc, diag ? diag : "(null)");

  char *mainSrc = fmt("%s/cxxmain.swift", dir);
  writeFile(mainSrc, "import CxxOps\nprint(\"c++ says\", cxx_answer())\n");
  const char *incArgs[] = {"-I", inc, NULL};
  const char *objs[] = {obj, NULL};
  Build b = buildObjs(mainSrc, "cxx-swift-main", incArgs, objs);
  report("cxx-swift-main", &b);
  CHECK(b.rc == 0, "Swift main with the C++ module failed");
  if (!b.rc) {
    char *out = runWasm("cxx-swift-main");
    CHECK(!out || !strcmp(out, CXX_EXPECT), "cxx-swift-main output %s", out);
  }

  char *tmpl = fmt("%s/tmpl.cpp", dir), *tmplObj = fmt("%s/tmpl.o", dir);
  FILE *f = fopen(tmpl, "w");
  fprintf(f, "#include <map>\n"
             "template <int N> struct Key { unsigned v; bool operator<(const Key &o) const { return v < o.v; } };\n"
             "template <int N> unsigned useMap(unsigned x) {\n"
             "  std::map<Key<N>, unsigned> m;\n"
             "  for (unsigned i = 0; i < (x & 15u) + 4u; ++i) m[Key<N>{i * N + x}] = i;\n"
             "  unsigned s = 0;\n  for (const auto &kv : m) s += kv.first.v ^ kv.second;\n"
             "  return s + useMap<N - 1>(x);\n}\n"
             "template <> unsigned useMap<0>(unsigned x) { return x; }\n"
             "unsigned entry(unsigned x) { return useMap<150>(x); }\n");
  fclose(f);
  writeFile(tmplObj, "OLD OBJECT\n");
  CpuCancelCtx cc = {.cpuMs = 400, .thread = pthread_mach_thread_np(pthread_self())};
  atomic_init(&cc.done, 0);
  pthread_t ct;
  pthread_create(&ct, NULL, cpuCanceller, &cc);
  t0 = nowMs();
  rc = clangCxxCompile(tmpl, tmplObj, inc, "-O0", "cxx-cancelled", &diag);
  double returnedAt = nowMs(), cpuAtReturn = threadCpuMs(cc.thread);
  atomic_store(&cc.done, 1);
  pthread_join(ct, NULL);
  int fired = cc.requestedAt > 0 && cc.requestedAt <= returnedAt;
  printf("%-24s rc %3d  clang++ %6.0f ms\n", "cxx-cancelled", rc, returnedAt - t0);
  if (fired)
    printf("  cancel latency %.0f ms, %.0f ms of CPU\n", returnedAt - cc.requestedAt, cpuAtReturn - cc.cpuAtRequest);
  CHECK(fired, "cancelled C++ call: the call returned before the cancel was requested");
  CHECK(rc == SWIFTHOST_ECANCELED, "cancelled C++ call: rc %d (expected %d)", rc, SWIFTHOST_ECANCELED);
  CHECK(rc != SWIFTHOST_ECANCELED || onlyCancellation(diag),
        "cancelled C++ call: diagnostics other than \"compilation cancelled\":\n%s", diag);
  char *old = readFile(tmplObj);
  CHECK(old && !strcmp(old, "OLD OBJECT\n"), "cancelled C++ call changed the object at its -o path");
  CHECK(!fired || returnedAt - cc.requestedAt < 500, "C++ cancel took %.0f ms (bound 500 ms)",
        returnedAt - cc.requestedAt);

  rc = clangCxxCompile(src, obj, inc, "-O2", "cxx-after-cancel", &diag);
  printf("%-24s rc %3d\n", "cxx-after-cancel", rc);
  CHECK(rc == 0, "C++ call after a cancelled one: rc %d", rc);
}

/* The directory LLVM creates temporary files in (llvm::sys::path::
   system_temp_directory): $TMPDIR, $TMP, $TEMP, $TEMPDIR, else the user's
   Darwin temporary directory. */
static char *tempDir(void) {
  const char *names[] = {"TMPDIR", "TMP", "TEMP", "TEMPDIR"};
  for (int i = 0; i < 4; ++i)
    if (getenv(names[i]) && *getenv(names[i]))
      return fmt("%s", getenv(names[i]));
  char buf[PATH_MAX];
  size_t n = confstr(_CS_DARWIN_USER_TEMP_DIR, buf, sizeof buf);
  return fmt("%s", n > 0 && n <= sizeof buf ? buf : "/tmp");
}

/* The number of entries of dir whose name starts with prefix. */
static int countEntries(const char *dir, const char *prefix) {
  DIR *d = opendir(dir);
  if (!d)
    return -1;
  int n = 0;
  for (struct dirent *e; (e = readdir(d));)
    n += !strncmp(e->d_name, prefix, strlen(prefix));
  closedir(d);
  return n;
}

/* "#pragma clang module build" in a C, C++17 and C++20 source: the stand-alone
   clang compiles a nested module from it into a .pcm file in $TMPDIR (and,
   in C++20, accepts the source); swifthost_clang must fail the call with one
   line, create no file there, and compile normally afterwards. */
static void checkClangModuleBuild(void) {
  char *dir = fmt("%s/cxxmod", WORK), *inc = fmt("%s/cxxmod/include", WORK);
  char *tmp = tempDir();
  const char *module = "SwiftHostSmokePragmaModule";
  const char *body = "#pragma clang module build SwiftHostSmokePragmaModule\n"
                     "module SwiftHostSmokePragmaModule {}\n#pragma clang module contents\n"
                     "#pragma clang module begin SwiftHostSmokePragmaModule\n"
                     "int pragma_module_answer(void) { return 1; }\n"
                     "#pragma clang module end\n#pragma clang module endbuild\n"
                     "int pragma_module_main(void) { return 0; }\n";
  const char *c17[] = {"-std=c++17", "-fno-exceptions", NULL};
  const char *c20[] = {"-std=c++20", "-fno-exceptions", NULL};
  struct { const char *tag, *argv0, **extra, *ext; } cases[] = {
      {"pragma-module-build-c", "clang", NULL, "c"},
      {"pragma-module-build-cxx17", "clang++", c17, "cpp"},
      {"pragma-module-build-cxx20", "clang++", c20, "cpp"},
  };
  for (int i = 0; i < 3; ++i) {
    char *src = fmt("%s/%s.%s", dir, cases[i].tag, cases[i].ext), *obj = fmt("%s/%s.o", dir, cases[i].tag);
    writeFile(src, body);
    int before = countEntries(tmp, module);
    char *diag = NULL;
    int rc = clangCompileAs(cases[i].argv0, cases[i].extra, src, obj, inc, "-O2", cases[i].tag, &diag);
    int after = countEntries(tmp, module);
    int lines = 0;
    for (const char *c = diag; c && *c; ++c)
      lines += *c == '\n';
    printf("%-24s rc %3d  lines %d  new in TMPDIR %d\n", cases[i].tag, rc, lines, after - before);
    CHECK(rc == 1 && lines == 1 && strstr(diag, "error: swifthost_clang does not support #pragma clang module build"),
          "%s: rc %d, diagnostics:\n%s", cases[i].tag, rc, diag ? diag : "(null)");
    CHECK(before >= 0 && after == before, "%s: %d new %s* files in %s", cases[i].tag, after - before, module, tmp);
    CHECK(access(obj, F_OK) != 0, "%s: the failed call wrote %s", cases[i].tag, obj);
  }
  char *src = fmt("%s/cxxops.cpp", dir), *obj = fmt("%s/cxxops.o", dir), *diag = NULL;
  int rc = clangCxxCompile(src, obj, inc, "-O2", "cxx-after-pragma", &diag);
  printf("%-24s rc %3d\n", "cxx-after-pragma", rc);
  CHECK(rc == 0 && diag && !*diag, "C++ call after the module pragmas: rc %d, diagnostics: %s", rc,
        diag ? diag : "(null)");
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
  Build mb = buildMacro("macro-reply-brackets", PLUGIN_SERVER, "brackets");
  checkMacroBuild("macro-reply-brackets", &mb, serverCounter());
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
  CHECK(k.rc != SWIFTHOST_ECANCELED || onlyCancellation(k.diag),
        "cancelled build: diagnostics other than \"compilation cancelled\":\n%s", k.diag);

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
  CHECK(kf.rc != SWIFTHOST_ECANCELED || onlyCancellation(kf.diag),
        "cancelled function bodies: diagnostics other than \"compilation cancelled\":\n%s", kf.diag);

  /* One function body of 4,000 statements with three closures each, as an
     app's generator writes it, cancelled after 300 ms of the call's CPU time
     (type checking the body takes seconds): the type checker stops at the
     next statement, and the only diagnostic is "compilation cancelled" (with
     swifthost-6.4.2-4, one error per remaining statement). The bound leaves
     room for a loaded machine; an idle Mac takes a few tens of
     milliseconds. */
  char *body = fmt("%s/body.swift", WORK);
  FILE *wf = fopen(body, "w");
  fputs("func work() -> Int {\n    var total = 0\n", wf);
  for (int i = 0; i < 4000; ++i)
    fprintf(wf, "    total &+= [%d, %d, %d].map { $0 &* 3 }.filter { $0 > 1 }.reduce(0, &+)\n",
            i, i + 1, i + 2);
  fputs("    return total\n}\nprint(work())\n", wf);
  fclose(wf);
  const char *swift6[] = {"-swift-version", "6", NULL};
  CpuCancelCtx wc = {.cpuMs = 300, .thread = pthread_mach_thread_np(pthread_self())};
  atomic_init(&wc.done, 0);
  pthread_create(&ct, NULL, cpuCanceller, &wc);
  Build kw = build(body, "cancelled-one-body", swift6);
  double returnedAt = nowMs(), cpuAtReturn = threadCpuMs(wc.thread);
  atomic_store(&wc.done, 1);
  pthread_join(ct, NULL);
  /* The canceller may also have fired after the call had returned. */
  int fired = wc.requestedAt > 0 && wc.requestedAt <= returnedAt;
  latency = fired ? returnedAt - wc.requestedAt : -1;
  double latencyCpu = fired ? cpuAtReturn - wc.cpuAtRequest : -1;
  report("cancelled-one-body", &kw);
  if (fired)
    printf("  cancel latency %.0f ms, %.0f ms of CPU (frontend ran %.0f ms)\n", latency, latencyCpu, kw.fe);
  else
    printf("  the call returned before the cancel was requested (frontend ran %.0f ms)\n", kw.fe);
  CHECK(fired, "cancelled long body: the call returned before the cancel was requested");
  CHECK(kw.rc == SWIFTHOST_ECANCELED && kw.stage == 1,
        "cancelled long body: rc %d stage %d (expected %d in the frontend)", kw.rc, kw.stage,
        SWIFTHOST_ECANCELED);
  CHECK(!fired || latency < 500, "cancel in a long function body took %.0f ms (bound 500 ms)", latency);
  CHECK(kw.rc != SWIFTHOST_ECANCELED || onlyCancellation(kw.diag),
        "cancelled long body: diagnostics other than \"compilation cancelled\":\n%s", kw.diag);

  Build ak = build(hello, "after-cancel", NULL);
  report("after-cancel", &ak);
  CHECK(ak.rc == 0, "build after a cancelled build: rc %d", ak.rc);
  if (!ak.rc) {
    char *out = runWasm("after-cancel");
    CHECK(!out || !strcmp(out, HELLO_EXPECT), "after-cancel output %s", out);
  }

  checkClang();
  checkClangCxx();
  checkClangModuleBuild();

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
  /* -in-process-plugin-server-path is optional with a handler (the compiler
     makes up a default path, which is never opened). */
  Build noFlag = buildMacro("macro-handler-no-flag", NULL, NULL);
  checkMacroBuild("macro-handler-no-flag", &noFlag, swifthost_test_plugin_count);
  Build deep = buildMacro("macro-handler-deep", noServer, "deep");
  report("macro-handler-deep", &deep);
  CHECK(deep.rc == 1 && deep.diag && strstr(deep.diag, "failed to send request to plugin"),
        "macro-handler-deep: rc %d: %s", deep.rc, deep.diag ? deep.diag : "(null)");
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
