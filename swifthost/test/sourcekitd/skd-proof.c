// Functional proof of the in-process sourcekitd linked into SwiftHostCompiler:
// editor.open, code completion after `xs.`, semantic diagnostics of a type
// error, editor.close, all against the wasm32-unknown-wasip1-threads SDK.
//
// The requests are the YAML files next to this file. Before parsing, the
// harness replaces ${WORK} (directory the virtual source files live in; they
// are never written), ${SDK} (the SDK's target directory), ${MC} (module
// cache) and ${ARGS:<file>} (the compiler arguments of compilerargs.txt,
// preceded by ${WORK}/<file>, each as a quoted YAML string).
//
// skd_proof_run is callable from an app; main is a command-line wrapper:
//   skd-proof <yaml-dir> <sdk-target-dir> <work-dir> <module-cache-dir>

#include "sourcekitd/sourcekitd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct skd_proof_config {
  const char *dir;  // directory with the YAML files and compilerargs.txt
  const char *sdk;  // .../wasm32-unknown-wasip1-threads
  const char *work; // directory of the virtual source files
  const char *mc;   // module cache path
};

struct buf {
  char *p;
  size_t n, cap;
};

static void buf_add(struct buf *b, const char *s, size_t n) {
  if (b->n + n + 1 > b->cap) {
    b->cap = (b->n + n + 1) * 2;
    b->p = realloc(b->p, b->cap);
  }
  memcpy(b->p + b->n, s, n);
  b->n += n;
  b->p[b->n] = 0;
}

static char *read_file(const char *dir, const char *name) {
  char path[4096];
  snprintf(path, sizeof path, "%s/%s", dir, name);
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  struct buf b = {0};
  char chunk[4096];
  size_t n;
  while ((n = fread(chunk, 1, sizeof chunk, f)) > 0)
    buf_add(&b, chunk, n);
  fclose(f);
  if (!b.p)
    buf_add(&b, "", 0);
  return b.p;
}

static char *expand(const struct skd_proof_config *cfg, const char *in);

// "${WORK}/<file>", "<arg>", ... from compilerargs.txt (one argument per line).
static void add_args(const struct skd_proof_config *cfg, struct buf *out, const char *file, size_t filelen) {
  buf_add(out, "\"", 1);
  buf_add(out, cfg->work, strlen(cfg->work));
  buf_add(out, "/", 1);
  buf_add(out, file, filelen);
  buf_add(out, "\"", 1);
  char *args = read_file(cfg->dir, "compilerargs.txt");
  if (!args)
    return;
  for (char *line = strtok(args, "\n"); line; line = strtok(NULL, "\n")) {
    if (!*line)
      continue;
    char *arg = expand(cfg, line);
    buf_add(out, ", \"", 3);
    buf_add(out, arg, strlen(arg));
    buf_add(out, "\"", 1);
    free(arg);
  }
  free(args);
}

static char *expand(const struct skd_proof_config *cfg, const char *in) {
  struct buf out = {0};
  buf_add(&out, "", 0);
  while (*in) {
    const char *open = strstr(in, "${");
    const char *close = open ? strchr(open, '}') : NULL;
    if (!close) {
      buf_add(&out, in, strlen(in));
      break;
    }
    buf_add(&out, in, (size_t)(open - in));
    const char *name = open + 2;
    size_t len = (size_t)(close - name);
    if (len == 4 && !strncmp(name, "WORK", 4))
      buf_add(&out, cfg->work, strlen(cfg->work));
    else if (len == 3 && !strncmp(name, "SDK", 3))
      buf_add(&out, cfg->sdk, strlen(cfg->sdk));
    else if (len == 2 && !strncmp(name, "MC", 2))
      buf_add(&out, cfg->mc, strlen(cfg->mc));
    else if (len > 5 && !strncmp(name, "ARGS:", 5))
      add_args(cfg, &out, name + 5, len - 5);
    else
      buf_add(&out, open, (size_t)(close - open + 1));
    in = close + 1;
  }
  return out.p;
}

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

// Sends the request in <name>; returns the response (NULL on failure, reported to out).
static sourcekitd_response_t send_yaml(const struct skd_proof_config *cfg, const char *name, double *ms, FILE *out) {
  char *tmpl = read_file(cfg->dir, name);
  if (!tmpl) {
    fprintf(out, "%s: cannot read\n", name);
    return NULL;
  }
  char *yaml = expand(cfg, tmpl);
  free(tmpl);
  char *error = NULL;
  sourcekitd_object_t req = sourcekitd_request_create_from_yaml(yaml, &error);
  free(yaml);
  if (!req) {
    fprintf(out, "%s: invalid request: %s\n", name, error ? error : "?");
    free(error);
    return NULL;
  }
  double t0 = now_ms();
  sourcekitd_response_t resp = sourcekitd_send_request_sync(req);
  *ms = now_ms() - t0;
  sourcekitd_request_release(req);
  if (sourcekitd_response_is_error(resp)) {
    fprintf(out, "%s: error response: %s\n", name, sourcekitd_response_error_get_description(resp));
    sourcekitd_response_dispose(resp);
    return NULL;
  }
  return resp;
}

static const char *get_string(sourcekitd_variant_t dict, const char *key) {
  const char *s = sourcekitd_variant_dictionary_get_string(dict, sourcekitd_uid_get_from_cstr(key));
  return s ? s : "";
}

// Code completion: number of results, and whether `append` is among them.
static int check_completion(sourcekitd_response_t resp, double ms, const char *label, FILE *out) {
  sourcekitd_variant_t results = sourcekitd_variant_dictionary_get_value(
      sourcekitd_response_get_value(resp), sourcekitd_uid_get_from_cstr("key.results"));
  size_t count = sourcekitd_variant_array_get_count(results);
  const char *append = NULL;
  for (size_t i = 0; i < count && !append; i++) {
    sourcekitd_variant_t item = sourcekitd_variant_array_get_value(results, i);
    if (!strncmp(get_string(item, "key.name"), "append(", 7))
      append = get_string(item, "key.description");
  }
  fprintf(out, "codecomplete (%s): %zu results in %.1f ms; append: %s\n", label, count, ms,
          append ? append : "MISSING");
  return count > 0 && append ? 0 : 1;
}

// Diagnostics: prints each one; requires the type error of errors.swift.
static int check_diagnostics(sourcekitd_response_t resp, double ms, FILE *out) {
  sourcekitd_variant_t diags = sourcekitd_variant_dictionary_get_value(
      sourcekitd_response_get_value(resp), sourcekitd_uid_get_from_cstr("key.diagnostics"));
  size_t count = sourcekitd_variant_array_get_count(diags);
  int found = 0;
  fprintf(out, "diagnostics: %zu in %.1f ms\n", count, ms);
  for (size_t i = 0; i < count; i++) {
    sourcekitd_variant_t d = sourcekitd_variant_array_get_value(diags, i);
    sourcekitd_uid_t sev = sourcekitd_variant_dictionary_get_uid(d, sourcekitd_uid_get_from_cstr("key.severity"));
    const char *desc = get_string(d, "key.description");
    fprintf(out, "  %lld:%lld %s: %s\n",
            (long long)sourcekitd_variant_dictionary_get_int64(d, sourcekitd_uid_get_from_cstr("key.line")),
            (long long)sourcekitd_variant_dictionary_get_int64(d, sourcekitd_uid_get_from_cstr("key.column")),
            sev ? sourcekitd_uid_get_string_ptr(sev) : "?", desc);
    if (strstr(desc, "cannot convert value of type 'String' to specified type 'Int'"))
      found = 1;
  }
  return found ? 0 : 1;
}

int skd_proof_run(const struct skd_proof_config *cfg, FILE *out) {
  int failures = 0;
  double ms, t0 = now_ms();
  sourcekitd_initialize();
  fprintf(out, "initialize: %.1f ms\n", now_ms() - t0);

  sourcekitd_response_t resp = send_yaml(cfg, "open.yaml", &ms, out);
  if (resp) {
    fprintf(out, "editor.open: %.1f ms\n", ms);
    sourcekitd_response_dispose(resp);
  } else {
    failures++;
  }

  const char *labels[] = {"cold", "warm"};
  for (int i = 0; i < 2; i++) {
    if ((resp = send_yaml(cfg, "complete.yaml", &ms, out))) {
      failures += check_completion(resp, ms, labels[i], out);
      sourcekitd_response_dispose(resp);
    } else {
      failures++;
    }
  }

  if ((resp = send_yaml(cfg, "diagnostics.yaml", &ms, out))) {
    failures += check_diagnostics(resp, ms, out);
    sourcekitd_response_dispose(resp);
  } else {
    failures++;
  }

  if ((resp = send_yaml(cfg, "close.yaml", &ms, out))) {
    fprintf(out, "editor.close: %.1f ms\n", ms);
    sourcekitd_response_dispose(resp);
  } else {
    failures++;
  }

  t0 = now_ms();
  sourcekitd_shutdown();
  fprintf(out, "shutdown: %.1f ms\n", now_ms() - t0);
  fprintf(out, "%s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
  return failures ? 1 : 0;
}

#ifndef SKD_PROOF_NO_MAIN
int main(int argc, char **argv) {
  if (argc != 5) {
    fprintf(stderr, "usage: %s <yaml-dir> <sdk-target-dir> <work-dir> <module-cache-dir>\n", argv[0]);
    return 2;
  }
  struct skd_proof_config cfg = {argv[1], argv[2], argv[3], argv[4]};
  return skd_proof_run(&cfg, stdout);
}
#endif
