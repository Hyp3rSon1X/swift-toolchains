/*
 * Cancellation inside each stage of swifthost_swift_frontend (run-cancel-stages.sh builds and runs it).
 *
 *   cancel-stages <sdk-target-dir> <work-dir> <stages-file>
 *
 * Each case compiles a program on a thread with SWIFTHOST_MIN_STACK_SIZE of stack while the main thread samples
 * that thread's stack every quarter of a millisecond (thread_suspend + frame-pointer walk) and calls
 * swifthost_request_cancel() at the first sample whose stack is inside the case's stage. <stages-file> has
 * "<stage> <lo> <hi>" lines: the unslid address ranges of the functions that mark each stage, from nm of this binary
 * (run-cancel-stages.sh).
 * A case passes when the call returns SWIFTHOST_ECANCELED within LATENCY_CPU_MS of the compiling thread's CPU time
 * after the request and leaves nothing in the case's output directory (no output, no temporary file), or, with a
 * file already at the output path, leaves only that file, unchanged.
 * A cancel that lands in a stage only stops it if the stage checks; a call that finishes first fails the case.
 * Then two sweeps: the constructs program (see constructsFile) is compiled uncancelled and then cancelled at points
 * of CPU time spread over the whole compile and over its SIL generation, the defaults program (see defaultsProgram)
 * at points over its type checking and SIL generation; every call must return SWIFTHOST_ECANCELED without leaving
 * output, or succeed (without diagnostics) if it finished first. Every cancelled call must report
 * exactly one diagnostic, "error: compilation cancelled". At the end an uncancelled compile in the same process
 * must succeed (a crash would have left the library returning SWIFTHOST_EPOISONED).
 *
 * Programs: interp.swift, many functions whose statements are string interpolations (SIL generation emits each
 * interpolation's body as a brace statement inside an expression); lib.swift, 30 functions of 10 statements
 * with closures, at -O (about a second of SIL optimization and LLVM optimization); constructs/, six files of types
 * with derived conformances, caller-side default arguments and the like, whose synthesized code and defaults SIL
 * generation type-checks on demand; defaults.swift, many calls with caller-side default arguments.
 */
#include "SwiftHostCompiler.h"

#include <dirent.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

enum {
  LATENCY_CPU_MS = 500,
  MAXDEPTH = 256,
  MAXRANGES = 64,
  SAMPLE_US = 250,
  CONSTRUCT_FILES = 6,
  CONSTRUCT_TYPES = 8, /* per file */
  TRACE_CALLS = 24,     /* per traces function, in one statement */
  SWEEP_WHOLE = 16,     /* cancel points spread over the whole compile */
  SWEEP_SILGEN = 16,    /* and over its SIL generation */
  DEFAULT_FUNCS = 24,   /* defaults.swift: functions with caller-side defaults */
  DEFAULT_CALLS = 48,   /* calls per function, in one statement */
  SWEEP_CHECK = 24,     /* its cancel points, over type checking and SIL generation */
};

static const char *SDK, *WORK;
static int failures;

#define CHECK(cond, ...)                                                                                     \
  do {                                                                                                       \
    if (!(cond)) {                                                                                           \
      printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                            \
      printf(__VA_ARGS__);                                                                                   \
      printf("\n");                                                                                          \
      ++failures;                                                                                            \
    }                                                                                                        \
  } while (0)

static char *fmt(const char *f, ...) {
  va_list ap;
  va_start(ap, f);
  char *s = NULL;
  if (vasprintf(&s, f, ap) < 0)
    abort();
  va_end(ap);
  return s;
}

static double nowMs(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static double threadCpuMs(mach_port_t thread) {
  thread_basic_info_data_t bi;
  mach_msg_type_number_t n = THREAD_BASIC_INFO_COUNT;
  if (thread_info(thread, THREAD_BASIC_INFO, (thread_info_t)&bi, &n) != KERN_SUCCESS)
    return -1;
  return bi.user_time.seconds * 1e3 + bi.user_time.microseconds / 1e3 + bi.system_time.seconds * 1e3 +
         bi.system_time.microseconds / 1e3;
}

/* ------------------------------------------------------------------ stage ranges and sampling */

typedef struct {
  char stage[32];
  uint64_t lo, hi;
} Range;
static Range ranges[MAXRANGES];
static int nranges;
static uintptr_t slide;

static void loadRanges(const char *path) {
  FILE *f = fopen(path, "r");
  if (!f) {
    perror(path);
    exit(2);
  }
  unsigned long long lo, hi;
  char stage[32];
  while (nranges < MAXRANGES && fscanf(f, "%31s %llx %llx", stage, &lo, &hi) == 3) {
    snprintf(ranges[nranges].stage, sizeof ranges[nranges].stage, "%s", stage);
    ranges[nranges].lo = lo;
    ranges[nranges].hi = hi;
    ++nranges;
  }
  fclose(f);
  extern const struct mach_header_64 _mh_execute_header;
  for (uint32_t i = 0; i < _dyld_image_count(); ++i)
    if (_dyld_get_image_header(i) == (const struct mach_header *)&_mh_execute_header)
      slide = (uintptr_t)_dyld_get_image_vmaddr_slide(i);
}

static uint64_t strip(uint64_t p) { return p & 0x0000007fffffffffULL; }

/* Whether the thread's stack has a frame inside one of the stage's ranges. No allocation while the thread is
   suspended (it may hold the malloc lock). */
static int inStage(thread_act_t thread, uintptr_t stackLo, uintptr_t stackHi, const char *stage) {
  uint64_t pc[MAXDEPTH];
  int depth = 0;
  if (thread_suspend(thread) != KERN_SUCCESS)
    return 0;
  arm_thread_state64_t st;
  mach_msg_type_number_t cnt = ARM_THREAD_STATE64_COUNT;
  if (thread_get_state(thread, ARM_THREAD_STATE64, (thread_state_t)&st, &cnt) == KERN_SUCCESS) {
    pc[depth++] = strip(arm_thread_state64_get_pc(st)) - slide;
    pc[depth++] = strip(arm_thread_state64_get_lr(st)) - slide;
    uintptr_t fp = (uintptr_t)arm_thread_state64_get_fp(st);
    while (depth < MAXDEPTH && fp >= stackLo && fp + 16 <= stackHi && !(fp & 7)) {
      uintptr_t next = ((uintptr_t *)fp)[0];
      uint64_t ret = strip(((uint64_t *)fp)[1]);
      if (!ret)
        break;
      pc[depth++] = ret - slide;
      if (next <= fp)
        break;
      fp = next;
    }
  }
  thread_resume(thread);
  for (int i = 0; i < depth; ++i)
    for (int r = 0; r < nranges; ++r)
      if (!strcmp(ranges[r].stage, stage) && pc[i] >= ranges[r].lo && pc[i] < ranges[r].hi)
        return 1;
  return 0;
}

/* ------------------------------------------------------------------ one frontend call on a thread */

typedef struct {
  int argc;
  const char **argv;
  const char *errPath;
  int rc;
  double returnedAt, cpuAtReturn;
  atomic_int started, done;
  mach_port_t thread;
} Call;

static void *runCall(void *p) {
  Call *c = p;
  c->thread = pthread_mach_thread_np(pthread_self());
  atomic_store(&c->started, 1);
  int errFd = open(c->errPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  c->rc = swifthost_swift_frontend(c->argc, c->argv, 1, errFd);
  c->returnedAt = nowMs();
  c->cpuAtReturn = threadCpuMs(c->thread);
  close(errFd);
  atomic_store(&c->done, 1);
  return NULL;
}

typedef struct {
  const char *stage; /* cancel at the first sample inside this stage */
  double atCpu;      /* or, when >= 0, once the call has used this much CPU time (ms) */
  const char *watch; /* record the CPU times of the first and last samples inside this stage */
} Trigger;

typedef struct {
  int rc, cancelled;
  double latency, latencyCpu, total, cpu, watchFirst, watchLast;
} Outcome;

/* Runs the call on a new thread and cancels it as the trigger says. */
static Outcome run(const char **argv, int argc, const char *tag, Trigger t) {
  Call c = {.argc = argc, .argv = argv, .errPath = fmt("%s/%s.err", WORK, tag)};
  atomic_init(&c.started, 0);
  atomic_init(&c.done, 0);
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, SWIFTHOST_MIN_STACK_SIZE);
  pthread_t th;
  double t0 = nowMs();
  pthread_create(&th, &attr, runCall, &c);
  while (!atomic_load(&c.started))
    usleep(50);
  uintptr_t hi = (uintptr_t)pthread_get_stackaddr_np(th), lo = hi - pthread_get_stacksize_np(th);
  double requestedAt = 0, cpuAtRequest = 0, watchFirst = -1, watchLast = -1;
  while (!atomic_load(&c.done)) {
    double cpu = threadCpuMs(c.thread);
    int fire = !requestedAt && ((t.stage && inStage(c.thread, lo, hi, t.stage)) || (t.atCpu >= 0 && cpu >= t.atCpu));
    if (fire) {
      requestedAt = nowMs();
      cpuAtRequest = threadCpuMs(c.thread);
      swifthost_request_cancel();
    }
    if (t.watch && inStage(c.thread, lo, hi, t.watch)) {
      if (watchFirst < 0)
        watchFirst = cpu;
      watchLast = cpu;
    }
    usleep(SAMPLE_US);
  }
  pthread_join(th, NULL);
  pthread_attr_destroy(&attr);
  Outcome o = {.rc = c.rc, .total = c.returnedAt - t0, .cpu = c.cpuAtReturn, .watchFirst = watchFirst,
               .watchLast = watchLast};
  o.cancelled = requestedAt > 0 && requestedAt <= c.returnedAt;
  if (o.cancelled) {
    o.latency = c.returnedAt - requestedAt;
    o.latencyCpu = c.cpuAtReturn - cpuAtRequest;
  }
  return o;
}

/* ------------------------------------------------------------------ programs and cases */

static void writeFile(const char *path, void (*body)(FILE *)) {
  FILE *f = fopen(path, "w");
  if (!f) {
    perror(path);
    exit(2);
  }
  body(f);
  fclose(f);
}

static void interpProgram(FILE *f) {
  for (int i = 0; i < 150; ++i) {
    fprintf(f, "public func text%d(_ x: Int) -> String {\n  var s = \"\"\n", i);
    for (int j = 0; j < 10; ++j)
      fprintf(f, "  s += \"a\\(x) b\\(x &+ %d) c\\(x &* %d) d\\(x &- %d) e\\(x)\\n\"\n", j, i, j);
    fputs("  return s\n}\n", f);
  }
}

static void libProgram(FILE *f) {
  for (int i = 0; i < 30; ++i) {
    fprintf(f, "public func work%d(_ seed: Int) -> Int {\n  var total = seed\n", i);
    for (int j = 0; j < 10; ++j)
      fprintf(f, "  total &+= [%d, %d, total].map { $0 &* 3 }.filter { $0 > 1 }.reduce(0, &+)\n", i + j,
              i * j);
    fputs("  return total\n}\n", f);
  }
}

static void helloProgram(FILE *f) { fputs("print(\"hi\")\n", f); }

/* The names in dir other than keep, separated by spaces ("" when there are none). */
static char *otherFiles(const char *dir, const char *keep) {
  char *names = calloc(1, 1);
  DIR *d = opendir(dir);
  if (!d)
    return names;
  for (struct dirent *e; (e = readdir(d));) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || (keep && !strcmp(e->d_name, keep)))
      continue;
    char *more = fmt("%s%s%s", names, *names ? " " : "", e->d_name);
    free(names);
    names = more;
  }
  closedir(d);
  return names;
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

static void clearDir(const char *dir) {
  DIR *d = opendir(dir);
  if (!d)
    return;
  for (struct dirent *e; (e = readdir(d));)
    if (strcmp(e->d_name, ".") && strcmp(e->d_name, ".."))
      unlink(fmt("%s/%s", dir, e->d_name));
  closedir(d);
}

static int exists(const char *path) {
  struct stat st;
  return stat(path, &st) == 0;
}

static char *readFile(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  char *buf = calloc(1, 4096);
  fread(buf, 1, 4095, f);
  fclose(f);
  return buf;
}

typedef struct {
  const char *tag, *stage, *source, *mode, *ext;
  int optimize, sentinel;
} Case;

/* The constructs program: CONSTRUCT_FILES files of declarations whose code the compiler synthesizes and type-checks
   on demand, some of it only while generating SIL (derived Equatable, Hashable, Codable and RawRepresentable
   members, CodingKeys, memberwise initializers), next to string literals and interpolations, lazy properties,
   property wrappers, default arguments, generics, protocols and closures; main.swift uses some of it. The trace
   functions have caller-side default arguments (#file, #fileID, #line, #column, #function, nil, [], [:]), which
   SIL generation type-checks at each call: calls in plain statements (many in one statement in the traces
   functions, so that a cancel in the middle of one is followed by more defaults), in closures and local
   functions (whose captures are computed on demand), in a default argument, and in initial values that become
   synthesized code (a lazy property, a property wrapper's backing initializer, a stored property used by a
   memberwise initializer). */
static void constructsFile(FILE *f, int k) {
  if (k == 0)
    fputs("@propertyWrapper\n"
          "public struct Clamped<Value: Comparable> {\n"
          "  var value: Value\n"
          "  let range: ClosedRange<Value>\n"
          "  public init(wrappedValue: Value, _ range: ClosedRange<Value>) {\n"
          "    self.range = range\n"
          "    value = min(max(wrappedValue, range.lowerBound), range.upperBound)\n"
          "  }\n"
          "  public var wrappedValue: Value {\n"
          "    get { value }\n"
          "    set { value = min(max(newValue, range.lowerBound), range.upperBound) }\n"
          "  }\n"
          "}\n\n",
          f);
  fprintf(f, "public protocol Describable%d {\n  func describe(prefix: String) -> String\n}\n\n", k);
  for (int i = 0; i < CONSTRUCT_TYPES; ++i) {
    fprintf(f,
            "public enum Color%d_%d: String, Codable, CaseIterable, Sendable {\n"
            "  case red = \"red-%d\", green = \"green-%d\", blue = \"blue %d\", other = \"other-%d\"\n"
            "}\n\n",
            k, i, i, i, i, i);
    fprintf(f,
            "public struct Item%d_%d: Hashable, Codable, Sendable {\n"
            "  public var id: Int = %d\n"
            "  public var name: String = \"item %d\"\n"
            "  public var tags: [String] = [\"a\", \"b%d\"]\n"
            "  public var color: Color%d_%d = .green\n"
            "  public var score: Double = %d.5\n"
            "  public var note: String? = nil\n"
            "  public var counts: [String: Int] = [\"x\": %d]\n"
            "}\n\n",
            k, i, i, i, i, k, i, i, i);
    fprintf(f,
            "extension Item%d_%d: Describable%d {\n"
            "  public func describe(prefix: String = \"item\") -> String {\n"
            "    \"\\(prefix) \\(name) #\\(id) [\\(tags.joined(separator: \",\"))] \\(color.rawValue) \\(score)\"\n"
            "  }\n"
            "}\n\n",
            k, i, k);
    fprintf(f,
            "public func trace%d_%d(_ message: String, file: String = #file, fileID: String = #fileID,\n"
            "                     line: Int = #line, column: Int = #column, function: String = #function,\n"
            "                     x: Int? = nil, xs: [Int] = [], d: [String: Int] = [:]) -> String {\n"
            "  \"\\(fileID):\\(line):\\(column) \\(function) \\(message) \\(x ?? xs.count + d.count)\"\n"
            "    + \" \\(file.isEmpty)\"\n"
            "}\n\n",
            k, i);
    /* One statement with many calls: a cancel while SIL generation emits it is followed by more defaults. */
    fprintf(f, "public func traces%d_%d(_ n: Int, _ s: String) -> [String] {\n  let all: [String] = [\n", k, i);
    for (int j = 0; j < TRACE_CALLS; ++j) {
      switch (j % 4) {
      case 0: fprintf(f, "    trace%d_%d(\"t%d\"),\n", k, i, j); break;
      case 1: fprintf(f, "    trace%d_%d(\"t%d\", x: n),\n", k, i, j); break;
      case 2: fprintf(f, "    trace%d_%d(s, xs: [n, %d]),\n", k, i, j); break;
      default: fprintf(f, "    trace%d_%d(\"t%d\", d: [s: %d]),\n", k, i, j, j); break;
      }
    }
    fprintf(f, "  ]\n  return all + (0..<n).map { trace%d_%d(\"closure \\($0)\", x: $0) }\n}\n\n", k, i);
    fprintf(f,
            "public struct Mark%d_%d: Equatable {\n"
            "  public var at: String = trace%d_%d(\"memberwise\")\n"
            "  public var count: Int = 0\n"
            "}\n\n",
            k, i, k, i);
    fprintf(f,
            "public final class Box%d_%d {\n"
            "  @Clamped(0...100) public var level: Int = %d\n"
            "  @Clamped(0...100) public var depth: Int = trace%d_%d(\"wrapped\").count\n"
            "  public lazy var label: String = \"box \\(level) of \\(type(of: self))\"\n"
            "  public lazy var origin: String = trace%d_%d(\"lazy\", d: [\"k\": level])\n"
            "  public var made: String = trace%d_%d(\"stored\")\n"
            "  public var items: [Item%d_%d] = [Item%d_%d()]\n"
            "  public init() {}\n"
            "  public func summary(limit: Int = %d, separator: String = \"; \") -> String {\n"
            "    items.prefix(limit).map { $0.describe() }.joined(separator: separator) + label\n"
            "  }\n"
            "  public func log(_ n: Int, note: String = trace%d_%d(\"default argument\")) -> [String] {\n"
            "    var lines = [trace%d_%d(\"plain \\(n)\"), trace%d_%d(\"x\", x: n), origin, made]\n"
            "    lines += (0..<n).map { trace%d_%d(\"closure \\($0)\", xs: [$0, depth]) }\n"
            "    func local() -> String { trace%d_%d(\"local\", xs: [n]) + trace%d_%d(\"local again\") }\n"
            "    lines.append(local())\n"
            "    lines.append(Mark%d_%d(count: n).at)\n"
            "    lines.append(note)\n"
            "    return lines\n"
            "  }\n"
            "}\n\n",
            k, i, i % 100, k, i, k, i, k, i, k, i, k, i, i + 1, k, i, k, i, k, i, k, i, k, i, k, i, k, i);
    fprintf(f,
            "public func tally%d_%d<T: Hashable & Sendable>(_ xs: [T],\n"
            "                       key: (T) -> String = { \"\\($0)\" }) -> [String: Int] {\n"
            "  var result: [String: Int] = [:]\n"
            "  for x in xs where !key(x).isEmpty { result[key(x), default: 0] += 1 }\n"
            "  return result\n"
            "}\n\n",
            k, i);
  }
}

static void constructsMain(FILE *f) {
  for (int k = 0; k < CONSTRUCT_FILES; ++k)
    fprintf(f,
            "print(tally%d_0([Item%d_0(), Item%d_0()]).count, Box%d_0().summary(),\n"
            "      Color%d_0(rawValue: \"red-0\") ?? .other, Box%d_0().log(2).count, trace%d_0(\"main\"))\n",
            k, k, k, k, k, k, k);
}

static void writeConstructs(void) {
  char *dir = fmt("%s/constructs", WORK);
  mkdir(dir, 0755);
  for (int k = 0; k < CONSTRUCT_FILES; ++k) {
    char *path = fmt("%s/c%d.swift", dir, k);
    FILE *f = fopen(path, "w");
    if (!f) {
      perror(path);
      exit(2);
    }
    constructsFile(f, k);
    fclose(f);
  }
  writeFile(fmt("%s/main.swift", dir), constructsMain);
}

/* The defaults program: functions whose bodies call a function with caller-side default arguments many times. The
   type checker checks each call's defaults on demand after the body (actor isolation checking looks at them), and
   SIL generation does at each call: a cancel in the middle is followed by more of them. */
static void defaultsProgram(FILE *f) {
  for (int i = 0; i < DEFAULT_FUNCS; ++i) {
    fprintf(f,
            "public func trace%d(_ message: String, file: String = #file, fileID: String = #fileID,\n"
            "                   line: Int = #line, column: Int = #column, function: String = #function,\n"
            "                   x: Int? = nil, xs: [Int] = [], d: [String: Int] = [:]) -> String {\n"
            "  \"\\(fileID):\\(line):\\(column) \\(function) \\(message) \\(x ?? xs.count + d.count)\"\n"
            "    + \" \\(file.isEmpty)\"\n"
            "}\n\n",
            i);
    fprintf(f, "public func traces%d(_ n: Int, _ s: String) -> [String] {\n  let all: [String] = [\n", i);
    for (int j = 0; j < DEFAULT_CALLS; ++j) {
      switch (j % 4) {
      case 0: fprintf(f, "    trace%d(\"t%d\"),\n", i, j); break;
      case 1: fprintf(f, "    trace%d(\"t%d\", x: n),\n", i, j); break;
      case 2: fprintf(f, "    trace%d(s, xs: [n, %d]),\n", i, j); break;
      default: fprintf(f, "    trace%d(\"t%d\", d: [s: %d]),\n", i, j, j); break;
      }
    }
    fprintf(f, "  ]\n  return all + (0..<n).map { trace%d(\"closure \\($0)\", x: $0) }\n}\n\n", i);
  }
}

static int argsFor(const char **argv, const Case *k, const char *out) {
  int n = 0;
  argv[n++] = "swift-frontend";
  argv[n++] = k->mode;
  if (!strcmp(k->source, "constructs")) {
    for (int f = 0; f < CONSTRUCT_FILES; ++f)
      argv[n++] = fmt("%s/constructs/c%d.swift", WORK, f);
    argv[n++] = fmt("%s/constructs/main.swift", WORK);
  } else {
    argv[n++] = fmt("%s/%s", WORK, k->source);
  }
  argv[n++] = "-target";
  argv[n++] = "wasm32-unknown-wasip1";
  argv[n++] = "-disable-objc-interop";
  argv[n++] = "-sdk";
  argv[n++] = fmt("%s/WASI.sdk", SDK);
  argv[n++] = "-resource-dir";
  argv[n++] = fmt("%s/swift.xctoolchain/usr/lib/swift_static", SDK);
  argv[n++] = "-use-static-resource-dir";
  argv[n++] = "-module-cache-path";
  argv[n++] = fmt("%s/mc", WORK);
  argv[n++] = "-no-color-diagnostics";
  argv[n++] = "-module-name";
  argv[n++] = "main";
  if (strcmp(k->source, "hello.swift") && strcmp(k->source, "constructs"))
    argv[n++] = "-parse-as-library";
  if (k->optimize)
    argv[n++] = "-O";
  argv[n++] = "-o";
  argv[n++] = out;
  return n;
}

static void runCase(const Case *k) {
  const char *argv[32];
  /* Each case writes into a directory of its own, so that temporary files are seen too. */
  char *dir = fmt("%s/%s", WORK, k->tag), *name = fmt("out.%s", k->ext), *out = fmt("%s/%s", dir, name);
  mkdir(dir, 0755);
  clearDir(dir);
  const char *sentinel = "a file that was at the output path before the call\n";
  if (k->sentinel) {
    FILE *f = fopen(out, "w");
    fputs(sentinel, f);
    fclose(f);
  }
  int argc = argsFor(argv, k, out);
  Outcome o = run(argv, argc, k->tag, (Trigger){k->stage, -1, NULL});
  if (!o.cancelled) {
    printf("%-20s rc %3d  the call returned before a sample landed in %s (%.0f ms)\n", k->tag, o.rc, k->stage,
           o.total);
    CHECK(0, "%s: no cancel inside %s", k->tag, k->stage);
    return;
  }
  printf("%-20s rc %3d  cancel in %-8s latency %4.0f ms, %4.0f ms of CPU (call ran %.0f ms)\n", k->tag, o.rc,
         k->stage, o.latency, o.latencyCpu, o.total);
  CHECK(o.rc == SWIFTHOST_ECANCELED, "%s: rc %d (expected %d)", k->tag, o.rc, SWIFTHOST_ECANCELED);
  char *diag = readFile(fmt("%s/%s.err", WORK, k->tag));
  CHECK(onlyCancellation(diag), "%s: diagnostics other than \"compilation cancelled\":\n%s", k->tag,
        diag ? diag : "(none)");
  CHECK(o.latencyCpu < LATENCY_CPU_MS, "%s: cancel took %.0f ms of CPU (bound %d ms)", k->tag, o.latencyCpu,
        LATENCY_CPU_MS);
  if (k->sentinel) {
    char *now = readFile(out);
    CHECK(now && !strcmp(now, sentinel), "%s: the file at the output path changed", k->tag);
  }
  char *left = otherFiles(dir, k->sentinel ? name : NULL);
  CHECK(!*left, "%s: a cancelled call left files in %s: %s", k->tag, dir, left);
}

/* One whole-module compile of a program, uncancelled, then cancelled at `whole` points of CPU time spread over the
   whole compile, `silgen` points over its SIL generation (where the compiler type-checks synthesized code on demand)
   and `check` points over its type checking and SIL generation. Every cancelled call must return SWIFTHOST_ECANCELED
   and leave no output; a call that finished first must succeed. */
static void sweep(const char *name, const char *source, int whole, int silgen, int check) {
  Case k = {name, NULL, source, "-c", "o", 0, 0};
  const char *argv[64];
  char *dir = fmt("%s/%s-out", WORK, name), *out = fmt("%s/out.o", dir), *err = fmt("%s/%s.err", WORK, name);
  mkdir(dir, 0755);
  clearDir(dir);
  int argc = argsFor(argv, &k, out);
  Outcome full = run(argv, argc, name, (Trigger){NULL, -1, "SILGen"});
  printf("%-20s rc %3d  uncancelled: %.0f ms of CPU, SIL generation at %.0f-%.0f ms\n", name, full.rc, full.cpu,
         full.watchFirst, full.watchLast);
  char *fullDiag = readFile(err);
  CHECK(full.rc == 0 && exists(out) && fullDiag && !*fullDiag, "%s: uncancelled compile: rc %d, diagnostics:\n%s",
        name, full.rc, fullDiag ? fullDiag : "(none)");
  if (full.rc != 0 || full.watchFirst < 0)
    return;
  int points = 0, cancelled = 0, finished = 0, bad = 0;
  for (int i = 0; i < whole + silgen + check; ++i) {
    double at = i < whole            ? full.cpu * (i + 0.5) / whole
                : i < whole + silgen ? full.watchFirst + (full.watchLast - full.watchFirst) * (i - whole) /
                                                             (silgen > 1 ? silgen - 1 : 1)
                                     : full.watchLast * (i - whole - silgen + 0.5) / check;
    clearDir(dir);
    Outcome o = run(argv, argc, name, (Trigger){NULL, at, NULL});
    ++points;
    char *left = otherFiles(dir, NULL), *diag = readFile(err);
    if (o.rc == SWIFTHOST_ECANCELED && o.cancelled && !*left && onlyCancellation(diag)) {
      ++cancelled;
    } else if (o.rc == 0 && exists(out) && diag && !*diag) {
      ++finished;
    } else {
      ++bad;
      printf("%s: cancel at %.0f ms of CPU: rc %d%s%s, diagnostics:\n%s", name, at, o.rc, *left ? ", left " : "",
             left, diag ? diag : "(none)\n");
      CHECK(0, "%s: cancel at %.0f ms of CPU: rc %d, files left: %s", name, at, o.rc, *left ? left : "none");
    }
  }
  printf("%-20s %d points: %d cancelled, %d finished first, %d failed\n", fmt("%s sweep", name), points, cancelled,
         finished, bad);
  CHECK(cancelled >= silgen + check / 2, "%s: only %d of %d points were cancelled", name, cancelled, points);
}

int main(int argc, char **argv) {
  if (argc < 4) {
    fprintf(stderr, "usage: cancel-stages <sdk-target-dir> <work-dir> <stages-file>\n");
    return 2;
  }
  SDK = argv[1];
  WORK = argv[2];
  loadRanges(argv[3]);
  writeFile(fmt("%s/interp.swift", WORK), interpProgram);
  writeFile(fmt("%s/lib.swift", WORK), libProgram);
  writeFile(fmt("%s/hello.swift", WORK), helloProgram);
  writeConstructs();
  writeFile(fmt("%s/defaults.swift", WORK), defaultsProgram);

  /* Warm the module cache, so that no case spends its time building modules. */
  Case warm = {"warm", NULL, "hello.swift", "-c", "o", 0, 0};
  const char *wargv[32];
  char *wout = fmt("%s/warm.o", WORK);
  Outcome w = run(wargv, argsFor(wargv, &warm, wout), "warm", (Trigger){NULL, -1, NULL});
  printf("%-20s rc %3d  (%.0f ms)\n", "warm", w.rc, w.total);
  CHECK(w.rc == 0, "warm-up compile: rc %d", w.rc);

  static const Case cases[] = {
      /* SIL generation inside string interpolations (brace statements inside expressions). */
      {"silgen-interp", "SILGen", "interp.swift", "-c", "o", 0, 0},
      {"silgen-interp-sil", "SILGen", "interp.swift", "-emit-sil", "sil", 0, 0},
      /* The SIL optimizer at -O. */
      {"silpipe-sil", "SILPipe", "lib.swift", "-emit-sil", "sil", 1, 0},
      {"silpipe-obj", "SILPipe", "lib.swift", "-c", "o", 1, 0},
      /* The lowering passes, before the lowered SIL is written. */
      {"sillower-sil", "SILLower", "lib.swift", "-emit-lowered-sil", "sil", 1, 0},
      /* IR generation. */
      {"irgen-ir", "IRGen", "lib.swift", "-emit-ir", "ll", 1, 0},
      {"irgen-obj", "IRGen", "lib.swift", "-c", "o", 1, 0},
      /* LLVM optimization: object, IR (written by the pipeline itself), and an older object at the path. */
      {"llvmopt-obj", "LLVMOpt", "lib.swift", "-c", "o", 1, 0},
      {"llvmopt-ir", "LLVMOpt", "lib.swift", "-emit-ir", "ll", 1, 0},
      {"llvmopt-sentinel", "LLVMOpt", "lib.swift", "-c", "o", 1, 1},
  };
  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i)
    runCase(&cases[i]);

  sweep("constructs", "constructs", SWEEP_WHOLE, SWEEP_SILGEN, 0);
  sweep("defaults", "defaults.swift", 0, 0, SWEEP_CHECK);

  /* The library stays usable. */
  Case after = {"after", NULL, "hello.swift", "-c", "o", 0, 0};
  const char *aargv[32];
  char *aout = fmt("%s/after.o", WORK);
  unlink(aout);
  Outcome a = run(aargv, argsFor(aargv, &after, aout), "after", (Trigger){NULL, -1, NULL});
  printf("%-20s rc %3d  (%.0f ms)\n", "after", a.rc, a.total);
  CHECK(a.rc == 0 && exists(aout), "compile after the cancelled ones: rc %d", a.rc);

  printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
  return failures ? 1 : 0;
}
