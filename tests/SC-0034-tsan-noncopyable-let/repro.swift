// Swift 6.4 (swiftlang-6.4.0.34.1): -sanitize=thread turns a legal access to disjoint stored
// properties of a ~Copyable struct into a static exclusivity error. Without the sanitizer it
// compiles. The Copyable twin compiles in both modes.
final class Box { var value = 0 }

struct Inner {
    var x = 0
    mutating func add(_ v: inout Int) { x += v }
}

// error (only with -sanitize=thread): overlapping accesses to 'self.inner'
struct NoncopyableSink: ~Copyable {
    let box: Box
    var inner = Inner()
    mutating func run() { inner.add(&box.value) }
}

// compiles in both modes
struct CopyableSink {
    let box: Box
    var inner = Inner()
    mutating func run() { inner.add(&box.value) }
}

// the workaround: the reference is read into a local before the modify starts
struct NoncopyableSinkWorkaround: ~Copyable {
    let box: Box
    var inner = Inner()
    mutating func run() {
        let box = self.box
        inner.add(&box.value)
    }
}
