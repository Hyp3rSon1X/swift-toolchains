// SC-0037 (same bug as swiftlang/swift#92412): reported by SwixForge (Games) 2026-11-16. Linux variant: no availability checks.
// IRGen/LifetimeDependenceScopeFixup: the Array returned by a throwing call, borrowed as `.span` directly in another call's
// argument, gives invalid LLVM IR ("Instruction does not dominate all uses", release of a phi). A local `let` compiles.
func make(_ n: Int) throws -> [Int] {
  if n < 0 { throw CancellationError() }
  return Array(0..<n)
}
func total(_ s: Span<Int>) -> Int {
  var t = 0
  for i in s.indices { t += s[i] }
  return t
}

func run(_ n: Int) throws -> Int {
  total(try make(n).span)
}
do {
  print("total \(try run(5))")
}
