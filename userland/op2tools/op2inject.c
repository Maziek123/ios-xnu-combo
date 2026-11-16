/*
 * op2inject PID /path/lib.dylib: load a library into a running process (debugging only).
 * Creates a thread in the target that calls pthread_create_from_mach_thread(), whose new pthread runs a
 * 3-instruction stub: dlopen(path, RTLD_NOW). The raw mach thread then spins and is terminated from here.
 * The shared cache sits at the same address in every process, so local symbol addresses are valid there.
 * Needs task_for_pid-allow (op2stack.ent) and cs_enforcement_disable=1 (the stub page is unsigned).
 */
#include <mach/mach.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(x) do { kern_return_t kr_ = (x); if (kr_ != KERN_SUCCESS) { \
	fprintf(stderr, "%s: %s\n", #x, mach_error_string(kr_)); return 1; } } while (0)

int
main(int argc, char **argv)
{
	if (argc != 3) {
		fprintf(stderr, "usage: op2inject PID /path/lib.dylib\n");
		return 2;
	}
	task_t t;
	CHECK(task_for_pid(mach_task_self(), atoi(argv[1]), &t));

	uint64_t p_dlopen = (uint64_t)dlsym(RTLD_DEFAULT, "dlopen");
	uint64_t p_create = (uint64_t)dlsym(RTLD_DEFAULT, "pthread_create_from_mach_thread");
	if (!p_dlopen || !p_create) {
		fprintf(stderr, "symbols missing\n");
		return 1;
	}

	/* code page: [0] stub for the pthread: x1 = RTLD_NOW; br dlopen   [16] spin for the mach thread */
	uint32_t code[8] = {
		0xD2800041,   /* movz x1, #2            */
		0x58000050,   /* ldr  x16, #8 (-> +12)  */
		0xD61F0200,   /* br   x16               */
		0, 0,         /* .quad dlopen at +12    */
		0x14000000,   /* +20: b .  (spin)       */
		0, 0
	};
	memcpy(&code[3], &p_dlopen, 8);

	vm_address_t rcode = 0, rdata = 0, rstack = 0;
	vm_size_t stack_size = 64 * 1024;
	CHECK(vm_allocate(t, &rcode, PAGE_SIZE, VM_FLAGS_ANYWHERE));
	CHECK(vm_write(t, rcode, (vm_offset_t)code, sizeof(code)));
	CHECK(vm_protect(t, rcode, PAGE_SIZE, FALSE, VM_PROT_READ | VM_PROT_EXECUTE));
	CHECK(vm_allocate(t, &rdata, PAGE_SIZE, VM_FLAGS_ANYWHERE));
	char data[512] = {0};   /* [0] pthread_t out, [64] path */
	strlcpy(data + 64, argv[2], sizeof(data) - 64);
	CHECK(vm_write(t, rdata, (vm_offset_t)data, sizeof(data)));
	CHECK(vm_allocate(t, &rstack, stack_size, VM_FLAGS_ANYWHERE));

	arm_thread_state64_t st;
	memset(&st, 0, sizeof(st));
	st.__x[0] = rdata;            /* pthread_t *              */
	st.__x[1] = 0;                /* attr                     */
	st.__x[2] = rcode;            /* start routine: the stub  */
	st.__x[3] = rdata + 64;       /* arg: the path            */
	st.__pc = p_create;
	st.__lr = rcode + 20;         /* return into the spin     */
	st.__sp = (rstack + stack_size - 256) & ~15ULL;
	thread_act_t th;
	CHECK(thread_create_running(t, ARM_THREAD_STATE64, (thread_state_t)&st, ARM_THREAD_STATE64_COUNT, &th));

	/* wait until the mach thread reaches the spin, then remove it */
	for (int i = 0; i < 300; i++) {
		usleep(10000);
		mach_msg_type_number_t c = ARM_THREAD_STATE64_COUNT;
		if (thread_get_state(th, ARM_THREAD_STATE64, (thread_state_t)&st, &c) == KERN_SUCCESS &&
		    st.__pc == rcode + 20) {
			printf("pthread_create_from_mach_thread returned %llu; dlopen running on a new pthread\n",
			    (unsigned long long)st.__x[0]);
			thread_terminate(th);
			return 0;
		}
	}
	fprintf(stderr, "thread did not finish (pc 0x%llx); terminating it\n", (unsigned long long)st.__pc);
	thread_terminate(th);
	return 1;
}
