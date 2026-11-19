// Probe E: other await shapes, N each (default 100,000): protocol witness, closure, throwing, generic,
// async let, TaskGroup child, continuation resumed at once, and MainActor.run from a detached task.
protocol Stepper { func step(_ x: Int) async -> Int }
struct S: Stepper { func step(_ x: Int) async -> Int { x &+ 1 } }
struct Err: Error {}
@inline(never) func throwing(_ x: Int) async throws -> Int { if x < 0 { throw Err() }; return x &+ 1 }
@inline(never) func generic<T: Stepper>(_ t: T, _ x: Int) async -> Int { await t.step(x) }
@inline(never) func viaContinuation(_ x: Int) async -> Int {
    await withCheckedContinuation { c in c.resume(returning: x &+ 1) }
}
@inline(never) func viaUnsafeContinuation(_ x: Int) async -> Int {
    await withUnsafeContinuation { c in c.resume(returning: x &+ 1) }
}

@main struct PE {
    static func main() async {
        let n = CommandLine.arguments.count > 2 ? (Int(CommandLine.arguments[2]) ?? 100_000) : 100_000
        let which = CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "all"
        func run(_ tag: String, _ body: (Int) async throws -> Int) async {
            guard which == "all" || which == tag else { return }
            var s = 0
            do {
                for i in 0..<n { progress(tag, i); s = try await body(s) }
            } catch { print("pe \(tag) error \(error)"); return }
            print("pe \(tag) ok n=\(n) s=\(s)")
        }
        let st: any Stepper = S()
        let cl: (Int) async -> Int = { $0 &+ 1 }
        await run("witness") { await st.step($0) }
        await run("closure") { await cl($0) }
        await run("throwing") { try await throwing($0) }
        await run("generic") { await generic(S(), $0) }
        await run("cont") { await viaContinuation($0) }
        await run("ucont") { await viaUnsafeContinuation($0) }
        await run("asynclet") { x in async let y = x &+ 1; return await y }
        await run("group") { x in
            await withTaskGroup(of: Int.self) { g in g.addTask { x &+ 1 }; return await g.next()! }
        }
        if which == "all" || which == "mainactor" {
            let r = await Task.detached { () async -> Int in
                var s = 0
                for i in 0..<n { progress("mainactor", i); s = await MainActor.run { [s] in s &+ 1 } }
                return s
            }.value
            print("pe mainactor ok n=\(n) s=\(r)")
        }
    }
}
