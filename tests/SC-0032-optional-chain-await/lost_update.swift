struct Entry { var estimate = 0; var waiters = 0 }
actor Box {
    var entries: [Int: Entry] = [1: Entry()]
    var opt: Entry? = Entry()
    func slow() async -> Int { try? await Task.sleep(for: .milliseconds(100)); return 42 }
    func addWaiter() { entries[1]?.waiters += 1; opt?.waiters += 1 }
    func optionalChain() async { entries[1]?.estimate = await slow() }
    func optStored() async { opt?.estimate = await slow() }
    func hoisted() async { let v = await slow(); entries[1]?.estimate = v }
    func reset() { entries = [1: Entry()]; opt = Entry() }
    func snapshot() -> (Entry?, Entry?) { (entries[1], opt) }
}
@main struct Repro {
    static func run(_ name: String, _ box: Box, _ body: @escaping @Sendable () async -> Void) async {
        await box.reset()
        let t = Task { await body() }
        try? await Task.sleep(for: .milliseconds(30))
        await box.addWaiter()
        await t.value
        let (e, o) = await box.snapshot()
        print("\(name): dict estimate=\(e?.estimate ?? -1) waiters=\(e?.waiters ?? -1) | opt estimate=\(o?.estimate ?? -1) waiters=\(o?.waiters ?? -1)")
    }
    static func main() async {
        let box = Box()
        await run("dict[k]?.x = await", box) { await box.optionalChain() }
        await run("opt?.x = await    ", box) { await box.optStored() }
        await run("hoisted           ", box) { await box.hoisted() }
    }
}
