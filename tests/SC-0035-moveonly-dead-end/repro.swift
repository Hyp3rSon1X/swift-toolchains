// SC-0035: -O crashes the compiler with "SIL verification failed: read-only scope invalidated by a
// local write". Needs all of: a ~Copyable struct (the Copyable twin compiles), a mutating method that
// calls a throwing/rethrows function with a non-throwing closure, a local copy of a stored class
// reference whose box is then written, and the mutating helper inlined into that caller.
// Reduced 2026-11-15 from SwixForge 2f604c46 DriverSink.applyBatch.
struct Root {
    var x = 0
    @inline(never)
    mutating func update(_ body: (inout Int) throws -> Int) rethrows -> Int {
        try body(&x)
    }
}

final class Box {
    var root = Root()
}

struct Sink: ~Copyable {
    let box: Box
    var count = 0

    mutating func run() {
        let box = self.box
        box.root.x += 1
        count += value()
    }

    mutating func value() -> Int {
        box.root.update { $0 }
    }
}
