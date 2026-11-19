// Probe 1: a TaskGroup summing 100 child tasks (expected 5050).
@main struct P1 {
    static func main() async {
        let sum = await withTaskGroup(of: Int.self) { group in
            for i in 1...100 { group.addTask { i } }
            var s = 0
            for await v in group { s += v }
            return s
        }
        print("p1 taskgroup sum=\(sum)")
    }
}
