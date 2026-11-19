// SwiftHostCompiler: C entry points that run swift-frontend,
// swift-autolink-extract and wasm-ld in-process. See SwiftHostCompiler.h for
// the contract.
#include "SwiftHostCompiler.h"

#include "swift/AST/ASTContext.h"
#include "swift/AST/PluginRegistry.h"
#include "swift/Basic/InitializeSwiftModules.h"
#include "swift/Basic/Version.h"
#include "swift/Frontend/Frontend.h"
#include "swift/FrontendTool/FrontendTool.h"
#include "lld/Common/Driver.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/CrashRecoveryContext.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
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
std::once_flag gInitOnce;

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

// The app's handler. It can be set until the first frontend call starts.
std::mutex gPluginHandlerLock;
bool gPluginHandlerFrozen = false;
std::atomic<MessageHandler> gAppHandler{nullptr};

// Without an app handler: the entry point of the plugin server library that
// the running frontend call names (-in-process-plugin-server-path).
std::atomic<MessageHandler> gServerHandler{nullptr};

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

bool handlePluginMessage(const char *message, size_t length, char **response,
                         size_t *responseLength) {
  MessageHandler handler = gAppHandler.load(std::memory_order_acquire);
  if (!handler)
    handler = gServerHandler.load(std::memory_order_acquire);
  if (!handler)
    return handlerError(response, responseLength, "no in-process plugin server");
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

// The entry point of a plugin server library, or null if it cannot be loaded
// (the compiler then tries itself and reports the error). Libraries stay
// loaded, as they do when the compiler loads them.
MessageHandler loadPluginServer(const char *path) {
  static std::mutex lock;
  static std::string lastPath;
  static MessageHandler lastHandler = nullptr;
  std::lock_guard<std::mutex> guard(lock);
  if (lastHandler && lastPath == path)
    return lastHandler;
  void *library = dlopen(path, RTLD_LAZY | RTLD_LOCAL);
  if (!library)
    return nullptr;
  auto handler = reinterpret_cast<MessageHandler>(
      dlsym(library, "swift_inproc_plugins_handle_message"));
  if (handler) {
    lastPath = path;
    lastHandler = handler;
  }
  return handler;
}

// Routes the plugin messages of the frontend call with these arguments
// through handlePluginMessage: to the app's handler, or else to the server
// library the arguments name. With neither, the compiler's own route stays.
void configurePluginRoute(int argc, const char **argv) {
  MessageHandler server = nullptr;
  if (!gAppHandler.load(std::memory_order_acquire)) {
    const char *path = nullptr; // the last one counts, as in the compiler
    for (int i = 1; i + 1 < argc; ++i)
      if (argv[i] && !std::strcmp(argv[i], "-in-process-plugin-server-path"))
        path = argv[i + 1];
    if (path)
      server = loadPluginServer(path);
  }
  gServerHandler.store(server, std::memory_order_release);
  bool routed = server || gAppHandler.load(std::memory_order_acquire);
  swift::InProcessPlugins::setHandleMessageOverride(routed ? handlePluginMessage
                                                           : nullptr);
}

// Hands the call's flag to the compiler. The type checker's constraint solver
// polls ASTContext::CancellationFlag (ConstraintSystem::isTooComplex), and the
// frontend checks it after SIL generation, SIL optimization, SIL lowering and
// IR generation.
class CancellationObserver : public swift::FrontendObserver {
public:
  explicit CancellationObserver(std::shared_ptr<std::atomic<bool>> flag)
      : flag(std::move(flag)) {}
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

// Runs body under the global lock, with crash recovery enabled only for the
// duration of the call. body sets poison when the compiler state can no
// longer be trusted.
template <typename Body> int runGuarded(int errFd, Body body) {
  std::lock_guard<std::mutex> lock(gLock);
  if (gPoisoned) {
    static const char msg[] = "error: the compiler crashed earlier in this "
                              "process; restart the app to compile again\n";
    writeAll(errFd, msg, sizeof(msg) - 1);
    return SWIFTHOST_EPOISONED;
  }
  std::call_once(gInitOnce, [] { initializeSwiftModules(); });
  llvm::cl::ResetAllOptionOccurrences();

  AlternateSignalStack altStack;
  unsigned crashes = llvm::CrashRecoveryContext::getNumRecoveredCrashes();
  llvm::CrashRecoveryContext::Enable();
  bool poison = false;
  int rc = body(poison);
  llvm::CrashRecoveryContext::Disable();
  // Crash recovery contexts inside the compiler (clang module builds and Swift
  // module interface builds, on helper threads) turn a crash into an ordinary
  // error. Calls are serialized, so any recovery since the call began was
  // this call's, and it skipped destructors like the call's own recovery.
  if (llvm::CrashRecoveryContext::getNumRecoveredCrashes() != crashes)
    poison = true;
  if (poison) {
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

// Reports LLVM fatal errors (report_fatal_error) to the call's err_fd. LLVM
// then exits through sys::Process::Exit, which the CrashRecoveryContext turns
// into a return.
struct FatalErrorToFd {
  explicit FatalErrorToFd(int fd) : handler(&report, (void *)(intptr_t)fd) {}
  static void report(void *fd, const char *reason, bool) {
    int errFd = (int)(intptr_t)fd;
    static const char prefix[] = "error: fatal error in the linker: ";
    writeAll(errFd, prefix, sizeof(prefix) - 1);
    writeAll(errFd, reason, std::strlen(reason));
    writeAll(errFd, "\n", 1);
  }
  llvm::ScopedFatalErrorHandler handler;
};

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
  return runGuarded(err_fd, [&](bool &poison) {
    configurePluginRoute(argc, argv);
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
  return runGuarded(err_fd, [&](bool &poison) {
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
  return runGuarded(err_fd, [&](bool &poison) {
    FdStream outOS(out_fd, /*unbuffered=*/false);
    FdStream errOS(err_fd, /*unbuffered=*/true);
    FatalErrorToFd fatalErrors(err_fd);
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
