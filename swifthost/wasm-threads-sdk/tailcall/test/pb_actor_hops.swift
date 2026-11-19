// Probe B: N calls (default 100,000) from a detached, nonisolated task to an idle actor and back.
// Every call is an executor switch to the actor and back to the generic executor (swift_task_switch);
// with an idle actor both switches run inline, which nests frames without tail calls.
actor Counter {
    var n = 0
    func bump() -> Int { n += 1; return n }
}

@main struct PB {
    static func main() async {
        let n = argN(100_000)
        let c = Counter()
        let last = await Task.detached {
            var v = 0
            for i in 0..<n {
                progress("B", i)
                v = await c.bump()
            }
            return v
        }.value
        print("pb actor-hops ok n=\(n) last=\(last)")
    }
}
