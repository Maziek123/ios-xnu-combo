/* op2ps: pid, uid, name of every process (the phone has no ps with users). */
#include <stdio.h>
#include <stdlib.h>
#include <sys/sysctl.h>
int main(void) {
	int mib[3] = { CTL_KERN, KERN_PROC, KERN_PROC_ALL };
	size_t len = 0;
	sysctl(mib, 3, NULL, &len, NULL, 0);
	struct kinfo_proc *kp = malloc(len);
	if (sysctl(mib, 3, kp, &len, NULL, 0)) { perror("sysctl"); return 1; }
	for (size_t i = 0; i < len / sizeof(*kp); i++)
		printf("%5d %5d %s\n", kp[i].kp_proc.p_pid, kp[i].kp_eproc.e_ucred.cr_uid, kp[i].kp_proc.p_comm);
	return 0;
}
