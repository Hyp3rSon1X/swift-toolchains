// SC-0004 generic variant: a generic class whose field `isSealed` follows a
// resilient field but precedes the generic one, so it also uses a direct
// field-offset global (written during generic metadata instantiation).
import Lib

final class Completion {}

final class Segment<T> {
    var desired: Token
    var isSealed = false
    let completion = Completion()
    var extra: T

    init(desired: Token, extra: T) {
        self.desired = desired
        self.extra = extra
    }
}

final class Owner {
    var segments: [Int: Segment<Int>] = [:]

    @inline(never)
    func offer(_ key: Int, _ id: Token) -> Bool {
        if let segment = segments[key], !segment.isSealed {
            segment.desired = id
            return true
        }
        let segment = Segment(desired: id, extra: key)
        segments[key] = segment
        return false
    }
}

let owner = Owner()
_ = owner.offer(1, Token(42))
let first = owner.segments[1]!
let isa = Unmanaged.passUnretained(first).toOpaque().load(as: UInt.self)
let metadata = unsafeBitCast(Segment<Int>.self as AnyClass, to: UInt.self)
print("isa=0x\(String(isa, radix: 16)) metadata=0x\(String(metadata, radix: 16)) intact=\(isa == metadata) isSealed=\(first.isSealed)")
