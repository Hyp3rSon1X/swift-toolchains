import Foundation

struct P { var n: Int; var s: String }
let pred = #Predicate<P> { $0.n > 3 && $0.s.contains("a") }
let expr = #Expression<P, Int> { $0.n + 1 }
