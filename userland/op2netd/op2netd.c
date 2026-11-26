/*
 * op2netd: tells iOS's configd about the phone's networks (wl0 Wi-Fi from boot-arg wifi=11, ue0 USB), so Safari gets
 * a default route and DNS. It is op2shell's publish_net() (userland/op2shell/op2_publish.h) without the shell, SSH
 * and the builds those need, for phones where op2shell is not installed.
 *
 * How it runs without a launchd job of its own: tools/build-xnu.sh builds it into the kernel; at boot the kernel
 * writes it to /usr/sbin/op2netd (bsd/kern/op2_shell_sync.c), and when launchd starts /usr/sbin/wifid (Apple's Wi-Fi
 * daemon, which has no Apple Wi-Fi hardware to drive on this phone) the kernel starts this file instead. Here the
 * process forks: the child leaves the job's process group and publishes for as long as the phone runs (one copy:
 * a lock file), the parent becomes the real wifid with execv (the kernel does not redirect op2netd's own exec), so
 * launchd's job and wifid's Mach services carry on as before. Log lines go to /dev/console (the kernel log).
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

#define PUBLISHER "op2netd"

static void
say(const char *s)
{
	int fd = open("/dev/console", O_WRONLY | O_NOCTTY);
	if (fd >= 0) {
		(void)write(fd, s, strlen(s));
		close(fd);
	}
}

#include "../op2shell/op2_publish.h"

int
main(int argc, char **argv)
{
	(void)argc;
	pid_t pid = fork();
	if (pid == 0) {
		setsid();
		int fd = open("/var/run/op2netd.lock", O_CREAT | O_RDWR, 0644);
		if (fd < 0) {
			fd = open("/tmp/op2netd.lock", O_CREAT | O_RDWR, 0644);   /* not root: /var/run is root's */
		}
		if (fd >= 0 && flock(fd, LOCK_EX | LOCK_NB) != 0) {
			_exit(0);   /* another op2netd publishes already (wifid was restarted) */
		}
		char msg[160];
		snprintf(msg, sizeof(msg), "op2netd: publishing ue0/wl0 to configd (pid %d, uid %d)\n", (int)getpid(),
		    (int)getuid());
		say(msg);
		publish_net();   /* never returns */
		_exit(0);
	}
	if (pid < 0) {
		say("op2netd: fork failed; nothing published this boot\n");
	}
	argv[0] = "/usr/sbin/wifid";
	execv("/usr/sbin/wifid", argv);
	char msg[120];
	snprintf(msg, sizeof(msg), "op2netd: could not start /usr/sbin/wifid (errno %d); staying as its stand-in\n", errno);
	say(msg);
	for (;;) {
		pause();
	}
}
