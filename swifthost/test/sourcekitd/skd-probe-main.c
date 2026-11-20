// macOS command-line wrapper around skd_probe_run (see skd-probe.h).
//
//   skd-probe --yaml-dir DIR --sdk SDKDIR --mc CACHEDIR (--module-dir DIR | --file F ...) [--primary F] [options]
//
// --file F            a source file of the module (repeatable); --module-dir D adds every D/*.swift
// --primary F         the editor document (default: the first --file, or D/main.swift)
// --complete-file F   file of the completion/cancel steps (default: primary)
// --complete-after S  completion point right behind the first S in that file; --complete-at S [--complete-delta N]
//                     puts it at the start of S plus N; --complete-offset N is an explicit byte offset
// --complete-expect P a result named P... must exist     --cursor-at S / --cursor-after S / --cursor-offset N
// --cursor-expect P   key.name must start with P         --diag-expect S   a diagnostic must contain S
// --steps LIST        see skd-probe.h                    --extra-arg A (repeatable)
// --reuse N --interval N   global configuration of the config step
// --limit N --filter S --syntaxmap --substructure --cancel-delay MS --sample-ms MS
#include "skd-probe.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_line(const char *line, void *ctx) {
  (void)ctx;
  puts(line);
  fflush(stdout);
}

static int ends_with(const char *s, const char *suffix) {
  size_t a = strlen(s), b = strlen(suffix);
  return a >= b && !strcmp(s + a - b, suffix);
}

static int cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

int main(int argc, char **argv) {
  skd_probe_config cfg;
  skd_probe_config_init(&cfg);
  const char *files[4096], *extra[64];
  size_t nfiles = 0, nextra = 0;
  const char *module_dir = NULL;
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    const char *v = i + 1 < argc ? argv[i + 1] : NULL;
#define OPT(name) (!strcmp(a, name) && v && (i++, 1))
    if (OPT("--yaml-dir")) cfg.yaml_dir = v;
    else if (OPT("--args-file")) cfg.compiler_args_file = v;
    else if (OPT("--sdk")) cfg.sdk = v;
    else if (OPT("--mc")) cfg.module_cache = v;
    else if (OPT("--module-dir")) module_dir = v;
    else if (OPT("--file")) files[nfiles++] = v;
    else if (OPT("--primary")) cfg.primary = v;
    else if (OPT("--complete-file")) cfg.complete_file = v;
    else if (OPT("--complete-after")) { cfg.complete_pattern = v; cfg.complete_delta = (int)strlen(v); }
    else if (OPT("--complete-at")) cfg.complete_pattern = v;
    else if (OPT("--complete-delta")) cfg.complete_delta = atoi(v);
    else if (OPT("--complete-offset")) cfg.complete_offset = atol(v);
    else if (OPT("--complete-expect")) cfg.complete_expect = v;
    else if (OPT("--cursor-after")) { cfg.cursor_pattern = v; cfg.cursor_delta = (int)strlen(v); }
    else if (OPT("--cursor-at")) cfg.cursor_pattern = v;
    else if (OPT("--cursor-delta")) cfg.cursor_delta = atoi(v);
    else if (OPT("--cursor-offset")) cfg.cursor_offset = atol(v);
    else if (OPT("--cursor-expect")) cfg.cursor_expect = v;
    else if (OPT("--diag-expect")) cfg.diag_expect = v;
    else if (OPT("--steps")) cfg.steps = v;
    else if (OPT("--extra-arg")) extra[nextra++] = v;
    else if (OPT("--reuse")) cfg.config_reuse_count = atoi(v);
    else if (OPT("--interval")) cfg.config_check_interval = atoi(v);
    else if (OPT("--limit")) cfg.request_limit = atoi(v);
    else if (OPT("--filter")) cfg.update_filter = v;
    else if (OPT("--cancel-delay")) cfg.cancel_delay_ms = atoi(v);
    else if (OPT("--sample-ms")) cfg.sample_interval_ms = atoi(v);
    else if (!strcmp(a, "--syntaxmap")) cfg.enable_syntax_map = 1;
    else if (!strcmp(a, "--substructure")) cfg.enable_substructure = 1;
    else {
      fprintf(stderr, "unknown or incomplete option %s (see the header of skd-probe-main.c)\n", a);
      return 2;
    }
  }
  if (module_dir) {
    DIR *d = opendir(module_dir);
    if (!d) {
      perror(module_dir);
      return 2;
    }
    size_t first = nfiles;
    struct dirent *e;
    while ((e = readdir(d)) && nfiles < 4096) {
      if (!ends_with(e->d_name, ".swift"))
        continue;
      char *path;
      if (asprintf(&path, "%s/%s", module_dir, e->d_name) < 0)
        return 2;
      files[nfiles++] = path;
    }
    closedir(d);
    qsort(files + first, nfiles - first, sizeof(char *), cmp);
    if (!cfg.primary)
      for (size_t i = first; i < nfiles; i++)
        if (ends_with(files[i], "/main.swift"))
          cfg.primary = files[i];
  }
  if (!cfg.primary && nfiles)
    cfg.primary = files[0];
  cfg.files = files;
  cfg.file_count = nfiles;
  cfg.extra_args = extra;
  cfg.extra_arg_count = nextra;
  if (!cfg.yaml_dir) {
    fprintf(stderr, "--yaml-dir is required\n");
    return 2;
  }
  return skd_probe_run(&cfg, print_line, NULL) == 0 ? 0 : 1;
}
