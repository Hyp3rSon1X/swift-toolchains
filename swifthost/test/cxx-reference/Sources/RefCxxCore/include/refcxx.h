#ifndef REFCXX_H
#define REFCXX_H
#ifdef __cplusplus
extern "C" {
#endif

/* The value a global object's constructor stored (42 if it ran). */
int refcxx_global_ctor_value(void);
/* Calls a function with a function-local static counter; returns the count. */
int refcxx_local_static_next(void);
/* Templates, std::string, std::vector, std::map, std::unordered_map, <algorithm>, std::function and lambdas:
   writes a one-line report into buf (NUL-terminated), returns its length. */
int refcxx_containers(char *buf, int cap);
/* A virtual class hierarchy with dynamic_cast and typeid, held in std::shared_ptr: writes a report into buf. */
int refcxx_shapes(char *buf, int cap);
/* std::ostringstream formatting of x with n digits and of n in hex: writes it into buf. */
int refcxx_format(double x, int n, char *buf, int cap);
/* Sums 1..per in each of nthreads std::threads, adding into one total under a std::mutex. */
long long refcxx_threaded_sum(int nthreads, int per);

#ifdef __cplusplus
}
#endif
#endif
