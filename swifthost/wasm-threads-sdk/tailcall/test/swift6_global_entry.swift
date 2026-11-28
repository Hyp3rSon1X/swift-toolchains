// Swift 6 language mode: a global function is nonisolated, so it is a valid pthread_create entry.
import WASILibc
import wasi_pthread

func worker(_: UnsafeMutableRawPointer?) -> UnsafeMutableRawPointer? {
  print("worker")
  return nil
}

var thread = pthread_t(bitPattern: 0)
let rc = pthread_create(&thread, nil, worker, nil)
pthread_join(thread!, nil)
print("done \(rc)")
