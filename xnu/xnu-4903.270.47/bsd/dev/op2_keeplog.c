/*
 * op2_keeplog.c: a second log for the lines that matter (Wi-Fi bring-up, the Wi-Fi manager, op2netd, the kernel's
 * helper-program checks, what wifid asks for, Bluetooth), kept apart from the console so a flood of other messages
 * (a frozen screen, GPU timeouts) cannot push them out before the phone is pulled.
 *
 * The 1 MB ramoops region at 0xac000000 is outside the RAM iOS manages (tools/mkadt.py: DRAM ends there). TWRP's
 * ramoops (its /sys/module/ramoops/parameters, read on the phone 2026-11-27: mem_size 1 MB, record_size 4 KB,
 * console 256 KB, ftrace 4 KB, pmsg 4 KB) lays it out as 190 record ("dump") zones of 4 KB, then the console we
 * already write (ADT op2-fbcon, 0xac0be000 = base + 0xbe000), then ftrace and pmsg. Each zone starts with Linux's
 * persistent_ram_buffer header { 'DBGC', start, size }; at its next boot Linux keeps a zone whose header is valid
 * and shows it in /sys/fs/pstore. The headers are not there when iOS starts (something between TWRP and this
 * kernel clears them), so this file writes them itself: the first 64 record zones (256 KB) are emptied at the first
 * kept line and filled in order, each beginning with a "====seconds.microseconds" line as Linux's own records do;
 * when they are full, the last one becomes a ring of the newest lines. TWRP shows them as dmesg-ramoops-0, -1, ...
 * (read them in number order; Linux stops at the first empty one).
 *
 * op2_keep(fmt, ...) from kernel code; sysctl kern.op2keep (root writes a line) from op2netd. Each line gets the
 * time since boot. Boot-arg op2keep=0 turns it off.
 */
#if defined(ARM64_BOARD_CONFIG_MSM8994)

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/sysctl.h>
#include <kern/locks.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <stdarg.h>
#include <libkern/OSAtomic.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

void op2_keep(const char *fmt, ...) __printflike(1, 2);
void op2_keep_line(const char *line);

#define SIG          0x43474244u   /* "DBGC" */
#define HDR          12u
#define BELOW        0xbe000u      /* the console's offset in the region: 190 record zones of 4 KB before it */
#define ZONE         0x1000u       /* TWRP's record_size */
#define ZONES        64u           /* the ones used here: 256 KB */
#define DATA         (ZONE - HDR)

struct prb {
	volatile uint32_t sig, start, size;
	volatile char data[];
};

static volatile uint8_t *keep_base;   /* zone 0 */
static uint32_t keep_zone;            /* the zone being written */
static volatile UInt32 keep_state;    /* 0 not set up yet, 1 ready, 2 being set up, 3 off */
static lck_spin_t *keep_lock;

static struct prb *
zone(uint32_t i)
{
	return (struct prb *)(uintptr_t)(keep_base + (size_t)i * ZONE);
}

static void
zone_begin(uint32_t i)
{
	struct prb *z = zone(i);
	char h[40];
	uint64_t ns;
	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	int n = snprintf(h, sizeof(h), "====%llu.%06llu\n", ns / 1000000000ull, (ns / 1000ull) % 1000000ull);
	for (int k = 0; k < n; k++) {
		z->data[k] = h[k];
	}
	z->start = (uint32_t)n;
	z->size = (uint32_t)n;
}

static void
keep_find(void)
{
	DTEntry e;
	uintptr_t *reg = NULL;
	unsigned int size = 0;
	uint32_t en = 1;
	PE_parse_boot_argn("op2keep", &en, sizeof(en));
	if (!en || DTFindEntry("name", "op2-fbcon", &e) != kSuccess ||
	    DTGetProperty(e, "reg", (void **)&reg, &size) != kSuccess || size < 16) {
		keep_state = 3;
		return;
	}
	uint64_t console = reg[0];
	if (console != 0xac0be000ull) {   /* the layout above is TWRP's for this console address only */
		keep_state = 3;
		printf("OP2 KEEP: the console is at %llx, not 0xac0be000: no kept log\n", console);
		return;
	}
	keep_base = (volatile uint8_t *)ml_io_map((vm_offset_t)(console - BELOW), ZONES * ZONE);
	if (keep_base == NULL) {
		keep_state = 3;
		printf("OP2 KEEP: ml_io_map failed: no kept log\n");
		return;
	}
	for (uint32_t i = 0; i < ZONES; i++) {   /* empty: Linux ignores a zone of size 0 */
		struct prb *z = zone(i);
		z->sig = SIG;
		z->start = 0;
		z->size = 0;
	}
	keep_zone = 0;
	zone_begin(0);
	__asm__ volatile("dmb oshst" ::: "memory");
	keep_lock = lck_spin_alloc_init(lck_grp_alloc_init("op2-keep", LCK_GRP_ATTR_NULL), LCK_ATTR_NULL);
	keep_state = 1;
	printf("OP2 KEEP: important lines also go to ramoops record zones 0-%u at %llx (%u KB; TWRP: "
	    "/sys/fs/pstore/dmesg-ramoops-0, -1, ...)\n", ZONES - 1, console - BELOW, ZONES * ZONE / 1024u);
}

/* one line (it fits in a zone): into the current zone, or the next one; the last zone wraps */
static void
keep_put(const char *s, size_t n)
{
	if (n > DATA - 40u) {
		n = DATA - 40u;
	}
	struct prb *z = zone(keep_zone);
	if (keep_zone + 1u < ZONES && z->size + n > DATA) {
		keep_zone++;
		zone_begin(keep_zone);
		z = zone(keep_zone);
	}
	uint32_t start = z->start, size = z->size;
	for (size_t i = 0; i < n; i++) {
		if (start >= DATA) {
			start = 0;
		}
		z->data[start++] = s[i];
		if (size < DATA) {
			size++;
		}
	}
	z->start = start >= DATA ? 0 : start;
	z->size = size;
	__asm__ volatile("dmb oshst" ::: "memory");
}

void
op2_keep_line(const char *line)
{
	if (keep_state == 0 && OSCompareAndSwap(0, 2, &keep_state)) {
		keep_find();   /* the first caller (bsdinit's helper check, after the VM is up) */
	}
	if (keep_state != 1 || line == NULL) {
		return;
	}
	char buf[560];
	uint64_t ns;
	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	size_t n = strnlen(line, 512);
	int tn = snprintf(buf, sizeof(buf), "[%5llu.%03llu] %.*s%s", ns / 1000000000ull, (ns / 1000000ull) % 1000ull,
	    (int)n, line, (n == 0 || line[n - 1] != '\n') ? "\n" : "");
	lck_spin_lock(keep_lock);
	keep_put(buf, (size_t)(tn < (int)sizeof(buf) ? tn : (int)sizeof(buf) - 1));
	lck_spin_unlock(keep_lock);
}

void
op2_keep(const char *fmt, ...)
{
	char line[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	op2_keep_line(line);
}

static int
keep_sysctl(struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	char buf[256];
	int err;
	buf[0] = '\0';
	err = sysctl_handle_string(oidp, buf, sizeof(buf), req);
	if (err != 0 || req->newptr == USER_ADDR_NULL) {
		return err;
	}
	op2_keep_line(buf);
	return 0;
}
SYSCTL_PROC(_kern, OID_AUTO, op2keep, CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_LOCKED, NULL, 0, keep_sysctl, "A",
    "append a line to the kept log (root)");

#endif /* ARM64_BOARD_CONFIG_MSM8994 */
