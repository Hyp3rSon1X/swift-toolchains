// SC-0034, second symptom (reported by SwixForge/Games 2026-11-16, ForgeAudio MixerControl.reclaimAfterRenderer):
// with -sanitize=thread, SILGen applied builtin tsanInoutAccess to the load_borrow of a ~Copyable self and to the
// struct_extract of its `let` class reference. When the struct's first stored property is not a pointer (UInt64 here),
// IRGen bitcasts that scalar to ptr and the LLVM verifier aborts: "Invalid bitcast", "Broken module found" (signal 6).
// Workaround variant: a local copy of the let class reference before use compiles on stock compilers.
// Build with: swiftc -sanitize=thread -Onone tsan-bitcast-workaround.swift
struct Store { var n = 7; mutating func freeRetiring() { n = 0 } }
final class Channel { var store = Store() }
struct Control: ~Copyable {
  var next: UInt64 = 0
  let channel: Channel
  init() { channel = Channel() }
  mutating func reclaim() { let channel = self.channel; channel.store.freeRetiring() }
}
var control = Control()
control.reclaim()
print("reclaimed \(control.channel.store.n)")
