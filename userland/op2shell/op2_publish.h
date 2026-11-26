/*
 * op2_publish.h: publish_net(), shared by op2shell (userland/op2shell/op2shell.c) and op2netd
 * (userland/op2netd/op2netd.c). The includer defines say(const char *) first, and may define PUBLISHER, the name
 * its log lines start with.
 */
#ifndef PUBLISHER
#define PUBLISHER "op2shell"
#endif
/*
 * Internet for iOS: publish network services into configd so IPMonitor picks a primary interface (default route)
 * and gives apps DNS. ue0 and wl0 have no IOKit object, so IPConfiguration never adopts them; we publish the
 * services' State: keys ourselves.
 *  - op2usb: ue0 (the kernel set 192.168.7.2/24), the Mac (192.168.7.1, NAT) as router.
 *  - op2wifi: wl0, while the Wi-Fi driver (boot-arg wifi=11) has a lease: the kernel's sysctl net.op2wlan says
 *    "wl0 <address> <netmask> <router> <dns>" (empty while down). Wi-Fi comes first in the ServiceOrder. DNS: the
 *    lease's server then 1.1.1.1, or the includer's list (op2netd: a network's own DNS and search domain settings).
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

struct publish_state {
	SCDynamicStoreRef st;
	char wifi_pub[300];  /* the lease (and the lists below) published as op2wifi ("" = none) */
	int refused;
	/* set by the includer before each publish_step (op2netd's per-network settings); "" = the defaults */
	char dns_list[80];   /* Wi-Fi DNS servers, space-separated, in order (default: the lease's, then 1.1.1.1) */
	char search[130];    /* Wi-Fi search domains, space-separated (default: none) */
};

/* up to max space-separated words of s as CFStrings; the count */
static int
words(const char *s, const void **out, int max)
{
	int n = 0;
	char w[130];
	int used = 0;
	for (const char *p = s; n < max && sscanf(p, "%129s%n", w, &used) == 1; p += used)
		out[n++] = str(w);
	return n;
}

/* one look at the lease and configd's keys; publishes or removes what changed. Call every few seconds. */
static void
publish_step(struct publish_state *ps)
{
	char *wifi_pub = ps->wifi_pub;
	do {
		if (ps->st == NULL)
			ps->st = SCDynamicStoreCreate(NULL, CFSTR(PUBLISHER), NULL, NULL);
		SCDynamicStoreRef st = ps->st;
		if (st == NULL)
			return;
		char ip[16], mask[16], router[16], dns[16], now[300] = "", msg[400], dl[80];
		/* Wi-Fi: ready once the kernel has a lease and KernelEventMonitor has seen wl0's address */
		if (wifi_lease(ip, mask, router, dns) && exists(st, CFSTR("State:/Network/Interface/wl0/IPv4")))
			snprintf(now, sizeof(now), "%s %s %s %s|%s|%s", ip, mask, router, dns, ps->dns_list, ps->search);
		int wl_have = exists(st, CFSTR("State:/Network/Service/op2wifi/IPv4"));
		if (now[0] != '\0' && (strcmp(now, wifi_pub) != 0 || !wl_have)) {
			const void *a[] = { str(ip) }, *m[] = { str(mask) }, *r = str(router);
			if (ps->dns_list[0] != '\0')
				snprintf(dl, sizeof(dl), "%s", ps->dns_list);
			else
				snprintf(dl, sizeof(dl), "%s 1.1.1.1", dns);
			const void *dnsv[4], *sd[8];
			int nd = words(dl, dnsv, 4), ns = words(ps->search, sd, 8);
			const void *k4[] = { CFSTR("Addresses"), CFSTR("SubnetMasks"), CFSTR("Router"), CFSTR("InterfaceName") };
			const void *v4[] = { arr(1, a), arr(1, m), r, CFSTR("wl0") };
			const void *kd[] = { CFSTR("ServerAddresses"), CFSTR("SearchDomains") }, *vd[] = { arr(nd, dnsv), arr(ns, sd) };
			CFDictionaryRef d4 = dict(4, k4, v4), dd = dict(ns > 0 ? 2 : 1, kd, vd);
			CFMutableDictionaryRef all = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
			    &kCFTypeDictionaryValueCallBacks);
			CFDictionarySetValue(all, CFSTR("State:/Network/Service/op2wifi/IPv4"), d4);
			CFDictionarySetValue(all, CFSTR("State:/Network/Service/op2wifi/DNS"), dd);
			set_order(1, all);
			if (SCDynamicStoreSetMultiple(st, all, NULL, NULL)) {
				snprintf(wifi_pub, sizeof(ps->wifi_pub), "%s", now);
				snprintf(msg, sizeof(msg), PUBLISHER ": wl0 published as primary network (%s/%s, router %s, DNS %s%s%s)\n",
				    ip, mask, router, dl, ns > 0 ? ", search " : "", ps->search);
				say(msg);
			} else if (!ps->refused) {
				ps->refused = 1;
				snprintf(msg, sizeof(msg), PUBLISHER ": configd refused the wl0 service (uid %d); retrying every 3 s\n",
				    (int)getuid());
				say(msg);
			}
			CFRelease(all);
			CFRelease(d4);
			CFRelease(dd);
			release_all(2, v4);
			release_all(2, vd);
			release_all(1, a);
			release_all(1, m);
			CFRelease(r);
			release_all(nd, dnsv);
			release_all(ns, sd);
		} else if (now[0] == '\0' && (wifi_pub[0] != '\0' || wl_have)) {   /* the lease is gone: back to USB */
			const void *rm[] = { CFSTR("State:/Network/Service/op2wifi/IPv4"), CFSTR("State:/Network/Service/op2wifi/DNS") };
			CFArrayRef remove = arr(2, rm);
			CFMutableDictionaryRef all = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
			    &kCFTypeDictionaryValueCallBacks);
			set_order(0, all);
			if (SCDynamicStoreSetMultiple(st, all, remove, NULL)) {
				wifi_pub[0] = '\0';
				say(PUBLISHER ": wl0 gone, op2wifi removed (USB is the network again)\n");
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
				say(wifi_pub[0] != '\0' ? PUBLISHER ": ue0 published (Wi-Fi stays primary)\n" :
				    PUBLISHER ": ue0 published as primary network (router 192.168.7.1, DNS 1.1.1.1)\n");
			CFRelease(all);
			CFRelease(d4);
			CFRelease(dd);
			release_all(2, v4);
			release_all(1, vd);
		}
	} while (0);
}

static void __attribute__((unused))
publish_net(void)   /* op2shell: publish forever */
{
	struct publish_state ps;
	memset(&ps, 0, sizeof(ps));
	for (;;) {
		publish_step(&ps);
		sleep(3);
	}
}
