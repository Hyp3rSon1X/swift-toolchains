// Runs the repro's three sinks in a -sanitize=thread executable: the patched compiler must accept them, and the
// instrumented program must run clean (no TSan report, no crash from instrumenting a non-address).
@main
struct Main {
    static func main() {
        var a = NoncopyableSink(box: Box())
        var b = CopyableSink(box: Box())
        var c = NoncopyableSinkWorkaround(box: Box())
        for i in 1...100 {
            a.box.value = i; a.run()
            b.box.value = i; b.run()
            c.box.value = i; c.run()
        }
        print("sums \(a.inner.x) \(b.inner.x) \(c.inner.x)")
    }
}
