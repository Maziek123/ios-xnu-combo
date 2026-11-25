#include <stdint.h>
#include <stddef.h>
typedef uint64_t IOPhysicalAddress;
typedef unsigned long vm_size_t;
typedef unsigned long vm_offset_t;
extern "C" {
void IOLog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void IOSleep(unsigned ms);
void IODelay(unsigned us);
void *IOMallocContiguous(vm_size_t size, vm_size_t alignment, IOPhysicalAddress *phys) __attribute__((deprecated));
}
