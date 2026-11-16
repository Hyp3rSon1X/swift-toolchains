// SC-0037 gate executable: normal and error paths of every shape in repro.swift, plus the top-level-code shape.
print("run \(try run(5))")
do { print("run -1 returned \(try run(-1))") } catch { print("run -1 \(error)") }
print("typed \(try runTyped(3))")
do { print("typed -1 returned \(try runTyped(-1))") } catch { print("typed -1 \(error)") }
print("load \(try load(Cache(), 4).size)")
do { print("load -2 returned \(try load(Cache(), -2).size)") } catch { print("load -2 \(error)") }
print("top \(total(try make(4).span))")
