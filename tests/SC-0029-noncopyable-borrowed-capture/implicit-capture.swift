struct Handle: ~Copyable { borrowing func check() {} }
func withHandle<T>(_ body: (borrowing Handle) -> T) -> T {
    let handle = Handle()
    return body(handle)
}
struct Box { let action: () -> Void }
func escaped() -> Box {
    withHandle { handle in Box(action: { handle.check() }) }
}
