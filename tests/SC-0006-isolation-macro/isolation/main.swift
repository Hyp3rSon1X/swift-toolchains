// SC-0006 executable variant. Built with -enable-upcoming-feature NonisolatedNonsendingByDefault, so
// `run` and the async closure type are both nonisolated(nonsending): they run on their caller's
// isolation, and #isolation inside the closure is the isolation of whoever called run(_:).
func run(_ body: @escaping @Sendable () async -> Bool) async -> Bool { await body() }

// Called from the MainActor, so run (and the closure) inherit MainActor: must be true.
@MainActor func probeMain() async -> Bool {
    await run { #isolation === MainActor.shared }
}

// @concurrent forces the caller onto the global executor with no actor isolation, so run and the
// closure see nil isolation: #isolation === MainActor.shared must be false.
@concurrent func probeConcurrent() async -> Bool {
    await run { #isolation === MainActor.shared }
}

let a = await probeMain()
let b = await probeConcurrent()
print("main: \(a)")
print("concurrent: \(b)")
