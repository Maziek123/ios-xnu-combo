/*
 * op2netd: the phone's own network helper, and its Wi-Fi manager.
 *
 *  - Tells iOS's configd about wl0 (Wi-Fi) and ue0 (USB), so Safari gets a default route and DNS: op2shell's
 *    publish code (userland/op2shell/op2_publish.h), without the shell, SSH and the builds those need.
 *  - The Wi-Fi manager (kernel boot-arg wifi=12): a page at http://127.0.0.1:8080 on the phone (Safari; "Add to Home
 *    Screen" gives it an icon) lists the networks the driver heard, joins one with its password, disconnects,
 *    forgets, turns Wi-Fi off. It drives the kernel through sysctls (bsd/dev/op2_wlan.c): net.op2wlan_cmd (scan,
 *    join <name in hex> <PMK in hex | ->, leave, off), net.op2wlan_scan, net.op2wlan_state. The password never
 *    reaches the kernel: the PMK is derived here (PBKDF2-SHA1, 4096 rounds, as WPA2 specifies) and saved with the
 *    network's name in /var/root/Library/op2netd/networks (root only, 0600). While not connected it joins the
 *    strongest saved network it hears, at most once a minute per network; "Disconnect" stops that until a network
 *    is picked again.
 *
 * How it runs without a launchd job of its own: tools/build-xnu.sh builds it into the kernel; at boot the kernel
 * writes it to /usr/sbin/op2netd (bsd/kern/op2_shell_sync.c), and when launchd starts /usr/sbin/wifid (Apple's Wi-Fi
 * daemon, which has no Apple Wi-Fi hardware to drive on this phone) the kernel starts this file instead. Here the
 * process forks: the child leaves the job's process group and does the work for as long as the phone runs (one copy:
 * a lock file), the parent becomes the real wifid with execv (the kernel does not redirect op2netd's own exec), so
 * launchd's job and wifid's Mach services carry on as before. Log lines go to /dev/console (the kernel log).
 */
#include <CommonCrypto/CommonKeyDerivation.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
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

/* ---- the kernel's side ---- */

static int
kread(const char *name, char *buf, size_t n)   /* a string sysctl; "" when it is not there */
{
	size_t len = n - 1;
	if (sysctlbyname(name, buf, &len, NULL, 0) != 0) {
		buf[0] = '\0';
		return 0;
	}
	buf[len < n ? len : n - 1] = '\0';
	return 1;
}

static int
kcmd(const char *cmd)
{
	return sysctlbyname("net.op2wlan_cmd", NULL, NULL, (void *)(uintptr_t)cmd, strlen(cmd) + 1) == 0;
}

static void
hex_of(const unsigned char *p, size_t n, char *out)   /* out: 2n + 1 */
{
	static const char d[] = "0123456789abcdef";
	for (size_t i = 0; i < n; i++) {
		out[2 * i] = d[p[i] >> 4];
		out[2 * i + 1] = d[p[i] & 15];
	}
	out[2 * n] = '\0';
}

static int
unhex(const char *s, unsigned char *out, size_t max)   /* bytes, or -1 */
{
	size_t n = strlen(s);
	if (n % 2 != 0 || n / 2 > max) {
		return -1;
	}
	for (size_t i = 0; i < n / 2; i++) {
		unsigned v;
		if (sscanf(s + 2 * i, "%2x", &v) != 1) {
			return -1;
		}
		out[i] = (unsigned char)v;
	}
	return (int)(n / 2);
}

/* ---- saved networks: "<name in hex> <PMK in hex | ->" per line ---- */

#ifndef SAVED_DIR
#define SAVED_DIR  "/var/root/Library/op2netd"
#endif
#define SAVED_FILE SAVED_DIR "/networks"
#define SAVED_MAX  32

struct saved {
	char name[65];
	char pmk[65];   /* "-": open */
	time_t tried;   /* last automatic join */
};
static struct saved saved[SAVED_MAX];
static int nsaved;

static void
saved_load(void)
{
	FILE *f = fopen(SAVED_FILE, "r");
	nsaved = 0;
	if (f == NULL) {
		return;
	}
	char line[200];
	while (nsaved < SAVED_MAX && fgets(line, sizeof(line), f) != NULL) {
		struct saved s;
		memset(&s, 0, sizeof(s));
		if (sscanf(line, "%64s %64s", s.name, s.pmk) == 2) {
			saved[nsaved++] = s;
		}
	}
	fclose(f);
}

static void
saved_store(void)
{
	char parent[sizeof(SAVED_DIR)];
	snprintf(parent, sizeof(parent), "%s", SAVED_DIR);
	*strrchr(parent, '/') = '\0';
	mkdir(parent, 0755);
	mkdir(SAVED_DIR, 0700);
	int fd = open(SAVED_FILE ".new", O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) {
		say("op2netd: cannot write " SAVED_FILE "\n");
		return;
	}
	for (int i = 0; i < nsaved; i++) {
		char line[140];
		int n = snprintf(line, sizeof(line), "%s %s\n", saved[i].name, saved[i].pmk);
		(void)write(fd, line, (size_t)n);
	}
	fsync(fd);
	close(fd);
	rename(SAVED_FILE ".new", SAVED_FILE);
}

static struct saved *
saved_find(const char *name)
{
	for (int i = 0; i < nsaved; i++) {
		if (strcmp(saved[i].name, name) == 0) {
			return &saved[i];
		}
	}
	return NULL;
}

static void
saved_put(const char *name, const char *pmk)
{
	struct saved *s = saved_find(name);
	if (s == NULL && nsaved < SAVED_MAX) {
		s = &saved[nsaved++];
	}
	if (s == NULL) {
		s = &saved[0];   /* full: the first one goes */
	}
	memset(s, 0, sizeof(*s));
	snprintf(s->name, sizeof(s->name), "%s", name);
	snprintf(s->pmk, sizeof(s->pmk), "%s", pmk);
	saved_store();
}

static void
saved_forget(const char *name)
{
	for (int i = 0; i < nsaved; i++) {
		if (strcmp(saved[i].name, name) == 0) {
			saved[i] = saved[--nsaved];
			saved_store();
			return;
		}
	}
}

/* ---- the scan list (net.op2wlan_scan) ---- */

#define NETS_MAX 40

struct net {
	char bssid[18], sec[8], name[65];   /* name in hex */
	int chan, dbm;
};
static struct net nets[NETS_MAX];
static int nnets;

static void
nets_load(void)
{
	static char buf[4096];
	nnets = 0;
	if (!kread("net.op2wlan_scan", buf, sizeof(buf))) {
		return;
	}
	char *line = buf, *next;
	for (; line != NULL && *line != '\0' && nnets < NETS_MAX; line = next) {
		next = strchr(line, '\n');
		if (next != NULL) {
			*next++ = '\0';
		}
		struct net n;
		memset(&n, 0, sizeof(n));
		if (sscanf(line, "%17s %d %d %7s %64s", n.bssid, &n.chan, &n.dbm, n.sec, n.name) != 5 ||
		    strcmp(n.name, "-") == 0) {
			continue;   /* hidden networks: "Other..." */
		}
		int dup = 0;   /* one row per name (the strongest: the list is sorted) */
		for (int i = 0; i < nnets && !dup; i++) {
			dup = strcmp(nets[i].name, n.name) == 0;
		}
		if (!dup) {
			nets[nnets++] = n;
		}
	}
}

/* ---- automatic joining ---- */

static int manual_off;   /* "Disconnect" pressed: no automatic joins until a network is picked */

static void
autojoin(void)
{
	char state[200];
	kread("net.op2wlan_state", state, sizeof(state));
	if (manual_off || strcmp(state, "idle") != 0) {
		return;
	}
	nets_load();
	time_t now = time(NULL);
	for (int i = 0; i < nnets; i++) {   /* strongest first */
		struct saved *s = saved_find(nets[i].name);
		if (s != NULL && (s->tried == 0 || now - s->tried >= 60)) {
			char cmd[160];
			snprintf(cmd, sizeof(cmd), "join %s %s", s->name, s->pmk);
			s->tried = now;
			if (kcmd(cmd)) {
				say("op2netd: joining a saved network\n");
			}
			memset(cmd, 0, sizeof(cmd));
			return;
		}
	}
}

/* ---- the page ---- */

struct out {
	char *p;
	size_t n, cap;
};

static void
put(struct out *o, const char *s, size_t n)
{
	if (o->n + n + 1 > o->cap) {
		size_t cap = (o->cap ? o->cap * 2 : 8192) + n;
		char *p = realloc(o->p, cap);
		if (p == NULL) {
			return;
		}
		o->p = p;
		o->cap = cap;
	}
	memcpy(o->p + o->n, s, n);
	o->n += n;
	o->p[o->n] = '\0';
}

static void
puts_(struct out *o, const char *s)
{
	put(o, s, strlen(s));
}

static void
put_text(struct out *o, const unsigned char *b, size_t n)   /* HTML-escaped; control characters as "?" */
{
	for (size_t i = 0; i < n; i++) {
		unsigned char x = b[i];
		if (x == '&') {
			puts_(o, "&amp;");
		} else if (x == '<') {
			puts_(o, "&lt;");
		} else if (x == '>') {
			puts_(o, "&gt;");
		} else if (x == '"') {
			puts_(o, "&quot;");
		} else if (x < 0x20 || x == 0x7f) {
			puts_(o, "?");
		} else {
			char c = (char)x;   /* UTF-8 bytes pass through */
			put(o, &c, 1);
		}
	}
}

static void
put_name(struct out *o, const char *hex)   /* a network's name, from hex */
{
	unsigned char b[32];
	int n = unhex(hex, b, sizeof(b));
	if (n <= 0) {
		puts_(o, "(hidden network)");
		return;
	}
	put_text(o, b, (size_t)n);
}

static const char kHead[] =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
    "<meta name=\"apple-mobile-web-app-capable\" content=\"yes\"><meta name=\"apple-mobile-web-app-title\" content=\"Wi-Fi\">"
    "<title>Wi-Fi</title><style>"
    "body{font:17px -apple-system,Helvetica,sans-serif;background:#efeff4;margin:0;color:#000}"
    "h1{font-size:28px;margin:18px 16px 8px}h2{font-size:13px;color:#6d6d72;text-transform:uppercase;margin:22px 16px 6px;"
    "font-weight:normal}.box{background:#fff;border-top:1px solid #c8c7cc;border-bottom:1px solid #c8c7cc}"
    ".row{display:flex;align-items:center;padding:11px 16px;border-bottom:1px solid #e5e5ea;color:#000;text-decoration:none}"
    ".row:last-child{border-bottom:0}.name{flex:1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}"
    ".meta{color:#8e8e93;font-size:15px;margin-left:8px}.ok{color:#34c759}.bad{color:#ff3b30}"
    "form{margin:0}button,input[type=submit]{font:17px -apple-system,Helvetica;background:none;border:0;color:#007aff;"
    "padding:11px 16px;width:100%;text-align:left}.danger{color:#ff3b30}"
    "input[type=password],input[type=text]{font:17px -apple-system;border:0;padding:11px 16px;width:100%;"
    "box-sizing:border-box}label{display:block;padding:11px 16px 0;color:#6d6d72;font-size:15px}"
    "</style>";

static void
page_main(struct out *o)
{
	char state[200], verb[20] = "", name[70] = "", rest[160] = "";
	kread("net.op2wlan_state", state, sizeof(state));
	sscanf(state, "%19s %69s %159[^\n]", verb, name, rest);
	nets_load();
	puts_(o, kHead);
	puts_(o, "<meta http-equiv=\"refresh\" content=\"5\"></head><body><h1>Wi-Fi</h1><div class=\"box\">"
	    "<div class=\"row\"><span class=\"name\">");
	if (state[0] == '\0') {
		puts_(o, "The Wi-Fi manager is not running (boot-arg <b>wifi=12</b>)");
	} else if (strcmp(verb, "connected") == 0) {
		puts_(o, "<span class=\"ok\">&#10003;</span> ");
		put_name(o, name);
		puts_(o, "</span><span class=\"meta\">");
		put_text(o, (const unsigned char *)rest, strlen(rest));
	} else if (strcmp(verb, "joining") == 0) {
		puts_(o, "Joining ");
		put_name(o, name);
		puts_(o, "&hellip;");
	} else if (strcmp(verb, "failed") == 0) {
		puts_(o, "<span class=\"bad\">Could not join</span> ");
		put_name(o, name);
		puts_(o, "</span></div><div class=\"row\"><span class=\"meta\" style=\"margin:0;white-space:normal\">");
		put_text(o, (const unsigned char *)rest, strlen(rest));
	} else if (strcmp(verb, "scanning") == 0) {
		puts_(o, "Not connected &middot; looking for networks&hellip;");
	} else if (strcmp(verb, "off") == 0) {
		puts_(o, "Wi-Fi is off until the phone restarts");
	} else {
		puts_(o, manual_off ? "Not connected (automatic joining paused)" : "Not connected");
	}
	puts_(o, "</span></div>");
	if (strcmp(verb, "connected") == 0 || strcmp(verb, "joining") == 0) {
		puts_(o, "<form method=\"post\" action=\"/leave\"><button>Disconnect</button></form>");
	}
	puts_(o, "</div><h2>Networks</h2><div class=\"box\">");
	for (int i = 0; i < nnets; i++) {
		char line[300];
		snprintf(line, sizeof(line), "<a class=\"row\" href=\"/join?n=%s\"><span class=\"name\">", nets[i].name);
		puts_(o, line);
		put_name(o, nets[i].name);
		snprintf(line, sizeof(line), "</span><span class=\"meta\">%s%s%s ch %d</span></a>",
		    saved_find(nets[i].name) != NULL ? "saved &middot; " : "", strcmp(nets[i].sec, "open") == 0 ? "" : "&#128274; ",
		    nets[i].dbm >= -55 ? "&#9679;&#9679;&#9679;" : nets[i].dbm >= -70 ? "&#9679;&#9679;&#9675;" :
		    "&#9679;&#9675;&#9675;", nets[i].chan);
		puts_(o, line);
	}
	if (nnets == 0) {
		puts_(o, "<div class=\"row\"><span class=\"meta\" style=\"margin:0\">None heard yet</span></div>");
	}
	puts_(o, "<a class=\"row\" href=\"/join\"><span class=\"name\">Other&hellip;</span></a>"
	    "<form method=\"post\" action=\"/scan\"><button>Scan again</button></form></div>");
	if (nsaved > 0) {
		puts_(o, "<h2>Saved networks</h2><div class=\"box\">");
		for (int i = 0; i < nsaved; i++) {
			char line[300];
			snprintf(line, sizeof(line), "<form class=\"row\" method=\"post\" action=\"/forget\"><input type=\"hidden\" "
			    "name=\"n\" value=\"%s\"><span class=\"name\">", saved[i].name);
			puts_(o, line);
			put_name(o, saved[i].name);
			puts_(o, "</span><button class=\"danger\" style=\"width:auto\">Forget</button></form>");
		}
		puts_(o, "</div>");
	}
	puts_(o, "<h2>&nbsp;</h2><div class=\"box\"><form method=\"post\" action=\"/off\"><button class=\"danger\">Turn Wi-Fi "
	    "off until restart</button></form></div><h2>In Safari, Share &rarr; Add to Home Screen gives this page an "
	    "icon</h2></body></html>");
}

static void
page_join(struct out *o, const char *hexname)   /* NULL: "Other...", a name to type */
{
	const char *sec = "WPA2";
	nets_load();
	for (int i = 0; hexname != NULL && i < nnets; i++) {
		if (strcmp(nets[i].name, hexname) == 0) {
			sec = nets[i].sec;
		}
	}
	puts_(o, kHead);
	puts_(o, "</head><body><h1>");
	if (hexname != NULL) {
		put_name(o, hexname);
	} else {
		puts_(o, "Other network");
	}
	puts_(o, "</h1><form method=\"post\" action=\"/join\"><div class=\"box\">");
	if (hexname != NULL) {
		char h[120];
		snprintf(h, sizeof(h), "<input type=\"hidden\" name=\"n\" value=\"%s\">", hexname);
		puts_(o, h);
	} else {
		puts_(o, "<label>Name</label><input type=\"text\" name=\"name\" autocapitalize=\"off\" autocorrect=\"off\">");
	}
	if (hexname == NULL || strcmp(sec, "open") != 0) {
		puts_(o, "<label>Password (empty for an open network)</label><input type=\"password\" name=\"p\" autofocus>");
	}
	puts_(o, "<input type=\"submit\" value=\"Join\"></div></form>"
	    "<h2><a href=\"/\" style=\"color:#007aff;text-decoration:none\">&larr; Back</a></h2></body></html>");
}

/* ---- HTTP ---- */

static void
form_get(const char *body, const char *key, char *out, size_t n)   /* application/x-www-form-urlencoded */
{
	size_t kl = strlen(key);
	out[0] = '\0';
	for (const char *p = body; p != NULL && *p != '\0';) {
		const char *amp = strchr(p, '&');
		size_t len = amp != NULL ? (size_t)(amp - p) : strlen(p);
		if (len > kl && strncmp(p, key, kl) == 0 && p[kl] == '=') {
			size_t o = 0;
			for (size_t i = kl + 1; i < len && o + 1 < n; i++) {
				char c = p[i];
				unsigned v;
				if (c == '+') {
					c = ' ';
				} else if (c == '%' && i + 2 < len && sscanf(p + i + 1, "%2x", &v) == 1) {
					c = (char)v;
					i += 2;
				}
				out[o++] = c;
			}
			out[o] = '\0';
			return;
		}
		p = amp != NULL ? amp + 1 : NULL;
	}
}

static void
reply(int fd, const char *status, const char *extra, const char *body, size_t n)
{
	char h[300];
	int hn = snprintf(h, sizeof(h), "HTTP/1.1 %s\r\nContent-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\n"
	    "Content-Length: %zu\r\nConnection: close\r\n%s\r\n", status, n, extra);
	(void)write(fd, h, (size_t)hn);
	size_t off = 0;
	while (off < n) {
		ssize_t w = write(fd, body + off, n - off);
		if (w <= 0) {
			break;
		}
		off += (size_t)w;
	}
}

static void
do_join(const char *body)
{
	char n[80], name[40], pw[100];
	unsigned char ssid[32];
	int sl;
	form_get(body, "n", n, sizeof(n));
	if (n[0] != '\0') {
		sl = unhex(n, ssid, sizeof(ssid));
	} else {
		form_get(body, "name", name, sizeof(name));
		sl = (int)strlen(name);
		if (sl > 32) {
			sl = -1;
		} else {
			memcpy(ssid, name, (size_t)sl);
		}
	}
	form_get(body, "p", pw, sizeof(pw));
	if (sl <= 0) {
		memset(pw, 0, sizeof(pw));
		return;
	}
	char hexname[65], pmkhex[65] = "-";
	hex_of(ssid, (size_t)sl, hexname);
	size_t pl = strlen(pw);
	unsigned char pmk[32];
	if (pl == 64 && unhex(pw, pmk, sizeof(pmk)) == 32) {   /* a raw 256-bit key */
		hex_of(pmk, 32, pmkhex);
	} else if (pl >= 8 && pl <= 63) {
		if (CCKeyDerivationPBKDF(kCCPBKDF2, pw, pl, ssid, (size_t)sl, kCCPRFHmacAlgSHA1, 4096, pmk, 32) != 0) {
			say("op2netd: PBKDF2 failed\n");
			memset(pw, 0, sizeof(pw));
			return;
		}
		hex_of(pmk, 32, pmkhex);
	} else if (pl != 0) {
		say("op2netd: a WPA2 password has 8 to 63 characters; not joined\n");
		memset(pw, 0, sizeof(pw));
		return;
	}
	memset(pw, 0, sizeof(pw));
	memset(pmk, 0, sizeof(pmk));
	saved_put(hexname, pmkhex);
	manual_off = 0;
	struct saved *s = saved_find(hexname);
	if (s != NULL) {
		s->tried = time(NULL);
	}
	char cmd[160];
	snprintf(cmd, sizeof(cmd), "join %s %s", hexname, pmkhex);
	if (!kcmd(cmd)) {
		say("op2netd: the kernel took no join command (boot-arg wifi=12)\n");
	}
	memset(cmd, 0, sizeof(cmd));
	memset(pmkhex, 0, sizeof(pmkhex));
}

static void
serve(int fd)
{
	static char req[8192];
	size_t n = 0, want = 0;
	char *body = NULL;
	struct timeval tv = { 2, 0 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	while (n < sizeof(req) - 1) {
		ssize_t r = read(fd, req + n, sizeof(req) - 1 - n);
		if (r <= 0) {
			break;
		}
		n += (size_t)r;
		req[n] = '\0';
		char *end = body != NULL ? NULL : strstr(req, "\r\n\r\n");
		if (end != NULL) {
			body = end + 4;
			*end = '\0';   /* headers end here; the body follows */
			const char *cl = strcasestr(req, "\r\nContent-Length:");
			want = cl != NULL ? (size_t)strtoul(cl + 17, NULL, 10) : 0;
		}
		if (body != NULL && (size_t)(req + n - body) >= want) {
			break;
		}
	}
	req[n] = '\0';
	char method[8] = "", path[256] = "";
	if (sscanf(req, "%7s %255s", method, path) != 2) {
		return;
	}
	if (body == NULL) {
		body = req + n;
	}
	struct out o = { NULL, 0, 0 };
	if (strcmp(method, "GET") == 0 && strcmp(path, "/") == 0) {
		page_main(&o);
	} else if (strcmp(method, "GET") == 0 && strncmp(path, "/join", 5) == 0 && (path[5] == '\0' || path[5] == '?')) {
		char hx[80] = "";
		unsigned char tmp[32];
		if (path[5] == '?') {
			form_get(path + 6, "n", hx, sizeof(hx));
		}
		page_join(&o, hx[0] != '\0' && unhex(hx, tmp, sizeof(tmp)) > 0 ? hx : NULL);
	} else if (strcmp(method, "POST") == 0) {
		if (strcmp(path, "/join") == 0) {
			do_join(body);
		} else if (strcmp(path, "/leave") == 0) {
			manual_off = 1;
			kcmd("leave");
		} else if (strcmp(path, "/scan") == 0) {
			kcmd("scan");
		} else if (strcmp(path, "/forget") == 0) {
			char hx[80], state[200], cur[70] = "";
			form_get(body, "n", hx, sizeof(hx));
			kread("net.op2wlan_state", state, sizeof(state));
			sscanf(state, "connected %69s", cur);
			if (hx[0] != '\0' && strcmp(cur, hx) == 0) {
				manual_off = 1;
				kcmd("leave");
			}
			saved_forget(hx);
		} else if (strcmp(path, "/off") == 0) {
			kcmd("off");
		}
		memset(req, 0, sizeof(req));   /* a password may be in it */
		reply(fd, "303 See Other", "Location: /\r\n", "", 0);
		return;
	} else {
		reply(fd, "404 Not Found", "", "not here", 8);
		return;
	}
	reply(fd, "200 OK", "", o.p != NULL ? o.p : "", o.n);
	free(o.p);
}

static int
listen_local(void)
{
	int s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0) {
		return -1;
	}
	int one = 1;
	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	struct sockaddr_in a;
	memset(&a, 0, sizeof(a));
#ifdef __APPLE__
	a.sin_len = sizeof(a);
#endif
	a.sin_family = AF_INET;
	a.sin_port = htons(8080);
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   /* only the phone itself */
	if (bind(s, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(s, 8) != 0) {
		close(s);
		return -1;
	}
	return s;
}

static void
run(void)
{
	struct publish_state ps;
	memset(&ps, 0, sizeof(ps));
	saved_load();
	int ls = -1;
	time_t last = 0, last_try = 0;
	for (;;) {
		time_t now = time(NULL);
		if (ls < 0 && now - last_try >= 10) {   /* loopback may not be up yet: retry */
			last_try = now;
			ls = listen_local();
			if (ls >= 0) {
				say("op2netd: Wi-Fi page at http://127.0.0.1:8080\n");
			}
		}
		if (now - last >= 3) {
			last = now;
			publish_step(&ps);
			autojoin();
		}
		fd_set rd;
		FD_ZERO(&rd);
		if (ls >= 0) {
			FD_SET(ls, &rd);
		}
		struct timeval tv = { 1, 0 };
		if (select(ls >= 0 ? ls + 1 : 0, &rd, NULL, NULL, &tv) > 0 && ls >= 0 && FD_ISSET(ls, &rd)) {
			int c = accept(ls, NULL, NULL);
			if (c >= 0) {
				serve(c);
				close(c);
			}
		}
	}
}

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
			_exit(0);   /* another op2netd runs already (wifid was restarted) */
		}
		char msg[160];
		snprintf(msg, sizeof(msg), "op2netd: publishing ue0/wl0 to configd, Wi-Fi page (pid %d, uid %d)\n", (int)getpid(),
		    (int)getuid());
		say(msg);
		run();   /* never returns */
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
