// SC-0037 (swiftlang/swift#92412): the Array returned by a throwing call, borrowed as `.span` directly in another call's
// argument. LifetimeDependenceScopeFixup put a destroy of the try_apply result into the error block, and every
// optimization level aborted with "Instruction does not dominate all uses" (LLVM verifier). Library part of the gate.
public enum MakeError: Error { case negative }

public func make(_ n: Int) throws -> [Int] {
  if n < 0 { throw MakeError.negative }
  return Array(0..<n)
}

public func makeTyped(_ n: Int) throws(MakeError) -> [UInt8] {
  if n < 0 { throw .negative }
  return Array(repeating: 7, count: n)
}

public func total(_ s: Span<Int>) -> Int {
  var t = 0
  for i in s.indices { t += s[i] }
  return t
}

public func byteSum(_ s: RawSpan) -> Int {
  var t = 0
  for i in 0..<s.byteCount { t += Int(s.unsafeLoad(fromByteOffset: i, as: UInt8.self)) }
  return t
}

// The reported shape (SwixForge): a throwing initializer decoding the bytes of a throwing call's result.
public struct Manifest {
  public let size: Int
  public init(decoding bytes: RawSpan) throws {
    guard bytes.byteCount > 0 else { throw MakeError.negative }
    size = byteSum(bytes)
  }
}

public struct Cache {
  public init() {}
  public func bytes(_ digest: Int) throws -> [UInt8] {
    if digest < 0 { throw MakeError.negative }
    return Array(repeating: 1, count: digest)
  }
}

public func run(_ n: Int) throws -> Int { total(try make(n).span) }

public func runTyped(_ n: Int) throws(MakeError) -> Int { byteSum(try makeTyped(n).span.bytes) }

public func load(_ cache: Cache, _ digest: Int) throws -> Manifest {
  try Manifest(decoding: try cache.bytes(digest).span.bytes)
}
