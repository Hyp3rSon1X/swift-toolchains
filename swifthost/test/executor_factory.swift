// An async main installs the main module's DefaultExecutorFactory: each
// factory getter prints once, before main's first line (from swifthost-6.4.2-12;
// earlier versions needed -disable-availability-checking for this).
@_spi(ExperimentalCustomExecutors) import _Concurrency

struct RecordingExecutorFactory: ExecutorFactory {
  static var mainExecutor: any MainExecutor {
    print("factory: mainExecutor")
    return PlatformExecutorFactory.mainExecutor
  }
  static var defaultExecutor: any TaskExecutor {
    print("factory: defaultExecutor")
    return PlatformExecutorFactory.defaultExecutor
  }
}

typealias DefaultExecutorFactory = RecordingExecutorFactory

print("main: start")
let value = await Task.detached { 42 }.value
print("main: \(value)")
