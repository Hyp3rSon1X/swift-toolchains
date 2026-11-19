// Probe 4: small Foundation program (Date, JSONEncoder).
import Foundation

struct Item: Codable { var name: String; var when: Date; var tags: [String] }

@main struct P4 {
    static func main() throws {
        let enc = JSONEncoder()
        enc.outputFormatting = [.sortedKeys]
        enc.dateEncodingStrategy = .iso8601
        let item = Item(name: "probe", when: Date(timeIntervalSince1970: 86_400), tags: ["a", "b"])
        let data = try enc.encode(item)
        print("p4 json=\(String(decoding: data, as: UTF8.self))")
        let back = try JSONDecoder().decode([String: [Int]].self, from: Data("{\"k\":[1,2,3]}".utf8))
        print("p4 decoded=\(back["k"]!.reduce(0, +))")
    }
}
