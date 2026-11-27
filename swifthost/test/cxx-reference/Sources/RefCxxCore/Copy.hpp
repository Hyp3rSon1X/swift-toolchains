#pragma once
#include <algorithm>
#include <cstring>
#include <string>

namespace refcxx {
// Copies s into a C buffer of cap bytes (NUL-terminated, truncated), returns the copied length.
inline int copyOut(const std::string &s, char *buf, int cap) {
  if (!buf || cap <= 0)
    return 0;
  int n = std::min<int>((int)s.size(), cap - 1);
  std::memcpy(buf, s.data(), n);
  buf[n] = 0;
  return n;
}
} // namespace refcxx
