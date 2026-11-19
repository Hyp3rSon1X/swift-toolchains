import Testing

extension Tag { @Tag static var slow: Self }

@Suite("Math", .tags(.slow))
struct MathTests {
  @Test("adds") func add() { #expect(1 + 1 == 2) }
  @Test(arguments: [1, 2, 3]) func param(x: Int) throws {
    let y = try #require(Optional(x))
    #expect(y > 0, "positive")
    #expect(throws: Never.self) { _ = x }
  }
  @Test(.disabled("no")) func off() async { #expect(Bool(true)) }
}

@Test func free() { #expect(String(1) == "1") }
