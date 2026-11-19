// One WASI command that serves every built-in macro library. Each library stays its own
// module, so String(reflecting:) yields "ObservationMacros.ObservableMacro" etc. and
// CompilerPlugin.resolveMacro(moduleName:typeName:) matches whichever module name the host
// loaded this file under.
import FoundationMacros
import ObservationMacros
import SwiftCompilerPlugin
import SwiftMacros
import SwiftSyntaxMacros
import TestingMacros

@main
struct BuiltinMacrosPlugin: CompilerPlugin {
  var providingMacros: [any Macro.Type] {
    [
      // ObservationMacros (swift/lib/Macros)
      ObservableMacro.self,
      ObservationTrackedMacro.self,
      ObservationIgnoredMacro.self,
      // SwiftMacros (swift/lib/Macros)
      OptionSetMacro.self,
      TaskLocalMacro.self,
      DebugDescriptionMacro.self,
      _DebugDescriptionPropertyMacro.self,
      DistributedResolvableMacro.self,
      SwiftifyImportMacro.self,
      SwiftifyImportProtocolMacro.self,
      // TestingMacros (swift-testing), same list as its TestingMacrosMain.swift
      SuiteDeclarationMacro.self,
      TestDeclarationMacro.self,
      ExpectMacro.self,
      RequireMacro.self,
      AmbiguousRequireMacro.self,
      NonOptionalRequireMacro.self,
      ExpectThrowsMacro.self,
      RequireThrowsMacro.self,
      RequireThrowsNeverMacro.self,
      ExitTestExpectMacro.self,
      ExitTestRequireMacro.self,
      ExitTestCapturedValueMacro.self,
      ExitTestBadCapturedValueMacro.self,
      ExitTestIncorrectlyCapturedValueMacro.self,
      TagMacro.self,
      SourceLocationMacro.self,
      PragmaMacro.self,
      // FoundationMacros (swift-foundation), same list as its FoundationMacros.swift
      FoundationMacros.PredicateMacro.self,
      FoundationMacros.ExpressionMacro.self,
      FoundationMacros.BundleMacro.self,
    ]
  }
}
