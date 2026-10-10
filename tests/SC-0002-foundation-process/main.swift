// SC-0002 reproducer: Foundation `Process` on Linux over-reads `dirent.d_name` in findMaximumOpenFromProcSelfFD()
// (swift-corelibs-foundation #5472). Every Process.run() lists /proc/self/fd with readdir() to close the
// descriptors the child must not inherit; the stock code copies the whole 256-byte `d_name` array of each entry.
// With enough open descriptors glibc's readdir buffer fills up, the last records sit at its very end, and the
// copy reads past it (heap over-read; SIGSEGV only if the next page is unmapped, so it is a rare crash but a
// deterministic valgrind/ASan finding).
//
// Usage: repro <openFDs> <launches> <threads> [verifyEvery]
//   openFDs      descriptors to hold open in the parent (without O_CLOEXEC) while launching
//   launches     total number of Process launches, spread over <threads> threads
//   verifyEvery  every Nth launch runs `/bin/ls -1 /proc/self/fd` and checks that the child inherited none of the
//                <openFDs> descriptors above 3 (ls itself holds 3) (default 50); the others run /bin/true
// Exit: 0 ok, 1 a child inherited an unexpected descriptor or a launch failed, 139 SIGSEGV in the stock library.
import Foundation

setvbuf(stdout, nil, _IOLBF, 0)
let args = CommandLine.arguments
guard args.count >= 4, let openFDs = Int(args[1]), let launches = Int(args[2]), let threads = Int(args[3]) else {
    print("usage: repro <openFDs> <launches> <threads> [verifyEvery]"); exit(2)
}
let verifyEvery = args.count > 4 ? Int(args[4]) ?? 50 : 50

var lim = rlimit()
#if canImport(Musl)
let nofile = RLIMIT_NOFILE  // musl declares the resource as a plain Int32
#else
let nofile = __rlimit_resource_t(RLIMIT_NOFILE.rawValue)
#endif
getrlimit(nofile, &lim)
lim.rlim_cur = lim.rlim_max
setrlimit(nofile, &lim)

var held = [Int32]()
for _ in 0..<openFDs {
    let fd = open("/dev/null", O_RDONLY)  // deliberately inheritable: Process must close these in the child
    if fd < 0 { print("open failed after \(held.count) descriptors: errno \(errno)"); exit(2) }
    held.append(fd)
}

let heldSet = Set(held)

final class Counter: @unchecked Sendable {
    private let lock = NSLock()
    private var launched = 0, verified = 0, failures = 0
    func next() -> Int { lock.lock(); defer { lock.unlock() }; launched += 1; return launched }
    func verifiedOne() { lock.lock(); verified += 1; lock.unlock() }
    func fail(_ msg: String) { lock.lock(); failures += 1; let f = failures; lock.unlock(); if f <= 10 { print("FAIL: \(msg)") } }
    var summary: (Int, Int) { lock.lock(); defer { lock.unlock() }; return (verified, failures) }
}
let counter = Counter()

func launch(_ n: Int) {
    let p = Process()
    if n % verifyEvery == 0 {
        p.executableURL = URL(fileURLWithPath: "/bin/ls")
        p.arguments = ["-1", "/proc/self/fd"]
        let pipe = Pipe()
        p.standardOutput = pipe
        do { try p.run() } catch { counter.fail("run() threw: \(error)"); return }
        let data = pipe.fileHandleForReading.readDataToEndOfFile()
        p.waitUntilExit()
        let names = String(decoding: data, as: UTF8.self).split(separator: "\n").compactMap { Int($0) }
        let unexpected = names.filter { $0 > 3 && heldSet.contains(Int32($0)) }  // fds from other threads' concurrent launches are a separate, known race
        if p.terminationStatus != 0 || !unexpected.isEmpty {
            counter.fail("launch \(n): status \(p.terminationStatus), child inherited descriptors \(unexpected.prefix(8))")
        } else { counter.verifiedOne() }
    } else {
        p.executableURL = URL(fileURLWithPath: "/bin/true")
        do { try p.run() } catch { counter.fail("run() threw: \(error)"); return }
        p.waitUntilExit()
        if p.terminationStatus != 0 { counter.fail("launch \(n): /bin/true status \(p.terminationStatus)") }
    }
}

let start = Date()
let group = DispatchGroup()
for _ in 0..<threads {
    group.enter()
    Thread.detachNewThread {
        while true {
            let n = counter.next()
            if n > launches { break }
            launch(n)
        }
        group.leave()
    }
}
group.wait()
let (v, f) = counter.summary
print("\(f == 0 ? "ok" : "FAILED") openFDs=\(held.count) launches=\(launches) verified=\(v) failures=\(f) seconds=\(Int(Date().timeIntervalSince(start)))")
exit(f == 0 ? 0 : 1)
