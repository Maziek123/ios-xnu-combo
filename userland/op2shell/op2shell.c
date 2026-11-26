/*
 * op2shell: a root shell on the USB console (docs/USB-CONSOLE-PLAN.md stage 2).
 *
 * Installed IN PLACE over /usr/local/bin/op2-air-run (9 MB slot, tools/rootfs_g3.py), whose launchd job
 * com.op2.air-run runs it once at every boot as root. It carries dash (out/shell/dash-0.5.12), toybox,
 * dropbear+scp and an authorized_keys inside itself (.incbin), installs them (plus toybox applet symlinks and
 * a /bin/sh link for dropbear's login shell), starts dropbear on port 22, then forks away from launchd and
 * supervises: open /dev/op2usb (bsd/dev/op2_usb.c, the CDC-ACM gadget's line-disciplined stream), run `dash -i`
 * on it as stdin/stdout/stderr, and when the shell exits (or the device is not ready yet) wait a second and
 * start another.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern const unsigned char op2_dash_start[], op2_dash_end[];
__asm__(".section __DATA,__dash\n"
        ".globl _op2_dash_start\n_op2_dash_start:\n"
        ".incbin \"" DASH_BIN "\"\n"
        ".globl _op2_dash_end\n_op2_dash_end:\n"
        ".text\n");
extern const unsigned char op2_toybox_start[], op2_toybox_end[];
__asm__(".section __DATA,__toybox\n"
        ".globl _op2_toybox_start\n_op2_toybox_start:\n"
        ".incbin \"" TOYBOX_BIN "\"\n"
        ".globl _op2_toybox_end\n_op2_toybox_end:\n"
        ".text\n");
extern const unsigned char op2_dropbear_start[], op2_dropbear_end[];
__asm__(".section __DATA,__dropbear\n"
        ".globl _op2_dropbear_start\n_op2_dropbear_start:\n"
        ".incbin \"" DROPBEAR_BIN "\"\n"
        ".globl _op2_dropbear_end\n_op2_dropbear_end:\n"
        ".text\n");
extern const unsigned char op2_scp_start[], op2_scp_end[];
__asm__(".section __DATA,__scpbinary\n"
        ".globl _op2_scp_start\n_op2_scp_start:\n"
        ".incbin \"" SCP_BIN "\"\n"
        ".globl _op2_scp_end\n_op2_scp_end:\n"
        ".text\n");
extern const unsigned char op2_authkeys_start[], op2_authkeys_end[];
__asm__(".section __DATA,__authkeys\n"
        ".globl _op2_authkeys_start\n_op2_authkeys_start:\n"
        ".incbin \"" AUTHKEYS_BIN "\"\n"
        ".globl _op2_authkeys_end\n_op2_authkeys_end:\n"
        ".text\n");

#define DASH "/usr/local/bin/dash"
#define TOYBOX "/usr/local/bin/toybox"
#define DROPBEAR "/usr/local/bin/dropbear"
#define SCP "/usr/local/bin/scp"
#define AUTHKEYS "/var/root/.ssh/authorized_keys"
#define DOTPROFILE "/var/root/.profile"
static const char dotprofile_text[] = "export PATH=/usr/local/bin:$PATH\n";

static void
say(const char *s)   /* to the console (and so the USB log until the shell opens it) */
{
	int fd = open("/dev/console", O_WRONLY);
	if (fd >= 0) {
		write(fd, s, strlen(s));
		close(fd);
	}
}

static int
install_blob(const char *path, const unsigned char *start, const unsigned char *end)
{
	size_t n = (size_t)(end - start);
	struct stat st;
	char *tmp;
	if (stat(path, &st) == 0 && (size_t)st.st_size == n) {
		int fd = open(path, O_RDONLY);
		if (fd >= 0) {
			unsigned char *b = malloc(n);
			ssize_t r = b ? read(fd, b, n) : -1;
			close(fd);
			int same = r == (ssize_t)n && memcmp(b, start, n) == 0;
			free(b);
			if (same)
				return 0;
		}
	}
	mkdir("/usr/local", 0755);
	mkdir("/usr/local/bin", 0755);
	tmp = malloc(strlen(path) + 5);
	if (tmp == NULL)
		return -1;
	sprintf(tmp, "%s.new", path);
	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0755);
	ssize_t w = fd < 0 ? -1 : write(fd, start, n);
	if (fd >= 0) {
		fsync(fd);
		close(fd);
	}
	int rc = (w != (ssize_t)n || rename(tmp, path) != 0) ? -1 : 0;
	free(tmp);
	if (rc == 0)
		chmod(path, 0755);
	return rc;
}

static int
install_authkeys(void)
{
	size_t n = (size_t)(op2_authkeys_end - op2_authkeys_start);
	struct stat st;
	mkdir("/var/root", 0755);
	mkdir("/var/root/.ssh", 0700);
	chmod("/var/root/.ssh", 0700);
	if (stat(AUTHKEYS, &st) == 0 && (size_t)st.st_size == n) {
		int fd = open(AUTHKEYS, O_RDONLY);
		if (fd >= 0) {
			unsigned char *b = malloc(n);
			ssize_t r = b ? read(fd, b, n) : -1;
			close(fd);
			int same = r == (ssize_t)n && memcmp(b, op2_authkeys_start, n) == 0;
			free(b);
			if (same) {
				chmod(AUTHKEYS, 0600);
				return 0;
			}
		}
	}
	int fd = open(AUTHKEYS, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0)
		return -1;
	ssize_t w = write(fd, op2_authkeys_start, n);
	fsync(fd);
	close(fd);
	chmod(AUTHKEYS, 0600);
	return w == (ssize_t)n ? 0 : -1;
}

/* tool symlinks; never shadow an existing file. /usr/local/bin is first on the
 * interactive PATH; /bin covers ssh-exec/scp, whose default PATH lacks it. */
static void
install_link(const char *dir, const char *name, const char *target)
{
	char path[64];
	struct stat st;
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	if (lstat(path, &st) == 0)
		return;
	symlink(target, path);
}

static void
install_toybox_links(void)
{
	static const char *applets[] = { "ls", "cat", "grep", "od", "wc", "id", "uname", "file", "df", "du",
		"head", "tail", "tee", "tr", "basename", "dirname", "mkdir", "rm", "cp", "mv", "ln",
		"touch", "chmod", NULL };
	for (unsigned i = 0; applets[i] != NULL; i++) {
		install_link("/usr/local/bin", applets[i], "toybox");
		install_link("/bin", applets[i], "/usr/local/bin/toybox");
	}
	install_link("/bin", "dash", "/usr/local/bin/dash");
	install_link("/bin", "scp", "/usr/local/bin/scp");
}

/*
 * Internet for iOS: publish network services into configd so IPMonitor picks a primary interface (default route)
 * and gives apps DNS. ue0 and wl0 have no IOKit object, so IPConfiguration never adopts them; we publish the
 * services' State: keys ourselves.
 *  - op2usb: ue0 (the kernel set 192.168.7.2/24), the Mac (192.168.7.1, NAT) as router.
 *  - op2wifi: wl0, while the Wi-Fi driver (boot-arg wifi=11) has a lease: the kernel's sysctl net.op2wlan says
 *    "wl0 <address> <netmask> <router> <dns>" (empty while down). Wi-Fi comes first in the ServiceOrder.
 * Keys survive our session, not a configd restart, so this child checks every 3 s and re-publishes what is gone.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <sys/sysctl.h>
/* Exported by iOS's SystemConfiguration but marked unavailable in the SDK header: declared here. */
typedef struct __SCDynamicStore *SCDynamicStoreRef;
SCDynamicStoreRef SCDynamicStoreCreate(CFAllocatorRef, CFStringRef, void *, void *);
CFPropertyListRef SCDynamicStoreCopyValue(SCDynamicStoreRef, CFStringRef);
Boolean SCDynamicStoreSetMultiple(SCDynamicStoreRef, CFDictionaryRef, CFArrayRef, CFArrayRef);

static CFDictionaryRef
dict(int n, const void **k, const void **v)
{
	return CFDictionaryCreate(NULL, k, v, n, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
}

static CFArrayRef
arr(int n, const void **v)
{
	return CFArrayCreate(NULL, v, n, &kCFTypeArrayCallBacks);
}

static CFStringRef
str(const char *s)
{
	return CFStringCreateWithCString(NULL, s, kCFStringEncodingUTF8);
}

static void
release_all(int n, const void **v)
{
	for (int i = 0; i < n; i++)
		CFRelease(v[i]);
}

static int
exists(SCDynamicStoreRef st, CFStringRef key)
{
	CFPropertyListRef v = SCDynamicStoreCopyValue(st, key);
	if (v == NULL)
		return 0;
	CFRelease(v);
	return 1;
}

/* the Wi-Fi lease from the kernel (1), or none (0) */
static int
wifi_lease(char ip[16], char mask[16], char router[16], char dns[16])
{
	char buf[128], name[16];
	size_t n = sizeof(buf) - 1;
	if (sysctlbyname("net.op2wlan", buf, &n, NULL, 0) != 0 || n == 0)
		return 0;
	buf[n] = '\0';
	return sscanf(buf, "%15s %15s %15s %15s %15s", name, ip, mask, router, dns) == 5 && strcmp(name, "wl0") == 0;
}

static void
set_order(int wifi_first, CFMutableDictionaryRef into)
{
	const void *both[] = { CFSTR("op2wifi"), CFSTR("op2usb") }, *usb[] = { CFSTR("op2usb") };
	CFArrayRef order = wifi_first ? arr(2, both) : arr(1, usb);
	const void *ko[] = { CFSTR("ServiceOrder") }, *vo[] = { order };
	CFDictionaryRef g = dict(1, ko, vo);
	CFDictionarySetValue(into, CFSTR("Setup:/Network/Global/IPv4"), g);
	CFRelease(g);
	CFRelease(order);
}

static void
publish_net(void)
{
	SCDynamicStoreRef st = NULL;
	char wifi_pub[80] = "";   /* the lease published as op2wifi ("" = none) */
	for (;;) {
		if (st == NULL)
			st = SCDynamicStoreCreate(NULL, CFSTR("op2shell"), NULL, NULL);
		if (st == NULL) {
			sleep(3);
			continue;
		}
		char ip[16], mask[16], router[16], dns[16], now[80] = "", msg[200];
		/* Wi-Fi: ready once the kernel has a lease and KernelEventMonitor has seen wl0's address */
		if (wifi_lease(ip, mask, router, dns) && exists(st, CFSTR("State:/Network/Interface/wl0/IPv4")))
			snprintf(now, sizeof(now), "%s %s %s %s", ip, mask, router, dns);
		int wl_have = exists(st, CFSTR("State:/Network/Service/op2wifi/IPv4"));
		if (now[0] != '\0' && (strcmp(now, wifi_pub) != 0 || !wl_have)) {
			const void *a[] = { str(ip) }, *m[] = { str(mask) }, *r = str(router);
			const void *dnsv[] = { str(dns), CFSTR("1.1.1.1") };
			const void *k4[] = { CFSTR("Addresses"), CFSTR("SubnetMasks"), CFSTR("Router"), CFSTR("InterfaceName") };
			const void *v4[] = { arr(1, a), arr(1, m), r, CFSTR("wl0") };
			const void *kd[] = { CFSTR("ServerAddresses") }, *vd[] = { arr(2, dnsv) };
			CFDictionaryRef d4 = dict(4, k4, v4), dd = dict(1, kd, vd);
			CFMutableDictionaryRef all = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
			    &kCFTypeDictionaryValueCallBacks);
			CFDictionarySetValue(all, CFSTR("State:/Network/Service/op2wifi/IPv4"), d4);
			CFDictionarySetValue(all, CFSTR("State:/Network/Service/op2wifi/DNS"), dd);
			set_order(1, all);
			if (SCDynamicStoreSetMultiple(st, all, NULL, NULL)) {
				snprintf(wifi_pub, sizeof(wifi_pub), "%s", now);
				snprintf(msg, sizeof(msg), "op2shell: wl0 published as primary network (%s/%s, router %s, DNS %s)\n",
				    ip, mask, router, dns);
				say(msg);
			}
			CFRelease(all);
			CFRelease(d4);
			CFRelease(dd);
			release_all(2, v4);
			release_all(1, vd);
			release_all(1, a);
			release_all(1, m);
			CFRelease(r);
			CFRelease(dnsv[0]);
		} else if (now[0] == '\0' && (wifi_pub[0] != '\0' || wl_have)) {   /* the lease is gone: back to USB */
			const void *rm[] = { CFSTR("State:/Network/Service/op2wifi/IPv4"), CFSTR("State:/Network/Service/op2wifi/DNS") };
			CFArrayRef remove = arr(2, rm);
			CFMutableDictionaryRef all = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
			    &kCFTypeDictionaryValueCallBacks);
			set_order(0, all);
			if (SCDynamicStoreSetMultiple(st, all, remove, NULL)) {
				wifi_pub[0] = '\0';
				say("op2shell: wl0 gone, op2wifi removed (USB is the network again)\n");
			}
			CFRelease(all);
			CFRelease(remove);
		}
		/* USB, as before; the ServiceOrder keeps Wi-Fi first while it is published */
		if (exists(st, CFSTR("State:/Network/Interface/ue0/IPv4")) &&
		    !exists(st, CFSTR("State:/Network/Service/op2usb/IPv4"))) {
			const void *a[] = { CFSTR("192.168.7.2") }, *m[] = { CFSTR("255.255.255.0") };
			const void *dnsv[] = { CFSTR("1.1.1.1"), CFSTR("8.8.8.8") };
			const void *k4[] = { CFSTR("Addresses"), CFSTR("SubnetMasks"), CFSTR("Router"), CFSTR("InterfaceName") };
			const void *v4[] = { arr(1, a), arr(1, m), CFSTR("192.168.7.1"), CFSTR("ue0") };
			const void *kd[] = { CFSTR("ServerAddresses") }, *vd[] = { arr(2, dnsv) };
			CFDictionaryRef d4 = dict(4, k4, v4), dd = dict(1, kd, vd);
			CFMutableDictionaryRef all = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
			    &kCFTypeDictionaryValueCallBacks);
			CFDictionarySetValue(all, CFSTR("State:/Network/Service/op2usb/IPv4"), d4);
			CFDictionarySetValue(all, CFSTR("State:/Network/Service/op2usb/DNS"), dd);
			set_order(wifi_pub[0] != '\0', all);
			if (SCDynamicStoreSetMultiple(st, all, NULL, NULL))
				say(wifi_pub[0] != '\0' ? "op2shell: ue0 published (Wi-Fi stays primary)\n" :
				    "op2shell: ue0 published as primary network (router 192.168.7.1, DNS 1.1.1.1)\n");
			CFRelease(all);
			CFRelease(d4);
			CFRelease(dd);
			release_all(2, v4);
			release_all(1, vd);
		}
		sleep(3);
	}
}

int
main(void)
{
	if (fork() != 0)
		return 0;   /* launchd's job ends; the supervisor lives on */
	setsid();
	signal(SIGHUP, SIG_IGN);
	int ok = install_blob(DASH, op2_dash_start, op2_dash_end) == 0 &&
	    install_blob(TOYBOX, op2_toybox_start, op2_toybox_end) == 0 &&
	    install_blob(DROPBEAR, op2_dropbear_start, op2_dropbear_end) == 0 &&
	    install_blob(SCP, op2_scp_start, op2_scp_end) == 0 && install_authkeys() == 0;
	ok = ok && install_blob(DOTPROFILE, (const unsigned char *)dotprofile_text,
	    (const unsigned char *)(dotprofile_text + sizeof(dotprofile_text) - 1)) == 0;
	if (ok) {
		struct stat st;
		install_toybox_links();
		if (lstat("/bin/sh", &st) != 0)   /* dropbear runs the login shell */
			symlink(DASH, "/bin/sh");
		mkdir("/etc/dropbear", 0755);   /* -R generates host keys here on first boot */
		pid_t db = fork();
		if (db == 0) {   /* ssh daemon: host keys auto-created (-R), log to /tmp */
			int lfd = open("/tmp/dropbear.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
			if (lfd >= 0) {
				dup2(lfd, 1);
				dup2(lfd, 2);
				if (lfd > 2)
					close(lfd);
			}
			execl(DROPBEAR, "dropbear", "-R", "-E", "-p", "22", (char *)NULL);
			_exit(1);
		} else if (db > 0) {
			waitpid(db, NULL, 0);   /* reap the starter; the daemon lives on */
		}
	}
	if (fork() == 0) {
		publish_net();
		_exit(0);
	}
	say(ok ? "op2shell: dash+toybox+ssh ready, waiting for /dev/op2usb\n" :
	    "op2shell: cannot install shell\n");
	for (unsigned tries = 0;; tries++) {
		int fd = open("/dev/op2usb", O_RDWR);
		if (fd < 0) {
			sleep(tries < 60 ? 1 : 5);
			continue;
		}
		pid_t pid = fork();
		if (pid == 0) {
			setsid();
			dup2(fd, 0);
			dup2(fd, 1);
			dup2(fd, 2);
			if (fd > 2)
				close(fd);
			chdir("/");
			static char *env[] = { "PATH=/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin", "HOME=/var/root",
				"TERM=dumb", "USER=root", "SHELL=" DASH, "PS1=op2# ", NULL };
			write(1, "\r\nOnePlus 2 iOS root shell (dash). ^D or exit restarts it.\n", 59);
			execle(DASH, "-dash", "-i", (char *)NULL, env);
			_exit(127);
		}
		close(fd);
		int status;
		while (pid > 0 && waitpid(pid, &status, 0) < 0 && errno == EINTR)
			;
		sleep(1);
	}
}
