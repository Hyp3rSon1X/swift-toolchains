import SwiftCompilerPlugin
import SwiftSyntax
import SwiftSyntaxMacros

public struct AnswerMacro: ExpressionMacro {
    public static func expansion(of node: some FreestandingMacroExpansionSyntax, in context: some MacroExpansionContext) -> ExprSyntax {
        "42"
    }
}

@main
struct DemoPlugin: CompilerPlugin {
    let providingMacros: [Macro.Type] = [AnswerMacro.self]
}
