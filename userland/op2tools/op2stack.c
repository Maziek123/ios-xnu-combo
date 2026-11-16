/*
 * op2stack PID: print every thread's backtrace (frame-pointer walk) of a process,
 * with each address as image+offset, so a hung daemon can be read without a debugger.
 *   op2stack 25          (configd)
 * Needs root and the task_for_pid-allow entitlement (op2stack.ent); cs_enforcement_disable=1.
 */
#include <mach/mach.h>

#include <mach-o/dyld_images.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct img { uint64_t base; char path[256]; };
static struct img imgs[1024];
static int nimg;

static int rd(task_t t, uint64_t a, void *buf, size_t n)
{
	vm_size_t got = 0;
	return vm_read_overwrite(t, (vm_address_t)a, n, (vm_address_t)buf, &got) == KERN_SUCCESS && got == n ? 0 : -1;
}

static void load_images(task_t t)
{
	struct task_dyld_info di;
	mach_msg_type_number_t cnt = TASK_DYLD_INFO_COUNT;
	if (task_info(t, TASK_DYLD_INFO, (task_info_t)&di, &cnt) != KERN_SUCCESS)
		return;
	struct dyld_all_image_infos aii;
	if (rd(t, di.all_image_info_addr, &aii, sizeof(aii)) != 0)
		return;
	uint32_t n = aii.infoArrayCount;
	if (n > 1024)
		n = 1024;
	struct dyld_image_info *ii = calloc(n, sizeof(*ii));
	if (rd(t, (uint64_t)aii.infoArray, ii, n * sizeof(*ii)) != 0)
		return;
	for (uint32_t i = 0; i < n; i++) {
		imgs[nimg].base = (uint64_t)ii[i].imageLoadAddress;
		if (rd(t, (uint64_t)ii[i].imageFilePath, imgs[nimg].path, 255) != 0)
			strcpy(imgs[nimg].path, "?");
		imgs[nimg].path[255] = 0;
		nimg++;
	}
	printf("images %d, shared cache slide 0x%llx\n", nimg, (unsigned long long)aii.sharedCacheSlide);
}

static void sym(uint64_t a)
{
	int best = -1;
	for (int i = 0; i < nimg; i++)
		if (imgs[i].base <= a && (best < 0 || imgs[i].base > imgs[best].base))
			best = i;
	if (best < 0) {
		printf("  0x%llx\n", (unsigned long long)a);
		return;
	}
	const char *p = strrchr(imgs[best].path, '/');
	printf("  0x%llx %s+0x%llx\n", (unsigned long long)a, p ? p + 1 : imgs[best].path,
	    (unsigned long long)(a - imgs[best].base));
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: op2stack PID\n");
		return 2;
	}
	task_t t;
	kern_return_t kr = task_for_pid(mach_task_self(), atoi(argv[1]), &t);
	if (kr != KERN_SUCCESS) {
		fprintf(stderr, "task_for_pid: %s\n", mach_error_string(kr));
		return 1;
	}
	load_images(t);
	thread_act_array_t th;
	mach_msg_type_number_t nth;
	if (task_threads(t, &th, &nth) != KERN_SUCCESS)
		return 1;
	for (unsigned i = 0; i < nth; i++) {
		arm_thread_state64_t st;
		mach_msg_type_number_t c = ARM_THREAD_STATE64_COUNT;
		if (thread_get_state(th[i], ARM_THREAD_STATE64, (thread_state_t)&st, &c) != KERN_SUCCESS)
			continue;
		printf("thread %u\n", i);
		sym(st.__pc);
		sym(st.__lr);
		uint64_t fp = st.__fp;
		for (int d = 0; d < 40 && fp; d++) {
			uint64_t fr[2];
			if (rd(t, fp, fr, sizeof(fr)) != 0 || fr[1] == 0)
				break;
			sym(fr[1]);
			if (fr[0] <= fp)
				break;
			fp = fr[0];
		}
	}
	return 0;
}
