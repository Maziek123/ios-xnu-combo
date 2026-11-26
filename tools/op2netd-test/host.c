/* op2netd on a Linux host: CoreFoundation/SystemConfiguration stubbed (publishing is a no-op), CommonCrypto by
 * OpenSSL, sysctls from files in $OP2NETD_TEST_DIR/k (writes to net.op2wlan_cmd appended to k/cmds). tools/op2netd-test/run.sh */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <string.h>
#define SAVED_DIR OP2NETD_TEST_DIR "/state/op2netd"
#define main netd_main
#include "../../userland/op2netd/op2netd.c"
#undef main
#include <openssl/evp.h>
/* stubs */
const CFDictionaryKeyCallBacks kCFTypeDictionaryKeyCallBacks; const CFDictionaryValueCallBacks kCFTypeDictionaryValueCallBacks;
const CFArrayCallBacks kCFTypeArrayCallBacks;
CFStringRef __CFStringMakeConstantString(const char *s) { return (CFStringRef)s; }
CFDictionaryRef CFDictionaryCreate(CFAllocatorRef a, const void **k, const void **v, CFIndex n, const CFDictionaryKeyCallBacks *x, const CFDictionaryValueCallBacks *y) { return (CFDictionaryRef)1; }
CFMutableDictionaryRef CFDictionaryCreateMutable(CFAllocatorRef a, CFIndex n, const CFDictionaryKeyCallBacks *x, const CFDictionaryValueCallBacks *y) { return (CFMutableDictionaryRef)1; }
void CFDictionarySetValue(CFMutableDictionaryRef d, const void *k, const void *v) {}
CFArrayRef CFArrayCreate(CFAllocatorRef a, const void **v, CFIndex n, const CFArrayCallBacks *c) { return (CFArrayRef)1; }
CFStringRef CFStringCreateWithCString(CFAllocatorRef a, const char *s, CFStringEncoding e) { return (CFStringRef)1; }
void CFRelease(CFTypeRef x) {}
SCDynamicStoreRef SCDynamicStoreCreate(CFAllocatorRef a, CFStringRef n, void *x, void *y) { return NULL; }
CFPropertyListRef SCDynamicStoreCopyValue(SCDynamicStoreRef s, CFStringRef k) { return NULL; }
Boolean SCDynamicStoreSetMultiple(SCDynamicStoreRef s, CFDictionaryRef a, CFArrayRef b, CFArrayRef c) { return 0; }
int CCKeyDerivationPBKDF(int alg, const char *pw, size_t pwl, const uint8_t *salt, size_t sl, int prf, unsigned rounds, uint8_t *dk, size_t dkl)
{ return PKCS5_PBKDF2_HMAC_SHA1(pw, (int)pwl, salt, (int)sl, (int)rounds, (int)dkl, dk) == 1 ? 0 : -1; }
static int rdfile(const char *path, void *buf, size_t *len) {
	FILE *f = fopen(path, "r"); if (!f) return -1; size_t n = fread(buf, 1, *len, f); fclose(f); *len = n; return 0; }
int sysctlbyname(const char *name, void *old, size_t *oldlen, void *nw, size_t nwlen) {
	char path[200]; snprintf(path, sizeof path, OP2NETD_TEST_DIR "/k/%s", name);
	if (nw) {   /* the commands op2netd sends; the kept-log lines (kern.op2keep) are dropped */
		if (strcmp(name, "net.op2wlan_cmd") == 0) { FILE *f = fopen(OP2NETD_TEST_DIR "/k/cmds", "a"); fprintf(f, "%s\n", (char *)nw); fclose(f); }
		return 0;
	}
	return rdfile(path, old, oldlen);
}
int main(void) { run(); return 0; }
