struct Handle: ~Copyable { borrowing func check() {} }
func take(_ h: borrowing Handle) {}
func run(_ f: @escaping () -> Void) { f() }
func p10() { let g = { h in take(h); run { h.check() } }; g(Handle()) }
