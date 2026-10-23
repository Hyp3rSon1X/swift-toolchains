struct Entry { var log: [Int] = []; var waiters = 0
  mutating func set(_ v: Int) { log.append(v) }
  mutating func setAsync(_ v: Int) async { try? await Task.sleep(for: .milliseconds(100)); log.append(v) } }
actor Box {
    var dict: [Int: [Int]] = [1: []]
    var entries: [Int: Entry] = [1: Entry()]
    var opt: Entry? = Entry()
    func slow() async -> Int { try? await Task.sleep(for: .milliseconds(100)); return 42 }
    func poke() { dict[1]?.append(7); entries[1]?.waiters += 1; opt?.waiters += 1 }
    func append() async { dict[1]?.append(await slow()) }
    func mutate() async { entries[1]?.set(await slow()) }
    func mutateOpt() async { opt?.set(await slow()) }
    func reset() { dict = [1: []]; entries = [1: Entry()]; opt = Entry() }
    func snap() -> String { "dict=\(dict[1]!) entry=\(entries[1]!.log)/w\(entries[1]!.waiters) opt=\(opt!.log)/w\(opt!.waiters)" }
}
@main struct R {
    static func run(_ n: String, _ b: Box, _ body: @escaping @Sendable () async -> Void) async {
        await b.reset(); let t = Task { await body() }
        try? await Task.sleep(for: .milliseconds(30)); await b.poke(); await t.value
        print(n, await b.snap())
    }
    static func main() async {
        let b = Box()
        await run("dict[k]?.append(await)  ", b) { await b.append() }
        await run("entries[k]?.set(await)  ", b) { await b.mutate() }
        await run("opt?.set(await)         ", b) { await b.mutateOpt() }
    }
}
