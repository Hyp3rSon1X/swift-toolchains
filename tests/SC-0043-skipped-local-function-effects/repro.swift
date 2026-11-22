import Testing

func cond() -> Bool { true }

// Trigger 1: the outer body declares a local type, so the skip-bodies job type-checks it, but the local function
// inside it (no local types of its own) has a skipped body that was never type-checked or macro-expanded.
func v3() throws {
    typealias T = Int
    func inner() throws { try #require(cond()) }
    try inner()
}

// Trigger 2: a local function inside a closure that initializes a stored property (no local type needed).
struct Holder {
    var x: Int = {
        func f() throws -> Int { try #require(cond()); return 1 }
        return try! f()
    }()
}
