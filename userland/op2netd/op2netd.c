/*
 * op2netd: the phone's own network helper, and its Wi-Fi manager.
 *
 *  - Tells iOS's configd about wl0 (Wi-Fi) and ue0 (USB), so Safari gets a default route and DNS: op2shell's
 *    publish code (userland/op2shell/op2_publish.h), without the shell, SSH and the builds those need.
 *  - The Wi-Fi manager (kernel boot-arg wifi=12): a page at http://127.0.0.1:8080 on the phone (Safari; "Add to Home
 *    Screen" gives it an icon) lists the networks the driver heard, joins one with its password, disconnects,
 *    forgets, turns Wi-Fi off. It drives the kernel through sysctls (bsd/dev/op2_wlan.c): net.op2wlan_cmd (scan,
 *    join <name in hex> <PMK in hex | -> [static <address> <netmask> <router> <dns>], leave, off), net.op2wlan_scan,
 *    net.op2wlan_state. Each saved network has settings (the page's "Advanced"): automatic joining, manual IPv4
 *    instead of DHCP, DNS servers and search domains (given to configd with wl0). The password never
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
#include <sys/sysctl.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_dl.h>
#endif

#define PUBLISHER "op2netd"

static void
say(const char *s)   /* the console, and the kernel's kept log (kern.op2keep: safe from console floods) */
{
	int fd = open("/dev/console", O_WRONLY | O_NOCTTY);
	if (fd >= 0) {
		(void)write(fd, s, strlen(s));
		close(fd);
	}
	(void)sysctlbyname("kern.op2keep", NULL, NULL, (void *)(uintptr_t)s, strlen(s) + 1);
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

/* ---- saved networks: "<name in hex> <PMK in hex | -> [setting=value ...]" per line ----
 * Settings (the page's "Advanced"), none of them = the defaults: auto=0 (no automatic joining), ip=, mask=, router=
 * (a manual IPv4 setting instead of DHCP), dns=a,b,c (DNS servers, in order), dnsauto=0 (only those, not the
 * network's own), search=a.com,b.com (search domains). */

#ifndef SAVED_DIR
#define SAVED_DIR  "/var/root/Library/op2netd"
#endif
#define SAVED_FILE SAVED_DIR "/networks"
#define SAVED_MAX  32

struct saved {
	char name[65];
	char pmk[65];   /* "-": open */
	time_t tried;   /* last automatic join */
	int noauto;     /* no automatic joining */
	char ip[16], mask[16], router[16];   /* manual IPv4 ("" = DHCP) */
	char dns[64];   /* DNS servers, comma-separated ("" = the network's) */
	int dns_only;   /* the servers above only, not the network's too */
	char search[128];   /* search domains, comma-separated */
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
		int used = 0;
		if (sscanf(line, "%64s %64s%n", s.name, s.pmk, &used) == 2) {
			char tok[200];
			int more = 0;
			for (const char *p = line + used; sscanf(p, "%199s%n", tok, &more) == 1; p += more) {
				char *eq = strchr(tok, '=');
				if (eq == NULL) {
					continue;
				}
				*eq++ = '\0';
				if (strcmp(tok, "auto") == 0) {
					s.noauto = strcmp(eq, "0") == 0;
				} else if (strcmp(tok, "ip") == 0) {
					snprintf(s.ip, sizeof(s.ip), "%s", eq);
				} else if (strcmp(tok, "mask") == 0) {
					snprintf(s.mask, sizeof(s.mask), "%s", eq);
				} else if (strcmp(tok, "router") == 0) {
					snprintf(s.router, sizeof(s.router), "%s", eq);
				} else if (strcmp(tok, "dns") == 0) {
					snprintf(s.dns, sizeof(s.dns), "%s", eq);
				} else if (strcmp(tok, "dnsauto") == 0) {
					s.dns_only = strcmp(eq, "0") == 0;
				} else if (strcmp(tok, "search") == 0) {
					snprintf(s.search, sizeof(s.search), "%s", eq);
				}
			}
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
		const struct saved *v = &saved[i];
		char line[600];
		int n = snprintf(line, sizeof(line), "%s %s%s%s%s%s%s%s%s%s%s%s%s%s\n", v->name, v->pmk, v->noauto ? " auto=0" : "",
		    v->ip[0] ? " ip=" : "", v->ip, v->mask[0] ? " mask=" : "", v->mask, v->router[0] ? " router=" : "", v->router,
		    v->dns[0] ? " dns=" : "", v->dns, v->dns_only ? " dnsauto=0" : "", v->search[0] ? " search=" : "", v->search);
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
		memset(s, 0, sizeof(*s));
	}
	if (strcmp(s->name, name) != 0) {   /* a new entry; a known one keeps its settings */
		memset(s, 0, sizeof(*s));
		snprintf(s->name, sizeof(s->name), "%s", name);
	}
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

/* "join <name> <PMK | -> [static <address> <netmask> <router> <dns>]": the kernel's command for a saved network */
static void
hex_ip(const char *dotted, char out[9])   /* "" -> "00000000" */
{
	struct in_addr a;
	a.s_addr = 0;
	if (dotted[0] != '\0') {
		(void)inet_pton(AF_INET, dotted, &a);
	}
	uint32_t v = ntohl(a.s_addr);
	snprintf(out, 9, "%08x", v);
}

static void
join_cmd(const char *name, const char *pmk, const struct saved *s, char *out, size_t n)
{
	if (s == NULL || s->ip[0] == '\0') {
		snprintf(out, n, "join %s %s", name, pmk);
		return;
	}
	char dns1[16] = "", a[9], m[9], r[9], d[9];
	sscanf(s->dns, "%15[^,]", dns1);   /* the kernel takes one DNS server (net.op2wlan); configd gets them all */
	hex_ip(s->ip, a);
	hex_ip(s->mask, m);
	hex_ip(s->router, r);
	hex_ip(dns1[0] != '\0' ? dns1 : s->router, d);
	snprintf(out, n, "join %s %s static %s %s %s %s", name, pmk, a, m, r, d);
}

/* the DNS servers configd gets for a network, space-separated, in order ("" with no setting: the default, the lease's
 * server then 1.1.1.1); lease_dns is the kernel's (net.op2wlan) */
static void
dns_effective(const struct saved *s, const char *ldns, char *out, size_t n)
{
	out[0] = '\0';
	if (s == NULL || s->dns[0] == '\0') {
		return;
	}
	size_t o = 0;
	char w[64];
	snprintf(w, sizeof(w), "%s", s->dns);
	for (char *t = strtok(w, ","); t != NULL; t = strtok(NULL, ",")) {
		o += (size_t)snprintf(out + o, n - o, "%s%s", o ? " " : "", t);
		if (o >= n) {
			out[n - 1] = '\0';
			return;
		}
	}
	if (!s->dns_only && ldns[0] != '\0' && strcmp(ldns, "0.0.0.0") != 0) {
		char pad[100];
		snprintf(pad, sizeof(pad), " %s ", out);
		char me[24];
		snprintf(me, sizeof(me), " %s ", ldns);
		if (strstr(pad, me) == NULL && o + 1 + strlen(ldns) < n) {
			snprintf(out + o, n - o, " %s", ldns);
		}
	}
}

static void
commas_to_spaces(const char *in, char *out, size_t n)
{
	snprintf(out, n, "%s", in);
	for (char *p = out; *p != '\0'; p++) {
		if (*p == ',') {
			*p = ' ';
		}
	}
}

static void
lease_dns(char out[16])   /* the kernel's DNS server for wl0, "" while down */
{
	char lease[120] = "", ifn[8], a[16], b[16], c[16];
	out[0] = '\0';
	kread("net.op2wlan", lease, sizeof(lease));
	if (sscanf(lease, "%7s %15s %15s %15s %15s", ifn, a, b, c, out) != 5) {
		out[0] = '\0';
	}
}

/* ---- automatic joining ---- */

static int manual_off;   /* "Disconnect" pressed: no automatic joins until a network is picked */
static char note[160];   /* shown once at the top of the next page (a refused join) */
static char conn_name[70];   /* the network connected to, and since when (the page's "Connected for") */
static time_t conn_since;

static void
track_state(const char *state)
{
	char name[70] = "";
	if (sscanf(state, "connected %69s", name) != 1) {
		conn_name[0] = '\0';
	} else if (strcmp(name, conn_name) != 0) {
		snprintf(conn_name, sizeof(conn_name), "%s", name);
		conn_since = time(NULL);
	}
}

static int
wl_ifinfo(char *mac, size_t macn, unsigned long long *rx, unsigned long long *tx)   /* wl0's MAC and byte counts */
{
#ifdef __APPLE__
	struct ifaddrs *list, *a;
	int found = 0;
	if (getifaddrs(&list) != 0) {
		return 0;
	}
	for (a = list; a != NULL; a = a->ifa_next) {
		if (a->ifa_addr != NULL && a->ifa_addr->sa_family == AF_LINK && strcmp(a->ifa_name, "wl0") == 0) {
			const struct sockaddr_dl *dl = (const struct sockaddr_dl *)(const void *)a->ifa_addr;
			const unsigned char *m = (const unsigned char *)LLADDR(dl);
			if (dl->sdl_alen == 6) {
				snprintf(mac, macn, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
			}
			if (a->ifa_data != NULL) {
				const struct if_data *d = (const struct if_data *)a->ifa_data;
				*rx = d->ifi_ibytes;
				*tx = d->ifi_obytes;
			}
			found = 1;
			break;
		}
	}
	freeifaddrs(list);
	return found;
#else   /* the host test: "MAC rx tx" in the file standing in for sysctl test.wl0 */
	char b[100];
	kread("test.wl0", b, sizeof(b));
	char m[18];
	if (sscanf(b, "%17s %llu %llu", m, rx, tx) != 3) {
		return 0;
	}
	snprintf(mac, macn, "%s", m);
	return 1;
#endif
}

static void
autojoin(void)
{
	char state[200];
	kread("net.op2wlan_state", state, sizeof(state));
	track_state(state);
	if (manual_off || strcmp(state, "idle") != 0) {
		return;
	}
	nets_load();
	time_t now = time(NULL);
	for (int i = 0; i < nnets; i++) {   /* strongest first */
		struct saved *s = saved_find(nets[i].name);
		if (s != NULL && !s->noauto && (s->tried == 0 || now - s->tried >= 60)) {
			char cmd[220];
			join_cmd(s->name, s->pmk, s, cmd, sizeof(cmd));
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
    /* a network: tap the name (.pick) to join, the "..." (.more) opens a menu */
    ".net{display:flex;align-items:center;border-bottom:1px solid #e5e5ea;position:relative}.net:last-child{border-bottom:0}"
    ".net form{flex:1;min-width:0}.pick{display:block;flex:1;min-width:0;padding:11px 16px;color:#000;text-decoration:none;"
    "font:17px -apple-system,Helvetica;background:none;border:0;text-align:left;width:100%}"
    ".pick .name{display:block;white-space:normal;word-break:break-all}.pick .meta{display:block;margin:2px 0 0;font-size:13px}"
    ".more summary{list-style:none;padding:8px 16px;color:#007aff;font-size:22px;cursor:pointer}"
    ".more summary::-webkit-details-marker{display:none}"
    ".menu{position:absolute;right:8px;top:80%;background:#fff;border-radius:12px;box-shadow:0 4px 24px rgba(0,0,0,.3);"
    "z-index:2;min-width:220px;overflow:hidden}.menu a,.menu button{display:block;padding:12px 16px;color:#007aff;"
    "text-decoration:none;border-bottom:1px solid #e5e5ea}.menu .danger{color:#ff3b30}.menu form:last-child button{border-bottom:0}"
    ".note{background:#fff3cd;padding:11px 16px;font-size:15px}"
    /* the connected network: tap it for its details */
    ".info summary{list-style:none;display:block;padding:11px 16px;cursor:pointer;border-bottom:1px solid #e5e5ea}"
    ".info summary::-webkit-details-marker{display:none}.info .name{display:block}"
    ".chev{float:right;width:8px;height:8px;margin:4px 4px 0 12px;border-right:2px solid #c7c7cc;"
    "border-bottom:2px solid #c7c7cc;transform:rotate(45deg)}.info[open] .chev{transform:rotate(-135deg);margin-top:8px}"
    ".kv{display:flex;padding:9px 16px 9px 32px;border-bottom:1px solid #e5e5ea;font-size:15px}"
    ".kv span:first-child{flex:1}.kv span:last-child{color:#8e8e93;text-align:right;word-break:break-all}"
    /* the Advanced page: label + value rows, switches */
    ".field{display:flex;align-items:center;padding-left:16px;border-bottom:1px solid #e5e5ea}.field:last-child{border-bottom:0}"
    ".field>span{flex:none}.field input[type=text]{text-align:right;color:#3c3c43;padding-left:4px}"
    ".field select{flex:1;-webkit-appearance:none;appearance:none;background:none;border:0;font:17px -apple-system,"
    "Helvetica;color:#8e8e93;text-align:right;text-align-last:right;padding:11px 16px;direction:rtl}"
    ".sw{-webkit-appearance:none;appearance:none;width:51px;height:31px;border-radius:16px;background:#e5e5ea;"
    "position:relative;margin:6px 16px;flex:none;outline:0;cursor:pointer}.sw:checked{background:#34c759}"
    ".sw::after{content:'';position:absolute;top:2px;left:2px;width:27px;height:27px;border-radius:50%;background:#fff;"
    "box-shadow:0 2px 4px rgba(0,0,0,.25);transition:left .15s}.sw:checked::after{left:22px}"
    ".field .grow{flex:1}label.field{padding:0 0 0 16px;color:#000;font-size:17px}.field>span{width:140px}"
    ".field input:focus{outline:0}.hint{text-transform:none;line-height:1.35}h1{word-break:break-all}"
    "</style>";

/* reload every 5 s, but not while a network's menu is open (a reload would close it) */
static const char kRefresh[] =
    "<noscript><meta http-equiv=\"refresh\" content=\"5\"></noscript><script>(function r(){setTimeout(function(){"
    "if(document.querySelector('.more[open]')||document.activeElement.tagName=='INPUT')r();else location.reload()},5000)})()"
    "</script>";

/* at the end of the page: the details panel stays open across reloads (#info in the address) */
static const char kInfoScript[] =
    "<script>(function(){var d=document.getElementById('info');if(!d)return;if(location.hash=='#info')d.open=true;"
    "d.firstChild.onclick=function(){setTimeout(function(){history.replaceState(null,'',d.open?'#info':location.pathname)},0)}"
    "})()</script>";

static void
fmt_bytes(unsigned long long b, char *out, size_t n)
{
	if (b >= 1024ULL * 1024 * 1024) {
		snprintf(out, n, "%.2f GB", (double)b / (1024.0 * 1024 * 1024));
	} else if (b >= 1024ULL * 1024) {
		snprintf(out, n, "%.1f MB", (double)b / (1024.0 * 1024));
	} else {
		snprintf(out, n, "%llu KB", b / 1024);
	}
}

static void
put_kv(struct out *o, const char *k, const char *v)
{
	puts_(o, "<div class=\"kv\"><span>");
	puts_(o, k);
	puts_(o, "</span><span>");
	put_text(o, (const unsigned char *)v, strlen(v));
	puts_(o, "</span></div>");
}

static void
put_details(struct out *o, const char *hexname)   /* the connected network's details */
{
	char lease[120] = "", ifn[8], ip[20] = "", mask[20] = "", gw[20] = "", dns[20] = "", v[80];
	kread("net.op2wlan", lease, sizeof(lease));
	sscanf(lease, "%7s %19s %19s %19s %19s", ifn, ip, mask, gw, dns);
	put_kv(o, "IP address", ip[0] != '\0' ? ip : "-");
	put_kv(o, "Subnet mask", mask[0] != '\0' ? mask : "-");
	put_kv(o, "Router", gw[0] != '\0' ? gw : "-");
	const struct saved *sv = saved_find(hexname);
	char dl[100];
	dns_effective(sv, dns, dl, sizeof(dl));
	if (dl[0] == '\0' && dns[0] != '\0') {
		snprintf(dl, sizeof(dl), "%s 1.1.1.1", dns);   /* what op2_publish.h gives configd by default */
	}
	char ds[4][20];   /* one row per server, in order: "DNS 1", "DNS 2", ... ("DNS" when there is one) */
	int nds = sscanf(dl, "%19s %19s %19s %19s", ds[0], ds[1], ds[2], ds[3]);
	if (nds <= 0) {
		put_kv(o, "DNS", "-");
	}
	for (int i = 0; i < nds; i++) {
		char lab[12];
		snprintf(lab, sizeof(lab), nds == 1 ? "DNS" : "DNS %d", i + 1);
		put_kv(o, lab, ds[i]);
	}
	if (sv != NULL && sv->search[0] != '\0') {
		put_kv(o, "Search domains", sv->search);
	}
	put_kv(o, "IPv4", sv != NULL && sv->ip[0] != '\0' ? "Manual" : "Automatic (DHCP)");
	for (int i = 0; i < nnets; i++) {   /* the strongest access point of that name at the last scan */
		if (strcmp(nets[i].name, hexname) == 0) {
			put_kv(o, "Security", strcmp(nets[i].sec, "open") == 0 ? "None" : nets[i].sec);
			snprintf(v, sizeof(v), "%d (%s GHz)", nets[i].chan, nets[i].chan <= 14 ? "2.4" : "5");
			put_kv(o, "Channel", v);
			snprintf(v, sizeof(v), "%d dBm (last scan)", nets[i].dbm);
			put_kv(o, "Signal", v);
			put_kv(o, "Access point", nets[i].bssid);
			break;
		}
	}
	char mac[20] = "";
	unsigned long long rx = 0, tx = 0;
	if (wl_ifinfo(mac, sizeof(mac), &rx, &tx)) {
		if (mac[0] != '\0') {
			put_kv(o, "Wi-Fi address", mac);
		}
		fmt_bytes(rx, v, sizeof(v));
		put_kv(o, "Received", v);
		fmt_bytes(tx, v, sizeof(v));
		put_kv(o, "Sent", v);
	}
	if (strcmp(conn_name, hexname) == 0 && conn_since != 0) {
		long t = (long)(time(NULL) - conn_since);
		if (t >= 3600) {
			snprintf(v, sizeof(v), "%ld h %ld min", t / 3600, t / 60 % 60);
		} else {
			snprintf(v, sizeof(v), "%ld min %ld s", t / 60, t % 60);
		}
		put_kv(o, "Connected for", v);
	}
	if (sv != NULL) {
		char line[200];
		snprintf(line, sizeof(line), "<a class=\"row\" href=\"/net?n=%s\" style=\"color:#007aff;padding-left:32px\">"
		    "Advanced&hellip;</a>", hexname);
		puts_(o, line);
	}
}

static void
page_main(struct out *o)
{
	char state[200], verb[20] = "", name[70] = "", rest[160] = "";
	kread("net.op2wlan_state", state, sizeof(state));
	sscanf(state, "%19s %69s %159[^\n]", verb, name, rest);
	nets_load();
	puts_(o, kHead);
	puts_(o, kRefresh);
	puts_(o, "</head><body><h1>Wi-Fi</h1><div class=\"box\">");
	if (note[0] != '\0') {
		puts_(o, "<div class=\"note\">");
		put_text(o, (const unsigned char *)note, strlen(note));
		puts_(o, "</div>");
		note[0] = '\0';
	}
	track_state(state);
	if (strcmp(verb, "connected") == 0) {
		puts_(o, "<details class=\"info\" id=\"info\"><summary><span class=\"chev\"></span><span class=\"name\">"
		    "<span class=\"ok\">&#10003;</span> ");
		put_name(o, name);
		puts_(o, "</span><span class=\"meta\" style=\"margin:0\">");
		put_text(o, (const unsigned char *)rest, strlen(rest));
		puts_(o, "</span></summary>");
		put_details(o, name);
		puts_(o, "</details><div><span>");
	} else {
		puts_(o, "<div class=\"row\"><span class=\"name\">");
	}
	if (state[0] == '\0') {
		puts_(o, "The Wi-Fi manager is not running (boot-arg <b>wifi=12</b>)");
	} else if (strcmp(verb, "connected") == 0) {
		/* above */
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
	int busy = strcmp(verb, "connected") == 0 || strcmp(verb, "joining") == 0;
	for (int i = 0; i < nnets; i++) {
		const char *hx = nets[i].name;
		int known = saved_find(hx) != NULL, open = strcmp(nets[i].sec, "open") == 0;
		int cur = busy && strcmp(name, hx) == 0;
		char line[600], meta[200];
		snprintf(meta, sizeof(meta), "<span class=\"meta\">%s%s%s ch %d</span>", known ? "saved &middot; " : "",
		    open ? "" : "&#128274; ", nets[i].dbm >= -55 ? "&#9679;&#9679;&#9679;" : nets[i].dbm >= -70 ?
		    "&#9679;&#9679;&#9675;" : "&#9679;&#9675;&#9675;", nets[i].chan);
		puts_(o, "<div class=\"net\">");
		if (cur) {   /* the one in use: nothing to tap */
			puts_(o, "<span class=\"pick\"><span class=\"name\"><span class=\"ok\">&#10003;</span> ");
		} else if (known || open) {   /* joins at once: the saved password, or none */
			snprintf(line, sizeof(line), "<form method=\"post\" action=\"/join\"><input type=\"hidden\" name=\"n\" "
			    "value=\"%s\"><button class=\"pick\"><span class=\"name\">", hx);
			puts_(o, line);
		} else {   /* a password to type first */
			snprintf(line, sizeof(line), "<a class=\"pick\" href=\"/join?n=%s\"><span class=\"name\">", hx);
			puts_(o, line);
		}
		put_name(o, hx);   /* the whole name (wrapped): similar names differ at the end */
		puts_(o, "</span>");
		puts_(o, meta);
		puts_(o, cur ? "</span>" : known || open ? "</button></form>" : "</a>");
		if (known) {
			snprintf(line, sizeof(line), "<details class=\"more\"><summary>&#8943;</summary><div class=\"menu\">"
			    "<a href=\"/net?n=%s\">Advanced&hellip;</a><a href=\"/join?n=%s\">Change password&hellip;</a>"
			    "<form method=\"post\" action=\"/forget\"><input type=\"hidden\" name=\"n\" value=\"%s\">"
			    "<button class=\"danger\">Forget this network</button></form></div></details>", hx, hx, hx);
			puts_(o, line);
		}
		puts_(o, "</div>");
	}
	if (nnets == 0) {
		puts_(o, "<div class=\"row\"><span class=\"meta\" style=\"margin:0\">None heard yet</span></div>");
	}
	puts_(o, "<a class=\"row\" href=\"/join\"><span class=\"name\">Other&hellip;</span></a>"
	    "<form method=\"post\" action=\"/scan\"><button>Scan again</button></form></div>");
	int away = 0;   /* saved networks not heard now (the heard ones have their menu above) */
	for (int i = 0; i < nsaved; i++) {
		int heard = 0;
		for (int j = 0; j < nnets && !heard; j++) {
			heard = strcmp(nets[j].name, saved[i].name) == 0;
		}
		if (heard) {
			continue;
		}
		if (away++ == 0) {
			puts_(o, "<h2>Saved, not in range</h2><div class=\"box\">");
		}
		{
			char line[300];
			snprintf(line, sizeof(line), "<div class=\"net\"><a class=\"pick\" href=\"/net?n=%s\"><span class=\"name\">",
			    saved[i].name);
			puts_(o, line);
			put_name(o, saved[i].name);
			snprintf(line, sizeof(line), "</span><span class=\"meta\">settings &rsaquo;</span></a><form method=\"post\" "
			    "action=\"/forget\" style=\"flex:0\"><input type=\"hidden\" name=\"n\" value=\"%s\"><button "
			    "class=\"danger\" style=\"width:auto\">Forget</button></form></div>", saved[i].name);
			puts_(o, line);
		}
	}
	if (away > 0) {
		puts_(o, "</div>");
	}
	puts_(o, "<h2>&nbsp;</h2><div class=\"box\"><form method=\"post\" action=\"/off\"><button class=\"danger\">Turn Wi-Fi "
	    "off until restart</button></form></div><h2>In Safari, Share &rarr; Add to Home Screen gives this page an "
	    "icon</h2>");
	puts_(o, kInfoScript);
	puts_(o, "</body></html>");
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
	if (hexname != NULL && saved_find(hexname) != NULL) {
		puts_(o, "<label>New password (empty: keep the saved one)</label><input type=\"password\" name=\"p\" autofocus>");
	} else if (hexname == NULL) {
		puts_(o, "<label>Password (empty for an open network)</label><input type=\"password\" name=\"p\" autofocus>");
	} else if (strcmp(sec, "open") != 0) {
		puts_(o, "<label>Password</label><input type=\"password\" name=\"p\" autofocus>");
	}
	puts_(o, "<input type=\"submit\" value=\"Join\"></div></form>"
	    "<h2><a href=\"/\" style=\"color:#007aff;text-decoration:none\">&larr; Back</a></h2></body></html>");
}

static void form_get(const char *body, const char *key, char *out, size_t n);

static void
put_field(struct out *o, const char *label, const char *name, const char *value, const char *placeholder)
{
	puts_(o, "<div class=\"field\"><span>");
	puts_(o, label);
	puts_(o, "</span><input type=\"text\" name=\"");
	puts_(o, name);
	puts_(o, "\" value=\"");
	put_text(o, (const unsigned char *)value, strlen(value));
	puts_(o, "\" placeholder=\"");
	put_text(o, (const unsigned char *)placeholder, strlen(placeholder));
	puts_(o, "\" autocapitalize=\"off\" autocorrect=\"off\" spellcheck=\"false\"");
	puts_(o, strcmp(name, "search") == 0 ? ">" : " inputmode=\"decimal\">");
	puts_(o, "</div>");
}

static void
put_switch(struct out *o, const char *label, const char *name, int on)
{
	puts_(o, "<label class=\"field\"><span class=\"grow\">");
	puts_(o, label);
	puts_(o, "</span><input type=\"checkbox\" class=\"sw\" name=\"");
	puts_(o, name);
	puts_(o, on ? "\" checked></label>" : "\"></label>");
}

/* "Advanced": a saved network's settings (draft: the values just sent back, when they were refused) */
static void
page_net(struct out *o, const char *hexname, const struct saved *draft)
{
	const struct saved *s = draft != NULL ? draft : saved_find(hexname);
	puts_(o, kHead);
	puts_(o, "</head><body><h1>");
	put_name(o, hexname);
	puts_(o, "</h1>");
	if (note[0] != '\0') {
		puts_(o, "<div class=\"box\"><div class=\"note\">");
		put_text(o, (const unsigned char *)note, strlen(note));
		puts_(o, "</div></div>");
		note[0] = '\0';
	}
	if (s == NULL) {
		puts_(o, "<div class=\"box\"><div class=\"row\"><span class=\"meta\" style=\"margin:0;white-space:normal\">"
		    "Join this network from the list first: its settings are kept with the saved network.</span></div></div>"
		    "<h2><a href=\"/\" style=\"color:#007aff;text-decoration:none\">&larr; Back</a></h2></body></html>");
		return;
	}
	/* placeholders: what the network gave while connected to it */
	char lease[120] = "", ifn[8], ip[16] = "", mask[16] = "", gw[16] = "", dns[16] = "";
	if (strcmp(conn_name, hexname) == 0) {
		kread("net.op2wlan", lease, sizeof(lease));
		sscanf(lease, "%7s %15s %15s %15s %15s", ifn, ip, mask, gw, dns);
	}
	char d[3][16] = { "", "", "" };
	sscanf(s->dns, "%15[^,],%15[^,],%15s", d[0], d[1], d[2]);
	char h[200];
	snprintf(h, sizeof(h), "<form method=\"post\" action=\"/net\"><input type=\"hidden\" name=\"n\" value=\"%s\">",
	    hexname);
	puts_(o, h);
	puts_(o, "<h2>Joining</h2><div class=\"box\">");
	put_switch(o, "Join automatically", "auto", !s->noauto);
	puts_(o, "</div><h2>IPv4</h2><div class=\"box\"><div class=\"field\"><span>Configure IP</span><select name=\"ipm\" "
	    "id=\"ipm\" onchange=\"document.getElementById('man').style.display=this.value=='manual'?'':'none'\">");
	puts_(o, s->ip[0] != '\0' ? "<option value=\"dhcp\">Automatic (DHCP)</option><option value=\"manual\" selected>Manual"
	    "</option>" : "<option value=\"dhcp\" selected>Automatic (DHCP)</option><option value=\"manual\">Manual</option>");
	puts_(o, "</select></div><div id=\"man\"");
	puts_(o, s->ip[0] != '\0' ? ">" : " style=\"display:none\">");
	put_field(o, "Address", "ip", s->ip, ip[0] != '\0' ? ip : "192.168.1.50");
	put_field(o, "Subnet mask", "mask", s->mask, mask[0] != '\0' ? mask : "255.255.255.0");
	put_field(o, "Router", "router", s->router, gw[0] != '\0' ? gw : "192.168.1.1");
	puts_(o, "</div></div><h2>DNS</h2><div class=\"box\">");
	put_field(o, "Server 1", "d1", d[0], dns[0] != '\0' ? dns : "automatic");
	put_field(o, "Server 2", "d2", d[1], "optional");
	put_field(o, "Server 3", "d3", d[2], "optional");
	put_switch(o, "Also use the network&rsquo;s DNS", "dnsauto", !s->dns_only);
	put_field(o, "Search domains", "search", s->search, "lan, example.com");
	puts_(o, "</div><h2 class=\"hint\">Empty DNS servers: the network&rsquo;s own (from DHCP), then 1.1.1.1. DNS changes "
	    "apply at once; an IPv4 change reconnects.</h2><div class=\"box\"><input type=\"submit\" value=\"Save\"></div>"
	    "</form><h2><a href=\"/\" style=\"color:#007aff;text-decoration:none\">&larr; Back</a></h2></body></html>");
}

static int
valid_ip(const char *s)   /* a dotted IPv4 address, not 0.0.0.0 */
{
	struct in_addr a;
	return inet_pton(AF_INET, s, &a) == 1 && a.s_addr != 0;
}

static uint32_t
ip_num(const char *s)
{
	struct in_addr a;
	a.s_addr = 0;
	(void)inet_pton(AF_INET, s, &a);
	return ntohl(a.s_addr);
}

/* POST /net: 1 = saved (and applied), 0 = refused (note says why; *draft holds what was sent) */
static int
do_net(const char *body, char *hexname, size_t hn, struct saved *draft)
{
	form_get(body, "n", hexname, hn);
	struct saved *s = saved_find(hexname);
	if (s == NULL) {
		snprintf(note, sizeof(note), "Not a saved network");
		return 0;
	}
	struct saved v = *s;
	char ipm[10], on[8], d[3][40], search[200];
	v.noauto = (form_get(body, "auto", on, sizeof(on)), on[0] == '\0');
	v.dns_only = (form_get(body, "dnsauto", on, sizeof(on)), on[0] == '\0');
	form_get(body, "ipm", ipm, sizeof(ipm));
	form_get(body, "ip", v.ip, sizeof(v.ip));
	form_get(body, "mask", v.mask, sizeof(v.mask));
	form_get(body, "router", v.router, sizeof(v.router));
	form_get(body, "d1", d[0], sizeof(d[0]));
	form_get(body, "d2", d[1], sizeof(d[1]));
	form_get(body, "d3", d[2], sizeof(d[2]));
	form_get(body, "search", search, sizeof(search));
	*draft = v;
	draft->dns[0] = '\0';
	snprintf(draft->search, sizeof(draft->search), "%.127s", search);
	for (int i = 0; i < 3; i++) {   /* the draft shows them back as typed */
		size_t l = strlen(draft->dns);
		snprintf(draft->dns + l, sizeof(draft->dns) - l, "%s%s", i ? "," : "", d[i]);
	}
	if (strcmp(ipm, "manual") == 0) {
		uint32_t m = ip_num(v.mask);
		if (!valid_ip(v.ip)) {
			snprintf(note, sizeof(note), "Address: not an IPv4 address (like 192.168.1.50)");
			return 0;
		}
		if (!valid_ip(v.mask) || (m & (~m >> 1)) != 0 || m == 0xFFFFFFFFu) {
			snprintf(note, sizeof(note), "Subnet mask: not a mask (like 255.255.255.0)");
			return 0;
		}
		if (!valid_ip(v.router)) {
			snprintf(note, sizeof(note), "Router: not an IPv4 address (like 192.168.1.1)");
			return 0;
		}
		if ((ip_num(v.ip) & m) != (ip_num(v.router) & m) || ip_num(v.ip) == ip_num(v.router)) {
			snprintf(note, sizeof(note), "Router: it must be another address in the same subnet as the address");
			return 0;
		}
	} else {
		v.ip[0] = v.mask[0] = v.router[0] = '\0';
	}
	v.dns[0] = '\0';
	for (int i = 0; i < 3; i++) {
		if (d[i][0] == '\0') {
			continue;
		}
		if (!valid_ip(d[i])) {
			snprintf(note, sizeof(note), "DNS server %d: not an IPv4 address (like 1.1.1.1)", i + 1);
			return 0;
		}
		size_t l = strlen(v.dns);
		snprintf(v.dns + l, sizeof(v.dns) - l, "%s%s", l ? "," : "", d[i]);
	}
	if (v.dns[0] == '\0') {
		v.dns_only = 0;   /* no servers of its own: the network's */
	}
	v.search[0] = '\0';   /* words of letters, digits, dots and dashes; commas between */
	char w[200];
	snprintf(w, sizeof(w), "%s", search);
	for (char *t = strtok(w, ", "); t != NULL; t = strtok(NULL, ", ")) {
		for (const char *c = t; *c != '\0'; c++) {
			if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '.' ||
			    *c == '-')) {
				snprintf(note, sizeof(note), "Search domains: \"%.40s\" is not a domain name", t);
				return 0;
			}
		}
		size_t l = strlen(v.search);
		if (l + strlen(t) + 2 > sizeof(v.search)) {
			snprintf(note, sizeof(note), "Search domains: too long");
			return 0;
		}
		snprintf(v.search + l, sizeof(v.search) - l, "%s%s", l ? "," : "", t);
	}
	int ipv4_changed = strcmp(v.ip, s->ip) != 0 || strcmp(v.mask, s->mask) != 0 || strcmp(v.router, s->router) != 0 ||
	    (v.ip[0] != '\0' && strcmp(v.dns, s->dns) != 0);   /* (the kernel's DNS is the first server) */
	*s = v;
	saved_store();
	char nm[40] = "";
	unsigned char raw[32];
	int rl = unhex(hexname, raw, sizeof(raw));
	snprintf(nm, sizeof(nm), "%.*s", rl > 0 ? rl : 0, (const char *)raw);
	if (ipv4_changed && strcmp(conn_name, hexname) == 0) {   /* in use: join again with the new setting */
		char cmd[220];
		join_cmd(s->name, s->pmk, s, cmd, sizeof(cmd));
		manual_off = 0;
		(void)kcmd(cmd);
		memset(cmd, 0, sizeof(cmd));
		snprintf(note, sizeof(note), "%s: saved; reconnecting with the new IPv4 setting", nm);
	} else {
		snprintf(note, sizeof(note), "%s: settings saved", nm);
	}
	say("op2netd: a network's settings changed\n");
	return 1;
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
	struct saved *known = saved_find(hexname);
	if (pl == 0 && n[0] != '\0' && known != NULL) {   /* tapped a saved network: its saved password */
		snprintf(pmkhex, sizeof(pmkhex), "%s", known->pmk);
	} else if (pl == 0 && n[0] != '\0') {
		nets_load();
		for (int i = 0; i < nnets; i++) {
			if (strcmp(nets[i].name, hexname) == 0 && strcmp(nets[i].sec, "open") != 0) {
				snprintf(note, sizeof(note), "%.*s needs a password: not joined", sl, (const char *)ssid);
				memset(pw, 0, sizeof(pw));
				return;   /* never saved as open */
			}
		}
	} else if (pl == 64 && unhex(pw, pmk, sizeof(pmk)) == 32) {   /* a raw 256-bit key */
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
		snprintf(note, sizeof(note), "A WPA2 password has 8 to 63 characters: not joined");
		memset(pw, 0, sizeof(pw));
		return;
	}
	memset(pw, 0, sizeof(pw));
	memset(pmk, 0, sizeof(pmk));
	if (known == NULL || strcmp(known->pmk, pmkhex) != 0) {
		saved_put(hexname, pmkhex);
	}
	manual_off = 0;
	struct saved *s = saved_find(hexname);
	if (s != NULL) {
		s->tried = time(NULL);
	}
	char cmd[220];
	join_cmd(hexname, pmkhex, saved_find(hexname), cmd, sizeof(cmd));
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
	} else if (strcmp(method, "GET") == 0 && strncmp(path, "/net?", 5) == 0) {
		char hx[80] = "";
		unsigned char tmp[32];
		form_get(path + 5, "n", hx, sizeof(hx));
		if (unhex(hx, tmp, sizeof(tmp)) <= 0) {
			reply(fd, "404 Not Found", "", "not here", 8);
			return;
		}
		page_net(&o, hx, NULL);
	} else if (strcmp(method, "POST") == 0 && strcmp(path, "/net") == 0) {
		char hx[80] = "";
		struct saved draft;
		memset(&draft, 0, sizeof(draft));
		int saved_ok = do_net(body, hx, sizeof(hx), &draft);
		memset(req, 0, sizeof(req));
		if (saved_ok) {
			reply(fd, "303 See Other", "Location: /\r\n", "", 0);
			return;
		}
		unsigned char tmp[32];
		if (unhex(hx, tmp, sizeof(tmp)) <= 0) {
			reply(fd, "303 See Other", "Location: /\r\n", "", 0);
			return;
		}
		page_net(&o, hx, &draft);   /* refused: the form again, as sent, with the reason */
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
			char state[200], ld[16];
			kread("net.op2wlan_state", state, sizeof(state));
			track_state(state);
			const struct saved *cs = conn_name[0] != '\0' ? saved_find(conn_name) : NULL;
			lease_dns(ld);
			dns_effective(cs, ld, ps.dns_list, sizeof(ps.dns_list));
			commas_to_spaces(cs != NULL ? cs->search : "", ps.search, sizeof(ps.search));
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
