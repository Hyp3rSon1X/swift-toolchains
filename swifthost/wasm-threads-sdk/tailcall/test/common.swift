// Progress goes to stderr unbuffered, so the last line before a stack-overflow trap is the depth reached.
import WASILibc

func progress(_ tag: String, _ i: Int) {
    if (i < 20_000 && i % 100 == 0) || i % 100_000 == 0 {
        fputs("\(tag) i=\(i)\n", stderr)
    }
}

func argN(_ def: Int) -> Int {
    CommandLine.arguments.count > 1 ? (Int(CommandLine.arguments[1]) ?? def) : def
}
