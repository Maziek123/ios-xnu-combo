/*
 * op2crash.dylib: crash reports for daemons that die before anything else can watch them (diagnostic only).
 * Loaded first through the job's DYLD_INSERT_LIBRARIES (xpcd cache EnvironmentVariables). On SIGSEGV/SIGBUS/
 * SIGABRT/SIGILL/SIGTRAP it appends to /private/var/tmp/crash-<pid>.txt: the faulting pc/lr/registers from the
 * signal context, the frame-pointer chain, every image's crash annotation (__DATA,__crash_info: abort and
 * assertion messages) and the image list; then lets the signal kill the process as before.
 */
#include <dlfcn.h>
#include <mach/mach.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ucontext.h>
#include <unistd.h>

static int gFd = -1;

static void
out(const char *fmt, ...)
{
	char b[512];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(b, sizeof b, fmt, ap);
	va_end(ap);
	if (gFd >= 0 && n > 0)
		write(gFd, b, (size_t)(n < (int)sizeof b ? n : (int)sizeof b - 1));
}

static void
sym(const char *tag, uint64_t a)
{
	Dl_info di;
	if (a && dladdr((void *)a, &di) && di.dli_fname) {
		const char *f = strrchr(di.dli_fname, '/');
		out("%s 0x%llx %s+0x%llx %s\n", tag, (unsigned long long)a, f ? f + 1 : di.dli_fname,
		    (unsigned long long)(a - (uint64_t)di.dli_fbase), di.dli_sname ? di.dli_sname : "");
	} else {
		out("%s 0x%llx\n", tag, (unsigned long long)a);
	}
}

/* CrashReporterClient's struct in __DATA,__crash_info: version, message, signature string, backtrace, message2 */
struct crinfo {
	uint64_t version;
	const char *message;
	const char *signature;
	const char *backtrace;
	const char *message2;
};

static void
annotations(void)
{
	for (uint32_t i = 0; i < _dyld_image_count(); i++) {
		const struct mach_header_64 *h = (const struct mach_header_64 *)_dyld_get_image_header(i);
		unsigned long size = 0;
		uint8_t *p = getsectiondata(h, "__DATA", "__crash_info", &size);
		if (!p || size < sizeof(struct crinfo))
			continue;
		struct crinfo *c = (struct crinfo *)p;
		const char *name = strrchr(_dyld_get_image_name(i), '/');
		if (c->message)
			out("annotation %s: %s\n", name ? name + 1 : "?", c->message);
		if (c->message2)
			out("annotation2 %s: %s\n", name ? name + 1 : "?", c->message2);
		if (c->signature)
			out("signature %s: %s\n", name ? name + 1 : "?", c->signature);
	}
}

/* who owns the memory at a: region bounds, protection, VM tag (VM_MEMORY_*: 1 malloc, 21 IOKit, ...) */
static void
region(const char *tag, uint64_t a)
{
	vm_address_t addr = (vm_address_t)a;
	vm_size_t size = 0;
	natural_t depth = 1;
	struct vm_region_submap_info_64 info;
	mach_msg_type_number_t cnt = VM_REGION_SUBMAP_INFO_COUNT_64;
	kern_return_t kr = vm_region_recurse_64(mach_task_self(), &addr, &size, &depth, (vm_region_recurse_info_t)&info, &cnt);
	if (kr != KERN_SUCCESS) {
		out("%s 0x%llx: no region (%d)\n", tag, (unsigned long long)a, kr);
		return;
	}
	out("%s 0x%llx: region 0x%lx-0x%lx%s prot %d/%d tag %u share %u\n", tag, (unsigned long long)a,
	    (unsigned long)addr, (unsigned long)(addr + size), (uint64_t)addr > a ? " (next region; address unmapped)" : "",
	    info.protection, info.max_protection, info.user_tag, info.share_mode);
}

static void
handler(int sig, siginfo_t *si, void *ucv)
{
	char path[64];
	snprintf(path, sizeof path, "/private/var/tmp/crash-%d.txt", getpid());
	gFd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0666);
	out("=== %s pid %d signal %d addr %p\n", getprogname(), getpid(), sig, si ? si->si_addr : NULL);
	ucontext_t *uc = ucv;
	if (uc && uc->uc_mcontext) {
		_STRUCT_ARM_THREAD_STATE64 *ss = &uc->uc_mcontext->__ss;
		sym("pc", ss->__pc);
		sym("lr", ss->__lr);
		for (int r = 0; r < 8; r++)
			out("x%d 0x%llx\n", r, (unsigned long long)ss->__x[r]);
		region("fault", (uint64_t)(si ? si->si_addr : 0));
		region("x0", ss->__x[0]);
		region("x6", ss->__x[6]);
		region("before-fault", (uint64_t)(si ? si->si_addr : 0) - 0x20);
		uint64_t fp = ss->__fp;
		for (int d = 0; d < 40 && fp && (fp & 7) == 0; d++) {
			uint64_t *f = (uint64_t *)fp;
			if (!f[1])
				break;
			sym("  frame", f[1]);
			if (f[0] <= fp)
				break;
			fp = f[0];
		}
	}
	annotations();
	for (uint32_t i = 0; i < _dyld_image_count(); i++) {
		uint64_t base = (uint64_t)_dyld_get_image_header(i);
		if (base < 0x180000000ULL)
			out("image 0x%llx %s\n", (unsigned long long)base, _dyld_get_image_name(i));
	}
	close(gFd);
	signal(sig, SIG_DFL);
	raise(sig);
}

__attribute__((constructor)) static void
op2crash_init(void)
{
	static char stack[64 * 1024];
	stack_t ss = { .ss_sp = stack, .ss_size = sizeof stack, .ss_flags = 0 };
	sigaltstack(&ss, NULL);
	struct sigaction sa;
	memset(&sa, 0, sizeof sa);
	sa.sa_sigaction = handler;
	sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
	int sigs[] = { SIGSEGV, SIGBUS, SIGABRT, SIGILL, SIGTRAP, SIGFPE };
	for (unsigned i = 0; i < sizeof sigs / sizeof sigs[0]; i++)
		sigaction(sigs[i], &sa, NULL);
}
