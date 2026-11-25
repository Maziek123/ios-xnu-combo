#include <stdint.h>
extern "C" { void clock_get_uptime(uint64_t *result); void absolutetime_to_nanoseconds(uint64_t abstime, uint64_t *result); }
