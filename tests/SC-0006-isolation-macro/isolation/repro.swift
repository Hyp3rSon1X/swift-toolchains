// SC-0006 (swiftlang/swift#92798), the reported reproducer: compile only (-c).
// Stock frontend aborts in the LLVM verifier ("PHINode should have one entry for each predecessor").
func run(_ body: @escaping @Sendable () async -> Bool) async -> Bool { await body() }
@MainActor func probe() async -> Bool {
    await run { #isolation === MainActor.shared }
}
