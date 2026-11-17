/* op2home [N]: press Home N times (HID consumer usage 0x40 Menu), like the button: wakes and, with no passcode, unlocks. */
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>
#include <stdlib.h>
#include <unistd.h>
typedef void *IOHIDEventRef, *IOHIDEventSystemClientRef;
IOHIDEventSystemClientRef IOHIDEventSystemClientCreate(CFAllocatorRef);
IOHIDEventRef IOHIDEventCreateKeyboardEvent(CFAllocatorRef, uint64_t, uint32_t page, uint32_t usage, Boolean down, uint32_t flags);
void IOHIDEventSystemClientDispatchEvent(IOHIDEventSystemClientRef, IOHIDEventRef);
int main(int argc, char **argv) {
	int n = argc > 1 ? atoi(argv[1]) : 1;
	IOHIDEventSystemClientRef c = IOHIDEventSystemClientCreate(NULL);
	for (int i = 0; i < n; i++) {
		for (int down = 1; down >= 0; down--) {
			IOHIDEventRef e = IOHIDEventCreateKeyboardEvent(NULL, mach_absolute_time(), 0x0C, 0x40, down, 0);
			IOHIDEventSystemClientDispatchEvent(c, e);
			CFRelease(e);
			usleep(down ? 80000 : 400000);
		}
		usleep(600000);
	}
	return 0;
}
