struct Entry { var estimate = 0; var waiters = 0 }
@MainActor var gdict: [Int: Entry] = [1: Entry()]
actor Box {
    var entries: [Int: Entry] = [1: Entry()]
    func slow() async -> Int { try? await Task.sleep(for: .milliseconds(100)); return 42 }
    func addWaiter() { entries[1]?.waiters += 1 }
    func compound() async { entries[1]?.estimate += await slow() }
    func forced() async { entries[1]!.estimate += await slow() }
    func reset() { entries = [1: Entry()] }
    func snap() -> Entry? { entries[1] }
}
@MainActor func mainSlow() async -> Int { try? await Task.sleep(for: .milliseconds(100)); return 42 }
@MainActor func mainChain() async { gdict[1]?.estimate = await mainSlow() }
@main struct R {
    static func run(_ n: String, _ b: Box, _ body: @escaping @Sendable () async -> Void) async {
        await b.reset(); let t = Task { await body() }
        try? await Task.sleep(for: .milliseconds(30)); await b.addWaiter(); await t.value
        print(n, await b.snap()!)
    }
    @MainActor static func main() async {
        let b = Box()
        await run("?. +=", b) { await b.compound() }
        await run("!. +=", b) { await b.forced() }
        let t = Task { @MainActor in await mainChain() }
        try? await Task.sleep(for: .milliseconds(30)); gdict[1]?.waiters += 1; await t.value
        print("MainActor global ?.=", gdict[1]!)
    }
}
