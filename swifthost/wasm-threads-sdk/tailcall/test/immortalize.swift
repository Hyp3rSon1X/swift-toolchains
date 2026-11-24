// SC-0047 reproducer: calls _swift_stdlib_immortalize through the SwiftShims (C) declaration, exactly as
// swift-collections 1.7.6 does (InternalCollectionsUtilities/Immortalize.swift, inlined into the client).
// Then checks that the object is immortal: unbalanced releases do not free it, and retain/release leave the
// refcount word unchanged (swiftlang/swift#94088). Exit code 0 = all checks hold.
import SwiftShims
#if canImport(WASILibc)
import WASILibc
#elseif canImport(Darwin)
import Darwin
#elseif canImport(Glibc)
import Glibc
#endif

nonisolated(unsafe) var deinits = 0

final class Canary {
  var payload = 42
  deinit { deinits += 1 }
}

// HeapObject is { metadata pointer, inline refcount word }; on wasm32 both are 32 bits wide.
@inline(never)
func refcountWord(_ object: AnyObject) -> UInt {
  Unmanaged.passUnretained(object).toOpaque()
    .load(fromByteOffset: MemoryLayout<UnsafeRawPointer>.size, as: UInt.self)
}

func hex(_ v: UInt) -> String { "0x" + String(v, radix: 16) }

@inline(never)
func run() -> Int32 {
  var failures: Int32 = 0
  func check(_ ok: Bool, _ what: String) {
    print("\(ok ? "PASS" : "FAIL") \(what)")
    if !ok { failures += 1 }
  }
  let object = Canary()
  let um = Unmanaged.passUnretained(object)
  print("refcount word before immortalize: \(hex(refcountWord(object)))")
  _swift_stdlib_immortalize(um.toOpaque())
  let immortal = refcountWord(object)
  print("refcount word after immortalize:  \(hex(immortal))")
  um.release()
  let afterRelease = refcountWord(object)
  print("refcount word after one release:  \(hex(afterRelease))")
  check(afterRelease == immortal, "one unbalanced release leaves the refcount word unchanged")
  for _ in 0..<1000 { _ = um.retain(); um.release() }
  check(refcountWord(object) == immortal, "1000 retain/release pairs leave the refcount word unchanged")
  for _ in 0..<10 { um.release() }
  check(refcountWord(object) == immortal, "10 more unbalanced releases leave the refcount word unchanged")
  check(deinits == 0 && object.payload == 42, "the object was never freed (deinit count \(deinits))")
  _fixLifetime(object)
  return failures
}

let failures = run()
print(failures == 0 ? "immortalize OK" : "immortalize FAILED (\(failures))")
exit(failures)
