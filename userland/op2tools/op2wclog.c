/* Diagnostic only: loaded into WebContent through DYLD_INSERT_LIBRARIES (its XPC Info.plist), it sends stderr
 * (where os_log goes with OS_ACTIVITY_DT_MODE=1) to /private/var/tmp/wc-<pid>.log. Remove after use. */
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
__attribute__((constructor)) static void op2wclog(void)
{
	char path[128];
	snprintf(path, sizeof path, "/private/var/tmp/wc-%d.log", getpid());
	int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0666);
	if (fd >= 0) { dup2(fd, 2); close(fd); }
	fprintf(stderr, "op2wclog: pid %d\n", getpid());
}
