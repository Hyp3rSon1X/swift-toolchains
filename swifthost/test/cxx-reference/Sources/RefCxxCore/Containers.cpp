#include "Copy.hpp"
#include "refcxx.h"
#include <algorithm>
#include <functional>
#include <map>
#include <numeric>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
template <typename T, typename F> std::vector<T> mapped(const std::vector<T> &in, F f) {
  std::vector<T> out;
  out.reserve(in.size());
  std::transform(in.begin(), in.end(), std::back_inserter(out), f);
  return out;
}

template <typename K, typename V> V sumValues(const std::map<K, V> &m) {
  V total{};
  for (const auto &kv : m)
    total += kv.second;
  return total;
}
} // namespace

extern "C" int refcxx_containers(char *buf, int cap) {
  std::vector<int> v{5, 3, 9, 1, 7};
  std::sort(v.begin(), v.end());
  std::function<int(int)> square = [](int x) { return x * x; };
  std::vector<int> sq = mapped(v, square);
  int sum = std::accumulate(sq.begin(), sq.end(), 0);

  std::map<std::string, int> words;
  for (const char *w : {"pear", "apple", "fig", "apple", "pear", "apple"})
    ++words[w];
  std::unordered_map<int, std::string> names{{1, "one"}, {2, "two"}, {3, "three"}};
  std::string joined;
  for (int k = 1; k <= 3; ++k)
    joined += (k > 1 ? "," : "") + names.at(k);

  std::ostringstream os;
  os << "sorted";
  for (int x : v)
    os << ' ' << x;
  os << " squares-sum " << sum << " words";
  for (const auto &kv : words)
    os << ' ' << kv.first << '=' << kv.second;
  os << " total " << sumValues(words) << " names " << joined;
  return refcxx::copyOut(os.str(), buf, cap);
}
