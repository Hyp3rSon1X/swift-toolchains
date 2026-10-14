final class NS { var x = 0 }
struct Seq: AsyncSequence { struct AsyncIterator: AsyncIteratorProtocol { mutating func next() async -> NS? { nil } }; func makeAsyncIterator() -> AsyncIterator { .init() } }
nonisolated(nonsending) func d() async -> Int { var s = 0; for await v in Seq() { s += v.x }; return s }
@concurrent func e() async -> Int { var s = 0; for await v in Seq() { s += v.x }; return s }
@MainActor func f() async -> Int { var s = 0; for await v in Seq() { s += v.x }; return s }
