/* op2chown [-R] UID GID PATH...: chown (toybox has none); -R recurses (lchown, never follows symlinks). */
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static uid_t U; static gid_t G; static long N; static int RC;
static int one(const char *p, const struct stat *st, int type, struct FTW *f)
{
	(void)st; (void)type; (void)f;
	if (lchown(p, U, G) != 0) { perror(p); RC = 1; } else N++;
	return 0;
}
int main(int argc, char **argv)
{
	int rec = argc > 1 && strcmp(argv[1], "-R") == 0;
	if (argc < 4 + rec) { fprintf(stderr, "usage: op2chown [-R] uid gid path...\n"); return 2; }
	U = (uid_t)atoi(argv[1 + rec]); G = (gid_t)atoi(argv[2 + rec]);
	for (int i = 3 + rec; i < argc; i++) {
		if (rec) nftw(argv[i], one, 64, FTW_PHYS);
		else one(argv[i], NULL, 0, NULL);
	}
	printf("op2chown: %ld changed\n", N);
	return RC;
}
