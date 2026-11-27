// SwiftHostCompiler: C entry points that run swift-frontend, clang (one C
// compile job per call), swift-autolink-extract and wasm-ld in-process and, in
// a build with SourceKit
// (SWIFTHOST_SOURCEKIT), the policy that lets the in-process sourcekitd share
// the process with them. See SwiftHostCompiler.h for the contract.
#include "SwiftHostCompiler.h"

#include "swift/AST/ASTContext.h"
#include "swift/AST/PluginRegistry.h"
#include "swift/Basic/Compiler.h"
#include "swift/Basic/InitializeSwiftModules.h"
#include "swift/Basic/LLVMInitialize.h"
#include "swift/Basic/Version.h"
#include "swift/Frontend/Frontend.h"
#include "swift/FrontendTool/FrontendTool.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/DiagnosticOptions.h"
#include "clang/Basic/Stack.h"
#include "clang/CodeGen/CodeGenAction.h"
#include "clang/CodeGen/ObjectFilePCHContainerWriter.h"
#include "clang/Driver/Compilation.h"
#include "clang/Driver/Driver.h"
#include "clang/Driver/Job.h"
#include "clang/Driver/Options.h"
#include "clang/Driver/Tool.h"
#include "clang/Driver/ToolChain.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/CompilerInvocation.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Frontend/MultiplexConsumer.h"
#include "clang/Frontend/TextDiagnosticBuffer.h"
#include "clang/Frontend/TextDiagnosticPrinter.h"
#include "clang/Index/IndexingAction.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Lex/PreprocessorOptions.h"
#include "clang/Rewrite/Frontend/FrontendActions.h"
#include "clang/Serialization/ObjectFilePCHContainerReader.h"
#include "lld/Common/Driver.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/IR/CompilationCancellation.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/PassInstrumentation.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/CrashRecoveryContext.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/StringSaver.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "llvm/Support/raw_ostream.h"

#if SWIFTHOST_SOURCEKIT
#include "SourceKit/Support/Concurrency.h"
#include "sourcekitd/Internal.h"
#include "sourcekitd/sourcekitdInProc-Internal.h"
#include <dispatch/dispatch.h>
#include <unordered_set>
#include <vector>
#endif

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <dlfcn.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

LLD_HAS_DRIVER(wasm)

// lib/DriverTool/autolink_extract_main.cpp
int autolink_extract_main(llvm::ArrayRef<const char *> Args, const char *Argv0,
                          void *MainAddr, llvm::raw_ostream &diagOS);

namespace {

std::mutex gLock;
bool gPoisoned = false;

// Whether a call since the last reset may have set LLVM's process-wide
// options (-Xllvm, or wasm-ld's -mllvm). Guarded by gLock.
bool gLLVMOptionsSet = false;

// Crashes recovered in sourcekitd's work, so that a frontend call does not
// count them as its own (readRecoveryCounts). Each is counted twice: on the
// crashing thread before LLVM counts the recovery (bookSourceKitRecovery), and
// once the work item's RunSafely has returned (runSourceKitProtected). While
// the counts differ, a recovery is between the two.
std::atomic<unsigned> gSourceKitRecoveriesBegun{0};
std::atomic<unsigned> gSourceKitRecoveriesEnded{0};

// The crash recovery context of the sourcekitd work item that runs on this
// thread, and whether its crash has been counted.
thread_local llvm::CrashRecoveryContext *tSourceKitRecovery = nullptr;
thread_local bool tSourceKitRecoveryBooked = false;

// Test hook only (SWIFTHOST_TEST_SOURCEKIT_RECOVERY_DELAY_MS, read once at
// setup; the tests link the release object, so it stays in): a work item
// whose crash was recovered waits this long before it counts the recovery as
// ended, as if its thread were preempted right there. Frontend calls wait for
// it meanwhile (up to kRecoveryWaitLimit each, then they may be poisoned).
std::atomic<unsigned> gTestRecoveryDelayMs{0};

// Called on the crashing thread, from the signal handler or the fatal error
// router, before LLVM counts the recovery: counts a crash that the work item's
// own crash recovery context is about to recover as sourcekitd's. Returns
// whether this call counted it. Async-signal-safe.
bool bookSourceKitRecovery() {
  llvm::CrashRecoveryContext *crc = tSourceKitRecovery;
  if (!crc || tSourceKitRecoveryBooked ||
      llvm::CrashRecoveryContext::GetCurrent() != crc)
    return false;
  tSourceKitRecoveryBooked = true;
  gSourceKitRecoveriesBegun.fetch_add(1, std::memory_order_seq_cst);
  // Orders this count before LLVM's (relaxed) count of the same recovery.
  std::atomic_thread_fence(std::memory_order_seq_cst);
  return true;
}

#if SWIFTHOST_SOURCEKIT
// Counts the booked recovery of this thread as ended, if there is one: once
// the work item's RunSafely has returned, or when the crash it was booked for
// turned out not to be recovered after all. Async-signal-safe.
void endSourceKitRecovery() {
  if (!tSourceKitRecoveryBooked)
    return;
  tSourceKitRecoveryBooked = false;
  gSourceKitRecoveriesEnded.fetch_add(1, std::memory_order_seq_cst);
}
#endif

// What the calling thread is doing, for the fatal error router: a frontend
// or autolink call (Compiler), a clang call (CCompiler) or a wasm-ld call
// (Linker), with the call's err_fd, or sourcekitd's work (SourceKit).
enum class ThreadRole : unsigned char {
  None,
  Compiler,
  CCompiler,
  Linker,
  SourceKit
};
thread_local ThreadRole tRole = ThreadRole::None;
thread_local int tErrFd = -1;
// The reason of a fatal error in sourcekitd's work on this thread.
thread_local char tFatalReason[256];

class ThreadRoleScope {
public:
  ThreadRoleScope(ThreadRole role, int errFd)
      : savedRole(tRole), savedErrFd(tErrFd) {
    tRole = role;
    tErrFd = errFd;
  }
  ~ThreadRoleScope() {
    tRole = savedRole;
    tErrFd = savedErrFd;
  }

private:
  ThreadRole savedRole;
  int savedErrFd;
};

// Cancellation: each frontend or clang call gets a fresh flag, published here
// while it runs, so a request can never leak into a later call.
std::mutex gCancelLock;
std::shared_ptr<std::atomic<bool>> gCancelFlag;

// Macro plugin messages. Every reply passes through handlePluginMessage,
// which bounds its nesting depth: swift-syntax's JSON scanner recurses once
// per array or object level (about 160 bytes of stack each), so a reply of
// some 100,000 '[' would overflow the calling thread's stack, and a helper
// thread's (module interface builds) is not recoverable at all.
using MessageHandler = swift::InProcessPlugins::HandleMessageFunction;

// The app's handler. It can be set until the first frontend call starts or
// the first plugin message is routed.
std::mutex gPluginHandlerLock;
bool gPluginHandlerFrozen = false;
std::atomic<MessageHandler> gAppHandler{nullptr};

constexpr size_t kMaxReplyDepth = 512;

void freezePluginHandler() {
  std::lock_guard<std::mutex> lock(gPluginHandlerLock);
  gPluginHandlerFrozen = true;
}

// Whether the arrays and objects of a JSON text nest deeper than limit
// (brackets inside strings do not count).
bool nestsDeeperThan(const char *text, size_t length, size_t limit) {
  size_t depth = 0;
  bool inString = false;
  for (size_t i = 0; i < length; ++i) {
    char c = text[i];
    if (inString) {
      if (c == '\\')
        ++i;
      else if (c == '"')
        inString = false;
    } else if (c == '"') {
      inString = true;
    } else if (c == '[' || c == '{') {
      if (++depth > limit)
        return true;
    } else if ((c == ']' || c == '}') && depth > 0) {
      --depth;
    }
  }
  return false;
}

bool handlerError(char **response, size_t *responseLength, const char *message) {
  size_t n = std::strlen(message);
  char *buffer = static_cast<char *>(std::malloc(n));
  if (buffer)
    std::memcpy(buffer, message, n);
  *response = buffer;
  *responseLength = buffer ? n : 0;
  return true;
}

// The entry point of the plugin server library at path, or null if it cannot
// be loaded. Libraries stay loaded, as they do when the compiler loads them.
MessageHandler loadPluginServer(const char *path) {
  static std::mutex lock;
  static std::map<std::string, MessageHandler> loaded;
  std::lock_guard<std::mutex> guard(lock);
  auto known = loaded.find(path);
  if (known != loaded.end())
    return known->second;
  void *library = dlopen(path, RTLD_LAZY | RTLD_LOCAL);
  if (!library)
    return nullptr;
  auto handler = reinterpret_cast<MessageHandler>(
      dlsym(library, "swift_inproc_plugins_handle_message"));
  if (handler)
    loaded.emplace(path, handler);
  return handler;
}

// The compiler's handler for every in-process plugin message, of frontend
// calls and of sourcekitd alike: registered once, it routes each message by
// the server path of the compilation that sends it, so no compilation changes
// another one's route. Messages go to the app's handler or else to the server
// library at that path.
bool handlePluginMessage(const char *serverPath, const char *message,
                         size_t length, char **response,
                         size_t *responseLength) {
  freezePluginHandler();
  MessageHandler handler = gAppHandler.load(std::memory_order_acquire);
  if (!handler && serverPath)
    handler = loadPluginServer(serverPath);
  if (!handler)
    return handlerError(response, responseLength,
                        "cannot load the in-process plugin server");
  char *reply = nullptr;
  size_t replyLength = 0;
  bool hadError = handler(message, length, &reply, &replyLength);
  if (!hadError && reply && nestsDeeperThan(reply, replyLength, kMaxReplyDepth)) {
    std::free(reply);
    return handlerError(response, responseLength,
                        "plugin reply nests arrays or objects too deeply");
  }
  *response = reply;
  *responseLength = replyLength;
  return hadError;
}

// Hands the call's flag to the compiler (ASTContext::CancellationFlag). The
// type checker, SIL generation, the SIL pass manager, IR generation and (via
// IRGenOptions, which the frontend fills in from the ASTContext) LLVM check it
// at their statement, function and pass boundaries, and the frontend returns
// an error once it is set. Also keeps the frontend from replacing the
// process-wide fatal error handler (the router below), which sourcekitd's
// threads need while a frontend call runs.
class CancellationObserver : public swift::FrontendObserver {
public:
  explicit CancellationObserver(std::shared_ptr<std::atomic<bool>> flag)
      : flag(std::move(flag)) {}
  bool shouldInstallFatalErrorHandler() override { return false; }
  void configuredCompiler(swift::CompilerInstance &instance) override {
    instance.getASTContext().CancellationFlag = flag;
  }

private:
  std::shared_ptr<std::atomic<bool>> flag;
};

void writeAll(int fd, const char *data, size_t size) {
  while (size > 0) {
    ssize_t n = ::write(fd, data, size);
    if (n <= 0)
      return;
    data += n;
    size -= (size_t)n;
  }
}

void writeText(int fd, const char *text) { writeAll(fd, text, std::strlen(text)); }

// LLVM's one fatal error handler for the whole process (report_fatal_error),
// installed by the library for good: the frontend calls do not install their
// own (CancellationObserver), nor does sourcekitd_initialize
// (shouldInstallFatalErrorHandler). It reports each fatal error where the thread's work belongs:
// a frontend, clang or linker call's err_fd, or sourcekitd's crash containment.
void routeFatalError(void *, const char *reason, bool) {
  switch (tRole) {
  case ThreadRole::Compiler:
    // The frontend's own text. LLVM then removes the files registered for
    // removal and exits through sys::Process::Exit (or abort()), which the
    // call's CrashRecoveryContext turns into a return.
    writeText(tErrFd, "<unknown>:0: error: fatal error encountered during "
                      "compilation; " SWIFT_BUG_REPORT_MESSAGE "\n"
                      "<unknown>:0: note: ");
    writeText(tErrFd, reason);
    writeText(tErrFd, "\n");
    return;
  case ThreadRole::CCompiler:
    writeText(tErrFd, "error: fatal error in the C compiler: ");
    writeText(tErrFd, reason);
    writeText(tErrFd, "\n");
    return;
  case ThreadRole::Linker:
    writeText(tErrFd, "error: fatal error in the linker: ");
    writeText(tErrFd, reason);
    writeText(tErrFd, "\n");
    return;
  case ThreadRole::SourceKit:
    std::snprintf(tFatalReason, sizeof(tFatalReason), "%s", reason);
    break;
  case ThreadRole::None:
    // Compiler helper threads (clang module builds, module interface builds)
    // of either side, or a thread of the app: LLVM's own text.
    writeText(2, "LLVM ERROR: ");
    writeText(2, reason);
    writeText(2, "\n");
    break;
  }
  // Return to the thread's crash recovery right away: LLVM would first remove
  // every file registered for removal in the process, including the output
  // files of a frontend call that runs at the same time.
  if (llvm::CrashRecoveryContext *crc = llvm::CrashRecoveryContext::GetCurrent()) {
    bookSourceKitRecovery();
    crc->HandleExit(1);
  }
}

void installFatalErrorRouter() {
  llvm::remove_fatal_error_handler();
  llvm::install_fatal_error_handler(routeFatalError, nullptr);
}

// One-time setup of the process-wide state that frontend calls and sourcekitd
// share, done by whichever comes first.
void ensureProcessSetup() {
  static std::once_flag once;
  std::call_once(once, [] {
    installFatalErrorRouter();
    swift::InProcessPlugins::setHandleMessageOverride(handlePluginMessage);
    initializeSwiftModules();
    // LLVM's targets, before any wasm-ld call: wasm-ld registers them at
    // every link without a guard, which can link a target into the registry
    // twice when it is not registered yet, and only rewrites its entries with
    // the same values once it is.
    swift::initializeLLVMOnce();
    if (const char *delay = getenv("SWIFTHOST_TEST_SOURCEKIT_RECOVERY_DELAY_MS"))
      gTestRecoveryDelayMs.store((unsigned)strtoul(delay, nullptr, 10));
  });
}

// CrashRecoveryContext::Enable and Disable are process-wide and not counted;
// frontend calls and sourcekitd's work items overlap, so the handlers are
// installed while at least one of them runs.
std::mutex gRecoveryLock;
unsigned gRecoveryUsers = 0;

#if SWIFTHOST_SOURCEKIT
// The signals crash recovery handles (CrashRecoveryContext.cpp).
constexpr int kRecoverySignals[] = {SIGABRT, SIGBUS,  SIGFPE,
                                    SIGILL,  SIGSEGV, SIGTRAP};
constexpr size_t kNumRecoverySignals =
    sizeof(kRecoverySignals) / sizeof(kRecoverySignals[0]);

// LLVM's crash recovery signal handler (a file-static function there, the
// same for every signal and for the life of the process), learned from the
// first Enable that installs it (learnRecoveryHandler); null until then.
std::atomic<void (*)(int)> gLLVMRecoveryHandler{nullptr};

// Runs directly in front of LLVM's crash recovery signal handler, so that a
// crash in sourcekitd's work is counted as sourcekitd's before LLVM counts
// it. It is only ever installed over LLVM's handler and only forwards to it.
// LLVM's handler does not return for a crash on a thread with a crash
// recovery context, which booking requires; should it return all the same,
// no recovery took place and the booking is ended right here, so a booking
// never stays open.
void bookingSignalHandler(int signal) {
  bool booked = bookSourceKitRecovery();
  if (void (*handler)(int) = gLLVMRecoveryHandler.load()) {
    handler(signal);
    if (booked)
      endSourceKitRecovery();
    return;
  }
  // Not reached: the handler is only installed once LLVM's is known.
  if (booked)
    endSourceKitRecovery();
  ::signal(signal, SIG_DFL);
  raise(signal);
}

void readRecoveryHandlers(void (*handlers[kNumRecoverySignals])(int),
                          int flags[kNumRecoverySignals]) {
  for (size_t i = 0; i < kNumRecoverySignals; ++i) {
    struct sigaction current;
    if (sigaction(kRecoverySignals[i], nullptr, &current) != 0) {
      handlers[i] = nullptr;
      flags[i] = 0;
      continue;
    }
    handlers[i] = current.sa_handler;
    flags[i] = current.sa_flags;
  }
}

// Learns LLVM's handler from an Enable call: before (the handlers read just
// before it) differs from the handlers right after it only when that Enable
// installed LLVM's handlers, one function for all six signals with
// SA_ONSTACK and without SA_SIGINFO. Anything else (crash recovery already
// enabled, or another handler installed in between) teaches nothing; until
// a later Enable does, the booking handler is not installed and a crash in
// sourcekitd's work is not attributed (a frontend call running meanwhile
// is poisoned). Under gRecoveryLock.
void learnRecoveryHandler(void (*const before[kNumRecoverySignals])(int)) {
  if (gLLVMRecoveryHandler.load())
    return;
  void (*after[kNumRecoverySignals])(int);
  int flags[kNumRecoverySignals];
  readRecoveryHandlers(after, flags);
  void (*candidate)(int) = after[0];
  if (!candidate || candidate == SIG_DFL || candidate == SIG_IGN)
    return;
  for (size_t i = 0; i < kNumRecoverySignals; ++i)
    if (after[i] != candidate || after[i] == before[i] ||
        (flags[i] & SA_SIGINFO) || !(flags[i] & SA_ONSTACK))
      return;
  gLLVMRecoveryHandler.store(candidate);
}

// Puts bookingSignalHandler directly in front of LLVM's crash recovery
// handler, for each signal whose current handler is LLVM's. A handler the app
// installed over LLVM's (or over the booking handler) while crash recovery
// was enabled is left alone: the booking handler forwards only to LLVM's, and
// a handler of the app's that deals with a fault itself never passes through
// it. Under gRecoveryLock.
void installRecoveryBooking() {
  void (*llvmHandler)(int) = gLLVMRecoveryHandler.load();
  if (!llvmHandler)
    return;
  for (size_t i = 0; i < kNumRecoverySignals; ++i) {
    struct sigaction current;
    if (sigaction(kRecoverySignals[i], nullptr, &current) != 0 ||
        (current.sa_flags & SA_SIGINFO) || current.sa_handler != llvmHandler)
      continue;
    struct sigaction booking = current;
    booking.sa_handler = bookingSignalHandler;
    sigaction(kRecoverySignals[i], &booking, nullptr);
  }
}
#endif

class CrashRecoveryUse {
public:
  CrashRecoveryUse() {
    std::lock_guard<std::mutex> lock(gRecoveryLock);
    ++gRecoveryUsers;
    // Also when already counted: a crash on a thread without a crash recovery
    // context disables crash recovery behind the count's back.
#if SWIFTHOST_SOURCEKIT
    void (*before[kNumRecoverySignals])(int) = {};
    int beforeFlags[kNumRecoverySignals];
    bool learning = !gLLVMRecoveryHandler.load();
    if (learning)
      readRecoveryHandlers(before, beforeFlags);
    llvm::CrashRecoveryContext::Enable();
    if (learning)
      learnRecoveryHandler(before);
    // Disabling crash recovery (when the last user ends) restores the
    // handlers from before it was enabled, which removes this one as well.
    installRecoveryBooking();
#else
    llvm::CrashRecoveryContext::Enable();
#endif
  }
  ~CrashRecoveryUse() {
    std::lock_guard<std::mutex> lock(gRecoveryLock);
    if (--gRecoveryUsers == 0)
      llvm::CrashRecoveryContext::Disable();
  }
};

// Whether a call with these arguments may set LLVM's process-wide options.
bool setsLLVMOptions(int argc, const char **argv) {
  for (int i = 1; i < argc; ++i)
    if (argv[i] && (!std::strncmp(argv[i], "-Xllvm", 6) ||
                    !std::strncmp(argv[i], "-mllvm", 6) ||
                    !std::strncmp(argv[i], "--mllvm", 7)))
      return true;
  return false;
}

// A stream on one of the caller's file descriptors. raw_fd_ostream reports a
// write error it still holds when it is destroyed through report_fatal_error,
// which exits the process: these streams are destroyed outside crash
// recovery, and an invalid, closed or full descriptor (or a pipe whose reader
// is gone) must only lose the output.
class FdStream : public llvm::raw_fd_ostream {
public:
  FdStream(int fd, bool unbuffered)
      : llvm::raw_fd_ostream(fd, /*shouldClose=*/false, unbuffered) {}
  ~FdStream() override {
    flush();
    clear_error();
  }
};

// Gives the calling thread an alternate signal stack for the duration of a
// call: the crash recovery handlers run on it (they are installed with
// SA_ONSTACK), so a stack overflow on the calling thread is recovered like
// any other crash. The thread's previous setting is restored afterwards.
class AlternateSignalStack {
public:
  AlternateSignalStack() {
    size_t page = (size_t)getpagesize();
    void *map = mmap(nullptr, page + kSize, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANON, -1, 0);
    if (map == MAP_FAILED)
      return;
    // A guard page, so that a handler overflowing this stack faults instead
    // of writing past it.
    mprotect(map, page, PROT_NONE);
    stack_t stack = {};
    stack.ss_sp = (char *)map + page;
    stack.ss_size = kSize;
    // Fails while the thread runs on its alternate stack (a call from a
    // signal handler); the call then runs without one.
    if (sigaltstack(&stack, &previous) != 0) {
      munmap(map, page + kSize);
      return;
    }
    mapping = map;
    mappingSize = page + kSize;
  }
  ~AlternateSignalStack() {
    if (!mapping)
      return;
    stack_t restore = previous;
    restore.ss_flags &= SS_DISABLE;
    // Darwin rejects a size below MINSIGSTKSZ even to disable the stack
    // (ENOMEM), which would leave this one installed.
    if ((restore.ss_flags & SS_DISABLE) && restore.ss_size < MINSIGSTKSZ)
      restore.ss_size = MINSIGSTKSZ;
    // If the kernel still considers the thread to be on this stack, keep the
    // memory rather than unmap a stack that may still be used.
    if (sigaltstack(&restore, nullptr) == 0)
      munmap(mapping, mappingSize);
  }

private:
  static constexpr size_t kSize = 256 * 1024;
  void *mapping = nullptr;
  size_t mappingSize = 0;
  stack_t previous = {};
};

// LLVM's count of the crashes recovered in the process, and how many of them
// were sourcekitd's.
struct RecoveryCounts {
  unsigned all;
  unsigned sourceKit;
};

// How long readRecoveryCounts waits for a recovery in sourcekitd's work to
// complete. It takes microseconds (the thread returns from its signal handler
// or fatal error to its RunSafely); the limit only matters for a thread that
// is stopped in between.
constexpr auto kRecoveryWaitLimit = std::chrono::seconds(5);

// Reads the counts at a moment when no recovery in sourcekitd's work is
// between its two counts, so that each is in both counts or in neither: every
// crash LLVM counted on such a thread is then in sourceKit as well. Another
// recovery after that point, or one that is not sourcekitd's, appears in all
// only.
RecoveryCounts readRecoveryCounts(bool atEnd) {
  auto limit = std::chrono::steady_clock::now() + kRecoveryWaitLimit;
  for (;;) {
    unsigned begun = gSourceKitRecoveriesBegun.load();
    unsigned ended = gSourceKitRecoveriesEnded.load();
    unsigned all = llvm::CrashRecoveryContext::getNumRecoveredCrashes();
    std::atomic_thread_fence(std::memory_order_seq_cst);
    unsigned begunAgain = gSourceKitRecoveriesBegun.load();
    if (begun == ended && begun == begunAgain)
      return {all, begun};
    if (std::chrono::steady_clock::now() >= limit) {
      // Leave recoveries in progress out of sourcekitd's share of the call's
      // window, which may poison the call but never hides one of its own.
      return {all, atEnd ? ended : begunAgain};
    }
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }
}

// Runs body under the global lock, with crash recovery enabled for the
// duration of the call. body sets poison when the compiler state can no
// longer be trusted.
template <typename Body>
int runGuarded(ThreadRole role, int argc, const char **argv, int errFd,
               Body body) {
  std::lock_guard<std::mutex> lock(gLock);
  if (gPoisoned) {
    static const char msg[] = "error: the compiler crashed earlier in this "
                              "process; restart the app to compile again\n";
    writeAll(errFd, msg, sizeof(msg) - 1);
    return SWIFTHOST_EPOISONED;
  }
  ensureProcessSetup();
  // Undo the LLVM options an earlier call set. Only then: the reset writes
  // every option, which sourcekitd's threads may be reading.
  if (gLLVMOptionsSet) {
    llvm::cl::ResetAllOptionOccurrences();
    gLLVMOptionsSet = false;
  }
  if (setsLLVMOptions(argc, argv))
    gLLVMOptionsSet = true;

  ThreadRoleScope roleScope(role, errFd);
  AlternateSignalStack altStack;
  RecoveryCounts before = readRecoveryCounts(/*atEnd=*/false);
  const void *stackState = llvm::SavePrettyStackState();
  bool poison = false;
  int rc;
  {
    CrashRecoveryUse recovery;
    rc = body(poison);
  }
  // Crash recovery contexts inside the compiler (clang module builds and Swift
  // module interface builds, on helper threads) turn a crash into an ordinary
  // error, but it skipped destructors like the call's own recovery. Calls are
  // serialized; a recovery that is not sourcekitd's own was this call's (or
  // one in a helper thread of sourcekitd's, which cannot be told apart: it
  // poisons too).
  RecoveryCounts after = readRecoveryCounts(/*atEnd=*/true);
  if ((int)(after.all - before.all) > (int)(after.sourceKit - before.sourceKit))
    poison = true;
  if (poison) {
    // The recovery skipped the destructors of the pretty stack trace entries
    // on this thread; drop them.
    llvm::RestorePrettyStackState(stackState);
    gPoisoned = true;
    // A call that poisons never reports success or a mere cancellation (for
    // example -Xllvm -version, which ends in LLVM's exit path with status 0).
    if (rc == 0 || rc == SWIFTHOST_ECANCELED)
      rc = SWIFTHOST_EPOISONED;
  }
  return rc;
}

// Runs fn inside a CrashRecoveryContext. A signal returns 128 + signal; a
// fatal error (sys::Process::Exit) returns its exit status. Both skip
// destructors, so both poison.
template <typename Fn> int runRecovering(Fn fn, bool &poison) {
  int rc = 0;
  llvm::CrashRecoveryContext crc;
  if (!crc.RunSafely([&] { rc = fn(); })) {
    rc = crc.RetCode;
    poison = true;
  }
  return rc;
}

// In-process clang (swifthost_clang): clang's driver, used only to turn a
// command line into its compile job, and that job run in-process the way
// clang's cc1_main and ExecuteCompilerInvocation run it, without their
// process-level parts (fatal error handler, stack rlimit, timers, the
// compilation cache, deliberately leaked memory).

// The target of a command line without -target: never the host.
constexpr char kClangDefaultTriple[] = "wasm32-unknown-wasip1";

void writeClangError(llvm::raw_ostream &os, const llvm::Twine &message) {
  os << "error: " << message << "\n";
  os.flush();
}

// The inputs swifthost_clang compiles: C and C++ sources and their
// preprocessed forms, as the driver classifies them and as cc1 sees them.
// Headers, header units and C++20 module units (TY_CXXModule) stay out: they
// produce module files rather than one object.
bool isSupportedDriverInput(clang::driver::types::ID type) {
  namespace types = clang::driver::types;
  return type == types::TY_C || type == types::TY_PP_C ||
         type == types::TY_CXX || type == types::TY_PP_CXX;
}

bool isSupportedFrontendInput(const clang::InputKind &kind) {
  return (kind.getLanguage() == clang::Language::C ||
          kind.getLanguage() == clang::Language::CXX) &&
         kind.getFormat() == clang::InputKind::Source && !kind.isHeader() &&
         !kind.isHeaderUnit();
}

// What the input messages list as supported.
constexpr char kSupportedInputs[] =
    "only C and C++ sources (.c, .cc, .cp, .cpp, .cxx, .c++, .C, .CC, .CPP, "
    ".CXX, .C++, -x c, -x c++) and preprocessed C and C++ (.i, .ii, "
    "-x cpp-output, -x c++-cpp-output)";

// Whether a path names one of the process's standard streams or its
// terminal: "-", the device paths /dev/stdin, /dev/stdout, /dev/stderr,
// /dev/tty and /dev/fd/N (also with "." and ".." in them), a path that
// resolves into /dev (a symbolic link to one of those), or an existing
// device, pipe or socket (what the streams usually are). /dev/null stays
// allowed.
bool namesStandardStream(llvm::StringRef value) {
  if (value == "-")
    return true;
  llvm::SmallString<128> path(value);
  if (llvm::sys::path::is_absolute(path))
    llvm::sys::path::remove_dots(path, /*remove_dot_dot=*/true);
  llvm::StringRef normalized = path.str();
  if (normalized == "/dev/stdin" || normalized == "/dev/stdout" ||
      normalized == "/dev/stderr" || normalized == "/dev/tty" ||
      normalized.starts_with("/dev/fd/"))
    return true;
  llvm::SmallString<128> real;
  if (!llvm::sys::fs::real_path(value, real) &&
      llvm::StringRef(real).starts_with("/dev/") && real != "/dev/null")
    return true;
  llvm::sys::fs::file_status status, null;
  if (llvm::sys::fs::status(value, status))
    return false;
  switch (status.type()) {
  case llvm::sys::fs::file_type::character_file:
    return llvm::sys::fs::status("/dev/null", null) ||
           status.getUniqueID() != null.getUniqueID();
  case llvm::sys::fs::file_type::block_file:
  case llvm::sys::fs::file_type::fifo_file:
  case llvm::sys::fs::file_type::socket_file:
    return true;
  default:
    return false;
  }
}

// Whether an argument's value names the process's standard input or output.
bool usesStandardStream(const llvm::opt::Arg &A) {
  for (const char *value : A.getValues())
    if (value && namesStandardStream(value))
      return true;
  return false;
}

// The driver option, if A is one swifthost_clang rejects: options that make
// the driver print to the process's stdout or stderr (everything
// Driver::HandleImmediateArgs handles except -###, and -ccc-print-*), that
// need more than the one compile job (-save-temps), that load code into the
// process (plugins), that offload to GPUs (the driver runs amdgpu-arch or
// nvptx-arch to find the GPUs, and the SDK has no OpenMP runtime), that write
// a compilation database (-MJ), that change the language (Objective-C;
// --driver-mode= is checked with argv[0] in rejectedDriverCommand), clang
// modules and C++20 modules (implicit module builds, module maps found by
// search, module files read or written: -fmodules, -fmodule-*,
// -fimplicit-module-maps, -fprebuilt-module-path=, --precompile, ...; but not
// -fmodule-map-file=, see below), and configuration files, whose options
// would bypass this check (-fopenmp-simd stays allowed). Also every
// argument whose value names the process's standard input or output
// (namesStandardStream). Returns the option as written, or an empty string.
std::string rejectedDriverOption(const llvm::opt::Arg &A,
                                 const llvm::opt::ArgList &args) {
  namespace opts = clang::driver::options;
  static const unsigned rejected[] = {
      opts::OPT_v,
      opts::OPT__version,
      opts::OPT_help,
      opts::OPT__help_hidden,
      opts::OPT_dumpmachine,
      opts::OPT_dumpversion,
      opts::OPT_autocomplete,
      opts::OPT_ccc_print_phases,
      opts::OPT_ccc_print_bindings,
      opts::OPT_save_temps,
      opts::OPT_save_temps_EQ,
      opts::OPT_fplugin_EQ,
      opts::OPT_fpass_plugin_EQ,
      opts::OPT_config,
      opts::OPT_config_system_dir_EQ,
      opts::OPT_config_user_dir_EQ,
      opts::OPT_MJ,
      opts::OPT_ObjC,
      opts::OPT_ObjCXX,
  };
  // Option names (without their leading dashes) by prefix: -print-* and
  // --print-* print instead of compiling; then the offloading options (CUDA,
  // HIP, OpenMP, SYCL); then the module options ("fmodule" covers -fmodules,
  // -fmodules-*, -fmodule-file=, -fmodule-output, -fmodule-header and
  // -fmodule-name=; "precompile" is --precompile). -fmodule-map-file= is
  // allowed (SwiftPM passes one for every C target a target depends on): with
  // modules off clang only parses the map and the maps its extern module
  // lines name, trusted input like headers (FrontendAction loads them into
  // HeaderSearch); an #include a map turns into a module import (C++20 and
  // later) reaches SwiftHostCompilerInstance::loadModule and is refused there.
  static const char *const rejectedPrefixes[] = {
      "print-",         "offload",           "no-offload",
      "foffload",       "cuda-",             "fcuda",
      "hip-",           "fhip",              "rocm-",
      "fgpu-",          "gpu-",              "fopenmp",
      "fsycl",          "Xopenmp-target",    "Xoffload",
      "Xcuda",          "Xarch_device",      "fmodule",
      "fcxx-modules",   "fimplicit-module",  "fprebuilt-",
      "fbuiltin-module-map",                 "precompile",
      "fexperimental-modules-reduced-bmi",
  };
  const llvm::opt::Option &option = A.getOption();
  if (usesStandardStream(A))
    return A.getAsString(args);
  for (unsigned id : rejected)
    if (option.matches(id))
      return A.getAsString(args);
  // OpenMP's SIMD directives need neither the OpenMP runtime nor offloading.
  bool openMPSimd = option.matches(opts::OPT_fopenmp_simd) ||
                    option.matches(opts::OPT_fno_openmp_simd);
  bool moduleMapFile = option.matches(opts::OPT_fmodule_map_file);
  if (!openMPSimd && !moduleMapFile &&
      option.getKind() != llvm::opt::Option::InputClass &&
      option.getKind() != llvm::opt::Option::UnknownClass)
    for (const char *prefix : rejectedPrefixes)
      if (option.getName().starts_with(prefix))
        return A.getAsString(args);
  // The cc1 options that load plugins, passed through -Xclang.
  if (option.matches(opts::OPT_Xclang)) {
    llvm::StringRef value = A.getValue();
    if (value.starts_with("-load") || value.starts_with("-plugin") ||
        value.starts_with("-add-plugin") || value.starts_with("-fplugin") ||
        value.starts_with("-fpass-plugin"))
      return A.getAsString(args);
  }
  return std::string();
}

// What makes a driver command line unsupported before the driver runs: a
// driver mode other than gcc (the default) and g++ (argv[0] "clang++", or
// --driver-mode=g++), found as the driver finds it (the last --driver-mode=
// argument, else argv[0]): cl, cpp, flang, dxc and cache change how the
// arguments are read or what runs. Also a target other than WebAssembly (the
// GPU toolchains run programs to find the GPUs, and the library only has the
// WebAssembly backend), and inputs that are not C or C++ (by -x or by their
// extension; an unknown extension is a linker input; in g++ mode the driver
// compiles .c files as C++). Returns the message, or an empty string.
std::string rejectedDriverCommand(llvm::ArrayRef<const char *> argv,
                                  const llvm::opt::InputArgList &args) {
  namespace opts = clang::driver::options;
  namespace types = clang::driver::types;
  llvm::StringRef mode = clang::driver::getDriverMode(argv[0], argv.slice(1));
  if (!mode.empty() && mode != "gcc" && mode != "g++") {
    if (clang::driver::getDriverMode("clang", argv.slice(1)).empty())
      return std::string("swifthost_clang does not support the driver mode of "
                         "argv[0] \"") +
             argv[0] + "\" (use \"clang\" or \"clang++\")";
    return "swifthost_clang does not support --driver-mode=" + mode.str() +
           " (only gcc and g++)";
  }
  clang::driver::ParsedClangName name =
      clang::driver::ToolChain::getTargetAndModeFromProgramName(argv[0]);
  std::string triple = kClangDefaultTriple;
  if (name.TargetIsValid)
    triple = name.TargetPrefix;
  if (const llvm::opt::Arg *target = args.getLastArg(opts::OPT_target))
    triple = target->getValue();
  if (!llvm::Triple(triple).isWasm())
    return "swifthost_clang compiles only for WebAssembly, not for " + triple;
  types::ID language = types::TY_INVALID;
  for (const llvm::opt::Arg *A : args) {
    if (A->getOption().matches(opts::OPT_x)) {
      llvm::StringRef value = A->getValue();
      language = value == "none" ? types::TY_INVALID
                                 : types::lookupTypeForTypeSpecifier(A->getValue());
      if (value != "none" && !isSupportedDriverInput(language))
        return "swifthost_clang does not support -x " + value.str() +
               " (only -x c, -x cpp-output, -x c++, -x c++-cpp-output)";
      continue;
    }
    if (A->getOption().getKind() != llvm::opt::Option::InputClass)
      continue;
    llvm::StringRef input = A->getValue();
    types::ID type = language;
    if (type == types::TY_INVALID) {
      llvm::StringRef extension = llvm::sys::path::extension(input);
      type = extension.empty() ? types::TY_INVALID
                               : types::lookupTypeForExtension(extension.drop_front());
    }
    if (!isSupportedDriverInput(type))
      return "swifthost_clang does not support the input " + input.str() +
             " (" + kSupportedInputs + ")";
  }
  return std::string();
}

// The frontend option, if the invocation uses one that swifthost_clang
// rejects: options that print to the process's stdout or stderr, that load
// code, that use process-wide state clang cannot share (the time trace
// profiler, timers, the compilation cache), that would delete an older output
// of a failed or cancelled call (-fno-temp-file writes the output in place),
// and the actions that wrap the compile in further work. The cc1 command
// lines of the driver mode pass through here too. These are the known ones;
// other cc1 options given through -Xclang pass unchecked.
const char *rejectedFrontendOption(clang::CompilerInvocation &invocation) {
  const clang::FrontendOptions &frontend = invocation.getFrontendOpts();
  const clang::DependencyOutputOptions &dependencies =
      invocation.getDependencyOutputOpts();
  if (frontend.ShowHelp)
    return "-help";
  if (frontend.ShowVersion)
    return "-version";
  if (!frontend.Plugins.empty())
    return "-load";
  if (!frontend.AddPluginActions.empty() ||
      frontend.ProgramAction == clang::frontend::PluginAction)
    return "-plugin/-add-plugin";
  if (!invocation.getCodeGenOpts().PassPlugins.empty())
    return "-fpass-plugin=";
  if (frontend.PrintSupportedCPUs)
    return "--print-supported-cpus";
  if (frontend.PrintSupportedExtensions)
    return "--print-supported-extensions";
  if (frontend.PrintEnabledExtensions)
    return "--print-enabled-extensions";
  if (frontend.ShowStats)
    return "-print-stats";
  if (invocation.getHeaderSearchOpts().Verbose)
    return "-v";
  if (!frontend.TimeTracePath.empty())
    return "-ftime-trace";
  if (invocation.getCodeGenOpts().TimePasses)
    return "-ftime-report";
  if (frontend.CacheCompileJob)
    return "-fcache-compile-job";
  if (!frontend.UseTemporary)
    return "-fno-temp-file";
  if (!frontend.CodeCompletionAt.FileName.empty())
    return "-code-completion-at=";
  // Module files written or read, and module maps found by search (which,
  // with modules on, let #include build modules implicitly). Module maps named
  // by -fmodule-map-file= are only parsed (see rejectedDriverOption).
  const clang::HeaderSearchOptions &headerSearch =
      invocation.getHeaderSearchOpts();
  if (!frontend.ModuleOutputPath.empty())
    return "-fmodule-output";
  if (!frontend.ModuleFiles.empty() ||
      !headerSearch.PrebuiltModuleFiles.empty() ||
      !headerSearch.PrebuiltModulePaths.empty())
    return "-fmodule-file= (or -fprebuilt-module-path=)";
  if (headerSearch.ImplicitModuleMaps)
    return "-fimplicit-module-maps";
  if (!headerSearch.ModuleCachePath.empty())
    return "-fmodules-cache-path=";
  // Precompiled headers. The driver also turns "-include x.h" into
  // -include-pch when x.h.pch or x.h.gch exists next to it.
  const clang::PreprocessorOptions &preprocessor =
      invocation.getPreprocessorOpts();
  if (!preprocessor.ImplicitPCHInclude.empty())
    return "precompiled headers (-include-pch, or an -include whose .pch or "
           ".gch file exists)";
  if (!preprocessor.ChainedIncludes.empty())
    return "-chain-include";
  const clang::LangOptions &lang = invocation.getLangOpts();
  if (lang.DumpRecordLayouts || lang.DumpRecordLayoutsSimple ||
      lang.DumpRecordLayoutsCanonical || lang.DumpRecordLayoutsComplete)
    return "-fdump-record-layouts";
  if (lang.DumpVTableLayouts)
    return "-fdump-vtable-layouts";
  if (invocation.getCodeGenOpts().DebugPassManager)
    return "-fdebug-pass-manager";
  if (frontend.FixAndRecompile)
    return "-fixit-recompile";
  if (frontend.EmitSymbolGraph)
    return "-emit-symbol-graph";
  if (!frontend.ASTMergeFiles.empty())
    return "-ast-merge";
  if (dependencies.ShowHeaderIncludes &&
      (dependencies.HeaderIncludeOutputFile.empty() ||
       dependencies.HeaderIncludeOutputFile == "-"))
    return "-H (without a header include output file)";
  if (dependencies.ShowIncludesDest != clang::ShowIncludesDestination::None)
    return "/showIncludes";
  if (dependencies.OutputFile == "-")
    return "-MF - (dependencies to standard output)";
  // -E without -o reaches cc1 as -o -.
  if (frontend.OutputFile == "-" ||
      (frontend.OutputFile.empty() &&
       frontend.ProgramAction == clang::frontend::PrintPreprocessedInput))
    return "-o - (output to standard output)";
  return nullptr;
}

// The action of an invocation, for the actions swifthost_clang runs; null for
// any other. Code generation uses the call's context.
std::unique_ptr<clang::FrontendAction>
createClangAction(clang::CompilerInstance &instance,
                  llvm::LLVMContext *context) {
  namespace fe = clang::frontend;
  switch (instance.getFrontendOpts().ProgramAction) {
  case fe::EmitObj:
    return std::make_unique<clang::EmitObjAction>(context);
  case fe::EmitAssembly:
    return std::make_unique<clang::EmitAssemblyAction>(context);
  case fe::EmitBC:
    return std::make_unique<clang::EmitBCAction>(context);
  case fe::EmitLLVM:
    return std::make_unique<clang::EmitLLVMAction>(context);
  case fe::EmitLLVMOnly:
    return std::make_unique<clang::EmitLLVMOnlyAction>(context);
  case fe::ParseSyntaxOnly:
    return std::make_unique<clang::SyntaxOnlyAction>();
  case fe::PrintPreprocessedInput:
    if (instance.getPreprocessorOutputOpts().RewriteIncludes ||
        instance.getPreprocessorOutputOpts().RewriteImports)
      return std::make_unique<clang::RewriteIncludesAction>();
    return std::make_unique<clang::PrintPreprocessedAction>();
  default:
    return nullptr;
  }
}

// Puts the diagnostics engine in its state after a fatal error, without
// printing one (the line printed stays the error reported before it). Sema
// then instantiates no further template (Sema::InstantiatingTemplate is
// invalid once a fatal error and an uncompilable error occurred), so the
// pending instantiations at the end of the translation unit
// (PerformPendingInstantiations), and those that one declaration triggers,
// end within one instantiation each. The engine records a fatal error only
// once the diagnostic after it arrives (so that its notes still print), or
// when told that diagnostic was ignored: the fatal one goes to a consumer
// that drops it, and setLastDiagnosticIgnored completes the record.
void enterFatalState(clang::DiagnosticsEngine &diags, unsigned fatalID) {
  std::unique_ptr<clang::DiagnosticConsumer> owner = diags.takeClient();
  clang::DiagnosticConsumer *client = diags.getClient();
  clang::IgnoringDiagConsumer ignoring;
  diags.setClient(&ignoring, /*ShouldOwnClient=*/false);
  diags.Report(fatalID);
  if (owner)
    diags.setClient(owner.release(), /*ShouldOwnClient=*/true);
  else
    diags.setClient(client, /*ShouldOwnClient=*/false);
  diags.setLastDiagnosticIgnored(true);
}

// The cancellation of one clang call: its flag, checked at the three layers
// (parse/Sema, LLVM optimization, LLVM code generation), and the error that
// reports it, once and without a source location. The error makes clang
// discard the call's output files when the action ends (EndSourceFile).
class ClangCancellation {
public:
  ClangCancellation(const std::atomic<bool> &flag,
                    clang::CompilerInstance &instance)
      : flag(flag), instance(instance),
        diagID(instance.getDiagnostics().getCustomDiagID(
            clang::DiagnosticsEngine::Error, "compilation cancelled")),
        fatalID(instance.getDiagnostics().getCustomDiagID(
            clang::DiagnosticsEngine::Fatal, "compilation cancelled")) {}

  // Whether the call is cancelled; reports it the first time.
  bool check() {
    if (reported)
      return true;
    if (!flag.load(std::memory_order_relaxed))
      return false;
    reported = true;
    clang::DiagnosticsEngine &diags = instance.getDiagnostics();
    diags.Report(diagID);
    enterFatalState(diags, fatalID);
    // Nothing is reported after it: neither what a stopped code generator or
    // the end of the file might still diagnose, nor the "1 error generated."
    // count, which clang writes to the verbose output stream at the end.
    diags.setSuppressAllDiagnostics(true);
    instance.setVerboseOutputStream(llvm::nulls());
    return true;
  }

private:
  const std::atomic<bool> &flag;
  clang::CompilerInstance &instance;
  unsigned diagID;
  unsigned fatalID;
  bool reported = false;
};

// Forwards everything to the action's own consumer, and stops the parser
// (ParseAST ends when HandleTopLevelDecl returns false) and the work at the
// end of the translation unit (code generation) once the call is cancelled.
// Sema calls the other callbacks while it works inside one declaration or at
// the end of the translation unit, where HandleTopLevelDecl's result is
// ignored (C++: every class template instantiated, every function
// instantiation requested or done, inline member functions, vtables): there a
// cancel stops further template instantiation (enterFatalState),
// and nothing more is passed on to code generation, whose output is
// discarded anyway.
class CancellingConsumer : public clang::MultiplexConsumer {
public:
  CancellingConsumer(std::unique_ptr<clang::ASTConsumer> consumer,
                     ClangCancellation &cancellation)
      : MultiplexConsumer(std::move(consumer)), cancellation(cancellation) {}

  bool HandleTopLevelDecl(clang::DeclGroupRef group) override {
    if (cancellation.check())
      return false;
    bool more = MultiplexConsumer::HandleTopLevelDecl(group);
    return more && !cancellation.check();
  }

  void HandleInlineFunctionDefinition(clang::FunctionDecl *decl) override {
    if (!cancellation.check())
      MultiplexConsumer::HandleInlineFunctionDefinition(decl);
  }

  void HandleInterestingDecl(clang::DeclGroupRef group) override {
    if (!cancellation.check())
      MultiplexConsumer::HandleInterestingDecl(group);
  }

  void HandleTagDeclDefinition(clang::TagDecl *decl) override {
    if (!cancellation.check())
      MultiplexConsumer::HandleTagDeclDefinition(decl);
  }

  void HandleCXXImplicitFunctionInstantiation(clang::FunctionDecl *decl) override {
    if (!cancellation.check())
      MultiplexConsumer::HandleCXXImplicitFunctionInstantiation(decl);
  }

  void HandleCXXStaticMemberVarInstantiation(clang::VarDecl *decl) override {
    if (!cancellation.check())
      MultiplexConsumer::HandleCXXStaticMemberVarInstantiation(decl);
  }

  void HandleVTable(clang::CXXRecordDecl *decl) override {
    if (!cancellation.check())
      MultiplexConsumer::HandleVTable(decl);
  }

  void HandleTranslationUnit(clang::ASTContext &context) override {
    if (cancellation.check())
      return;
    MultiplexConsumer::HandleTranslationUnit(context);
    // A cancel during optimization or code generation stops them part-way
    // (the pass instrumentation and the context's flag); their output must
    // not be kept.
    cancellation.check();
  }

private:
  ClangCancellation &cancellation;
};

// Wraps an AST action so that its consumer is a CancellingConsumer.
class CancellableAction : public clang::WrapperFrontendAction {
public:
  CancellableAction(std::unique_ptr<clang::FrontendAction> action,
                    ClangCancellation &cancellation)
      : WrapperFrontendAction(std::move(action)), cancellation(cancellation) {}

protected:
  // Whatever the action did, a call that is cancelled by its end reports it
  // before EndSourceFile, so that its outputs are discarded (an action that
  // builds no AST, or work after the end of the translation unit).
  void ExecuteAction() override {
    WrapperFrontendAction::ExecuteAction();
    cancellation.check();
  }

  std::unique_ptr<clang::ASTConsumer>
  CreateASTConsumer(clang::CompilerInstance &instance,
                    llvm::StringRef inFile) override {
    std::unique_ptr<clang::ASTConsumer> consumer =
        WrapperFrontendAction::CreateASTConsumer(instance, inFile);
    if (!consumer)
      return nullptr;
    return std::make_unique<CancellingConsumer>(std::move(consumer),
                                                cancellation);
  }

private:
  ClangCancellation &cancellation;
};

// The compiler instance of one call, which is also its preprocessor's and
// Sema's module loader. It refuses clang modules: "#pragma clang module
// build" would compile a nested module on a helper stack, outside the
// call's cancellation, writing a module file to $TMPDIR
// (CompilerInstance::createModuleFromSource), and an import (C++20 import,
// @import, #pragma clang module import/load, an #include that a module map
// turns into an import) would read module files or module maps. The first
// request ends the call with one line, like a cancellation: later
// diagnostics, the "N errors generated." count and further module requests
// are dropped, and the failed call writes no output. Clang stops reading the
// source there, as after a module that fails to load (HadFatalFailure, and
// the lexers cut off), so nothing after the request is parsed: what a
// cancel could still have to stop is only the end of the translation unit.
class SwiftHostCompilerInstance : public clang::CompilerInstance {
public:
  using clang::CompilerInstance::CompilerInstance;

  clang::ModuleLoadResult loadModule(clang::SourceLocation,
                                     clang::ModuleIdPath,
                                     clang::Module::NameVisibilityKind,
                                     bool) override {
    reject("swifthost_clang does not support module imports");
    return clang::ModuleLoadResult();
  }

  void createModuleFromSource(clang::SourceLocation, llvm::StringRef,
                              llvm::StringRef) override {
    reject("swifthost_clang does not support #pragma clang module build");
  }

private:
  template <size_t N> void reject(const char (&message)[N]) {
    if (rejected)
      return;
    rejected = true;
    HadFatalFailure = true;
    clang::DiagnosticsEngine &diags = getDiagnostics();
    diags.Report(
        diags.getCustomDiagID(clang::DiagnosticsEngine::Error, message));
    enterFatalState(diags, diags.getCustomDiagID(
                               clang::DiagnosticsEngine::Fatal, message));
    diags.setSuppressAllDiagnostics(true);
    setVerboseOutputStream(llvm::nulls());
    if (hasPreprocessor()) {
      clang::Preprocessor &preprocessor = getPreprocessor();
      StopLexing::cutOff(preprocessor);
      preprocessor.addPPCallbacks(std::make_unique<StopLexing>(preprocessor));
    }
  }

  // Ends the file being lexed (the request's file now, then every file that
  // included it as the lexer returns to it), as clang's own fatal module
  // failures do (Lexer::cutOffLexing, which is private; seek to the end of
  // the buffer is the same).
  class StopLexing : public clang::PPCallbacks {
  public:
    explicit StopLexing(clang::Preprocessor &preprocessor)
        : preprocessor(preprocessor) {}

    void FileChanged(clang::SourceLocation, FileChangeReason reason,
                     clang::SrcMgr::CharacteristicKind,
                     clang::FileID) override {
      if (reason == ExitFile)
        cutOff(preprocessor);
    }

    // Clang's only PreprocessorLexer is the Lexer.
    static void cutOff(clang::Preprocessor &preprocessor) {
      if (clang::PreprocessorLexer *lexer = preprocessor.getCurrentFileLexer()) {
        auto *fileLexer = static_cast<clang::Lexer *>(lexer);
        fileLexer->seek(fileLexer->getBuffer().size(),
                        /*IsAtStartOfLine=*/true);
      }
    }

  private:
    clang::Preprocessor &preprocessor;
  };

  bool rejected = false;
};

// Runs one cc1 command line (args[0] is "-cc1") as cc1_main does. Under gLock.
int runClangFrontend(llvm::ArrayRef<const char *> args, const char *argv0,
                     llvm::raw_ostream &errOS, const std::atomic<bool> &flag) {
  // Clang modules and PCHs wrapped in object files, as cc1_main registers.
  auto pchOps = std::make_shared<clang::PCHContainerOperations>();
  pchOps->registerWriter(std::make_unique<clang::ObjectFilePCHContainerWriter>());
  pchOps->registerReader(std::make_unique<clang::ObjectFilePCHContainerReader>());

  // The diagnostics of argument parsing are buffered until the diagnostics
  // engine that the parsed options describe exists.
  clang::DiagnosticOptions bufferOptions;
  auto *buffer = new clang::TextDiagnosticBuffer;
  clang::DiagnosticsEngine bufferDiags(clang::DiagnosticIDs::create(),
                                       bufferOptions, buffer);
  auto invocation = std::make_shared<clang::CompilerInvocation>();
  bool parsed = clang::CompilerInvocation::CreateFromArgs(*invocation, args,
                                                          bufferDiags, argv0);

  // The call's LLVM context, with the call's flag attached for the legacy pass
  // manager (code generation). It outlives the compiler instance and the
  // action, which hold its module.
  llvm::LLVMContext context;
  llvm::CancellationScope codeGenCancellation(context, &flag);

  auto instance = std::make_unique<SwiftHostCompilerInstance>(
      std::move(invocation), std::move(pchOps));
  // A file system of the call's own: one from getRealFileSystem() would
  // change the process's working directory for -working-directory.
  instance->createVirtualFileSystem(llvm::vfs::createPhysicalFileSystem(),
                                    buffer);
  instance->createDiagnostics(
      new clang::TextDiagnosticPrinter(errOS, instance->getDiagnosticOpts()),
      /*ShouldOwnClient=*/true);
  if (!instance->hasDiagnostics())
    return 1;
  // "N errors generated." and the like.
  instance->setVerboseOutputStream(errOS);
  buffer->FlushDiagnostics(instance->getDiagnostics());
  auto finishDiagnostics = llvm::make_scope_exit(
      [&] { instance->getDiagnosticClient().finish(); });
  if (!parsed)
    return 1;

  // Standard input or output in any cc1 argument (the parsed options only
  // show some of them).
  {
    unsigned missingIndex = 0, missingCount = 0;
    llvm::opt::InputArgList cc1Args = clang::driver::getDriverOptTable().ParseArgs(
        args.slice(1), missingIndex, missingCount,
        llvm::opt::Visibility(clang::driver::options::CC1Option));
    for (const llvm::opt::Arg *A : cc1Args)
      if (usesStandardStream(*A)) {
        writeClangError(errOS, "swifthost_clang does not support " +
                                   A->getAsString(cc1Args));
        return 1;
      }
    // Clang modules. The parsed LangOptions::Modules cannot tell: C++20
    // turns it on by default (for C++20 modules), with -fno-implicit-modules.
    if (const llvm::opt::Arg *A = cc1Args.getLastArg(
            clang::driver::options::OPT_fmodules,
            clang::driver::options::OPT_fcxx_modules)) {
      writeClangError(errOS, "swifthost_clang does not support " +
                                 A->getAsString(cc1Args));
      return 1;
    }
  }
  if (const char *option = rejectedFrontendOption(instance->getInvocation())) {
    writeClangError(errOS, llvm::Twine("swifthost_clang does not support ") +
                               option);
    return 1;
  }
  for (const clang::FrontendInputFile &input : instance->getFrontendOpts().Inputs)
    if (!isSupportedFrontendInput(input.getKind())) {
      writeClangError(errOS, "swifthost_clang does not support the input " +
                                 input.getFile() + " (" + kSupportedInputs +
                                 ")");
      return 1;
    }
  clang::FrontendOptions &frontendOpts = instance->getFrontendOpts();
  // The driver passes -disable-free; in a long-lived process the AST, the
  // module and everything else must be freed when the call ends.
  frontendOpts.DisableFree = false;
  instance->getCodeGenOpts().DisableFree = false;
  frontendOpts.MayEmitDiagnosticsAfterProcessingSourceFiles = true;
  if (instance->getHeaderSearchOpts().UseBuiltinIncludes &&
      instance->getHeaderSearchOpts().ResourceDir.empty()) {
    writeClangError(errOS, "swifthost_clang needs -resource-dir (the SDK's "
                           "swift.xctoolchain/usr/lib/swift_static/clang)");
    return 1;
  }

  // -mllvm, as ExecuteCompilerInvocation applies it (with its bounds-safety
  // default). LLVM's options are process-wide: the next call resets them.
  // Parse errors go to err_fd and fail the call; -mllvm -help or -version
  // still end it through LLVM's exit path (which poisons the library).
  std::vector<std::string> &llvmArgs = frontendOpts.LLVMArgs;
  if (instance->getLangOpts().BoundsSafety &&
      std::none_of(llvmArgs.begin(), llvmArgs.end(), [](const std::string &a) {
        return llvm::StringRef(a).starts_with("-enable-constraint-elimination");
      }))
    llvmArgs.push_back("-enable-constraint-elimination");
  if (!llvmArgs.empty()) {
    gLLVMOptionsSet = true;
    std::vector<const char *> llvmArgv;
    llvmArgv.push_back("clang (LLVM option parsing)");
    for (const std::string &a : llvmArgs)
      llvmArgv.push_back(a.c_str());
    llvmArgv.push_back(nullptr);
    if (!llvm::cl::ParseCommandLineOptions(
            (int)llvmArgv.size() - 1, llvmArgv.data(), /*Overview=*/"", &errOS,
            &instance->getVirtualFileSystem()))
      return 1;
  }

  std::unique_ptr<clang::FrontendAction> action =
      createClangAction(*instance, &context);
  if (!action) {
    writeClangError(errOS, "swifthost_clang runs only compile jobs that write "
                           "an object, assembly, LLVM IR or bitcode, "
                           "preprocess (-E) or check syntax (-fsyntax-only)");
    return 1;
  }
  bool preprocessOnly = action->usesPreprocessorOnly();
  // SwiftPM passes -index-store-path in debug builds.
  if (!frontendOpts.IndexStorePath.empty()) {
    instance->getCodeGenOpts().ClearASTBeforeBackend = false;
    action = clang::index::createIndexDataRecordingAction(frontendOpts,
                                                          std::move(action));
    instance->setGenModuleActionWrapper(
        &clang::index::createIndexDataRecordingAction);
  }

  ClangCancellation cancellation(flag, *instance);
  // Parse/Sema: checked at each top-level declaration. -E has no AST and
  // runs to the end.
  if (!preprocessOnly)
    action = std::make_unique<CancellableAction>(std::move(action),
                                                 cancellation);
  // LLVM optimization (new pass manager): no further optional pass once the
  // call is cancelled.
  const std::atomic<bool> *flagPtr = &flag;
  instance->getCodeGenOpts().PassBuilderCallbacks.push_back(
      [flagPtr](llvm::PassBuilder &builder) {
        if (llvm::PassInstrumentationCallbacks *callbacks =
                builder.getPassInstrumentationCallbacks())
          callbacks->registerShouldRunOptionalPassCallback(
              [flagPtr](llvm::StringRef, llvm::Any) {
                return !flagPtr->load(std::memory_order_relaxed);
              });
      });

  // Clang moves deeply recursive work to a thread with a fresh 8 MiB stack
  // when this one runs low; the measure starts here.
  clang::noteBottomOfStack(/*ForceSet=*/true);
  bool success = instance->ExecuteAction(*action);
  return success ? 0 : 1;
}

// Runs a clang driver command line that has exactly one compile job, or with
// -### prints its jobs. Under gLock.
int runClangDriver(llvm::ArrayRef<const char *> argv, llvm::raw_ostream &errOS,
                   const std::atomic<bool> &flag) {
  namespace opts = clang::driver::options;
  unsigned missingIndex = 0, missingCount = 0;
  llvm::opt::InputArgList parsed = clang::driver::getDriverOptTable().ParseArgs(
      argv.slice(1), missingIndex, missingCount);
  bool printJobs = false, hasResourceDir = false;
  std::vector<bool> dropped(argv.size(), false);
  for (const llvm::opt::Arg *A : parsed) {
    if (A->getOption().matches(opts::OPT__HASH_HASH_HASH)) {
      // Handled here: the driver would print its version to stderr.
      printJobs = true;
      dropped[A->getIndex() + 1] = true;
      continue;
    }
    if (A->getOption().matches(opts::OPT_resource_dir))
      hasResourceDir = true;
    std::string option = rejectedDriverOption(*A, parsed);
    if (!option.empty()) {
      writeClangError(errOS, "swifthost_clang does not support " + option);
      return 1;
    }
  }
  if (!hasResourceDir) {
    writeClangError(errOS, "swifthost_clang needs -resource-dir (the SDK's "
                           "swift.xctoolchain/usr/lib/swift_static/clang)");
    return 1;
  }
  std::string unsupported = rejectedDriverCommand(argv, parsed);
  if (!unsupported.empty()) {
    writeClangError(errOS, unsupported);
    return 1;
  }

  // No configuration file is read: they are looked for next to the
  // executable, which for argv[0] without a directory is the working
  // directory.
  llvm::SmallVector<const char *, 256> driverArgs;
  driverArgs.push_back(argv[0]);
  driverArgs.push_back("--no-default-config");
  for (size_t i = 1; i < argv.size(); ++i)
    if (!dropped[i])
      driverArgs.push_back(argv[i]);

  std::unique_ptr<clang::DiagnosticOptions> diagOpts =
      clang::CreateAndPopulateDiagOpts(driverArgs);
  diagOpts->DiagnosticSuppressionMappingsFile.clear();
  // Colors only when asked for: by default the driver decides by whether the
  // process's stderr is a terminal (and passes that on to cc1), but the
  // diagnostics go to err_fd.
  const llvm::opt::Arg *color = parsed.getLastArg(
      opts::OPT_fcolor_diagnostics, opts::OPT_fno_color_diagnostics,
      opts::OPT_fdiagnostics_color_EQ);
  if (!color || (color->getOption().matches(opts::OPT_fdiagnostics_color_EQ) &&
                 llvm::StringRef(color->getValue()) != "always"))
    diagOpts->ShowColors = false;
  auto *printer = new clang::TextDiagnosticPrinter(errOS, *diagOpts);
  printer->setPrefix(llvm::sys::path::stem(argv[0]).str());
  clang::DiagnosticsEngine diags(clang::DiagnosticIDs::create(), *diagOpts,
                                 printer);
  llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> fileSystem =
      llvm::vfs::createPhysicalFileSystem();
  clang::ProcessWarningOptions(diags, *diagOpts, *fileSystem,
                               /*ReportDiags=*/false);
  clang::driver::Driver driver(argv[0], kClangDefaultTriple, diags,
                               "clang LLVM compiler", fileSystem);
  driver.setTargetAndMode(
      clang::driver::ToolChain::getTargetAndModeFromProgramName(argv[0]));
  std::unique_ptr<clang::driver::Compilation> compilation(
      driver.BuildCompilation(driverArgs));
  if (!compilation || compilation->containsError() || diags.hasErrorOccurred())
    return 1;

  const clang::driver::JobList &jobs = compilation->getJobs();
  if (printJobs) {
    driver.PrintVersion(*compilation, errOS);
    jobs.Print(errOS, "\n", /*Quote=*/true);
    errOS.flush();
    return 0;
  }
  if (jobs.size() != 1) {
    writeClangError(errOS, "swifthost_clang runs exactly one compile job; "
                           "this command needs " +
                               llvm::Twine((unsigned)jobs.size()) + " jobs");
    return 1;
  }
  const clang::driver::Command &job = *jobs.begin();
  const llvm::opt::ArgStringList &jobArgs = job.getArguments();
  for (const clang::driver::InputInfo &input : job.getInputInfos())
    if (!isSupportedDriverInput(input.getType())) {
      writeClangError(errOS,
                      llvm::Twine("swifthost_clang does not support the input ") +
                          input.getAsString() + " (" + kSupportedInputs +
                          ")");
      return 1;
    }
  if (jobArgs.empty() || llvm::StringRef(jobArgs[0]) != "-cc1") {
    writeClangError(errOS,
                    llvm::Twine("swifthost_clang runs exactly one compile "
                                "job; this command needs a job of ") +
                        job.getCreator().getName());
    return 1;
  }
  return runClangFrontend(jobArgs, argv[0], errOS, flag);
}

// A clang call: response files expanded as clang's main does, then the -cc1
// passthrough or the driver. Under gLock.
int runClang(int argc, const char **argv, llvm::raw_ostream &errOS,
             const std::atomic<bool> &flag) {
  llvm::BumpPtrAllocator allocator;
  llvm::SmallVector<const char *, 256> args(argv, argv + argc);
  llvm::cl::ExpansionContext expansion(allocator,
                                       llvm::cl::TokenizeGNUCommandLine);
  if (llvm::Error error = expansion.expandResponseFiles(args)) {
    writeClangError(errOS, llvm::toString(std::move(error)));
    return 1;
  }
  if (args.size() >= 2 && llvm::StringRef(args[1]) == "-cc1")
    return runClangFrontend(llvm::ArrayRef<const char *>(args).slice(1),
                            args[0], errOS, flag);
  return runClangDriver(args, errOS, flag);
}

#if SWIFTHOST_SOURCEKIT
// sourcekitd's crash containment. sourcekitd runs its work on its own threads
// (libdispatch workers, 8 MB threads for stack-deep work, the main queue);
// every work item runs under crash recovery (WorkQueue's execution hook), and
// every request passes through the service hooks, which track it until it is
// answered. The first crash that is recovered stops sourcekitd: every request
// that has not been answered yet gets an error, and from then on every
// request gets the same error at once, without reaching sourcekitd, whose
// state (and the locks the crashed thread held) can no longer be trusted.
using ResponseReceiver = sourcekitdInProc::ServiceHooks::ResponseReceiver;

// 0 while sourcekitd runs, then the signal number or
// SWIFTHOST_SOURCEKITD_FATAL_ERROR of the crash that stopped it.
std::atomic<int> gSourceKitStatus{0};

struct PendingRequest {
  PendingRequest(ResponseReceiver receiver, bool synchronous)
      : receiver(std::move(receiver)), synchronous(synchronous) {}
  ResponseReceiver receiver;
  const bool synchronous;
  std::atomic<bool> answered{false};
};

// Guards gPendingRequests and gStopDescription, and the transition of
// gSourceKitStatus from 0.
std::mutex gSourceKitLock;
std::unordered_set<std::shared_ptr<PendingRequest>> gPendingRequests;
std::string gStopDescription;

struct Delivery {
  ResponseReceiver receiver;
  sourcekitd_response_t response;
};

void deliver(void *context) {
  std::unique_ptr<Delivery> delivery(static_cast<Delivery *>(context));
  delivery->receiver(delivery->response);
}

// Answers a request with the error of the stopped service: a synchronous one
// on this thread (its caller only waits for the answer), an asynchronous one on
// a global queue, as the service would (never inside sourcekitd_send_request,
// whose caller may hold a lock its receiver takes).
void answerStopped(ResponseReceiver receiver, bool synchronous) {
  std::string description;
  {
    std::lock_guard<std::mutex> lock(gSourceKitLock);
    description = gStopDescription;
  }
  sourcekitd_response_t response =
      sourcekitd::createErrorRequestInterrupted(description);
  if (synchronous) {
    receiver(response);
    return;
  }
  dispatch_async_f(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0),
                   new Delivery{std::move(receiver), response}, deliver);
}

// Stops sourcekitd after a recovered crash (only the first one counts) and
// answers every request that is still waiting.
void stopSourceKit(int status, std::string description) {
  std::vector<std::shared_ptr<PendingRequest>> pending;
  {
    std::lock_guard<std::mutex> lock(gSourceKitLock);
    if (gSourceKitStatus.load(std::memory_order_relaxed) != 0)
      return;
    gStopDescription = std::move(description);
    gSourceKitStatus.store(status, std::memory_order_release);
    pending.assign(gPendingRequests.begin(), gPendingRequests.end());
    gPendingRequests.clear();
  }
  for (auto &request : pending)
    if (!request->answered.exchange(true))
      answerStopped(request->receiver, request->synchronous);
}

// Describes a crash or fatal error in sourcekitd's work on this thread, which
// crash recovery returned retCode for; returns sourcekitd's new status.
int recoveryStatus(int retCode, char *description, size_t size) {
  if (retCode > 128) {
    // A signal (CrashRecoveryContext returns 128 + the signal number).
    int signal = retCode - 128;
    std::snprintf(description, size,
                  "sourcekitd stopped: recovered crash (signal %d)", signal);
    return signal;
  }
  // report_fatal_error (through the router) or another exit path.
  if (tFatalReason[0])
    std::snprintf(description, size,
                  "sourcekitd stopped: recovered fatal error (%s)",
                  tFatalReason);
  else
    std::snprintf(description, size,
                  "sourcekitd stopped: recovered fatal error (exit status %d)",
                  retCode);
  return SWIFTHOST_SOURCEKITD_FATAL_ERROR;
}

// The app's code that sourcekitd calls while one of its work items runs under
// crash recovery on this thread (response receivers, the notification
// handler): the work item calls it once it has left crash recovery, so that a
// crash in the app's code is not taken for one in sourcekitd's, and the
// recovery never skips the app's destructors or leaves its locks held.
thread_local std::vector<std::function<void()>> *tAppCalls = nullptr;

void callApp(std::function<void()> call) {
  if (tAppCalls) {
    tAppCalls->push_back(std::move(call));
    return;
  }
  call();
}

// Runs fn, a piece of sourcekitd's work, under crash recovery on the calling
// thread (with an alternate signal stack, so that a stack overflow is
// recovered too); a crash or fatal error stops sourcekitd. Then calls the app
// code that fn produced (callApp), outside crash recovery.
template <typename Fn> void runSourceKitProtected(Fn fn) {
  // A thread that is already under crash recovery: that recovery covers it.
  if (llvm::CrashRecoveryContext::GetCurrent()) {
    fn();
    return;
  }
  std::vector<std::function<void()>> appCalls;
  int status = 0;
  char description[400];
  {
    ThreadRoleScope roleScope(ThreadRole::SourceKit, -1);
    AlternateSignalStack altStack;
    CrashRecoveryUse recovery;
    const void *stackState = llvm::SavePrettyStackState();
    tFatalReason[0] = '\0';
    llvm::CrashRecoveryContext crc;
    tAppCalls = &appCalls;
    tSourceKitRecovery = &crc;
    tSourceKitRecoveryBooked = false;
    bool completed = crc.RunSafely(fn);
    tSourceKitRecovery = nullptr;
    tAppCalls = nullptr;
    if (!completed && tSourceKitRecoveryBooked) {
      if (unsigned delay = gTestRecoveryDelayMs.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(delay));
    }
    // Whether or not the work item completed: a booking still open here (a
    // crash that was booked but did not end in this recovery) must not keep
    // frontend calls waiting.
    endSourceKitRecovery();
    if (!completed) {
      llvm::RestorePrettyStackState(stackState);
      status = recoveryStatus(crc.RetCode, description, sizeof(description));
    }
  }
  if (status)
    stopSourceKit(status, description);
  // A crash may have hit while a call was being added.
  for (auto &call : appCalls)
    if (call)
      call();
}

// WorkQueue's execution hook: every asynchronous and every stack-deep work
// item of SourceKit runs through it.
void executeWorkItem(void *context, SourceKit::WorkQueue::DispatchFn fn) {
  // A stopped service drops its work; whoever waits for it has been answered.
  if (gSourceKitStatus.load(std::memory_order_acquire) != 0)
    return;
  runSourceKitProtected([&] { fn(context); });
}

// The SendRequest hook of sourcekitd_send_request{,_sync}.
void sendRequest(sourcekitd_object_t, bool synchronous,
                 ResponseReceiver receiver,
                 std::function<void(ResponseReceiver)> send) {
  ensureProcessSetup();
  auto request =
      std::make_shared<PendingRequest>(std::move(receiver), synchronous);
  bool stopped;
  {
    std::lock_guard<std::mutex> lock(gSourceKitLock);
    stopped = gSourceKitStatus.load(std::memory_order_relaxed) != 0;
    if (!stopped)
      gPendingRequests.insert(request);
  }
  if (stopped) {
    answerStopped(std::move(request->receiver), synchronous);
    return;
  }
  // The receiver is the app's (for a synchronous request sourcekitd's own,
  // which wakes the caller). The request counts as answered only when it is
  // called: if the work item that produced the response crashes or never
  // ends, stopping sourcekitd answers the request instead.
  ResponseReceiver tracked = [request](sourcekitd_response_t response) {
    callApp([request, response] {
      // Already answered with the error of the stopped service.
      if (request->answered.exchange(true)) {
        sourcekitd_response_dispose(response);
        return;
      }
      {
        std::lock_guard<std::mutex> lock(gSourceKitLock);
        gPendingRequests.erase(request);
      }
      request->receiver(response);
    });
  };
  if (!synchronous) {
    // Only dispatches the request to sourcekitd's queue.
    send(std::move(tracked));
    return;
  }
  // A synchronous request is handled on one of SourceKit's 8 MB threads under
  // crash recovery (the execution hook), while its caller waits for the
  // answer: neither a crash nor a lock that a crashed thread holds can keep
  // the caller from getting one.
  SourceKit::WorkQueue::dispatchConcurrent(
      [send = std::move(send), tracked = std::move(tracked)] { send(tracked); },
      SourceKit::WorkQueue::Priority::Default, /*isStackDeep=*/true);
}

bool shouldUseService() {
  ensureProcessSetup();
  return gSourceKitStatus.load(std::memory_order_acquire) == 0;
}

// sourcekitd_initialize keeps the router (it would install a fatal error
// handler that aborts).
bool shouldInstallFatalErrorHandler() { return false; }

// The notification handler is the app's code.
void deliverNotification(std::function<void()> deliver) {
  callApp(std::move(deliver));
}

// Installs the hooks before anything can call into sourcekitd.
struct SourceKitHooksRegistration {
  SourceKitHooksRegistration() {
    SourceKit::WorkQueue::setExecutionHook(executeWorkItem);
    sourcekitdInProc::ServiceHooks hooks;
    hooks.SendRequest = sendRequest;
    hooks.ShouldUseService = shouldUseService;
    hooks.ShouldInstallFatalErrorHandler = shouldInstallFatalErrorHandler;
    hooks.DeliverNotification = deliverNotification;
    sourcekitdInProc::setServiceHooks(hooks);
  }
} gSourceKitHooksRegistration;
#endif

} // namespace

extern "C" const char *swifthost_version(void) {
  // The release's full version string is fixed in the sources ("Swift version
  // 6.4.2 (swift-6.4.2-RELEASE)"); the build's tag (SWIFT_TOOLCHAIN_VERSION)
  // identifies this library.
  static const std::string version = [] {
    std::string v = swift::version::getSwiftFullVersion();
    llvm::StringRef tag = swift::version::getCurrentCompilerTag();
    if (!tag.empty())
      v += ", " + tag.str();
    return v;
  }();
  return version.c_str();
}

extern "C" int swifthost_sourcekitd_status(void) {
#if SWIFTHOST_SOURCEKIT
  return gSourceKitStatus.load(std::memory_order_acquire);
#else
  return 0;
#endif
}

extern "C" void swifthost_request_cancel(void) {
  std::lock_guard<std::mutex> lock(gCancelLock);
  if (gCancelFlag)
    gCancelFlag->store(true, std::memory_order_relaxed);
}

extern "C" void
swifthost_set_plugin_message_handler(swifthost_plugin_message_handler handler) {
  std::lock_guard<std::mutex> lock(gPluginHandlerLock);
  if (gPluginHandlerFrozen || !handler)
    return;
  gAppHandler.store(handler, std::memory_order_release);
  gPluginHandlerFrozen = true;
}

extern "C" int swifthost_swift_frontend(int argc, const char **argv,
                                        int out_fd, int err_fd) {
  (void)out_fd;
  if (argc < 1 || !argv)
    return 2;
  freezePluginHandler();
  return runGuarded(ThreadRole::Compiler, argc, argv, err_fd, [&](bool &poison) {
    FdStream diagOS(err_fd, /*unbuffered=*/true);
    auto flag = std::make_shared<std::atomic<bool>>(false);
    {
      std::lock_guard<std::mutex> lock(gCancelLock);
      gCancelFlag = flag;
    }
    CancellationObserver observer(flag);
    int rc = runRecovering(
        [&] {
          return swift::performFrontend(
              llvm::ArrayRef<const char *>(argv + 1, argv + argc), argv[0],
              (void *)(intptr_t)&swifthost_swift_frontend, &observer, diagOS);
        },
        poison);
    {
      std::lock_guard<std::mutex> lock(gCancelLock);
      gCancelFlag.reset();
    }
    if (rc != 0 && !poison && flag->load(std::memory_order_relaxed))
      return SWIFTHOST_ECANCELED;
    return rc;
  });
}

extern "C" int swifthost_clang(int argc, const char **argv, int out_fd,
                               int err_fd) {
  (void)out_fd;
  if (argc < 1 || !argv)
    return 2;
  return runGuarded(ThreadRole::CCompiler, argc, argv, err_fd, [&](bool &poison) {
    FdStream diagOS(err_fd, /*unbuffered=*/true);
    auto flag = std::make_shared<std::atomic<bool>>(false);
    {
      std::lock_guard<std::mutex> lock(gCancelLock);
      gCancelFlag = flag;
    }
    int rc = runRecovering([&] { return runClang(argc, argv, diagOS, *flag); },
                           poison);
    {
      std::lock_guard<std::mutex> lock(gCancelLock);
      gCancelFlag.reset();
    }
    if (rc != 0 && !poison && flag->load(std::memory_order_relaxed))
      return SWIFTHOST_ECANCELED;
    return rc;
  });
}

extern "C" int swifthost_swift_autolink_extract(int argc, const char **argv,
                                                int out_fd, int err_fd) {
  (void)out_fd;
  if (argc < 1 || !argv)
    return 2;
  return runGuarded(ThreadRole::Compiler, argc, argv, err_fd, [&](bool &poison) {
    FdStream diagOS(err_fd, /*unbuffered=*/true);
    return runRecovering(
        [&] {
          return autolink_extract_main(
              llvm::ArrayRef<const char *>(argv + 1, argv + argc), argv[0],
              (void *)(intptr_t)&swifthost_swift_autolink_extract, diagOS);
        },
        poison);
  });
}

extern "C" int swifthost_wasm_ld(int argc, const char **argv, int out_fd,
                                 int err_fd) {
  if (argc < 1 || !argv)
    return 2;
  return runGuarded(ThreadRole::Linker, argc, argv, err_fd, [&](bool &poison) {
    FdStream outOS(out_fd, /*unbuffered=*/false);
    FdStream errOS(err_fd, /*unbuffered=*/true);
    // lldMain runs the link inside its own CrashRecoveryContext and reports
    // whether the linker's global state survived.
    lld::Result r =
        lld::lldMain(llvm::ArrayRef<const char *>(argv, argv + argc), outOS,
                     errOS, {{lld::Wasm, &lld::wasm::link}});
    outOS.flush();
    if (!r.canRunAgain)
      poison = true;
    return r.retCode;
  });
}
