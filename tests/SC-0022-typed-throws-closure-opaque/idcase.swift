enum EE: Error { case a, b, c }
func id<T>(_ x: T) -> T { x }
func callSync<T>(_ op: () throws -> T) -> String { do { _ = try op(); return "ok" } catch { return "\(error)" } }
@main struct M { static func main() {
  let f = id({ () throws(EE) -> Int in throw .a })
  print("1 id", callSync(f))
}}
