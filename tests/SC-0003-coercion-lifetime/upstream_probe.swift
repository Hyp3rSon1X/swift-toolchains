// Upstream regression test for the same bug, verbatim program from
// swiftlang/swift test/IRGen/async_coercion_frame_lifetime_exec.swift
// (PR #91312, commit fe3020b7c6d, main only). Expected on an affected
// toolchain at -O: "self=Holder(a: 0, b: 0) a=0 b=0 id=66" (no crash);
// fixed or -Onone: "self=Holder(a: 67108864, b: 6) a=67108864 b=6 id=66".

enum Project {
    case projectID((UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8))
}
struct Holder {
    let a = 67_108_864
    let b = 6
    func guardProbe(of project: Project) async {
        guard case .projectID(let id) = project else { return }
        print("guard: self=\(self) a=\(a) b=\(b) id=\(id.0)")
    }
}
await Holder().guardProbe(of: .projectID((66, 2, 3, 4, 5, 6, 7, 8, 9)))
