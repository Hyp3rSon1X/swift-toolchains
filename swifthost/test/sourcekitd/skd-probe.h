// Debug probe for the in-process sourcekitd inside SwiftHostCompiler: runs a sequence of
// SourceKit requests (editor.open, diagnostics, cursor info, completion, cancellation, ...)
// against real source files and reports one line per step with the time, a result summary and
// the process's memory footprint. Callable from an iOS app (no main); skd-probe-main.c is the
// macOS command-line wrapper.
//
// Report line format (space separated key=value; strings are quoted and never contain a newline):
//   step=<name>#<n> ms=<time> status=ok|error|skipped [check=pass|fail] result="<summary>"
//   fp=<MB> step_peak=<MB> peak=<MB> ledger_peak=<MB> avail=<MB>
// fp: phys_footprint after the step (task_info TASK_VM_INFO); step_peak: highest sampled value
// during the step (a sampler thread reads it every sample_interval_ms); peak: highest sampled value
// since the run started; ledger_peak: the kernel's phys_footprint peak since process start;
// avail: os_proc_available_memory() on iOS, 0 elsewhere.
#ifndef SKD_PROBE_H
#define SKD_PROBE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct skd_probe_config {
  // Directory with the probe-*.yaml request templates (shipped with the app) and, unless
  // compiler_args_file is set, compilerargs.txt.
  const char *yaml_dir;
  // File with the compiler arguments, one per line; ${SDK} and ${MC} are replaced. NULL: yaml_dir/compilerargs.txt.
  const char *compiler_args_file;
  const char *sdk;           // value of ${SDK}: the .../wasm32-unknown-wasip1-threads directory
  const char *module_cache;  // value of ${MC}: writable module cache directory
  // Appended after the arguments of the file (for example "-parse-as-library").
  const char *const *extra_args;
  size_t extra_arg_count;
  // All source files of the module (absolute paths). They go into key.compilerargs ahead of the arguments.
  const char *const *files;
  size_t file_count;
  const char *primary;       // the file opened as editor document; one of files
  const char *complete_file; // file the completion and cancel steps use; NULL: primary
  // Completion point: complete_offset > 0 is a byte offset; otherwise the offset is the start of the first
  // occurrence of complete_pattern in complete_file plus complete_delta (use strlen(pattern) to stand
  // right behind the pattern, for example behind a dot).
  long complete_offset;
  const char *complete_pattern;
  int complete_delta;
  const char *complete_expect; // a result whose key.name starts with this must exist; NULL: only count > 0
  // Cursor info / related identifiers position in primary, resolved like the completion point.
  long cursor_offset;
  const char *cursor_pattern;
  int cursor_delta;
  const char *cursor_expect;   // key.name must start with this and key.usr must be non-empty; NULL: no check
  // Substring one diagnostic of the "diag" step must contain (a deliberately broken file);
  // NULL: the file must have 0 errors.
  const char *diag_expect;
  // Comma separated steps, each optionally "*N" to repeat; NULL: the default sequence
  //   init,config,open,diag,tokens,cursor,related,complete*2,ccopen,ccupdate,ccclose,replace,diag,close,cancel,shutdown
  // Steps: init, config (reads or sets the global configuration), open, diag, tokens, cursor, related,
  // complete (source.request.codecomplete), ccopen, ccupdate, ccclose (session requests), replace
  // (appends a type error to the primary document), close, cancel, shutdown.
  const char *steps;
  // Global configuration the "config" step sends; -1 leaves the value alone.
  int config_reuse_count;     // key.completion_max_astcontext_reuse_count (SourceKit default 100)
  int config_check_interval;  // key.completion_check_dependency_interval (SourceKit default 5)
  int request_limit;          // key.codecomplete.requestlimit of ccopen/ccupdate (default 50)
  const char *update_filter;  // filter text of ccupdate (default "a")
  int enable_syntax_map;      // editor.open/replacetext with key.enablesyntaxmap (default 0)
  int enable_substructure;    // key.enablesubstructure (default 0)
  int cancel_delay_ms;        // wait between sending the async completion and cancelling it (default 0)
  int sample_interval_ms;     // memory sampler period (default 10; 0 disables the sampler)
} skd_probe_config;

// Fills cfg with the defaults named above (all strings NULL, all counts 0).
void skd_probe_config_init(skd_probe_config *cfg);

typedef void (*skd_probe_report_fn)(const char *line, void *ctx);

// Runs the steps in order and calls report once per line (on the calling thread, also for the
// final "summary" line). Steps that need an earlier step (open before cursor ...) are reported as
// skipped when that step failed. Returns 0 when every step succeeded and every check passed, the
// number of failed steps and checks otherwise, -1 when the configuration is unusable.
// The calling thread must be a thread that may block (requests are sent synchronously).
int skd_probe_run(const skd_probe_config *cfg, skd_probe_report_fn report, void *ctx);

#ifdef __cplusplus
}
#endif

#endif
