// @OptionSet is not declared in the stdlib at 6.4.2, so the fixture declares it itself.
@attached(member, names: named(RawValue), named(rawValue), named(`init`), arbitrary)
@attached(extension, conformances: OptionSet)
macro OptionSet<RawType>() = #externalMacro(module: "SwiftMacros", type: "OptionSetMacro")

@OptionSet<UInt8>
struct Flags {
  private enum Options: Int { case a, b, c }
}

enum Globals {
  @TaskLocal static var current: Int = 0
}

@DebugDescription
struct Pt: CustomDebugStringConvertible {
  var x = 1
  var debugDescription: String { "Pt(\(x))" }
}
