#include "refcxx.h"

namespace {
// A global object with a constructor: runs before main through the module's init functions.
struct Registry {
  int value;
  Registry() : value(0) {
    for (int i = 1; i <= 6; ++i)
      value += i * 2; // 42
  }
};
Registry gRegistry;

int nextTicket() {
  // A function-local static: initialized once, thread-safe (__cxa_guard_acquire).
  static int counter = [] { return 100; }();
  return ++counter;
}
} // namespace

extern "C" int refcxx_global_ctor_value(void) { return gRegistry.value; }
extern "C" int refcxx_local_static_next(void) { return nextTicket(); }
