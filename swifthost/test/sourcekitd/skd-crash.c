// Crash containment and coexistence test for the in-process sourcekitd inside SwiftHostCompiler
// (see "In-process sourcekitd" in SwiftHostCompiler.h). Each mode runs in a process of its own,
// because a stopped sourcekitd stays stopped:
//
//   crash-sync      a real completion crasher (validation-test/IDE/crashers) sent with
//                   sourcekitd_send_request_sync
//   crash-async     the same crasher sent with sourcekitd_send_request
//   overflow        a completion in a deep typealias chain overflows the stack of the 8 MiB
//                   thread the request runs on, while a slow diagnostics request is in flight
//   fatal           an LLVM fatal error in sourcekitd's work (#pragma clang __debug
//                   llvm_fatal_error in the bridging header of a completion request)
//   during-compile  the crasher while a long swifthost_swift_frontend compile runs; the compile
//                   must produce the same object as a compile alone
//   frontend-fatal  an LLVM fatal error in a frontend call: the call reports it and poisons the
//                   library, sourcekitd keeps working
//   macro           a macro in a sourcekitd request reaches the app's plugin message handler
//                   (swifthost_set_plugin_message_handler), during a frontend compile
//   attribution     the crasher while short frontend compiles run back to back, with the test hook
//                   SWIFTHOST_TEST_SOURCEKIT_RECOVERY_DELAY_MS holding the crashed work item between
//                   LLVM's count of the recovery and the end of its own: no compile is poisoned
//   receivers       the app's response receivers and notification handler run outside the
//                   library's crash recovery (no alternate signal stack of the library's)
//   receiver-crash  a trap in an async response receiver is not recovered: the process ends with
//                   SIGTRAP (run-crash.sh checks that), sourcekitd is not reported as stopped
//   notification-crash  the same for a trap in the notification handler
//
// After a crash mode stopped sourcekitd, the test checks: the crashing request got the agreed error,
// swifthost_sourcekitd_status() returns the signal, later requests (sync and async) fail at once,
// cancel/dispose/notification handler/shutdown return, and a swifthost_swift_frontend compile
// still succeeds. One line per check ("PASS ..." or "FAIL ..."); the exit status is the number of
// failed checks.
//
//   skd-crash <mode> --args compilerargs.txt --sdk DIR --mc DIR --work DIR
//             --crasher FILE --crasher-offset N --overflow FILE --slow FILE
//             --compile-args FILE --object PATH [--long-compile-args FILE --long-object PATH]
//
// compilerargs.txt as for skd-probe (${SDK}, ${MC} replaced); --crasher is a crasher source with
// its #^...^# token removed and --crasher-offset the token's byte offset; --overflow ends where its
// completion point is; --compile-args has one frontend argument per line (argv[0] first) and writes
// --object.
#include "SwiftHostCompiler.h"
#include "sourcekitd/sourcekitd.h"

#include <dispatch/dispatch.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// The canned plugin server of test/plugin-server.c, linked in as the app's handler.
bool swift_inproc_plugins_handle_message(const char *message, size_t length, char **response,
                                         size_t *response_length);
int swifthost_test_plugin_count(const char *kind);

static int failures;
static const char *WORK, *SDK, *MC, *ARGS_FILE;

#define CHECK(cond, ...)                                                                           \
  do {                                                                                             \
    printf((cond) ? "PASS " : "FAIL ");                                                            \
    printf(__VA_ARGS__);                                                                           \
    printf("\n");                                                                                  \
    fflush(stdout);                                                                                \
    if (!(cond))                                                                                   \
      ++failures;                                                                                  \
  } while (0)

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static char *fmt(const char *f, ...) __attribute__((format(printf, 1, 2)));
static char *fmt(const char *f, ...) {
  va_list ap;
  va_start(ap, f);
  char *s = NULL;
  if (vasprintf(&s, f, ap) < 0)
    abort();
  va_end(ap);
  return s;
}

static char *read_file(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  rewind(f);
  char *text = calloc((size_t)n + 1, 1);
  if (fread(text, 1, (size_t)n, f) != (size_t)n) {
    fclose(f);
    free(text);
    return NULL;
  }
  fclose(f);
  return text;
}

static void write_file(const char *path, const char *text) {
  FILE *f = fopen(path, "wb");
  if (!f || fputs(text, f) < 0) {
    perror(path);
    exit(2);
  }
  fclose(f);
}

static uint64_t file_hash(const char *path) {
  char *text = read_file(path);
  if (!text)
    return 0;
  struct stat st;
  stat(path, &st);
  uint64_t h = 1469598103934665603ull;
  for (off_t i = 0; i < st.st_size; i++)
    h = (h ^ (unsigned char)text[i]) * 1099511628211ull;
  free(text);
  return h;
}

// ---- requests

static sourcekitd_uid_t U(const char *s) { return sourcekitd_uid_get_from_cstr(s); }

static char *subst(const char *line) {
  char *out = strdup(line), *p;
  while ((p = strstr(out, "${SDK}")) || (p = strstr(out, "${MC}"))) {
    int sdk = !strncmp(p, "${SDK}", 6);
    *p = 0;
    char *next = fmt("%s%s%s", out, sdk ? SDK : MC, p + (sdk ? 6 : 5));
    free(out);
    out = next;
  }
  return out;
}

// key.compilerargs: the file, the arguments of compilerargs.txt, then extra (NULL-terminated).
static sourcekitd_object_t compiler_args(const char *file, const char *const *extra) {
  sourcekitd_object_t args = sourcekitd_request_array_create(NULL, 0);
  sourcekitd_request_array_set_string(args, SOURCEKITD_ARRAY_APPEND, file);
  char *text = read_file(ARGS_FILE);
  if (!text) {
    perror(ARGS_FILE);
    exit(2);
  }
  for (char *save, *l = strtok_r(text, "\n", &save); l; l = strtok_r(NULL, "\n", &save))
    if (*l) {
      char *a = subst(l);
      sourcekitd_request_array_set_string(args, SOURCEKITD_ARRAY_APPEND, a);
      free(a);
    }
  free(text);
  for (; extra && *extra; ++extra)
    sourcekitd_request_array_set_string(args, SOURCEKITD_ARRAY_APPEND, *extra);
  return args;
}

static sourcekitd_object_t new_request(const char *kind, const char *file, const char *const *extra) {
  sourcekitd_object_t req = sourcekitd_request_dictionary_create(NULL, NULL, 0);
  sourcekitd_request_dictionary_set_uid(req, U("key.request"), U(kind));
  sourcekitd_request_dictionary_set_string(req, U("key.sourcefile"), file);
  sourcekitd_object_t args = compiler_args(file, extra);
  sourcekitd_request_dictionary_set_value(req, U("key.compilerargs"), args);
  sourcekitd_request_release(args);
  return req;
}

static sourcekitd_object_t complete_request(const char *file, long offset, const char *const *extra) {
  sourcekitd_object_t req = new_request("source.request.codecomplete", file, extra);
  char *text = read_file(file);
  sourcekitd_request_dictionary_set_string(req, U("key.sourcetext"), text ? text : "");
  free(text);
  sourcekitd_request_dictionary_set_int64(req, U("key.offset"), offset);
  return req;
}

static sourcekitd_object_t open_request(const char *file) {
  sourcekitd_object_t req = new_request("source.request.editor.open", file, NULL);
  char *text = read_file(file);
  sourcekitd_request_dictionary_set_string(req, U("key.name"), file);
  sourcekitd_request_dictionary_set_string(req, U("key.sourcetext"), text ? text : "");
  free(text);
  sourcekitd_request_dictionary_set_int64(req, U("key.enablesyntaxmap"), 0);
  sourcekitd_request_dictionary_set_int64(req, U("key.enablesubstructure"), 0);
  return req;
}

static sourcekitd_object_t diag_request(const char *file, const char *const *extra) {
  return new_request("source.request.diagnostics", file, extra);
}

typedef struct {
  int is_error, kind;
  char desc[512];
  long count; // completion results or error diagnostics, -1 if none
  double ms;
} answer_t;

// Summarizes a response and disposes it.
static answer_t summarize(sourcekitd_response_t r, double ms) {
  answer_t a = {.count = -1, .ms = ms};
  if (sourcekitd_response_is_error(r)) {
    a.is_error = 1;
    a.kind = sourcekitd_response_error_get_kind(r);
    snprintf(a.desc, sizeof a.desc, "%s", sourcekitd_response_error_get_description(r));
  } else {
    sourcekitd_variant_t v = sourcekitd_response_get_value(r);
    sourcekitd_variant_t results = sourcekitd_variant_dictionary_get_value(v, U("key.results"));
    sourcekitd_variant_t diags = sourcekitd_variant_dictionary_get_value(v, U("key.diagnostics"));
    if (sourcekitd_variant_get_type(results) == SOURCEKITD_VARIANT_TYPE_ARRAY)
      a.count = (long)sourcekitd_variant_array_get_count(results);
    else if (sourcekitd_variant_get_type(diags) == SOURCEKITD_VARIANT_TYPE_ARRAY) {
      a.count = 0;
      size_t n = sourcekitd_variant_array_get_count(diags);
      for (size_t i = 0; i < n; i++) {
        sourcekitd_variant_t d = sourcekitd_variant_array_get_value(diags, i);
        if (sourcekitd_variant_dictionary_get_uid(d, U("key.severity")) ==
            U("source.diagnostic.severity.error")) {
          if (!a.count)
            snprintf(a.desc, sizeof a.desc, "%s",
                     sourcekitd_variant_dictionary_get_string(d, U("key.description")));
          a.count++;
        }
      }
    }
  }
  sourcekitd_response_dispose(r);
  return a;
}

static answer_t send_sync(sourcekitd_object_t req) {
  double t0 = now_ms();
  sourcekitd_response_t r = sourcekitd_send_request_sync(req);
  sourcekitd_request_release(req);
  return summarize(r, now_ms() - t0);
}

typedef struct {
  dispatch_semaphore_t done;
  double sent;
  answer_t answer;
  sourcekitd_request_handle_t handle;
} async_t;

static async_t *send_async(sourcekitd_object_t req) {
  async_t *a = calloc(1, sizeof *a);
  a->done = dispatch_semaphore_create(0);
  a->sent = now_ms();
  sourcekitd_send_request(req, &a->handle, ^(sourcekitd_response_t r) {
    a->answer = summarize(r, now_ms() - a->sent);
    dispatch_semaphore_signal(a->done);
  });
  sourcekitd_request_release(req);
  return a;
}

static int wait_async(async_t *a, int timeout_ms) {
  return !dispatch_semaphore_wait(a->done, dispatch_time(DISPATCH_TIME_NOW, (int64_t)timeout_ms * NSEC_PER_MSEC));
}

static const char *describe(const answer_t *a) {
  static char buf[700];
  if (a->is_error)
    snprintf(buf, sizeof buf, "error kind %d \"%s\" after %.1f ms", a->kind, a->desc, a->ms);
  else
    snprintf(buf, sizeof buf, "%ld results/errors%s%s after %.1f ms", a->count, a->desc[0] ? ", first: " : "",
             a->desc, a->ms);
  return buf;
}

// Whether the calling thread runs on an alternate signal stack of the library's size (256 KiB):
// the library gives one to every work item of sourcekitd's that it runs under crash recovery.
static int on_library_signal_stack(void) {
  stack_t ss;
  return !sigaltstack(NULL, &ss) && !(ss.ss_flags & SS_DISABLE) && ss.ss_size == 256 * 1024;
}

static sourcekitd_object_t test_notification_request(void) {
  sourcekitd_object_t req = sourcekitd_request_dictionary_create(NULL, NULL, 0);
  sourcekitd_request_dictionary_set_uid(req, U("key.request"), U("source.request.test_notification"));
  return req;
}

// ---- frontend calls (on a thread with a large stack, as the library requires)

typedef struct {
  const char *args_file, *object, *err_path;
  const char *const *extra;
  int rc;
  double ms;
  uint64_t hash;
} compile_t;

static void *compile_main(void *arg) {
  compile_t *c = arg;
  char *text = read_file(c->args_file);
  if (!text) {
    perror(c->args_file);
    exit(2);
  }
  const char *argv[4096];
  int argc = 0;
  for (char *save, *l = strtok_r(text, "\n", &save); l; l = strtok_r(NULL, "\n", &save))
    if (*l)
      argv[argc++] = l;
  for (const char *const *e = c->extra; e && *e; ++e)
    argv[argc++] = *e;
  argv[argc] = NULL;
  int err = open(c->err_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  unlink(c->object);
  double t0 = now_ms();
  c->rc = swifthost_swift_frontend(argc, argv, -1, err);
  c->ms = now_ms() - t0;
  close(err);
  c->hash = file_hash(c->object);
  free(text);
  return NULL;
}

static pthread_t start_compile(compile_t *c) {
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 64u << 20);
  pthread_t t;
  pthread_create(&t, &attr, compile_main, c);
  return t;
}

static void run_compile(compile_t *c) { pthread_join(start_compile(c), NULL); }

// ---- signal dispositions

static const int kSignals[] = {SIGABRT, SIGBUS, SIGFPE, SIGILL, SIGSEGV, SIGTRAP};
typedef struct {
  struct sigaction act[6];
} sigsnap_t;

static void snap(sigsnap_t *s) {
  for (int i = 0; i < 6; i++)
    sigaction(kSignals[i], NULL, &s->act[i]);
}

static int same_dispositions(const sigsnap_t *a, const sigsnap_t *b) {
  for (int i = 0; i < 6; i++)
    if (a->act[i].sa_handler != b->act[i].sa_handler || a->act[i].sa_flags != b->act[i].sa_flags)
      return 0;
  return 1;
}

// ---- the checks after sourcekitd stopped

static const char *HEALTHY;   // a small source with a member completion at its end
static long HEALTHY_OFFSET;
static const char *COMPILE_ARGS, *OBJECT;

static void check_healthy(const char *when) {
  answer_t a = send_sync(complete_request(HEALTHY, HEALTHY_OFFSET, NULL));
  CHECK(!a.is_error && a.count > 0, "%s: completion works: %s", when, describe(&a));
}

static void check_stopped(int expect_status, const char *expect_desc) {
  int status = swifthost_sourcekitd_status();
  CHECK(status == expect_status, "swifthost_sourcekitd_status() = %d (expected %d)", status, expect_status);

  answer_t s = send_sync(complete_request(HEALTHY, HEALTHY_OFFSET, NULL));
  CHECK(s.is_error && s.kind == SOURCEKITD_ERROR_CONNECTION_INTERRUPTED && !strcmp(s.desc, expect_desc) &&
            s.ms < 50,
        "later sync request fails fast: %s", describe(&s));

  async_t *a = send_async(complete_request(HEALTHY, HEALTHY_OFFSET, NULL));
  int answered = wait_async(a, 2000);
  CHECK(answered && a->answer.is_error && a->answer.kind == SOURCEKITD_ERROR_CONNECTION_INTERRUPTED &&
            !strcmp(a->answer.desc, expect_desc) && a->answer.ms < 50,
        "later async request fails fast: %s", answered ? describe(&a->answer) : "no answer within 2 s");

  answer_t o = send_sync(open_request(HEALTHY));
  CHECK(o.is_error && o.kind == SOURCEKITD_ERROR_CONNECTION_INTERRUPTED, "later editor.open fails: %s",
        describe(&o));

  double t0 = now_ms();
  sourcekitd_cancel_request(a->handle);
  sourcekitd_request_handle_dispose(a->handle);
  sourcekitd_set_notification_handler(^(sourcekitd_response_t r) { sourcekitd_response_dispose(r); });
  sourcekitd_shutdown();
  CHECK(now_ms() - t0 < 50, "cancel, dispose, notification handler and shutdown return (%.1f ms)", now_ms() - t0);

  compile_t c = {.args_file = COMPILE_ARGS, .object = OBJECT, .err_path = fmt("%s/after.err", WORK)};
  run_compile(&c);
  CHECK(c.rc == 0 && c.hash != 0, "frontend compile after the stop: rc %d, %.0f ms, object %s", c.rc, c.ms,
        c.hash ? "written" : "missing");
}

static const char *crash_desc(int signal) {
  return fmt("sourcekitd stopped: recovered crash (signal %d)", signal);
}

// ---- the modes that need more than a request

typedef struct {
  const char *name;
  dispatch_semaphore_t done;
  int outside, main_thread;
  answer_t answer;
} probe_t;

static probe_t *probe_new(const char *name) {
  probe_t *p = calloc(1, sizeof *p);
  p->name = name;
  p->done = dispatch_semaphore_create(0);
  return p;
}

static void probe_async(probe_t *p, sourcekitd_object_t req) {
  double sent = now_ms();
  sourcekitd_send_request(req, NULL, ^(sourcekitd_response_t r) {
    p->outside = !on_library_signal_stack();
    p->answer = summarize(r, now_ms() - sent);
    dispatch_semaphore_signal(p->done);
  });
  sourcekitd_request_release(req);
}

static void check_probe(probe_t *p) {
  int answered = !dispatch_semaphore_wait(p->done, dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC));
  CHECK(answered && p->outside && !p->answer.is_error, "%s receiver ran outside the library's crash recovery: %s",
        p->name, answered ? describe(&p->answer) : "no answer within 30 s");
}

// Marks the main queue (after dispatch_main() it runs on a thread that is not the main thread).
static char kMainQueueKey;

static void mode_receivers(void) {
  dispatch_queue_set_specific(dispatch_get_main_queue(), &kMainQueueKey, &kMainQueueKey, NULL);
  // The probe itself: it sees an alternate stack of the library's size.
  stack_t mine = {.ss_sp = malloc(256 * 1024), .ss_size = 256 * 1024};
  sigaltstack(&mine, NULL);
  int seen = on_library_signal_stack();
  // Darwin wants at least MINSIGSTKSZ even to disable it.
  sigaltstack(&(stack_t){.ss_flags = SS_DISABLE, .ss_size = MINSIGSTKSZ}, NULL);
  free(mine.ss_sp);
  CHECK(seen && !on_library_signal_stack(), "the probe detects a 256 KiB alternate signal stack");

  __block probe_t *note = probe_new("notification handler");
  sourcekitd_set_notification_handler(^(sourcekitd_response_t r) {
    sourcekitd_variant_t v = sourcekitd_response_get_value(r);
    if (sourcekitd_variant_dictionary_get_uid(v, U("key.notification")) == U("source.notification.test")) {
      note->outside = !on_library_signal_stack();
      note->main_thread = dispatch_get_specific(&kMainQueueKey) == &kMainQueueKey;
      dispatch_semaphore_signal(note->done);
    }
    sourcekitd_response_dispose(r);
  });
  probe_t *complete = probe_new("completion"), *open = probe_new("editor.open"), *diag = probe_new("diagnostics"),
          *test = probe_new("test_notification");
  probe_async(complete, complete_request(HEALTHY, HEALTHY_OFFSET, NULL));
  probe_async(open, open_request(HEALTHY));
  probe_async(diag, diag_request(HEALTHY, NULL));
  check_probe(complete);
  check_probe(open);
  check_probe(diag);
  probe_async(test, test_notification_request());
  check_probe(test);
  int arrived = !dispatch_semaphore_wait(note->done, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC));
  CHECK(arrived && note->outside && note->main_thread,
        "notification handler ran on the main queue (%s) outside the library's crash recovery (%s)",
        arrived ? (note->main_thread ? "yes" : "NO") : "-",
        arrived ? (note->outside ? "yes" : "NO") : "no notification within 10 s");
  CHECK(swifthost_sourcekitd_status() == 0, "sourcekitd status: %d", swifthost_sourcekitd_status());
}

// Ends the process with SIGTRAP from the app's code that sourcekitd calls; returns only if it did not.
static void mode_app_trap(int in_notification) {
  dispatch_semaphore_t reached = dispatch_semaphore_create(0);
  if (in_notification)
    sourcekitd_set_notification_handler(^(sourcekitd_response_t r) {
      (void)r;
      printf("TRAP in the notification handler, %s the library's crash recovery\n",
             on_library_signal_stack() ? "INSIDE" : "outside");
      fflush(stdout);
      dispatch_semaphore_signal(reached);
      __builtin_trap();
    });
  sourcekitd_object_t req = in_notification ? test_notification_request()
                                            : complete_request(HEALTHY, HEALTHY_OFFSET, NULL);
  sourcekitd_send_request(req, NULL, ^(sourcekitd_response_t r) {
    if (in_notification) {
      sourcekitd_response_dispose(r);
      return;
    }
    printf("TRAP in the response receiver, %s the library's crash recovery\n",
           on_library_signal_stack() ? "INSIDE" : "outside");
    fflush(stdout);
    dispatch_semaphore_signal(reached);
    __builtin_trap();
  });
  sourcekitd_request_release(req);
  dispatch_semaphore_wait(reached, dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC));
  // A recovered trap would stop sourcekitd; give it time to show.
  usleep(2000 * 1000);
  CHECK(0, "the process survived the trap in the app's code: sourcekitd status %d", swifthost_sourcekitd_status());
}

typedef struct {
  double start, end;
  int rc;
} timed_compile_t;

typedef struct {
  atomic_int stop;
  int count;
  timed_compile_t runs[4096];
} compile_loop_t;

static void *compile_loop(void *arg) {
  compile_loop_t *loop = arg;
  while (!atomic_load(&loop->stop) && loop->count < 4096) {
    compile_t c = {.args_file = COMPILE_ARGS, .object = OBJECT, .err_path = fmt("%s/loop.err", WORK)};
    timed_compile_t *t = &loop->runs[loop->count];
    t->start = now_ms();
    compile_main(&c);
    t->end = now_ms();
    t->rc = c.rc;
    loop->count++;
  }
  return NULL;
}

static void mode_attribution(const char *crasher, long crasher_offset, int delay_ms) {
  // The module cache, so that the loop's compiles are short.
  compile_t warm = {.args_file = COMPILE_ARGS, .object = OBJECT, .err_path = fmt("%s/warm.err", WORK)};
  run_compile(&warm);
  CHECK(warm.rc == 0, "warm-up compile: rc %d, %.0f ms", warm.rc, warm.ms);

  static compile_loop_t loop;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 64u << 20);
  pthread_t t;
  pthread_create(&t, &attr, compile_loop, &loop);
  usleep(1000 * 1000);
  double sent = now_ms();
  answer_t a = send_sync(complete_request(crasher, crasher_offset, NULL));
  double answered = now_ms();
  int sig = swifthost_sourcekitd_status();
  CHECK(a.is_error && !strcmp(a.desc, crash_desc(sig)) && sig > 0, "crashing request during the compiles: %s",
        describe(&a));
  usleep(1000 * 1000);
  atomic_store(&loop.stop, 1);
  pthread_join(t, NULL);

  int bad = 0, before = 0, held = 0;
  double longest_before = 0, longest_held = 0;
  for (int i = 0; i < loop.count; i++) {
    timed_compile_t *r = &loop.runs[i];
    if (r->rc != 0) {
      bad++;
      printf("compile %d: rc %d, %.0f-%.0f ms after the request was sent\n", i, r->rc, r->start - sent,
             r->end - sent);
    }
    double ms = r->end - r->start;
    if (r->end < sent) {
      before++;
      if (ms > longest_before)
        longest_before = ms;
    } else if (r->start < answered - delay_ms / 2.0 && r->end > answered - delay_ms / 4.0) {
      // Running when the crash happened and still running near the end of the recovery's delay.
      held++;
      if (ms > longest_held)
        longest_held = ms;
    }
  }
  CHECK(bad == 0, "%d compiles ran back to back, none failed or poisoned (%d failed)", loop.count, bad);
  CHECK(before > 0 && longest_before < delay_ms / 2.0,
        "%d compiles before the crash, the longest %.0f ms (shorter than half the %d ms delay)", before,
        longest_before, delay_ms);
  CHECK(held > 0, "%d compile(s) overlapped the delayed end of the recovery (the longest %.0f ms) and were not "
                  "poisoned; the crashing request was answered %.0f ms after it was sent",
        held, longest_held, answered - sent);
}

typedef struct {
  const char *mode, *crasher, *overflow, *slow, *long_args, *long_object;
  long crasher_offset;
} options_t;

static int run_mode(options_t *o);

static void *mode_thread(void *arg) { exit(run_mode(arg)); }

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: see the comment at the top of skd-crash.c\n");
    return 2;
  }
  const char *mode = argv[1], *crasher = NULL, *overflow = NULL, *slow = NULL;
  const char *long_args = NULL, *long_object = NULL;
  long crasher_offset = 0;
  for (int i = 2; i < argc; i++) {
    const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
#define OPT(name) (!strcmp(a, name) && v && (i++, 1))
    if (OPT("--args")) ARGS_FILE = v;
    else if (OPT("--sdk")) SDK = v;
    else if (OPT("--mc")) MC = v;
    else if (OPT("--work")) WORK = v;
    else if (OPT("--crasher")) crasher = v;
    else if (OPT("--crasher-offset")) crasher_offset = atol(v);
    else if (OPT("--overflow")) overflow = v;
    else if (OPT("--slow")) slow = v;
    else if (OPT("--compile-args")) COMPILE_ARGS = v;
    else if (OPT("--object")) OBJECT = v;
    else if (OPT("--long-compile-args")) long_args = v;
    else if (OPT("--long-object")) long_object = v;
    else {
      fprintf(stderr, "unknown or incomplete option %s\n", a);
      return 2;
    }
  }
  if (!ARGS_FILE || !SDK || !MC || !WORK || !COMPILE_ARGS || !OBJECT) {
    fprintf(stderr, "--args, --sdk, --mc, --work, --compile-args and --object are required\n");
    return 2;
  }
  // Read once, when the library first sets itself up.
  if (!strcmp(mode, "attribution"))
    setenv("SWIFTHOST_TEST_SOURCEKIT_RECOVERY_DELAY_MS", "2000", 1);
  static options_t o;
  o = (options_t){mode, crasher, overflow, slow, long_args, long_object, crasher_offset};
  // These modes need the main queue (notifications): it runs on the main thread, the mode on another.
  if (!strcmp(mode, "receivers") || !strcmp(mode, "notification-crash")) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8u << 20);
    pthread_t t;
    pthread_create(&t, &attr, mode_thread, &o);
    dispatch_main();
  }
  return run_mode(&o);
}

static int run_mode(options_t *o) {
  const char *mode = o->mode, *crasher = o->crasher, *overflow = o->overflow, *slow = o->slow;
  const char *long_args = o->long_args, *long_object = o->long_object;
  long crasher_offset = o->crasher_offset;
  mkdir(WORK, 0755);
  printf("mode %s, %s\n", mode, swifthost_version());

  // A completion that works: members of S at the end of the file.
  static const char healthy[] = "struct S { var value = 1; func twice() -> Int { value * 2 } }\nlet s = S()\n"
                                "let v = s.";
  HEALTHY = fmt("%s/healthy.swift", WORK);
  write_file(HEALTHY, healthy);
  HEALTHY_OFFSET = (long)strlen(healthy);

  sigsnap_t before;
  snap(&before);
  int macro = !strcmp(mode, "macro");
  if (macro)
    swifthost_set_plugin_message_handler(swift_inproc_plugins_handle_message);
  sourcekitd_initialize();
  check_healthy("before");

  if (!strcmp(mode, "crash-sync") || !strcmp(mode, "crash-async")) {
    answer_t a;
    if (!strcmp(mode, "crash-sync")) {
      a = send_sync(complete_request(crasher, crasher_offset, NULL));
    } else {
      async_t *r = send_async(complete_request(crasher, crasher_offset, NULL));
      if (!wait_async(r, 30000)) {
        CHECK(0, "crashing async request: no answer within 30 s");
        return failures;
      }
      a = r->answer;
    }
    int sig = swifthost_sourcekitd_status();
    CHECK(a.is_error && a.kind == SOURCEKITD_ERROR_CONNECTION_INTERRUPTED && !strcmp(a.desc, crash_desc(sig)) &&
              sig > 0 && sig < 64,
          "crashing request: %s", describe(&a));
    check_stopped(sig, crash_desc(sig));
  } else if (!strcmp(mode, "overflow")) {
    // A request in flight when the crash happens (its diagnostics take minutes). Completion does
    // not wait for the AST builds of diagnostics (their queue is serial).
    answer_t so = send_sync(open_request(slow));
    CHECK(!so.is_error, "editor.open of the slow source: %s", describe(&so));
    async_t *inflight = send_async(diag_request(slow, NULL));
    usleep(500 * 1000);
    char *deep = read_file(overflow);
    answer_t a = send_sync(complete_request(overflow, deep ? (long)strlen(deep) : 0, NULL));
    free(deep);
    int sig = swifthost_sourcekitd_status();
    CHECK(a.is_error && a.kind == SOURCEKITD_ERROR_CONNECTION_INTERRUPTED && !strcmp(a.desc, crash_desc(sig)) &&
              (sig == SIGSEGV || sig == SIGBUS),
          "overflowing request: %s", describe(&a));
    int answered = wait_async(inflight, 2000);
    CHECK(answered && inflight->answer.is_error && !strcmp(inflight->answer.desc, crash_desc(sig)),
          "request in flight answered: %s (%.1f ms after it was sent)",
          answered ? describe(&inflight->answer) : "no answer within 2 s", answered ? inflight->answer.ms : 0);
    check_stopped(sig, crash_desc(sig));
  } else if (!strcmp(mode, "fatal")) {
    char *header = fmt("%s/fatal.h", WORK);
    write_file(header, "#pragma clang __debug llvm_fatal_error\n");
    const char *extra[] = {"-import-objc-header", header, NULL};
    answer_t a = send_sync(complete_request(HEALTHY, HEALTHY_OFFSET, extra));
    int status = swifthost_sourcekitd_status();
    const char *expect = "sourcekitd stopped: recovered fatal error (#pragma clang __debug llvm_fatal_error)";
    CHECK(a.is_error && a.kind == SOURCEKITD_ERROR_CONNECTION_INTERRUPTED && !strcmp(a.desc, expect) &&
              status == SWIFTHOST_SOURCEKITD_FATAL_ERROR,
          "fatal error request: %s, status %d", describe(&a), status);
    check_stopped(SWIFTHOST_SOURCEKITD_FATAL_ERROR, expect);
  } else if (!strcmp(mode, "during-compile")) {
    compile_t ref = {.args_file = long_args, .object = long_object, .err_path = fmt("%s/ref.err", WORK)};
    run_compile(&ref);
    CHECK(ref.rc == 0, "reference compile alone: rc %d, %.0f ms, hash %016llx", ref.rc, ref.ms,
          (unsigned long long)ref.hash);
    compile_t c = {.args_file = long_args, .object = long_object, .err_path = fmt("%s/during.err", WORK)};
    pthread_t t = start_compile(&c);
    usleep(1500 * 1000);
    answer_t a = send_sync(complete_request(crasher, crasher_offset, NULL));
    int sig = swifthost_sourcekitd_status();
    CHECK(a.is_error && !strcmp(a.desc, crash_desc(sig)) && sig > 0, "crashing request during the compile: %s",
          describe(&a));
    pthread_join(t, NULL);
    CHECK(c.rc == 0 && c.hash == ref.hash,
          "compile that ran during the crash: rc %d, %.0f ms, hash %016llx (%s the compile alone)", c.rc, c.ms,
          (unsigned long long)c.hash, c.hash == ref.hash ? "same as" : "DIFFERENT from");
    check_stopped(sig, crash_desc(sig));
  } else if (!strcmp(mode, "frontend-fatal")) {
    char *header = fmt("%s/fatal.h", WORK);
    write_file(header, "#pragma clang __debug llvm_fatal_error\n");
    const char *extra[] = {"-import-objc-header", header, NULL};
    compile_t c = {.args_file = COMPILE_ARGS, .object = OBJECT, .err_path = fmt("%s/fatal.err", WORK),
                   .extra = extra};
    run_compile(&c);
    char *err = read_file(c.err_path);
    // The pragma asks for a crash report (report_fatal_error's GenCrashDiag): LLVM aborts after the
    // handler, so the call returns 128 + SIGABRT.
    CHECK((c.rc == 1 || c.rc == 128 + SIGABRT) && err && strstr(err, "fatal error encountered during compilation") &&
              strstr(err, "note: #pragma clang __debug llvm_fatal_error"),
          "frontend fatal error: rc %d, err_fd: %s", c.rc, err ? err : "(none)");
    compile_t p = {.args_file = COMPILE_ARGS, .object = OBJECT, .err_path = fmt("%s/poisoned.err", WORK)};
    run_compile(&p);
    CHECK(p.rc == SWIFTHOST_EPOISONED, "next frontend call: rc %d (poisoned)", p.rc);
    CHECK(swifthost_sourcekitd_status() == 0, "sourcekitd status after the frontend's fatal error: %d",
          swifthost_sourcekitd_status());
    check_healthy("after the frontend's fatal error");
  } else if (macro) {
    // A frontend compile runs while sourcekitd expands a macro through the app's handler.
    compile_t c = {.args_file = long_args ? long_args : COMPILE_ARGS,
                   .object = long_args ? long_object : OBJECT,
                   .err_path = fmt("%s/macro-compile.err", WORK)};
    pthread_t t = start_compile(&c);
    char *src = fmt("%s/macro.swift", WORK);
    write_file(src, "@freestanding(expression) macro answer() -> Int = "
                    "#externalMacro(module: \"M\", type: \"AnswerMacro\")\n"
                    "let x: Int = #answer\nlet y: String = #answer\n");
    char *lib = fmt("%s/M.wasm", WORK);
    write_file(lib, "");
    setenv("SWIFTHOST_TEST_PLUGIN_FILE", src, 1);
    const char *extra[] = {"-load-resolved-plugin", fmt("%s##M", lib), NULL};
    usleep(500 * 1000);
    answer_t o = send_sync(open_request(src));
    answer_t d = send_sync(diag_request(src, extra));
    // let y: String = #answer is the one error: the expansion (42) is an Int.
    CHECK(!o.is_error && !d.is_error && d.count == 1 && strstr(d.desc, "Int"),
          "diagnostics with the macro expanded by the app's handler: %s", describe(&d));
    int expansions = swifthost_test_plugin_count("expandFreestandingMacro");
    CHECK(expansions > 0, "the app's handler served %d expansion request(s) from sourcekitd", expansions);
    pthread_join(t, NULL);
    CHECK(c.rc == 0, "frontend compile that ran at the same time: rc %d, %.0f ms", c.rc, c.ms);
    CHECK(swifthost_sourcekitd_status() == 0, "sourcekitd status: %d", swifthost_sourcekitd_status());
  } else if (!strcmp(mode, "attribution")) {
    mode_attribution(crasher, crasher_offset, 2000);
    check_stopped(swifthost_sourcekitd_status(), crash_desc(swifthost_sourcekitd_status()));
  } else if (!strcmp(mode, "receivers")) {
    mode_receivers();
  } else if (!strcmp(mode, "receiver-crash") || !strcmp(mode, "notification-crash")) {
    mode_app_trap(!strcmp(mode, "notification-crash"));
  } else {
    fprintf(stderr, "unknown mode %s\n", mode);
    return 2;
  }

  if (!slow) {
    // sourcekitd's background work (semantic info of open documents) may still run for a moment.
    sigsnap_t after;
    double t0 = now_ms();
    do {
      snap(&after);
    } while (!same_dispositions(&before, &after) && now_ms() - t0 < 10000 && usleep(50 * 1000) == 0);
    CHECK(same_dispositions(&before, &after), "signal dispositions restored once nothing runs (after %.0f ms)",
          now_ms() - t0);
  }
  printf("%s: %d failure(s)\n", mode, failures);
  return failures;
}
