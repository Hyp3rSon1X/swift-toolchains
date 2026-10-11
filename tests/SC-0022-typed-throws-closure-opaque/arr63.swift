struct SE: Error { var m: String }
@main struct M { static func main() { let a: [() throws(SE) -> Int] = [{ () throws(SE) -> Int in throw SE(m: "a") }]; do { _ = try a[0]() } catch { print(error) } } }
