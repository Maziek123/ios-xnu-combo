/* op2ugid: does per-thread identity (pthread_setugid_np, used by cfprefsd to write users' files) work? */
#include <pthread.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
int pthread_setugid_np(uid_t, gid_t);
int main(void) {
	const char *f = "/var/mobile/op2ugid-test";
	unlink(f);
	int r = pthread_setugid_np(501, 501);
	printf("pthread_setugid_np(501,501) = %d (%s); geteuid %d\n", r, r ? strerror(errno) : "ok", geteuid());
	int fd = open(f, O_CREAT | O_WRONLY, 0600);
	printf("open = %d (%s)\n", fd, fd < 0 ? strerror(errno) : "ok");
	if (fd >= 0) close(fd);
	pthread_setugid_np((uid_t)-101, (gid_t)-101);
	struct stat st;
	if (stat(f, &st) == 0) printf("file owner %d:%d\n", st.st_uid, st.st_gid);
	unlink(f);
	return 0;
}
