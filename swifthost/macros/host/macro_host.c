// Test host: an in-process plugin server (swift_inproc_plugins_handle_message) that runs the
// combined wasm macro module in WAMR. It answers getCapability and loadPluginLibrary itself and
// forwards expansion messages to the module with the wasm plugin ABI of swift-syntax:
//   _start once, then swift_wasm_macro_v1_pump once per message; each message is a UTF-8 JSON
//   payload preceded by an 8-byte little-endian length, on the module's stdin (host -> module)
//   and stdout (module -> host).
// One WAMR instance per distinct file content (SHA-256); the same instance serves every module
// name the file was loaded under.
#include <CommonCrypto/CommonDigest.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>
#include "wasm_export.h"

#define MAX_INST 8
#define MAX_NAMES 64

typedef struct {
  uint8_t sha[CC_SHA256_DIGEST_LENGTH];
  uint8_t *buf;
  wasm_module_t mod;
  wasm_module_inst_t inst;
  wasm_exec_env_t env;
  wasm_function_inst_t pump;
  int in_w, out_r;
} Inst;

static Inst insts[MAX_INST];
static int ninst;
static struct { char name[128]; int inst; } names[MAX_NAMES];
static int nnames;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static bool rt_ready;
static char *hostcap_msg; // the compiler's getCapability, replayed into every new instance

static long rss_kib(void) {
  struct rusage ru; getrusage(RUSAGE_SELF, &ru);
  return ru.ru_maxrss / 1024; // macOS: bytes; peak RSS of the whole process
}

static char *err_resp(const char *fmt, const char *a, size_t *n) {
  char tmp[1024]; int k = snprintf(tmp, sizeof tmp, fmt, a ? a : "");
  char *r = malloc(k + 1); memcpy(r, tmp, k + 1); *n = k; return r;
}

// Extract the string value of "key":"..." at or after *from; unescapes \/ \\ \". Returns malloc'd.
static char *json_str(const char *m, size_t len, const char *key, const char *from) {
  char pat[64]; snprintf(pat, sizeof pat, "\"%s\":\"", key);
  const char *p = memmem(from ? from : m, len - (from ? (size_t)(from - m) : 0), pat, strlen(pat));
  if (!p) return NULL;
  p += strlen(pat);
  char *out = malloc(len + 1); size_t o = 0;
  for (; p < m + len && *p != '"'; p++) {
    if (*p == '\\' && p + 1 < m + len) p++;
    out[o++] = *p;
  }
  out[o] = 0; return out;
}

typedef struct { int fd; const uint8_t *p; size_t n; } WriteJob;
static void *writer(void *a) {
  WriteJob *j = a; uint64_t le = j->n; // little-endian on arm64/x86_64
  const uint8_t *hdr = (const uint8_t *)&le; size_t off = 0;
  while (off < 8) { ssize_t w = write(j->fd, hdr + off, 8 - off); if (w <= 0) return NULL; off += w; }
  off = 0;
  while (off < j->n) { ssize_t w = write(j->fd, j->p + off, j->n - off); if (w <= 0) return NULL; off += w; }
  return NULL;
}
typedef struct { int fd; uint8_t *buf; size_t n; bool ok; } ReadJob;
static bool rd(int fd, uint8_t *b, size_t n) {
  size_t off = 0; while (off < n) { ssize_t r = read(fd, b + off, n - off); if (r <= 0) return false; off += r; } return true;
}
static void *reader(void *a) {
  ReadJob *j = a; uint64_t le;
  if (!rd(j->fd, (uint8_t *)&le, 8)) return NULL;
  j->n = le; j->buf = malloc(le + 1);
  j->ok = rd(j->fd, j->buf, le); return NULL;
}

static bool forward(Inst *I, const char *msg, size_t len, char **resp, size_t *rlen);

static int load_instance(const char *path, char *err, size_t errsz) {
  FILE *f = fopen(path, "rb");
  if (!f) { snprintf(err, errsz, "cannot open %s", path); return -1; }
  fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
  uint8_t *buf = malloc(sz); fread(buf, 1, sz, f); fclose(f);
  uint8_t sha[CC_SHA256_DIGEST_LENGTH]; CC_SHA256(buf, (CC_LONG)sz, sha);
  for (int i = 0; i < ninst; i++) if (!memcmp(insts[i].sha, sha, sizeof sha)) { free(buf); return i; }
  if (ninst == MAX_INST) { snprintf(err, errsz, "too many instances"); return -1; }
  if (!rt_ready) {
    if (!wasm_runtime_init()) { snprintf(err, errsz, "wasm_runtime_init failed"); return -1; }
    rt_ready = true;
  }
  Inst *I = &insts[ninst]; memset(I, 0, sizeof *I); memcpy(I->sha, sha, sizeof sha); I->buf = buf;
  char e[256] = {0};
  I->mod = wasm_runtime_load(buf, (uint32_t)sz, e, sizeof e);
  if (!I->mod) { snprintf(err, errsz, "load: %s", e); return -1; }
  int inp[2], outp[2];
  if (pipe(inp) || pipe(outp)) { snprintf(err, errsz, "pipe failed"); return -1; }
  I->in_w = inp[1]; I->out_r = outp[0];
  const char *argv[] = {path};
  wasm_runtime_set_wasi_args_ex(I->mod, NULL, 0, NULL, 0, NULL, 0, (char **)argv, 1, inp[0], outp[1], 2);
  long before = rss_kib();
  I->inst = wasm_runtime_instantiate(I->mod, 4u << 20, 0, e, sizeof e);
  if (!I->inst) { snprintf(err, errsz, "instantiate: %s", e); return -1; }
  // _start installs the message handler and returns; the module stays usable for pump calls.
  if (!wasm_application_execute_main(I->inst, 0, NULL)) {
    snprintf(err, errsz, "_start: %s", wasm_runtime_get_exception(I->inst)); return -1;
  }
  I->env = wasm_runtime_create_exec_env(I->inst, 4u << 20);
  I->pump = wasm_runtime_lookup_function(I->inst, "swift_wasm_macro_v1_pump");
  if (!I->env || !I->pump) { snprintf(err, errsz, "export swift_wasm_macro_v1_pump missing"); return -1; }
  if (getenv("MACRO_HOST_STATS"))
    fprintf(stderr, "macro_host: instance %d ready, peak RSS %ld KiB (was %ld KiB)\n", ninst, rss_kib(), before);
  // The module must see the host capability: with it (hasExpandMacroResult) the plugin returns the
  // unified expandMacroResult and formats attached-macro output like the native plugin does.
  // Without it the module falls back to the legacy per-source results and the text differs.
  if (hostcap_msg) {
    char *r = NULL; size_t rl = 0;
    forward(I, hostcap_msg, strlen(hostcap_msg), &r, &rl); free(r);
  }
  return ninst++;
}

static bool forward(Inst *I, const char *msg, size_t len, char **resp, size_t *rlen) {
  WriteJob w = {I->in_w, (const uint8_t *)msg, len}; ReadJob r = {I->out_r, NULL, 0, false};
  pthread_t tw, tr; pthread_create(&tw, NULL, writer, &w); pthread_create(&tr, NULL, reader, &r);
  uint32_t argv[1] = {0};
  bool ok = wasm_runtime_call_wasm(I->env, I->pump, 0, argv);
  if (!ok) { // unblock the helper threads: they die with the pipes on process exit; report now
    *resp = err_resp("macro module trapped: %s", wasm_runtime_get_exception(I->inst), rlen);
    return true;
  }
  pthread_join(tw, NULL); pthread_join(tr, NULL);
  if (!r.ok) { *resp = err_resp("short read from macro module%s", "", rlen); return true; }
  *resp = (char *)r.buf; *rlen = r.n; return false;
}

bool swift_inproc_plugins_handle_message(const char *msg, size_t len, char **resp, size_t *rlen) {
  pthread_mutex_lock(&lock);
  bool had_error = false;
  if (len > 17 && !strncmp(msg, "{\"getCapability\"", 16)) {
    free(hostcap_msg); hostcap_msg = strndup(msg, len);
    // Protocol 8 = swift-syntax 6.4.2 PluginMessage.PROTOCOL_VERSION_NUMBER.
    const char *c = "{\"getCapabilityResult\":{\"capability\":{\"protocolVersion\":8,\"features\":[\"load-plugin-library\"]}}}";
    *resp = strdup(c); *rlen = strlen(c);
  } else if (memmem(msg, len > 40 ? 40 : len, "\"loadPluginLibrary\"", 19)) {
    char *path = json_str(msg, len, "libraryPath", NULL), *mod = json_str(msg, len, "moduleName", NULL);
    char err[512] = {0}; int idx = (path && mod) ? load_instance(path, err, sizeof err) : -1;
    if (idx >= 0 && nnames < MAX_NAMES) {
      snprintf(names[nnames].name, sizeof names[0].name, "%s", mod); names[nnames++].inst = idx;
      const char *ok = "{\"loadPluginLibraryResult\":{\"loaded\":true,\"diagnostics\":[]}}";
      *resp = strdup(ok); *rlen = strlen(ok);
    } else { *resp = err_resp("loadPluginLibrary failed: %s", err, rlen); had_error = true; }
    free(path); free(mod);
  } else {
    char *mod = NULL; const char *m = memmem(msg, len, "\"macro\":", 8);
    if (m) mod = json_str(msg, len, "moduleName", m);
    int idx = -1;
    for (int i = 0; mod && i < nnames; i++) if (!strcmp(names[i].name, mod)) idx = names[i].inst;
    if (idx < 0) { *resp = err_resp("macro host: no wasm plugin loaded for module '%s'", mod, rlen); had_error = true; }
    else {
      if (getenv("MACRO_HOST_STATS")) fprintf(stderr, "macro_host: forward to instance %d for module %s\n", idx, mod);
      had_error = forward(&insts[idx], msg, len, resp, rlen);
    }
    free(mod);
  }
  pthread_mutex_unlock(&lock);
  return had_error;
}
