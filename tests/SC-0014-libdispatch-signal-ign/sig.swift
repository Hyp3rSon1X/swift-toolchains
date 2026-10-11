// SC-0014: DispatchSource signal sources on Linux after the signal's disposition was set to SIG_IGN.
// Apple's documented pattern is `signal(SIG, SIG_IGN)` + makeSignalSource. On Darwin (kqueue) the source
// fires regardless of the disposition. On Linux libdispatch installs its own sigaction only for the first
// source of a signal per process (static signals_with_unotes), so a later SIG_IGN leaves the signal ignored
// and the kernel discards it.
import Dispatch
#if canImport(Glibc)
import Glibc
#else
import Darwin
#endif

let q = DispatchQueue(label: "sig")
func delivered(_ signo: Int32, label: String) -> Bool {
  let sem = DispatchSemaphore(value: 0)
  let src = DispatchSource.makeSignalSource(signal: signo, queue: q)
  src.setEventHandler { sem.signal() }
  src.resume()
  usleep(100_000)  // registration is asynchronous; give the manager thread time to install the source
  kill(getpid(), signo)
  let ok = sem.wait(timeout: .now() + 2) == .success
  src.cancel()
  usleep(100_000)
  print("\(label): \(ok ? "delivered" : "NOT delivered")")
  return ok
}
func ignoredAfterResume(_ signo: Int32, label: String) -> Bool {
  let sem = DispatchSemaphore(value: 0)
  let src = DispatchSource.makeSignalSource(signal: signo, queue: q)
  src.setEventHandler { sem.signal() }
  src.resume()
  usleep(100_000)
  signal(signo, SIG_IGN)  // e.g. a library that ignores the signal after the source exists
  kill(getpid(), signo)
  let ok = sem.wait(timeout: .now() + 2) == .success
  src.cancel()
  print("\(label): \(ok ? "delivered" : "NOT delivered")")
  return ok
}

var fails = 0
signal(SIGHUP, { _ in })
if !delivered(SIGHUP, label: "0 control (no SIG_IGN)") { fails += 1 }
// 1. SIG_IGN first, then the first source (Apple's pattern): libdispatch installs its handler.
signal(SIGUSR1, SIG_IGN)
if !delivered(SIGUSR1, label: "1 SIG_IGN, first source") { fails += 1 }
// 2. Same signal again after something reset SIG_IGN (second source, handler not reinstalled).
signal(SIGUSR1, SIG_IGN)
if !delivered(SIGUSR1, label: "2 SIG_IGN again, second source") { fails += 1 }
// 3. A source exists, then the disposition becomes SIG_IGN.
if !ignoredAfterResume(SIGUSR2, label: "3 SIG_IGN after resume") { fails += 1 }
print(fails == 0 ? "PASS" : "FAIL \(fails)")
exit(fails == 0 ? 0 : 1)
