// Intentionally unsafe standalone canary, reduced from the ee0f870 AF-39
// diagnosis. NOT a package target and NOT a production lint exemption.
// The typed operation is coerced to Task's `throws(any Error)` operation;
// Swift 6.4.0 corrupts the stored error read through Task.result.

enum CanaryFailure: Error, Equatable { case rejected, cancelled }

@main struct TypedThrowsTaskCanary {
    static func main() async {
        await probe()
    }

    // Deliberately off actor: a main-actor entry otherwise triggers an
    // unrelated sending diagnostic and never exercises the runtime defect.
    @concurrent static func probe() async {
        // Control: the untyped shape removed the ee0f870 failures. A broken
        // control means this toolchain/runtime cannot certify the canary.
        for _ in 0..<2_000 {
            let task = Task { () async throws -> Int in
                throw CanaryFailure.cancelled
            }
            guard case .failure(let error) = await task.result,
                error as? CanaryFailure == .cancelled
            else { fatalError("typed-throws canary: untyped control failed") }
        }

        for iteration in 0..<2_000 {
            let task = Task { @Sendable () async throws(CanaryFailure) -> Int in
                throw .cancelled
            }
            guard case .failure(let error) = await task.result,
                error as? CanaryFailure == .cancelled
            else {
                fatalError("typed-throws Task error storage corrupted at iteration \(iteration)")
            }
        }
        print("typed-throws canary: control and 2000 typed Task results passed")
    }
}
