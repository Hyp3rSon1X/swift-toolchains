// RUN: %target-run-simple-swift
// REQUIRES: executable_test
// REQUIRES: synchronization
// REQUIRES: OS=wasip1

// A woken waiter must re-acquire the mutex as contended. If it took the lock
// as locked, its unlock would not wake the threads still blocked on the mutex,
// and once no new thread contends for the lock they would wait forever, so
// this test would hang instead of finishing.

import Synchronization
#if _runtime(_multithreaded)
import WASILibc
import wasi_pthread
#endif

let threadCount = 4
let iterations = 100_000
let counter = Mutex<Int>(0)

@main
struct WasmMutexHandoff {
  static func main() {
#if _runtime(_multithreaded)
    var threads = [pthread_t?](repeating: nil, count: threadCount)
    for i in 0 ..< threadCount {
      let result = pthread_create(&threads[i], nil, { _ in
        for _ in 0 ..< iterations {
          counter.withLock { $0 += 1 }
        }
        return nil
      }, nil)
      precondition(result == 0, "pthread_create failed: \(result)")
    }
    for thread in threads {
      pthread_join(thread!, nil)
    }
#else
    for _ in 0 ..< threadCount * iterations {
      counter.withLock { $0 += 1 }
    }
#endif
    // CHECK: total=400000
    print("total=\(counter.withLock { $0 })")
  }
}
