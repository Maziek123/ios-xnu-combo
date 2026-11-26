/*
 * op2_keeplog.c: a second, small log for the lines that matter (Wi-Fi bring-up, the Wi-Fi manager, op2netd,
 * the kernel's helper-program checks), kept apart from the console so a flood of other messages (a frozen screen,
 * GPU timeouts) cannot push them out before the phone is pulled.
 *
 * The 1 MB ramoops region (0xac000000) is laid out by the recovery kernel (TWRP, Linux ramoops): record ("dump")
 * zones, then the 256 KB console we already write (ADT op2-fbcon, 0xac0be000), then ftrace and pmsg. Every zone
 * starts with Linux's persistent_ram_buffer header { 'DBGC', start, size }, and Linux keeps each zone's old
 * contents for /sys/fs/pstore at its next boot. Nothing writes the record zones under iOS, so the first one becomes
 * this log: at the first line of a boot it is found by its header (scanning 4 KB steps below the console; pmsg
 * above it if there is none), emptied, and filled as a ring in the same format, so TWRP shows it as
 * /sys/fs/pstore/dmesg-ramoops-0 (pmsg-ramoops-0 for the fallback). Its size is the distance between two
 * zone headers, so it never runs past what Linux thinks the zone is (Linux discards a zone whose size field is
 * larger). No header found: nothing is kept, said once on the console.
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
#define BELOW        0xbe000u      /* record zones: from the region's start to the console */
#define ABOVE        0x2000u       /* ftrace + pmsg after the console */
#define CAP_MAX      (128u * 1024u)

struct prb {
	volatile uint32_t sig, start, size;
	volatile char data[];
};

static struct prb *keep;
static uint32_t keep_cap;
static volatile UInt32 keep_state;   /* 0 not looked yet, 1 ready, 2 being looked for, 3 none / off */
static lck_spin_t *keep_lock;
static uint64_t keep_phys;

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
	uint64_t console = reg[0], console_size = reg[1];
	if (console < BELOW) {
		keep_state = 3;
		return;
	}
	/* the record zones below the console */
	volatile uint32_t *low = (volatile uint32_t *)ml_io_map((vm_offset_t)(console - BELOW), BELOW);
	uint32_t at[2] = { 0, 0 }, found = 0;
	for (uint32_t off = 0; low != NULL && off < BELOW && found < 2; off += 0x1000u) {
		if (low[off / 4u] == SIG) {
			at[found++] = off;
		}
	}
	if (found >= 1) {
		/* zone 0 runs to the next header (zone 1, or the console when there is only one) */
		uint32_t zone = (found == 2 ? at[1] : BELOW) - at[0];
		keep = (struct prb *)(uintptr_t)((vm_offset_t)low + at[0]);
		keep_cap = zone - HDR < CAP_MAX ? zone - HDR : CAP_MAX;
		keep_phys = console - BELOW + at[0];
	} else {
		/* pmsg (or ftrace) above the console: the last header there */
		volatile uint32_t *high = (volatile uint32_t *)ml_io_map((vm_offset_t)(console + console_size), ABOVE);
		for (uint32_t off = 0; high != NULL && off < ABOVE; off += 0x1000u) {
			if (high[off / 4u] == SIG) {
				keep = (struct prb *)(uintptr_t)((vm_offset_t)high + off);
				keep_cap = ABOVE - off - HDR;
				keep_phys = console + console_size + off;
			}
		}
	}
	if (keep == NULL || keep_cap < 1024u) {
		keep = NULL;
		keep_state = 3;
		printf("OP2 KEEP: no ramoops zone header found next to the console; important lines only on the console\n");
		return;
	}
	keep->start = 0;
	keep->size = 0;
	__asm__ volatile("dmb oshst" ::: "memory");
	keep_lock = lck_spin_alloc_init(lck_grp_alloc_init("op2-keep", LCK_GRP_ATTR_NULL), LCK_ATTR_NULL);
	keep_state = 1;
	printf("OP2 KEEP: important lines also go to the ramoops zone at %llx (%u bytes; %s in TWRP's /sys/fs/pstore)\n",
	    keep_phys, keep_cap, found ? "dmesg-ramoops-0" : "pmsg-ramoops-0");
}

static void
keep_put(const char *s, size_t n)
{
	uint32_t start = keep->start, size = keep->size;
	if (start >= keep_cap) {
		start = 0;
	}
	for (size_t i = 0; i < n; i++) {
		keep->data[start++] = s[i];
		if (start >= keep_cap) {
			start = 0;
		}
		if (size < keep_cap) {
			size++;
		}
	}
	keep->start = start;
	keep->size = size;
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
	char ts[24];
	uint64_t ns;
	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	int tn = snprintf(ts, sizeof(ts), "[%5llu.%03llu] ", ns / 1000000000ull, (ns / 1000000ull) % 1000ull);
	size_t n = strnlen(line, 512);
	lck_spin_lock(keep_lock);
	keep_put(ts, (size_t)tn);
	keep_put(line, n);
	if (n == 0 || line[n - 1] != '\n') {
		keep_put("\n", 1);
	}
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
