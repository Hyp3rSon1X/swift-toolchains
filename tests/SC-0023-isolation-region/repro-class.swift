final class NS { var x = 0 }
func pass<T>(_ value: T, isolation: isolated (any Actor)? = #isolation) async -> T { value }
func a() async -> Int { let v = await pass(NS()); return v.x }
struct It: AsyncIteratorProtocol { mutating func next() async -> NS? { nil } }
func b() async -> Int { var it = It(); return await it.next(isolation: #isolation)?.x ?? 0 }
func c() async -> Int { let v = await pass(NS(), isolation: #isolation); return v.x }
