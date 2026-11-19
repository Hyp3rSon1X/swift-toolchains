// Replays plugin messages (one JSON per line, as dumped with SWIFT_DUMP_PLUGIN_MESSAGING=1 and
// the "->(plugin:0) " prefix stripped) against libmacro_host.dylib and reports RSS per step.
#include <dlfcn.h>
#include <mach/mach.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <stdbool.h>
static long cur_kib(void) {
  mach_task_basic_info_data_t i; mach_msg_type_number_t n = MACH_TASK_BASIC_INFO_COUNT;
  task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&i, &n); return i.resident_size / 1024;
}
static long peak_kib(void) { struct rusage r; getrusage(RUSAGE_SELF, &r); return r.ru_maxrss / 1024; }
int main(int argc, char **argv) {
  void *h = dlopen(argv[1], RTLD_NOW); if (!h) { puts(dlerror()); return 1; }
  bool (*fn)(const char *, size_t, char **, size_t *) = dlsym(h, "swift_inproc_plugins_handle_message");
  FILE *f = fopen(argv[2], "r"); char *line = NULL; size_t cap = 0; ssize_t n; int i = 0;
  printf("start: rss=%ld KiB peak=%ld KiB\n", cur_kib(), peak_kib());
  while ((n = getline(&line, &cap, f)) > 0) {
    if (line[n - 1] == '\n') line[--n] = 0;
    char *r = NULL; size_t rl = 0; bool err = fn(line, n, &r, &rl);
    printf("msg %d (%.20s...): err=%d resp=%zuB rss=%ld KiB peak=%ld KiB\n", i++, line, err, rl, cur_kib(), peak_kib());
    free(r);
  }
}
