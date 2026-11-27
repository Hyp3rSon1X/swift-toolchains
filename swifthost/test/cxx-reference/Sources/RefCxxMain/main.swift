import RefCxxCore

func text(_ fill: (UnsafeMutablePointer<CChar>, Int32) -> Int32) -> String {
  var buf = [CChar](repeating: 0, count: 512)
  _ = buf.withUnsafeMutableBufferPointer { fill($0.baseAddress!, Int32($0.count)) }
  return String(cString: buf)
}

print("global ctor \(refcxx_global_ctor_value())")
print("local static \(refcxx_local_static_next()) \(refcxx_local_static_next())")
print(text { refcxx_containers($0, $1) })
print(text { refcxx_shapes($0, $1) })
print(text { refcxx_format(3.14159, 3, $0, $1) })
print("threads \(refcxx_threaded_sum(4, 1000))")
