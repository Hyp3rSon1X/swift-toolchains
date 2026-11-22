struct Handle: ~Copyable { borrowing func check() {} }
func withHandle<T>(_ body: (borrowing Handle) -> T) -> T {
    let handle = Handle()
    return body(handle)
}
func synchronous() {
    withHandle { handle in
        func invoke() { handle.check() }
        invoke()
    }
}
