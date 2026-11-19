// byval probe: structs passed by value in tail position. WebAssembly passes them as a pointer to a copy in the
// caller's frame, so these calls must not become return_call (llvm/llvm-project#227491). tail_ctl is a control
// that must still be a return_call.
#include <stdio.h>
struct Big { int a[8]; };
__attribute__((noinline)) int sum(struct Big b) { int s = 0; for (int i = 0; i < 8; i++) s += b.a[i]; return s; }
__attribute__((noinline)) int fwd_param(struct Big b) { return sum(b); }            // forwards a byval parameter
__attribute__((noinline)) int fwd_local(int k) {                                      // passes a local by value
  struct Big b; for (int i = 0; i < 8; i++) b.a[i] = k + i; return sum(b);
}
__attribute__((noinline)) int deep(int n, struct Big b) {                             // callee frames reuse the stack
  volatile int pad[64]; for (int i = 0; i < 64; i++) pad[i] = -1;
  return n == 0 ? sum(b) + pad[3] + 1 : deep(n - 1, b);
}
__attribute__((noinline)) int leaf(int x) { return x * 3; }
__attribute__((noinline)) int tail_ctl(int x) { return leaf(x + 1); }
int main(void) {
  struct Big b; for (int i = 0; i < 8; i++) b.a[i] = i + 1;
  int r1 = fwd_param(b), r2 = fwd_local(10), r3 = deep(50, b), r4 = tail_ctl(4);
  printf("byval fwd_param=%d fwd_local=%d deep=%d tail_ctl=%d %s\n", r1, r2, r3, r4,
         (r1 == 36 && r2 == 108 && r3 == 36 && r4 == 15) ? "OK" : "WRONG");
  return !(r1 == 36 && r2 == 108 && r3 == 36 && r4 == 15);
}
