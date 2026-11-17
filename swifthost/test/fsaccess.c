// Logs the paths a process opens or checks for, to find which SDK files the library reads.
//   xcrun clang -dynamiclib -O2 -o libfsaccess.dylib fsaccess.c
//   DYLD_INSERT_LIBRARIES=$PWD/libfsaccess.dylib FSACCESS_LOG=/tmp/access.log ./smoke <sdk> <work> - <test-dir>
// One line per call: "<open|openw|stat> <0|-1> <path>". Works only for executables without the hardened runtime.
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define DYLD_INTERPOSE(_replacement, _replacee) \
  __attribute__((used)) static struct { const void *replacement; const void *replacee; } _interpose_##_replacee \
  __attribute__((section("__DATA,__interpose"))) = {(const void *)(unsigned long)&_replacement, (const void *)(unsigned long)&_replacee};

static int logfd = -1;
__attribute__((constructor)) static void init(void) {
  const char *p = getenv("FSACCESS_LOG");
  if (p) logfd = open(p, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
}
static void logit(const char *call, int rc, int dirfd, const char *path) {
  if (logfd < 0 || !path) return;
  char buf[4096];
  int n;
  if (path[0] != '/' && dirfd != AT_FDCWD && dirfd >= 0) {
    char dir[PATH_MAX];
    if (fcntl(dirfd, F_GETPATH, dir) == -1) dir[0] = 0;
    n = snprintf(buf, sizeof buf, "%s %d %s/%s\n", call, rc, dir, path);
  } else if (path[0] != '/') {
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof cwd)) cwd[0] = 0;
    n = snprintf(buf, sizeof buf, "%s %d %s/%s\n", call, rc, cwd, path);
  } else {
    n = snprintf(buf, sizeof buf, "%s %d %s\n", call, rc, path);
  }
  if (n > 0) write(logfd, buf, n < (int)sizeof buf ? n : (int)sizeof buf - 1);
}

static int my_open(const char *path, int flags, ...) {
  int mode = 0;
  if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = va_arg(ap, int); va_end(ap); }
  int e, rc = open(path, flags, mode); e = errno;
  logit((flags & (O_WRONLY | O_RDWR)) ? "openw" : "open", rc < 0 ? -1 : 0, AT_FDCWD, path);
  errno = e; return rc;
}
static int my_openat(int dirfd, const char *path, int flags, ...) {
  int mode = 0;
  if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = va_arg(ap, int); va_end(ap); }
  int e, rc = openat(dirfd, path, flags, mode); e = errno;
  logit((flags & (O_WRONLY | O_RDWR)) ? "openw" : "open", rc < 0 ? -1 : 0, dirfd, path);
  errno = e; return rc;
}
static int my_stat(const char *path, struct stat *st) {
  int e, rc = stat(path, st); e = errno; logit("stat", rc, AT_FDCWD, path); errno = e; return rc;
}
static int my_lstat(const char *path, struct stat *st) {
  int e, rc = lstat(path, st); e = errno; logit("stat", rc, AT_FDCWD, path); errno = e; return rc;
}
static int my_fstatat(int dirfd, const char *path, struct stat *st, int flag) {
  int e, rc = fstatat(dirfd, path, st, flag); e = errno; logit("stat", rc, dirfd, path); errno = e; return rc;
}
static int my_access(const char *path, int mode) {
  int e, rc = access(path, mode); e = errno; logit("stat", rc, AT_FDCWD, path); errno = e; return rc;
}
static int my_faccessat(int dirfd, const char *path, int mode, int flag) {
  int e, rc = faccessat(dirfd, path, mode, flag); e = errno; logit("stat", rc, dirfd, path); errno = e; return rc;
}
DYLD_INTERPOSE(my_open, open)
DYLD_INTERPOSE(my_openat, openat)
DYLD_INTERPOSE(my_stat, stat)
DYLD_INTERPOSE(my_lstat, lstat)
DYLD_INTERPOSE(my_fstatat, fstatat)
DYLD_INTERPOSE(my_access, access)
DYLD_INTERPOSE(my_faccessat, faccessat)
