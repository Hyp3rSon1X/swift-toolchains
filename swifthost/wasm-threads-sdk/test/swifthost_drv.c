/* Minimal driver for the SwiftHostCompiler macos-arm64 slice, derived from swifthost/test/smoke.c's use of the
   three entry points. Usage: swifthost_drv <frontend|autolink|ld> <argsfile> <stdout-file> <stderr-file>
   argsfile holds one argv element per line (argv[0] included). */
#include "SwiftHostCompiler.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
  if (argc != 5) { fprintf(stderr, "usage: %s mode argsfile out err\n", argv[0]); return 2; }
  FILE *f = fopen(argv[2], "r");
  if (!f) { perror("argsfile"); return 2; }
  const char **av = calloc(4096, sizeof(char *));
  int n = 0; char *line = NULL; size_t cap = 0; ssize_t len;
  while ((len = getline(&line, &cap, f)) >= 0) {
    if (len && line[len - 1] == '\n') line[--len] = 0;
    if (!len) continue;
    av[n++] = strdup(line);
  }
  fclose(f);
  int out = open(argv[3], O_WRONLY | O_CREAT | O_TRUNC, 0644), err = open(argv[4], O_WRONLY | O_CREAT | O_TRUNC, 0644);
  fprintf(stderr, "swifthost_version=%s\n", swifthost_version());
  int rc;
  if (!strcmp(argv[1], "frontend")) rc = swifthost_swift_frontend(n, av, out, err);
  else if (!strcmp(argv[1], "autolink")) rc = swifthost_swift_autolink_extract(n, av, out, err);
  else if (!strcmp(argv[1], "ld")) rc = swifthost_wasm_ld(n, av, out, err);
  else { fprintf(stderr, "bad mode\n"); return 2; }
  fprintf(stderr, "%s rc=%d\n", argv[1], rc);
  return rc;
}
