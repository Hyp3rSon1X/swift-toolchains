// SC-0035 gate driver: runs Sink.run() twice (x 1 then 2, count 1 then 3).
var sink = Sink(box: Box())
sink.run()
sink.run()
print("count \(sink.count) x \(sink.box.root.x)")
