// Swift Testing's #expect(processExitsWith:) expands to `__requiringAwait({ ... })()` inside a
// local `@Sendable func ... async`. Reduced: requiringAwait has the same signature.
func requiringAwait<T>(_ value: consuming T, isolation: isolated (any Actor)? = #isolation) async -> T { value }
func work() {}

@Sendable func body() async {
    _ = await requiringAwait({ @Sendable in work() })()
}
