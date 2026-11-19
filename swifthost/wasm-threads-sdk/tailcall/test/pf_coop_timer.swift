// Probe F: the cooperative executor must not sleep for a pending timer while other jobs are ready
// (swiftlang/swift#93586). Scenario 1: a 20 ms sleep while another task has a 1 s sleep pending.
// Scenario 2: a task doing 10 Task.yield() while the main task sleeps 1 s.
@main struct PF {
    static func main() async {
        let clock = ContinuousClock()
        let t0 = clock.now
        let long = Task { () -> ContinuousClock.Instant in
            try? await Task.sleep(for: .seconds(1))
            return clock.now
        }
        await Task.yield()   // let the long task start and arm its 1 s timer
        let s = clock.now
        try? await Task.sleep(for: .milliseconds(20))
        let dSleep = clock.now - s
        let y = clock.now
        for _ in 0..<10 { await Task.yield() }
        let dYield = clock.now - y
        let end = await long.value
        print("pf1 sleep20ms=\(dSleep) yield10-after=\(dYield) long1s-done-after=\(end - t0)")

        let y0 = clock.now
        let yielder = Task.detached { () -> Duration in
            let a = clock.now
            for _ in 0..<10 { await Task.yield() }
            return clock.now - a
        }
        try? await Task.sleep(for: .seconds(1))
        let dy = await yielder.value
        print("pf2 detached-yield10=\(dy) total=\(clock.now - y0)")
    }
}
