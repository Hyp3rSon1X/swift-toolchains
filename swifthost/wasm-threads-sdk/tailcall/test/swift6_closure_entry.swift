// Swift 6 language mode: a closure in top-level code inherits @MainActor, also as a pthread_create entry, and the
// compiler checks the isolation at its entry. With -8 the check traps on the new pthread (as on Linux).
import WASILibc
import wasi_pthread

var thread = pthread_t(bitPattern: 0)
let rc = pthread_create(&thread, nil, { _ in
  print("worker")
  return nil
}, nil)
pthread_join(thread!, nil)
print("done \(rc)")
