/* op2kill SIG PID...: send a signal (the phone has no kill). */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
	if (argc < 3) { fprintf(stderr, "usage: op2kill SIG PID...\n"); return 2; }
	int rc = 0;
	for (int i = 2; i < argc; i++) if (kill(atoi(argv[i]), atoi(argv[1]))) { perror(argv[i]); rc = 1; }
	return rc;
}
