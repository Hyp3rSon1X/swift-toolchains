struct Handle: ~Copyable { borrowing func check() {} }
func take(_ h: borrowing Handle) {}
func run(_ f: @escaping () -> Void) { f() }
func p9() { let g = { h in take(h); func local() { h.check() }; local() }; g(Handle()) }
