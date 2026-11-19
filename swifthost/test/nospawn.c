// DYLD_INSERT_LIBRARIES interposer for the smoke test: logs process creation
// (and aborts) and exit calls.
#include <dlfcn.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#pragma clang diagnostic ignored "-Wdeprecated-declarations" // vfork
#define DYLD_INTERPOSE(_replacement, _replacee)                                \
  __attribute__((used)) static struct {                                        \
    const void *replacement;                                                   \
    const void *replacee;                                                      \
  } _interpose_##_replacee __attribute__((section("__DATA,__interpose"))) = { \
      (const void *)(unsigned long)&_replacement,                              \
      (const void *)(unsigned long)&_replacee};
static void logc(const char *what) {
  char buf[256];
  int n = snprintf(buf, sizeof buf, "NOSPAWN: %s called\n", what);
  write(2, buf, n);
}
static int my_posix_spawn(pid_t *p, const char *path, const posix_spawn_file_actions_t *fa,
                          const posix_spawnattr_t *a, char *const argv[], char *const envp[]) {
  logc("posix_spawn"); abort();
}
static int my_posix_spawnp(pid_t *p, const char *path, const posix_spawn_file_actions_t *fa,
                           const posix_spawnattr_t *a, char *const argv[], char *const envp[]) {
  logc("posix_spawnp"); abort();
}
static pid_t my_fork(void) { logc("fork"); abort(); }
static pid_t my_vfork(void) { logc("vfork"); abort(); }
static int my_execve(const char *p, char *const a[], char *const e[]) { logc("execve"); abort(); }
static int my_execvp(const char *p, char *const a[]) { logc("execvp"); abort(); }
#include <TargetConditionals.h>
#if TARGET_OS_OSX // system() is unavailable in the iOS SDK.
static int my_system(const char *c) { logc("system"); abort(); }
#endif
static FILE *my_popen(const char *c, const char *m) { logc("popen"); abort(); }
static void my_exit(int s) { char b[64]; snprintf(b, sizeof b, "exit(%d)", s); logc(b); exit(s); }
static void my__exit(int s) { char b[64]; snprintf(b, sizeof b, "_exit(%d)", s); logc(b); _exit(s); }
DYLD_INTERPOSE(my_posix_spawn, posix_spawn)
DYLD_INTERPOSE(my_posix_spawnp, posix_spawnp)
DYLD_INTERPOSE(my_fork, fork)
DYLD_INTERPOSE(my_vfork, vfork)
DYLD_INTERPOSE(my_execve, execve)
DYLD_INTERPOSE(my_execvp, execvp)
#if TARGET_OS_OSX
DYLD_INTERPOSE(my_system, system)
#endif
DYLD_INTERPOSE(my_popen, popen)
DYLD_INTERPOSE(my_exit, exit)
DYLD_INTERPOSE(my__exit, _exit)
