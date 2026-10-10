// A non-frozen struct in a library-evolution module: its size is unknown to
// clients, so a client class storing it has runtime-computed field offsets.
public struct Token {
    public var value: Int
    public init(_ value: Int) { self.value = value }
}
