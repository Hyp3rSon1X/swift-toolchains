// Reproducer for swiftlang/swift-corelibs-libdispatch#949 (use-after-free in the epoll backend).
// Linux only. Build: swiftc -O main.swift -o repro
// Run: ./repro <seconds> <parallel> <tool> <dispatchio|blocking>, e.g. ./repro 30 16 /bin/echo dispatchio
// Swift 6.4.0's libdispatch crashes within seconds at _dispatch_event_loop_drain + 1130.
//
// Mimics Swift Build 6.4's Processes.getOutput on Linux: Foundation Process + Pipe, both pipes
// drained concurrently by a fresh one-shot DispatchIO.read of 4 KiB per chunk until EOF
// (SWBUtil/Dispatch+Async.swift readChunk/dataStream), many processes in parallel.
import Dispatch
import Foundation

let seconds = Double(CommandLine.arguments.dropFirst().first ?? "60") ?? 60
let parallel = Int(CommandLine.arguments.dropFirst(2).first ?? "16") ?? 16
let tool = CommandLine.arguments.dropFirst(3).first ?? "/bin/echo"
// "dispatchio" (Swift Build 6.4's way) or "blocking" (plain read(2) on a detached thread).
let mode = CommandLine.arguments.dropFirst(4).first ?? "dispatchio"

func blockingDrain(_ fd: Int32) async -> Int {
    await withCheckedContinuation { (c: CheckedContinuation<Int, Never>) in
        Thread.detachNewThread {
            var buffer = [UInt8](repeating: 0, count: 4096)
            var total = 0
            while true {
                let n = buffer.withUnsafeMutableBytes { read(fd, $0.baseAddress, 4096) }
                if n > 0 { total += n; continue }
                if n < 0 && errno == EINTR { continue }
                break
            }
            c.resume(returning: total)
        }
    }
}

func readChunk(_ fd: Int32) async -> Int {
    await withCheckedContinuation { (c: CheckedContinuation<Int, Never>) in
        DispatchIO.read(fromFileDescriptor: fd, maxLength: 4096, runningHandlerOn: .global()) { data, error in
            c.resume(returning: error != 0 ? 0 : data.count)
        }
    }
}

func drain(_ fd: Int32) async -> Int {
    var total = 0
    while true {
        let n = await readChunk(fd)
        if n == 0 { return total }
        total += n
    }
}

func runOne() async -> Bool {
    let p = Process()
    p.executableURL = URL(fileURLWithPath: tool)
    p.arguments = tool.hasSuffix("echo") ? ["hello", "world"] : ["-v"]
    let out = Pipe(), err = Pipe()
    p.standardOutput = out
    p.standardError = err
    do { try p.run() } catch { return false }
    let outFD = out.fileHandleForReading.fileDescriptor, errFD = err.fileHandleForReading.fileDescriptor
    async let a = mode == "blocking" ? blockingDrain(outFD) : drain(outFD)
    async let b = mode == "blocking" ? blockingDrain(errFD) : drain(errFD)
    _ = await (a, b)
    p.waitUntilExit()
    withExtendedLifetime((out, err)) {}
    return true
}

let deadline = Date().addingTimeInterval(seconds)
let start = Date()
var count = 0
await withTaskGroup(of: Bool.self) { group in
    for _ in 0..<parallel { group.addTask { await runOne() } }
    while await group.next() != nil {
        count += 1
        if count % 2000 == 0 { FileHandle.standardError.write(Data("progress processes=\(count) seconds=\(Int(Date().timeIntervalSince(start)))\n".utf8)) }
        if Date() < deadline { group.addTask { await runOne() } }
    }
}
print("ok processes=\(count) seconds=\(Int(Date().timeIntervalSince(start)))")
