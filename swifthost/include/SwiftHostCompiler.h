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
 *   (-print-target-info, -help) write to the process's stdout.
 * - -Xllvm options go to LLVM's process-global option parser. -Xllvm -help,
 *   -Xllvm -version and an unknown or malformed -Xllvm option end the call
 *   through LLVM's exit path: the text goes to the process's stdout or
 *   stderr (not to out_fd/err_fd) and the call poisons the library (see
 *   below; it returns 1, or SWIFTHOST_EPOISONED where LLVM would exit with
 *   0). Pass only -Xllvm options that you have tested.
 * - Crash recovery: around each call the library enables LLVM's
 *   CrashRecoveryContext, which installs process-global handlers for SIGABRT,
 *   SIGBUS, SIGFPE, SIGILL, SIGSEGV and SIGTRAP (with SA_ONSTACK), and
 *   disables it before returning, which restores the handlers that were
 *   installed when the call began (a handler installed by someone else during
 *   the call is replaced). For the duration of each call the calling thread
 *   also gets an alternate signal stack of the library's own; the thread's
 *   previous alternate stack setting is restored before the call returns.
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
 * - A crash on any other thread during a call is not recovered. The handler
 *   turns crash recovery off for the rest of that call (a crash or fatal error
 *   in the call after that ends the process, a fatal error through exit()) and
 *   re-raises the signal with raise() to the handlers that were installed when
 *   the call began (or the default action), so they get it without its original
 *   details (no fault address; the signal looks sent by raise()). Code that
 *   handles its own faults through signals, for example a WebAssembly runtime
 *   that relies on guard pages, or a crash reporter that needs the fault
 *   address, must not run on another thread while a call is in progress.
 * - No other signal handlers are installed, no process is ever spawned and,
 *   except in the case above, exit() is never called.
 * - The frontend needs -module-cache-path pointing at a writable directory
 *   (clang modules for the SDK's C headers are built there on first use, on a
 *   helper thread with an 8 MiB stack that the call waits for; all other work
 *   runs on the calling thread).
 * - A running frontend call can be cancelled from another thread with
 *   swifthost_request_cancel(); see there.
 */
#ifndef SWIFTHOSTCOMPILER_H
#define SWIFTHOSTCOMPILER_H

#ifdef __cplusplus
extern "C" {
#endif

/* Returned by every call after an earlier call crashed (or otherwise poisoned
   the library), and by a call that poisoned it where it would have returned
   0 or SWIFTHOST_ECANCELED. */
#define SWIFTHOST_EPOISONED 125

/* Returned by a frontend call that swifthost_request_cancel() stopped. Not an
   error state: later calls work normally. */
#define SWIFTHOST_ECANCELED 124

/* Minimum stack size of the thread that calls into the library. */
#define SWIFTHOST_MIN_STACK_SIZE 16777216u /* 16 MiB; a plain literal so that Swift imports it */

/* The compiler and library version, for example
   "Swift version 6.4.2 (swift-6.4.2-RELEASE), swifthost-6.4.2-1".
   Static storage; never freed. */
const char *swifthost_version(void);

/* argv[0] = "swift-frontend", then frontend arguments (without "-frontend"),
   for example: -c main.swift -target wasm32-unknown-wasip1 ... -o main.o */
int swifthost_swift_frontend(int argc, const char **argv, int out_fd,
                             int err_fd);

/* argv[0] = "swift-autolink-extract", then the object files, -o <file>.
   Writes the linker flags the objects ask for (one per line) to <file>. */
int swifthost_swift_autolink_extract(int argc, const char **argv, int out_fd,
                                     int err_fd);

/* argv[0] = "wasm-ld", then wasm-ld arguments. */
int swifthost_wasm_ld(int argc, const char **argv, int out_fd, int err_fd);

/* Asks the swifthost_swift_frontend call that is running (on another thread) to
   stop early: the type checker stops at its next top-level declaration,
   function body or constraint-solver step, and the pipeline stops after the
   stage it is in (SIL generation, SIL optimization, SIL lowering, IR
   generation; LLVM code generation itself is not interruptible). That call then
   returns SWIFTHOST_ECANCELED, or its normal result if it finished first.
   Ignore the diagnostics and outputs of a cancelled call: a solver step that
   was stopped reports "the compiler is unable to type-check this expression in
   reasonable time". Does nothing when no frontend call is running (a call still
   waiting for its turn is not affected), and never affects a later call.
   Callable from any thread; returns immediately. Autolink and wasm-ld calls are
   not cancellable (they are short and bounded by their input). */
void swifthost_request_cancel(void);

#ifdef __cplusplus
}
#endif

#endif /* SWIFTHOSTCOMPILER_H */
