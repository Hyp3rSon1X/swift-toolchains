import Foundation

let date = Date(timeIntervalSince1970: 0)
let formatter = ISO8601DateFormatter()
let json = try! JSONSerialization.data(withJSONObject: ["n": 42], options: [.sortedKeys])
print("hi from Foundation", formatter.string(from: date), String(decoding: json, as: UTF8.self), URL(string: "https://example.org/a?b=c")!.host ?? "nil")
