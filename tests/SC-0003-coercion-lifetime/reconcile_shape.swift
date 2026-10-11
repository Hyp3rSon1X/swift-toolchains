// SC-0003: -O miscompile of async functions that receive an argument whose
// native explosion is coerced through a stack temporary.
//
// IRGen (lib/IRGen/GenCall.cpp, irgen::allocateForCoercion) sizes the
// llvm.lifetime.start/end markers of the coercion temporary in BITS instead of
// bytes (192 for a 24-byte { i64, i64, i8 }). CoroSplit moves the temporary
// into the async context frame; the 8x oversized lifetime range then covers
// the frame slots where the ramp spills `self` and the other arguments, and
// DSE deletes those spill stores. The first resume partial reloads `self`
// from never-written task-allocator memory.
//
// Shape taken from MailSteward's ActionDispatchEngine.reconcile(intentID:
// kind:graphReads:): actor method, UUID-like 16-byte payload enum parameter,
// first await hops to another actor. Expected on an affected toolchain at -O:
// crash "Bad pointer dereference at 0x005c5c5c5c5c5ccc" in
// Engine.reconcile(intentID:kind:) + 56 (self == the 0x5c pattern, +0x70 is
// the `store` field load). Expected at -Onone or on a fixed compiler: PASS.

typealias Bytes16 = (UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8,
                     UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8)
struct ID { var bytes: Bytes16 }                      // like Foundation.UUID
enum Destination: UInt8 { case inbox, review, quarantine }
enum Kind { case move(messageID: ID, destination: Destination) }  // like ActionKind

actor Store {
    func lookup(_ key: Int) -> Int { key &+ 1 }
}

actor Engine {
    let store = Store()

    @inline(never)
    func reconcile(intentID: Int, kind: Kind) async -> Int {
        guard case let .move(_, destination) = kind else { return 0 }
        let looked = await store.lookup(intentID)
        return looked &* 10 &+ Int(destination.rawValue)
    }
}

// Leaves a 0x5c pattern in the task allocator: scribble's async context sits
// at the address Engine.reconcile's context gets next, so a slot reconcile
// reads without having written it reads back as 0x5c5c5c5c5c5c5c5c.
@inline(never)
func scribble(_ s: [Int]) async -> Int {
    let a0 = s[0], a1 = s[1], a2 = s[2], a3 = s[3], a4 = s[4], a5 = s[5], a6 = s[6], a7 = s[7]
    let b0 = s[8], b1 = s[9], b2 = s[10], b3 = s[11], b4 = s[12], b5 = s[13], b6 = s[14], b7 = s[15]
    await Task.yield()
    return a0 ^ a1 ^ a2 ^ a3 ^ a4 ^ a5 ^ a6 ^ a7 ^ b0 ^ b1 ^ b2 ^ b3 ^ b4 ^ b5 ^ b6 ^ b7
}

// Run-time value, so the pattern is not constant-folded away.
let pattern = Array(repeating: 0x5c5c_5c5c_5c5c_5c5c &+ (CommandLine.arguments.count &- 1), count: 16)
let id = ID(bytes: (1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16))
let engine = Engine()
_ = await scribble(pattern)
let result = await engine.reconcile(intentID: 41, kind: .move(messageID: id, destination: .review))
print(result == 421 ? "PASS (result \(result))" : "FAIL: miscompiled (result \(result), expected 421)")
