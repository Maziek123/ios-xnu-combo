/* op2net HOST [PORT]: resolve HOST the way apps do (getaddrinfo -> mDNSResponder), connect, send an
 * HTTP HEAD, print the status line. Exit 0 on success. A network smoke test for the phone. */
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdlib.h>
int main(int argc, char **argv)
{
	const char *host = argc > 1 ? argv[1] : "example.com", *port = argc > 2 ? argv[2] : "80";
	struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *ai;
	int e = getaddrinfo(host, port, &hints, &ai);
	if (e) { printf("dns %s: %s\n", host, gai_strerror(e)); return 1; }
	char ip[64];
	inet_ntop(AF_INET, &((struct sockaddr_in *)ai->ai_addr)->sin_addr, ip, sizeof ip);
	printf("dns %s -> %s\n", host, ip);
	int s = socket(AF_INET, SOCK_STREAM, 0);
	if (getenv("OP2_BOUND")) {   /* bind to the interface like Network.framework does (scoped routing) */
		unsigned idx = if_nametoindex(getenv("OP2_BOUND"));
		printf("bound to %s (index %u): %d\n", getenv("OP2_BOUND"), idx, setsockopt(s, IPPROTO_IP, IP_BOUND_IF, &idx, sizeof idx));
	}
	struct timeval tv = { 10, 0 };
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
	if (connect(s, ai->ai_addr, ai->ai_addrlen)) { perror("connect"); return 2; }
	char req[256], buf[512];
	snprintf(req, sizeof req, "HEAD / HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", host);
	write(s, req, strlen(req));
	ssize_t n = read(s, buf, sizeof buf - 1);
	if (n <= 0) { perror("read"); return 3; }
	buf[n] = 0;
	char *nl = strchr(buf, '\r'); if (nl) *nl = 0;
	printf("http %s\n", buf);
	return 0;
}
