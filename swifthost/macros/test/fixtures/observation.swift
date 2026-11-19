import Observation

@Observable
final class Model {
  var count = 0
  var name: String = "x"
  var tags: [String] = []
  @ObservationIgnored var cache = 0
  let id = 7
}

@Observable
class Base { var a: Int? ; var b = (1, 2) }
