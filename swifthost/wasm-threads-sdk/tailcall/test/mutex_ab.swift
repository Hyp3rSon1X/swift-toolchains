// Mutex hand-off stress: stdlib Synchronization.Mutex (mode std) against the same algorithm with and without the re-acquire
// after a wake (modes copy and fixed). Usage: mutex_ab <std|fixed|copy> <iterations>

import WASILibc
import wasi_pthread
import Synchronization

@_extern(c, "llvm.wasm.memory.atomic.wait32")
func fxWait(_ on: UnsafePointer<UInt32>, _ expected: UInt32, _ timeout: Int64) -> UInt32
@_extern(c, "llvm.wasm.memory.atomic.notify")
func fxWake(_ on: UnsafePointer<UInt32>, _ count: UInt32) -> UInt32

// 0 unlocked, 1 locked, 2 contended. `fixed` selects the re-acquire after a wake.
final class LocalMutex: @unchecked Sendable {
  let word = UnsafeMutablePointer<UInt32>.allocate(capacity: 1)
  let fixed: Bool
  init(fixed: Bool) { self.fixed = fixed; word.initialize(to: 0) }
  var atomic: UnsafeAtomic { UnsafeAtomic(word) }
  func lock() {
    var (ok, state) = atomic.cas(0, 1)
    if ok { return }
    if fixed {
      // Linux/Drepper: after every wake, take the lock as contended.
      while true {
        if atomic.exchange(2) == 0 { return }
        _ = fxWait(UnsafePointer(word), 2, -1)
      }
    }
    // Verbatim shape of the 6.4.2 WasmImpl loop.
    while !ok {
      if state != 2, atomic.exchange(2) == 0 { return }
      _ = fxWait(UnsafePointer(word), 2, -1)
      (ok, state) = atomic.cas(0, 1)
    }
  }
  func unlock() {
    if atomic.exchange(0) == 2 { _ = fxWake(UnsafePointer(word), 1) }
  }
}

struct UnsafeAtomic {
  let p: UnsafeMutablePointer<UInt32>
  init(_ p: UnsafeMutablePointer<UInt32>) { self.p = p }
  var a: UnsafeMutablePointer<Atomic<UInt32>> { UnsafeMutableRawPointer(p).assumingMemoryBound(to: Atomic<UInt32>.self) }
  func cas(_ e: UInt32, _ d: UInt32) -> (Bool, UInt32) {
    let r = a.pointee.compareExchange(expected: e, desired: d, ordering: .acquiringAndReleasing)
    return (r.exchanged, r.original)
  }
  func exchange(_ v: UInt32) -> UInt32 { a.pointee.exchange(v, ordering: .acquiringAndReleasing) }
}

let mode = CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "std"
let iterations = CommandLine.arguments.count > 2 ? Int(CommandLine.arguments[2])! : 1000
let stdMutex = Mutex<Int>(0)
let local = LocalMutex(fixed: mode == "fixed")
nonisolated(unsafe) var localCount = 0
nonisolated(unsafe) var iters = iterations
nonisolated(unsafe) var useStd = mode == "std"

@main struct AB {
  static func main() {
    var tids = [pthread_t?](repeating: nil, count: 4)
    for i in 0..<4 {
      let rc = pthread_create(&tids[i], nil, { _ in
        for _ in 0..<iters {
          if useStd { stdMutex.withLock { $0 += 1 } } else { local.lock(); localCount += 1; local.unlock() }
        }
        return nil
      }, nil)
      precondition(rc == 0)
    }
    for t in tids { pthread_join(t!, nil) }
    let total = useStd ? stdMutex.withLock { $0 } : localCount
    print("mode=\(mode) total=\(total) expected=\(4 * iters)")
  }
}
