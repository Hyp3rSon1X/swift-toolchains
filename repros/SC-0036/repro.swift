// SC-0036: Linux x86_64, -Onone only: an array literal of a struct that holds an enum with a SIMD4<Float> payload
// next to three Bool? fields crashes at run time (signal 11, "Bad pointer dereference at 0x0"): an aligned `movaps`
// reads a stack temporary whose address is 8 mod 16. -O runs; macOS arm64 runs. Reported by SwixForge 2026-11-15
// (.scratch/evidence/render-p4a/p4a-b0-graph/linux-simd-repro.log).
public enum LoadAction: Hashable, Sendable { case load, clear(SIMD4<Float>), dontCare }
public enum StoreAction: Hashable, Sendable { case store, dontCare }
struct Row: Sendable {
    enum Intent: Sendable { case clear, preserve, other }
    var intent: Intent
    var earlier: Bool?
    var readLater: Bool?
    var imported: Bool?
    var load: LoadAction
    var store: StoreAction
    var memoryless: Bool
    static let clearValue = SIMD4<Float>(0.25, 0.5, 0.75, 1)
    static let table: [Row] = [
        Row(intent: .clear, earlier: nil, readLater: false, imported: false, load: .clear(clearValue), store: .dontCare, memoryless: true),
        Row(intent: .clear, earlier: nil, readLater: true, imported: nil, load: .clear(clearValue), store: .store, memoryless: false),
        Row(intent: .preserve, earlier: true, readLater: true, imported: nil, load: .load, store: .store, memoryless: false),
    ]
}
print(Row.table.count, Row.table[0].load)
