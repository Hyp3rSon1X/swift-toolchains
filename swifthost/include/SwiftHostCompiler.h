/*
 * SwiftHostCompiler: the Swift 6.4.2 compiler, clang and wasm-ld as an
 * in-process library that compiles Swift to wasm32-unknown-wasip1 against the
 * swift.org Swift SDK for WebAssembly (swift-6.4.2-RELEASE_wasm).
 *
 * Contract
 *
 * - Every entry point takes a full argv: argv[0] is the tool name, the rest
 *   are the tool's arguments exactly as on a command line. The return value is
 *   the tool's exit status (0 means success).
 * - Calls are serialized internally: one call runs at a time per process; a
 *   concurrent call waits. Call from any thread whose stack is at least
 *   SWIFTHOST_MIN_STACK_SIZE bytes (the main thread of an app is too small).
 * - err_fd receives the call's diagnostics, out_fd its standard output (only
 *   wasm-ld writes there, for example for --version). The library never
 *   closes them. A descriptor that cannot be written (-1, closed, disk full,
 *   a pipe without a reader) only loses that output; the result is the same.
 *   (Writing to a pipe without a reader also raises SIGPIPE, which ends the
 *   process unless the app ignores SIGPIPE or set F_SETNOSIGPIPE on it.)
 *   Exception: frontend actions that print to standard output
 *   (-print-target-info, -help) write to the process's stdout
 *   (swifthost_clang rejects the options that would).
 * - -Xllvm options (swifthost_clang: -mllvm) go to LLVM's process-global
 *   option parser. -Xllvm -help,
 *   -Xllvm -version and an unknown or malformed -Xllvm option end the call
 *   through LLVM's exit path: the text goes to the process's stdout or
 *   stderr (not to out_fd/err_fd) and the call poisons the library (see
 *   below; it returns 1, or SWIFTHOST_EPOISONED where LLVM would exit with
 *   0). Pass only -Xllvm options that you have tested.
 * - Crash recovery: around each call (and each work item of the in-process
 *   sourcekitd, see below) the library enables LLVM's CrashRecoveryContext,
 *   which installs process-global handlers for SIGABRT, SIGBUS, SIGFPE,
 *   SIGILL, SIGSEGV and SIGTRAP (with SA_ONSTACK), and disables it when no
 *   call and no such work item runs any more, which restores the handlers
 *   that were installed when the first of them began (a handler installed by
 *   someone else in between is replaced). In builds with SourceKit a small
 *   handler of the library's runs directly in front of LLVM's (it notes
 *   whether the crash is in sourcekitd's work, then calls LLVM's); it is only
 *   ever put over LLVM's own handler, never over one that someone else
 *   installed while crash recovery was enabled, and is removed with LLVM's.
 *   For the duration of each call the calling thread also gets an
 *   alternate signal stack of the library's own; the thread's previous
 *   alternate stack setting is restored before the call returns.
 * - A crash or fatal error inside a call is recovered: on the calling thread
 *   (including a stack overflow that reaches the stack's guard page, which
 *   stacks that pthread allocates have), and on the helper threads the call
 *   waits for (clang module builds, Swift module interface builds), where the
 *   compiler turns it into an error. The call returns 128 + the signal number,
 *   the fatal error's exit status (usually 1), or for a crash on a helper
 *   thread the compiler's error status (usually 1); never 0 or
 *   SWIFTHOST_ECANCELED, which become SWIFTHOST_EPOISONED. From then on every
 *   call returns SWIFTHOST_EPOISONED without doing anything, because the
 *   recovery skipped destructors and left the compiler's global state
 *   undefined. Only a process restart clears this.
 * - Recovery is impossible, and the process ends as it would without the
 *   library, when a helper thread overflows its stack (helper threads have
 *   no alternate signal stack) or the crash handler itself overflows the
 *   alternate stack (256 KiB).
 * - A crash on any other thread while a call or sourcekitd's work runs is
 *   not recovered. The handler turns crash recovery off until the next call
 *   or sourcekitd work item starts (a crash or fatal error in between ends the
 *   process, a fatal error through exit()) and re-raises the signal with
 *   raise() to the handlers that were installed when crash recovery was
 *   enabled (or the default action), so they get it without its original
 *   details (no fault address; the signal looks sent by raise()). Code that
 *   handles its own faults through signals, for example a WebAssembly runtime
 *   that relies on guard pages, or a crash reporter that needs the fault
 *   address, must not run on another thread while a call or sourcekitd's
 *   work is in progress.
 * - No other signal handlers are installed, no process is ever spawned and,
 *   except in the case above, exit() is never called.
 * - The frontend needs -module-cache-path pointing at a writable directory
 *   (clang modules for the SDK's C headers are built there on first use, on a
 *   helper thread with an 8 MiB stack that the call waits for; all other work
 *   runs on the calling thread). swifthost_clang likewise moves deeply
 *   recursive work (deeply nested source) to a helper thread with a fresh
 *   8 MiB stack when about 8 MiB of the calling thread's stack are used.
 *   clang ignores whether a crash there was recovered and carries on, so
 *   such a call returns the compiler's status or, where that would be 0,
 *   SWIFTHOST_EPOISONED; either way the library is poisoned.
 * - A running frontend or clang call can be cancelled from another thread
 *   with swifthost_request_cancel(); see there.
 * - Macros: the compiler sends macro expansion requests as JSON messages to an
 *   in-process plugin server, which is either a dynamic library that exports
 *   swift_inproc_plugins_handle_message (-in-process-plugin-server-path) or
 *   the app's own handler (swifthost_set_plugin_message_handler). Either
 *   way the library checks each reply before the compiler parses it: one
 *   that nests arrays or objects more than 512 levels deep is turned into an
 *   error for that macro. Executable plugins (-load-plugin-executable,
 *   -external-plugin-path, the server part of -load-resolved-plugin) are
 *   rejected with an error diagnostic; they are never launched.
 * - Diagnostics use swift-syntax's style, which prints source excerpts under
 *   each "file:line:column: error: ..." line; -diagnostic-style llvm gives
 *   one-line diagnostics as in swifthost-6.4.2-1.
 * - LLVM fatal errors (report_fatal_error) go to the library's own handler,
 *   which it installs for the whole process at the first call (or at
 *   sourcekitd_initialize, which installs no handler of its own) and keeps
 *   installed: in a frontend or autolink
 *   call they are reported to its err_fd ("fatal error encountered during
 *   compilation"), in a clang call to its err_fd ("fatal error in the C
 *   compiler"), in a wasm-ld call to its err_fd ("fatal error in the
 *   linker"), on a compiler helper thread or any other thread to stderr
 *   ("LLVM ERROR: ...").
 *
 * In-process sourcekitd (builds with SourceKit, which also export the
 * sourcekitd_* C API of sourcekitd.h)
 *
 * - sourcekitd and the compiler calls share the process and may run at the
 *   same time: completion keeps answering while a long build runs. LLVM's
 *   options (-Xllvm, wasm-ld's -mllvm) are process-wide: a call that passes
 *   them changes them for sourcekitd's work too until the next call, which
 *   resets them (sourcekitd never sets them). Likewise -print-stats and
 *   -stats-output-dir turn on LLVM's process-wide statistics, which then also
 *   count sourcekitd's work.
 * - Every swifthost_wasm_ld call, with or without -mllvm, resets all of
 *   LLVM's options to their defaults (cl::ResetAllOptionOccurrences) and then
 *   parses its -mllvm options, while sourcekitd's threads may be reading
 *   options; the library cannot serialize this (sourcekitd reads them without
 *   a lock). Options nobody set are rewritten with the value they already
 *   have: a formal data race, harmless for the bool options found on
 *   sourcekitd's paths (for example -assert-continue, -print-no-uuids).
 *   Options a call did set (-Xllvm, -mllvm) change under sourcekitd's feet,
 *   and a list or string option (the SIL passes and LLVM have some) that a
 *   thread reads while it is reset can be read after it was freed. So do not
 *   pass -Xllvm or -mllvm while sourcekitd is in use, and prefer to link
 *   while no sourcekitd request is in flight.
 * - Macro expansion inside sourcekitd requests uses the same route as
 *   frontend calls: the app's handler (swifthost_set_plugin_message_handler)
 *   or else the server library named by the request's
 *   -in-process-plugin-server-path.
 * - Crash containment: sourcekitd's work runs under crash recovery on every
 *   thread it uses (its request threads, its 8 MiB threads, the main queue
 *   for notifications), each item with an alternate signal stack of its own,
 *   so a stack overflow is recovered as well. A synchronous request
 *   (sourcekitd_send_request_sync) is handled on one of those threads while
 *   the calling thread waits. A crash or LLVM fatal error in that work stops
 *   sourcekitd instead of ending the process: the request it happened in, and
 *   every other request that has not been answered yet, gets an error
 *   response (sourcekitd_response_is_error, kind
 *   SOURCEKITD_ERROR_CONNECTION_INTERRUPTED, description "sourcekitd stopped:
 *   recovered crash (signal N)" or "sourcekitd stopped: recovered fatal error
 *   (<reason>)"). From then on every sourcekitd_send_request and
 *   sourcekitd_send_request_sync gets the same error at once (asynchronous
 *   ones on a global queue) without reaching sourcekitd; sourcekitd_initialize,
 *   sourcekitd_shutdown, sourcekitd_cancel_request,
 *   sourcekitd_request_handle_dispose and sourcekitd_set_notification_handler
 *   do nothing, and queued work is dropped. Threads that were running
 *   sourcekitd's work at that moment continue until they reach their next
 *   work item; one blocked on a lock the crashed thread held stays blocked
 *   (its request has been answered). swifthost_sourcekitd_status() tells
 *   whether sourcekitd stopped. Only a process restart starts sourcekitd
 *   again.
 * - After such a stop, compiler calls keep working on a best-effort basis
 *   only. The crashed thread skipped its destructors and kept whatever it
 *   held at that moment; if that was shared process state (a C++ static
 *   initialization guard, a swift_once, the malloc lock, a lock of the Swift
 *   runtime's caches, a clang module's lock file in the module cache), later
 *   compiler calls may block or fail. Restart the app process at the next
 *   convenient point after swifthost_sourcekitd_status() turns non-zero.
 * - A crash (signal) or LLVM fatal error recovered in sourcekitd's work
 *   never counts as a crash of a frontend call: it is attributed on the
 *   crashing thread, before LLVM counts it. A frontend call that starts or
 *   ends while such a recovery is in progress waits for it (it takes
 *   microseconds; after 5 seconds the call stops waiting and may be
 *   poisoned). Other exits through LLVM's exit path in sourcekitd's work are
 *   not attributed and poison a frontend call that runs meanwhile, as does a
 *   crash that a signal handler of the app's (installed while crash recovery
 *   was enabled) passes straight on to LLVM's handler. A fault that such a
 *   handler deals with itself is not a recovery and affects nothing.
 * - SWIFTHOST_TEST_SOURCEKIT_RECOVERY_DELAY_MS is a test hook of the library's
 *   own tests (read once, at the first frontend call or sourcekitd use). Never
 *   set it in production: it holds every recovered crash in sourcekitd's work
 *   for that many milliseconds before the recovery ends (delaying the stop of
 *   sourcekitd and the error responses), and stalls frontend
 *   calls meanwhile (up to 5 seconds each at their start and end, after which
 *   they may be poisoned).
 * - Not recovered (the process ends as without the library): a crash on a
 *   compiler helper thread that sourcekitd's work starts (clang module
 *   builds and module interface builds recover crashes themselves and turn
 *   them into errors, but a stack overflow there is fatal), a crash in the
 *   request and response object functions (sourcekitd_request_*,
 *   sourcekitd_response_*, sourcekitd_variant_*, sourcekitd_uid_*) and in
 *   the receivers the app passes (response receivers and the notification
 *   handler: they run outside crash recovery, after the work item that
 *   produced the response has ended, or on the calling thread), a fault
 *   inside libdispatch itself, and the crash handler overflowing the
 *   alternate stack (256 KiB). The app's plugin message handler is the
 *   exception: it answers the compiler synchronously from inside
 *   sourcekitd's work, so a crash in it is recovered and stops sourcekitd
 *   (see swifthost_plugin_message_handler). A crash
 *   that a compiler helper thread recovers during a frontend call poisons
 *   the library even when it was sourcekitd's helper thread (the two cannot
 *   be told apart). Calls into the app's receivers are held until the work
 *   item that produced them ends: if that work item crashes later on, a
 *   response it had already finished is dropped and the request gets the
 *   stop error instead. Notifications that were already queued are still
 *   delivered to the notification handler after sourcekitd stopped.
 */
#ifndef SWIFTHOSTCOMPILER_H
#define SWIFTHOSTCOMPILER_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returned by every call after an earlier call crashed (or otherwise poisoned
   the library), and by a call that poisoned it where it would have returned
   0 or SWIFTHOST_ECANCELED. */
#define SWIFTHOST_EPOISONED 125

/* Returned by a frontend or clang call that swifthost_request_cancel()
   stopped. Not an error state: later calls work normally. */
#define SWIFTHOST_ECANCELED 124

/* Returned by swifthost_sourcekitd_status() after an LLVM fatal error (or
   another exit path) stopped sourcekitd; not a signal number. */
#define SWIFTHOST_SOURCEKITD_FATAL_ERROR 256

/* Minimum stack size of the thread that calls into the library. */
#define SWIFTHOST_MIN_STACK_SIZE 16777216u /* 16 MiB; a plain literal so that Swift imports it */

/* The compiler and library version, for example
   "Swift version 6.4.2 (swift-6.4.2-RELEASE), swifthost-6.4.2-2".
   Static storage; never freed. */
const char *swifthost_version(void);

/* argv[0] = "swift-frontend", then frontend arguments (without "-frontend"),
   for example: -c main.swift -target wasm32-unknown-wasip1 ... -o main.o */
int swifthost_swift_frontend(int argc, const char **argv, int out_fd,
                             int err_fd);

/* Compiles one C or C++ source file with the library's clang, for the C and
   C++ targets of packages (C++ targets that offer Swift a C API; Swift's C++
   interoperability, -cxx-interoperability-mode, is not covered). argv[0] =
   "clang" or "clang++" (g++ mode, as --driver-mode=g++ selects it; its stem
   prefixes the driver's diagnostics; names that select another driver mode,
   such as "clang-cl" or "clang-cpp", are rejected), then clang driver
   arguments for exactly one compile job, for example:
     clang -target wasm32-unknown-wasip1-threads --sysroot <SDK>/WASI.sdk
           -resource-dir <SDK>/swift.xctoolchain/usr/lib/swift_static/clang
           -O2 -c x.c -o x.o -MD -MF x.d
   (SwiftPM compiles C++ files with "clang" too, adding -std=c++17 or the
   package's standard), or argv[1] = "-cc1" followed by cc1 arguments (as
   -### prints them). Response files (@file) are expanded first. Same contract
   as swifthost_swift_frontend: serialized with the other calls, crash
   recovery and poisoning, return value 0, 1 (an error, with diagnostics on
   err_fd), SWIFTHOST_ECANCELED, SWIFTHOST_EPOISONED or 128 + signal; out_fd
   is unused. An LLVM fatal error is reported on err_fd as "error: fatal error
   in the C compiler: <reason>" (C++ too).

   Inputs: C sources (.c, -x c), preprocessed C (.i, -x cpp-output), C++
   sources (.cc, .cp, .cpp, .cxx, .c++, .C, .CC, .CPP, .CXX, .C++, -x c++;
   in g++ mode .c files compile as C++, with the driver's deprecation
   warning) and preprocessed C++ (.ii, -x c++-cpp-output) only. Every other input (LLVM IR, assembler,
   Objective-C and Objective-C++, C and C++ headers, header units and
   precompiled headers, C++20 module units (.cppm, .ccm, .cxxm, .c++m,
   -x c++-module), OpenCL, CUDA, HIP, files of unknown type, which the driver
   would link, and "-" for standard input) is rejected before the driver
   runs, and the job's input is checked again afterwards. A .cpp file that
   declares a C++20 module (export module M;) compiles to its object only,
   as with the stand-alone clang; no module file is written. Module imports
   in the source (C++20 import, @import, #pragma clang module import and
   load) and #pragma clang module build (which the stand-alone clang compiles
   into a nested module, written to $TMPDIR) fail with one line, "error:
   swifthost_clang does not support module imports" or "... does not support
   #pragma clang module build", without a source location; clang stops
   reading the file there (as after a module that fails to load), nothing is
   reported after it (no "1 error generated."), and no file is written.

   C++: the driver adds the SDK's libc++ headers from --sysroot
   (WASI.sdk/include/wasm32-wasip1-threads/c++/v1 and
   WASI.sdk/include/c++/v1, the same -internal-isystem set as the
   stand-alone clang++), so the app must ship WASI.sdk/include/c++/v1, and the
   link must add -lc++ -lc++abi (the Swift link line already does). C++
   exceptions are on by default, as with the stand-alone clang, but the SDK's
   libc++abi has no exception runtime: code that throws, or that uses the
   throwing parts of libc++ (std::map::at, std::function's call operator,
   std::vector::at, ...), compiles but does not link (__cxa_throw and
   __cxa_allocate_exception are undefined). Such targets need
   -fno-exceptions, with which libc++ aborts instead of throwing and a
   "throw" in the target's own code is a compile error.

   Required: -target, a WebAssembly target (without it the call compiles for
   wasm32-unknown-wasip1, never for the host; other targets are rejected),
   --sysroot (the SDK's WASI.sdk; without it the C library headers are not
   found) and -resource-dir (clang's own headers; argv[0] is no real path in
   the app, so clang cannot find them next to it, and a call without
   -resource-dir is an error). The threads SDK
   (wasm32-unknown-wasip1-threads) also needs its toolset's C and C++ flags:
   -matomics -mbulk-memory -mthread-model posix -pthread
   -ftls-model=local-exec (and -mtail-call for the tail-call bundle).

   One call runs exactly one compile job: a command that needs more than one
   (two source files with -c, or no -c, which adds a link job) fails with
   "error: swifthost_clang runs exactly one compile job; this command needs N
   jobs". The actions it runs: -c (object), -S, -emit-llvm (with -c or -S),
   -fsyntax-only and -E (with -o). -### writes the version lines and the
   job's command line to err_fd (quoted, as clang prints it) and runs
   nothing. Debug builds may pass -index-store-path (as SwiftPM does). With
   the same compiler sources and flags the objects are byte-identical to the
   stand-alone clang's.

   Rejected with one error line ("error: swifthost_clang does not support
   <option>") and return value 1, before anything runs. These are the known
   options that would break the contract; -Xclang passes any other cc1
   option through unchecked, so pass only cc1 options that you have tested.
   - Every driver or cc1 argument whose value names the process's standard
     input or output or its terminal: "-", /dev/stdin, /dev/stdout,
     /dev/stderr, /dev/tty, /dev/fd/N, paths that resolve into /dev (a
     symbolic link to one of those) and existing devices, pipes and sockets,
     except /dev/null, which stays allowed (-o -, -o /dev/stdout, -MF /dev/fd/1, -MJ -,
     --serialize-diagnostics /dev/stderr, -foptimization-record-file=-, an
     input "-" or /dev/stdin, ...), and -E without -o.
   - Options that print to the process's stdout or stderr instead of err_fd:
     -v, --version, -help, --help, --help-hidden, -print-* and --print-*
     (including -print-targets, -print-resource-dir, -print-search-dirs,
     -print-file-name=, -print-prog-name=, --print-supported-cpus),
     --autocomplete=, -dumpmachine, -dumpversion, -ccc-print-phases,
     -ccc-print-bindings, -H without a header include file, /showIncludes,
     -ftime-report, and the cc1 options -print-stats, -help, -version,
     -code-completion-at=, -fdump-record-layouts (and its -simple,
     -canonical, -complete forms), -fdump-vtable-layouts and
     -fdebug-pass-manager.
   - Options that run more than the compile job (-save-temps,
     -save-temps=), that load code (-fplugin=, -fpass-plugin=, -Xclang
     -load, -plugin, -add-plugin, and their cc1 forms), and GPU offloading,
     whose driver runs programs to find the GPUs (--offload-arch=,
     --no-offload-arch=, --offload*, -foffload*, --cuda-*, -fcuda*, --hip-*,
     -fhip*, --rocm-*, -fgpu-*, --gpu-*, -fopenmp, -fopenmp= and the other
     -fopenmp* options except -fopenmp-simd and -fno-openmp-simd (the SDK
     has no OpenMP runtime either), -fsycl*, -Xopenmp-target, -Xoffload-linker, -Xcuda-*,
     -Xarch_device).
   - Options that change the language or the driver (-ObjC, -ObjC++, and
     --driver-mode= other than gcc and g++: cl, cpp, flang, dxc, cache; the
     last --driver-mode= counts, as for the driver), configuration files
     (--config, --config-system-dir=, --config-user-dir=; default
     configuration files are never read) and compilation databases (-MJ).
   - Clang modules and C++20 modules, which build modules implicitly (on
     helper threads, into a module cache) or read or write module files, for
     C and C++ alike: -fmodules, -fcxx-modules, every -fmodule* and
     -fmodules-* option (-fmodule-file=, -fmodule-output, -fmodule-output=,
     -fmodule-map-file=, -fmodule-name=, -fmodule-header, -fmodule-header=,
     -fmodules-cache-path=, -fmodules-reduced-bmi, ...),
     -fimplicit-module-maps, -fprebuilt-module-path=,
     -fprebuilt-implicit-modules, -fbuiltin-module-map, --precompile and
     -fexperimental-modules-reduced-bmi; at cc1 level -fmodules,
     -fcxx-modules, -fmodule-file=, -fprebuilt-module-path=,
     -fmodule-output=, -fmodule-map-file=, -fimplicit-module-maps and
     -fmodules-cache-path= (-fno-modules and -fno-implicit-modules are
     fine).
   - Precompiled headers: -include-pch, an -include whose .pch or .gch file
     exists next to the header (the driver turns it into -include-pch), and
     at cc1 level -include-pch and -chain-include.
   - -fno-temp-file (it writes the object in place, so a failed or cancelled
     call would delete an older one), process-wide profiling (-ftime-trace),
     the compilation cache (-fcache-compile-job) and the frontend actions
     that wrap the compile (-fixit-recompile, -emit-symbol-graph,
     -ast-merge). Other cc1 actions (-ast-dump, -ast-print, -emit-pch,
     -emit-module-interface, -emit-header-unit, ...) fail with an error.
   -mllvm options go to LLVM's process-global option parser as -Xllvm does
   for the frontend (the next call resets them); an unknown one fails the
   call with the parser's message on err_fd, but -mllvm -help and -mllvm
   -version end it through LLVM's exit path (process stdout, poisoned).

   One known exception writes to the process's stderr: the debugging
   pragmas "#pragma clang __debug" dump, macro, module_map, module_lookup,
   modules and diag_mapping (clang prints them with llvm::errs() or
   dump()), unless the call returns before it reaches them. ("#pragma clang
   __debug crash" and llvm_fatal_error end the call as other crashes and LLVM
   fatal errors do.)

   Diagnostics go to err_fd: the driver's ("clang: error: ..."), the
   compiler's ("file:line:column: error: ...", then "N errors generated."),
   all without colors unless -fcolor-diagnostics or
   -fdiagnostics-color=always is given. A failed call writes no object (an
   older file at the -o path stays as it was); like the stand-alone clang it
   still writes the -MD dependency file when the source was read.

   Cancellation (swifthost_request_cancel()): the call checks at every
   top-level declaration while it parses (the parse and semantic analysis of
   one declaration and its LLVM IR generation run to the end, as does the IR
   generation at the end of the file), before every optimization pass, in
   code generation at its next function, pass or block, and once more when
   the action ends. Measured (CPU time from the request to the return,
   freeing included, two runs): medians of about 10-40 ms while parsing and
   in code generation and 100-170 ms in optimization, up to about 0.5 s
   when one optimization pass works on one function of 20,000 statements.
   In C++ the call also checks whenever semantic analysis finishes a class
   template instantiation, requests or finishes a function template
   instantiation, defines an inline function or uses a vtable, and a cancel
   there stops all further template instantiation: inside one declaration,
   and the instantiations that are pending at the end of the file
   (implicit instantiations of function templates and of the members of
   class templates, which in C++ are often most of the work). What still
   runs to the end after a cancel: the instantiation in progress (one
   function or class), a constant evaluation in progress (bounded by
   -fconstexpr-steps), then the end of the file without further
   instantiation; the IR generation at the end of the file, which in C++
   emits all inline and template functions (CodeGenModule::EmitDeferred;
   clang has no hook in it); and, as in C, one running optimization pass.
   Measured in C++ (10,000 small functions; 250 std::map specializations
   instantiated at the end of the file; one function of 20,000 statements;
   -O0 and -O2; 96 cancels): medians of 2-17 ms while parsing, 14-18 ms
   during template instantiation (1.7-1.9 s, up to 2.8 s, without the C++
   checks), 36-100 ms during the IR generation at the end of the file (up
   to 0.53 s), 71 ms in optimization (up to 0.22 s) and 26-37 ms in code
   generation (up to 0.12 s).
   -E is not cancellable and runs to the end. A cancelled call reports
     error: compilation cancelled
   once, without a source location, and nothing after it (no "1 error
   generated."), and returns SWIFTHOST_ECANCELED. It writes no object (an
   older file at the -o path stays as it was), but the -MD dependency file is
   written (rewritten if it exists), listing the files read before the cancel;
   with -index-store-path the index store gets a unit for the object and
   records for the headers read. Ignore every output of a cancelled call. */
int swifthost_clang(int argc, const char **argv, int out_fd, int err_fd);

/* argv[0] = "swift-autolink-extract", then the object files, -o <file>.
   Writes the linker flags the objects ask for (one per line) to <file>. */
int swifthost_swift_autolink_extract(int argc, const char **argv, int out_fd,
                                     int err_fd);

/* argv[0] = "wasm-ld", then wasm-ld arguments. */
int swifthost_wasm_ld(int argc, const char **argv, int out_fd, int err_fd);

/* Asks the swifthost_swift_frontend or swifthost_clang call that is running
   (on another thread) to stop early. For swifthost_clang see there; the
   rest of this comment is about frontend calls. The call checks often: the
   type checker before its next
   statement, declaration, function body or constraint-solver step; SIL
   generation before its next statement or declaration; the SIL optimizer
   before its next pass and inside the passes and analyses that take long on
   large functions; IR generation before its next function; LLVM before its
   next optimization pass, and LLVM code generation (instruction selection,
   register allocation, the WebAssembly passes and the writing of the object)
   at its next block, instruction, function or pass, so that it, too, typically stops
   within about 0.1 s of CPU time (up to about 0.5 s measured on a function of
   4,000 statements). What runs to the end once started: one expression's
   constraint generation, module loading, one SIL pass on one giant function, a
   single running LLVM optimization pass, and CodeGenPrepare. Before it
   returns, the call frees what it built: the sendable checking of one function
   of 4,000 statements holds about 300 MB, and a cancel there returns within
   about 75 ms of CPU time. That call then returns
   SWIFTHOST_ECANCELED, or its normal result if it finished first. A call that
   returns SWIFTHOST_ECANCELED has written no object or IR file (an object
   already at the output path is left as it was; with -num-threads, the object
   and IR files written before the cancel are removed); other outputs written
   before the cancel, such as the module or SIL, may exist. Its only diagnostic
   of its own is the error "compilation cancelled" (diagnostic ID
   compilation_cancelled), reported once and without a source location, so
   that its line reads
     <unknown>:0: error: compilation cancelled
   Nothing is reported after it; diagnostics reported before the cancel (about
   the code) stay. Ignore every output of a cancelled call. Does nothing when
   no frontend or clang call is running (a call still waiting for its turn is not
   affected), and never affects a later call. Callable from any thread; returns
   immediately. Autolink and wasm-ld calls are not cancellable (they are short
   and bounded by their input). */
void swifthost_request_cancel(void);

/* 0 while the in-process sourcekitd works, otherwise what stopped it: the
   signal number of the recovered crash (for example 11 for SIGSEGV, 10 for
   SIGBUS on a stack overflow) or SWIFTHOST_SOURCEKITD_FATAL_ERROR. Always 0 in
   a build without SourceKit. Callable from any thread; returns immediately. */
int swifthost_sourcekitd_status(void);

/* Serves one compiler plugin message; the same contract as the
   swift_inproc_plugins_handle_message entry point of an in-process plugin
   server library. message/length: the request (JSON, not NUL-terminated,
   valid only during the call). Store a buffer allocated with malloc() in
   *response and its size in *response_length (exactly the reply's size);
   the compiler releases it with free(). Return false with the reply message,
   or true with an error message in the buffer: the compiler then fails the
   macro with "failed to send request to plugin" and does not show the
   message. Called during a swifthost_swift_frontend call, usually on the
   calling thread, but also on the compiler's 8 MiB helper thread when the
   call rebuilds a module from its .swiftinterface (that build gets the
   call's plugin options), and on sourcekitd's threads for macros in
   sourcekitd requests, possibly at the same time as for a frontend call. A
   crash in the handler is recovered like a compiler crash on the calling
   thread (it poisons the library, or stops sourcekitd on its threads) and
   ends the process on the helper thread. It must not call into this
   library. */
typedef bool (*swifthost_plugin_message_handler)(const char *message,
                                                 size_t length,
                                                 char **response,
                                                 size_t *response_length);

/* Makes the compiler pass every plugin message to handler instead of loading
   an in-process plugin server library, in frontend calls and in sourcekitd
   requests. Call it once, before the first swifthost_swift_frontend call and
   before the first sourcekitd request that expands a macro: a later call, a
   second call or a NULL handler does nothing. The handler is process-wide and stays set.
   -in-process-plugin-server-path is optional with a handler: a path given
   there is never opened and need not exist (without it the compiler makes
   up a default path, also never opened). Macro modules are named with
   -load-resolved-plugin <library>##<Module>: the handler receives <library>
   (an existing file; the compiler resolves symbolic links) in the
   loadPluginLibrary message. A malformed reply (not JSON, truncated, empty,
   the wrong message kind, nested more than 512 levels deep, positions
   outside the source) fails that macro with an error diagnostic (return
   value 1); the library stays usable. */
void swifthost_set_plugin_message_handler(
    swifthost_plugin_message_handler handler);

#ifdef __cplusplus
}
#endif

#endif /* SWIFTHOSTCOMPILER_H */
