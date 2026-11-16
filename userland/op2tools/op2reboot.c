/* op2reboot: sync and reset (lands in TWRP while the BCB cookie is armed). */
#include <unistd.h>
#define RB_AUTOBOOT 0
int reboot(int);
int main(void) { sync(); sync(); return reboot(RB_AUTOBOOT); }
