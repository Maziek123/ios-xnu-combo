#include <stdint.h>
#define NULL 0
#define __unused __attribute__((unused))
extern "C" { void IOLog(const char *, ...) __attribute__((format(printf,1,2))); void IOSleep(unsigned); void IODelay(unsigned); }
typedef uint64_t IOPhysicalAddress;
extern "C" { void *IOMallocContiguous(vm_size_t size, vm_size_t alignment, IOPhysicalAddress *physicalAddress) __attribute__((deprecated)); }
