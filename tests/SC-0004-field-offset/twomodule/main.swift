// SC-0004 without Foundation: same shape as ../reduction.swift, with the
// resilient field type coming from Lib (built with -enable-library-evolution).
import Lib

final class Completion {}

final class Segment {
    var desired: Token
    var isSealed = false
    let completion = Completion()

    init(desired: Token) {
        self.desired = desired
    }
}

final class Owner {
    var segments: [Int: Segment] = [:]

    @inline(never)
    func offer(_ key: Int, _ id: Token) -> Bool {
        if let segment = segments[key], !segment.isSealed {
            segment.desired = id
            return true
        }
        let segment = Segment(desired: id)
        segments[key] = segment
        return false
    }
}

let owner = Owner()
_ = owner.offer(1, Token(42))
let first = owner.segments[1]!
let isa = Unmanaged.passUnretained(first).toOpaque().load(as: UInt.self)
let metadata = unsafeBitCast(Segment.self as AnyClass, to: UInt.self)
print("isa=0x\(String(isa, radix: 16)) metadata=0x\(String(metadata, radix: 16)) intact=\(isa == metadata)")
