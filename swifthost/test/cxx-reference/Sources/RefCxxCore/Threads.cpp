#include "Copy.hpp"
#include "refcxx.h"
#include <iomanip>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

extern "C" long long refcxx_threaded_sum(int nthreads, int per) {
  std::mutex lock;
  long long total = 0;
  std::vector<std::thread> threads;
  for (int t = 0; t < nthreads; ++t)
    threads.emplace_back([&, per] {
      long long local = 0;
      for (int i = 1; i <= per; ++i)
        local += i;
      std::lock_guard<std::mutex> guard(lock);
      total += local;
    });
  for (auto &t : threads)
    t.join();
  return total;
}

extern "C" int refcxx_format(double x, int n, char *buf, int cap) {
  std::ostringstream os;
  os << std::fixed << std::setprecision(n) << x << " hex " << std::hex << n * 255;
  return refcxx::copyOut(os.str(), buf, cap);
}
