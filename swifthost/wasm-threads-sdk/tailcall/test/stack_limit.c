// Copy of wasi-libc test/src/pthread_stack_limit.c (Hyp3rSon1X/wasi-libc, branch hyp3r/wasm-sdk/6.4.2).
#undef NDEBUG
#define _GNU_SOURCE // pthread_getattr_np

#include <assert.h>
#include <pthread.h>
#include <semaphore.h>
#include <stddef.h>
#include <stdint.h>

// Each thread exports the lowest valid address of its stack in the
// `__stack_limit` global; the main thread keeps it at zero.
static uintptr_t stack_limit(void) {
  uintptr_t limit;
  __asm__ volatile(".globaltype __stack_limit, i32\n"
                   "global.get __stack_limit\n"
                   "local.set %0\n"
                   : "=r"(limit));
  return limit;
}

// Large enough that TLS and TSD do not fit in a 4 KiB application-provided
// stack, which makes pthread_create allocate them separately.
static _Thread_local volatile char tls_pad[1024];

enum tls_place { TLS_ANYWHERE, TLS_IN_STACK, TLS_OUTSIDE_STACK };

struct expect {
  // The application-provided stack, if any.
  char *stack;
  size_t stack_size;
  size_t guard;
  enum tls_place tls;
  sem_t *done;
};

static void *check_thread(void *arg) {
  struct expect *expect = arg;
  char local;
  uintptr_t limit = stack_limit();
  assert(limit != 0);

  pthread_attr_t attr;
  void *addr;
  size_t size, guard;
  assert(pthread_getattr_np(pthread_self(), &attr) == 0);
  assert(pthread_attr_getstack(&attr, &addr, &size) == 0);
  assert(pthread_attr_getguardsize(&attr, &guard) == 0);
  assert(pthread_attr_destroy(&attr) == 0);

  // `addr` is the low end of the stack: stackaddr - stacksize.
  assert(limit == (uintptr_t)addr);
  assert((uintptr_t)&local > limit);
  assert((uintptr_t)&local <= (uintptr_t)addr + size);
  assert(guard == expect->guard);

  if (expect->stack) {
    assert(limit == (uintptr_t)expect->stack);
    tls_pad[0] = 1;
    uintptr_t tls = (uintptr_t)tls_pad;
    int in_stack = tls >= (uintptr_t)expect->stack &&
                   tls < (uintptr_t)expect->stack + expect->stack_size;
    if (expect->tls == TLS_IN_STACK)
      assert(in_stack);
    if (expect->tls == TLS_OUTSIDE_STACK)
      assert(!in_stack);
  }
  if (expect->done)
    assert(sem_post(expect->done) == 0);
  return arg;
}

static void run(pthread_attr_t *attr, struct expect *expect) {
  pthread_t t;
  void *ret;
  assert(pthread_create(&t, attr, check_thread, expect) == 0);
  assert(pthread_join(t, &ret) == 0);
  assert(ret == expect);
}

static void run_with_guard(size_t guardsize) {
  pthread_attr_t attr;
  size_t guard;
  struct expect expect = {.guard = guardsize};
  assert(pthread_attr_init(&attr) == 0);
  assert(pthread_attr_setguardsize(&attr, guardsize) == 0);
  assert(pthread_attr_getguardsize(&attr, &guard) == 0);
  assert(guard == guardsize);
  run(&attr, &expect);
  assert(pthread_attr_destroy(&attr) == 0);
}

static void run_with_stack(char *stack, size_t size, enum tls_place tls) {
  pthread_attr_t attr;
  struct expect expect = {
      .stack = stack, .stack_size = size, .guard = 0, .tls = tls};
  assert(pthread_attr_init(&attr) == 0);
  assert(pthread_attr_setstack(&attr, stack, size) == 0);
  run(&attr, &expect);
  assert(pthread_attr_destroy(&attr) == 0);
}

static _Alignas(16) char user_stack[65536];
static _Alignas(16) char small_stack[4096];

int main(void) {
  pthread_attr_t attr, copy;
  size_t guard;

  // The main thread has no limit.
  assert(stack_limit() == 0);

  // Default attributes: a malloc'ed stack with an 8 KiB guard below the
  // limit.
  struct expect def = {.guard = 8192};
  run(NULL, &def);

  // The default guard size can be read and set again.
  assert(pthread_attr_init(&attr) == 0);
  assert(pthread_attr_getguardsize(&attr, &guard) == 0);
  assert(guard == 8192);
  assert(pthread_attr_init(&copy) == 0);
  assert(pthread_attr_setguardsize(&copy, guard) == 0);
  assert(pthread_attr_destroy(&copy) == 0);
  assert(pthread_attr_destroy(&attr) == 0);

  // Explicit guard sizes: no guard, the default and a larger one.
  run_with_guard(0);
  run_with_guard(8192);
  run_with_guard(65536);

  // An application-provided stack: the limit is its low end. TLS and TSD
  // fit in a 64 KiB stack and are placed at its top...
  run_with_stack(user_stack, sizeof(user_stack), TLS_IN_STACK);
  // ...but not in a 4 KiB one, so they are allocated separately.
  run_with_stack(small_stack, sizeof(small_stack), TLS_OUTSIDE_STACK);

  // A detached thread.
  sem_t done;
  pthread_t t;
  struct expect detached = {.guard = 8192, .done = &done};
  assert(sem_init(&done, 0, 0) == 0);
  assert(pthread_attr_init(&attr) == 0);
  assert(pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED) == 0);
  assert(pthread_create(&t, &attr, check_thread, &detached) == 0);
  assert(pthread_attr_destroy(&attr) == 0);
  assert(sem_wait(&done) == 0);
  assert(sem_destroy(&done) == 0);

  // Creating threads does not give the main thread a limit.
  assert(stack_limit() == 0);
  return 0;
}
