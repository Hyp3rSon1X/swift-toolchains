// SC-0019 (swiftlang/swift#92799): #isolation in a nonisolated(nonsending) function called from an
// actor method over-released the actor (use after free). Must be named main.swift (top-level code).
actor Worker {
    deinit { print("Worker.deinit") }
    func run() async -> String { await describeIsolation() }
}
nonisolated(nonsending) func describeIsolation() async -> String {
    String(describing: #isolation)
}
func test() async {
    let worker = Worker()
    print("1:", await worker.run())
    print("2:", await worker.run())
    print("end of test()")
}
await test()
print("done")
