// SwiftHostCompiler: C entry points that run swift-frontend,
// swift-autolink-extract and wasm-ld in-process and, in a build with SourceKit
// (SWIFTHOST_SOURCEKIT), the policy that lets the in-process sourcekitd share
// the process with them. See SwiftHostCompiler.h for the contract.
#include "SwiftHostCompiler.h"

#include "swift/AST/ASTContext.h"
#include "swift/AST/PluginRegistry.h"
#include "swift/Basic/Compiler.h"
#include "swift/Basic/InitializeSwiftModules.h"
#include "swift/Basic/Version.h"
#include "swift/Frontend/Frontend.h"
#include "swift/FrontendTool/FrontendTool.h"
#include "lld/Common/Driver.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/CrashRecoveryContext.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/PrettyStackTrace.h"
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
#include <cstring>
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

// Crashes recovered in sourcekitd's work (see runSourceKitProtected), so that
// a frontend call does not count them as its own.
std::atomic<unsigned> gSourceKitRecoveries{0};

// What the calling thread is doing, for the fatal error router: a frontend
// or autolink call (Compiler) or a wasm-ld call (Linker), with the call's
// err_fd, or sourcekitd's work (SourceKit).
enum class ThreadRole : unsigned char { None, Compiler, Linker, SourceKit };
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

// Cancellation: each frontend call gets a fresh flag, published here while it
// runs, so a request can never leak into a later call.
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

// Hands the call's flag to the compiler. The type checker's constraint solver
// polls ASTContext::CancellationFlag (ConstraintSystem::isTooComplex), and the
// frontend checks it after SIL generation, SIL optimization, SIL lowering and
// IR generation. Also keeps the frontend from replacing the process-wide
// fatal error handler (the router below), which sourcekitd's threads need
// while a frontend call runs.
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
// own (CancellationObserver), and it replaces the one sourcekitd_initialize
// installs. It reports each fatal error where the thread's work belongs:
// a frontend or linker call's err_fd, or sourcekitd's crash containment.
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
  if (llvm::CrashRecoveryContext *crc = llvm::CrashRecoveryContext::GetCurrent())
    crc->HandleExit(1);
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
  });
}

// CrashRecoveryContext::Enable and Disable are process-wide and not counted;
// frontend calls and sourcekitd's work items overlap, so the handlers are
// installed while at least one of them runs.
std::mutex gRecoveryLock;
unsigned gRecoveryUsers = 0;

class CrashRecoveryUse {
public:
  CrashRecoveryUse() {
    std::lock_guard<std::mutex> lock(gRecoveryLock);
    ++gRecoveryUsers;
    // Also when already counted: a crash on a thread without a crash recovery
    // context disables crash recovery behind the count's back.
    llvm::CrashRecoveryContext::Enable();
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

// Whether crashes other than sourcekitd's own were recovered since the
// counts were taken. A crash in sourcekitd's work is counted by LLVM first
// and by runSourceKitProtected right after its recovery returns, so a count
// that differs is read again after a moment.
bool recoveredOtherCrashes(unsigned crashes, unsigned sourceKitCrashes) {
  for (int attempt = 0;; ++attempt) {
    unsigned all = llvm::CrashRecoveryContext::getNumRecoveredCrashes() - crashes;
    unsigned sourceKit =
        gSourceKitRecoveries.load(std::memory_order_acquire) - sourceKitCrashes;
    if (all <= sourceKit || attempt == 2)
      return all > sourceKit;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
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
  unsigned crashes = llvm::CrashRecoveryContext::getNumRecoveredCrashes();
  unsigned sourceKitCrashes =
      gSourceKitRecoveries.load(std::memory_order_acquire);
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
  if (recoveredOtherCrashes(crashes, sourceKitCrashes))
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

// Runs fn, a piece of sourcekitd's work, under crash recovery on the calling
// thread (with an alternate signal stack, so that a stack overflow is
// recovered too); a crash or fatal error stops sourcekitd.
template <typename Fn> void runSourceKitProtected(Fn fn) {
  // A thread that is already under crash recovery: that recovery covers it.
  if (llvm::CrashRecoveryContext::GetCurrent()) {
    fn();
    return;
  }
  ThreadRoleScope roleScope(ThreadRole::SourceKit, -1);
  AlternateSignalStack altStack;
  CrashRecoveryUse recovery;
  const void *stackState = llvm::SavePrettyStackState();
  tFatalReason[0] = '\0';
  llvm::CrashRecoveryContext crc;
  if (crc.RunSafely(fn))
    return;
  llvm::RestorePrettyStackState(stackState);
  gSourceKitRecoveries.fetch_add(1, std::memory_order_release);
  char description[400];
  int status;
  if (crc.RetCode > 128) {
    // A signal (CrashRecoveryContext returns 128 + the signal number).
    status = crc.RetCode - 128;
    std::snprintf(description, sizeof(description),
                  "sourcekitd stopped: recovered crash (signal %d)", status);
  } else {
    // report_fatal_error (through the router) or another exit path.
    status = SWIFTHOST_SOURCEKITD_FATAL_ERROR;
    if (tFatalReason[0])
      std::snprintf(description, sizeof(description),
                    "sourcekitd stopped: recovered fatal error (%s)",
                    tFatalReason);
    else
      std::snprintf(description, sizeof(description),
                    "sourcekitd stopped: recovered fatal error (exit status %d)",
                    crc.RetCode);
  }
  stopSourceKit(status, description);
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
  ResponseReceiver tracked = [request](sourcekitd_response_t response) {
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

// sourcekitd_initialize installs a fatal error handler that aborts; the
// router replaces it.
void serviceInitialized() { installFatalErrorRouter(); }

// Installs the hooks before anything can call into sourcekitd.
struct SourceKitHooksRegistration {
  SourceKitHooksRegistration() {
    SourceKit::WorkQueue::setExecutionHook(executeWorkItem);
    sourcekitdInProc::ServiceHooks hooks;
    hooks.SendRequest = sendRequest;
    hooks.ShouldUseService = shouldUseService;
    hooks.ServiceInitialized = serviceInitialized;
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
