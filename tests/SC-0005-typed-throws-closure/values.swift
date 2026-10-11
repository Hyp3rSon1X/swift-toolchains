// SC-0005 (swiftlang/swift#92312): typed-throws closures passed to generic untyped-throws parameters.
// Stock 6.4.0: the struct case crashes the compiler, the enum case reads back a corrupted error.
enum E: Error { case a, b, c }
struct P: Error { var x: Int; var y: String }
func generic<T>(_ op: () async throws -> T) async -> String {
    do { _ = try await op(); return "ok" } catch { return "\(error)" }
}
@main struct M {
    static func main() async {
        let t1 = Task { @Sendable () async throws(E) -> Int in throw .c }
        if case .failure(let e) = await t1.result { print("enum:", e) }
        let t2 = Task { @Sendable () async throws(P) -> Int in throw P(x: 42, y: "hello") }
        if case .failure(let e) = await t2.result { print("struct:", e) }
        print("generic:", await generic { () async throws(P) -> Int in throw P(x: 7, y: "g") })
    }
}
