// SC-0036 gate reproducer: the fork's test/Interpreter/coercion_alloca_alignment.swift without the lit lines.
// Expected output, four lines: "3 clear(SIMD4<Float>(0.25, 0.5, 0.75, 1.0))",
// "clear clear(SIMD4<Float>(0.25, 0.5, 0.75, 1.0)) false", "preserve load true", "true false".
// Stock Linux x86_64 at -Onone: signal 11 (aligned movaps reads an 8-byte aligned coercion temporary).

// Values whose Swift type is more aligned (16 bytes) than the LLVM types of
// their native explosion are coerced through a temporary alloca. At -Onone,
// IRGen forwards that temporary as the source of a copy into a local, and the
// copy assumes the Swift type's alignment. With an 8-byte aligned temporary,
// x86_64 used movaps for the copy and crashed.

enum LoadAction: Equatable { case load, clear(SIMD4<Float>), dontCare }
enum StoreAction { case store, dontCare }

struct Row {
  enum Intent { case clear, preserve, other }
  var intent: Intent
  var earlier: Bool?
  var readLater: Bool?
  var imported: Bool?
  var load: LoadAction
  var store: StoreAction
  var memoryless: Bool

  static let clearValue = SIMD4<Float>(0.25, 0.5, 0.75, 1)

  // Each element is the direct result of the initializer, stored into a
  // temporary before it is moved into the array.
  static let table: [Row] = [
    Row(intent: .clear, earlier: nil, readLater: false, imported: false,
        load: .clear(clearValue), store: .dontCare, memoryless: true),
    Row(intent: .clear, earlier: nil, readLater: true, imported: nil,
        load: .clear(clearValue), store: .store, memoryless: false),
    Row(intent: .preserve, earlier: true, readLater: true, imported: nil,
        load: .load, store: .store, memoryless: false),
  ]
}

// The argument is stored into a local.
@inline(never)
func describe(_ row: Row) -> String {
  var copy = row
  copy.memoryless.toggle()
  return "\(copy.intent) \(copy.load) \(copy.memoryless)"
}

print(Row.table.count, Row.table[0].load)
print(describe(Row.table[0]))
print(describe(Row.table[2]))

// The synthesized hashValue getter stores `self` into a local (swiftlang/swift#93129). Linux only, so no
// availability annotations (the lit test uses the SwiftStdlib 6.0 macro).
enum Value: Hashable {
  case small(UInt32)
  case wide(UInt128, UInt8)
}

let v = Value.wide(1, 64)
print(v.hashValue == Value.wide(1, 64).hashValue, v == .small(1))
