// Probe 2: four pthreads each add 1000 to a Synchronization Atomic<Int> (expected 4000).
import WASILibc
import wasi_pthread  // WASILibc does not re-export pthread.h (wasi-libc.modulemap declares it as the separate module wasi_pthread)
import Synchronization

let counter = Atomic<Int>(0)

@main struct P2 {
    static func main() {
        var tids = [pthread_t?](repeating: nil, count: 4)
        for i in 0..<4 {
            let rc = pthread_create(&tids[i], nil, { _ in
                for _ in 0..<1000 { counter.add(1, ordering: .relaxed) }
                return nil
            }, nil)
            precondition(rc == 0, "pthread_create failed: \(rc)")
        }
        for t in tids { pthread_join(t!, nil) }
        print("p2 atomic total=\(counter.load(ordering: .sequentiallyConsistent))")
    }
}
