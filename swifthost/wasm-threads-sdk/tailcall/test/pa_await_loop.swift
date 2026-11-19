// Probe A: N awaits in a row on a trivial async func that never suspends (default 1,000,000).
// Without tail calls every await nests a frame: callee "returns" by calling the caller's resume function.
@inline(never) func trivial(_ x: Int) async -> Int { x &+ 1 }

@main struct PA {
    static func main() async {
        let n = argN(1_000_000)
        var s = 0
        for i in 0..<n {
            progress("A", i)
            s = await trivial(s)
        }
        print("pa await-loop ok n=\(n) s=\(s)")
    }
}
