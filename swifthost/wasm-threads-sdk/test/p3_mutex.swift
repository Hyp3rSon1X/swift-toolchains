// Probe 3: a Synchronization Mutex shared across four pthreads (expected 4000), plus pthread_mutex_t visibility.
import WASILibc
import wasi_pthread  // WASILibc does not re-export pthread.h (wasi-libc.modulemap declares it as the separate module wasi_pthread)
import Synchronization

let shared = Mutex<Int>(0)
nonisolated(unsafe) var raw = pthread_mutex_t()

@main struct P3 {
    static func main() {
        pthread_mutex_init(&raw, nil)
        var tids = [pthread_t?](repeating: nil, count: 4)
        for i in 0..<4 {
            let rc = pthread_create(&tids[i], nil, { _ in
                for _ in 0..<1000 { shared.withLock { $0 += 1 } }
                return nil
            }, nil)
            precondition(rc == 0, "pthread_create failed: \(rc)")
        }
        for t in tids { pthread_join(t!, nil) }
        print("p3 mutex total=\(shared.withLock { $0 })")
    }
}
