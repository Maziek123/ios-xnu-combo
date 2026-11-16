/* op2sfjs: what Safari's -[NSUserDefaults _sf_javaScriptEnabled] answers (as mobile with OP2_UID=501), and where it is. */
#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <objc/runtime.h>
#include <objc/message.h>
#include <mach-o/dyld.h>
#include <stdlib.h>
#include <unistd.h>
int main(void) {
	@autoreleasepool {
		if (getenv("OP2_UID")) { int u = atoi(getenv("OP2_UID")); setgid(u); setuid(u); }
		dlopen("/System/Library/Frameworks/SafariServices.framework/SafariServices", RTLD_NOW);
		dlopen("/System/Library/PrivateFrameworks/SafariShared.framework/SafariShared", RTLD_NOW);
		unsigned n = 0;
		Method *ms = class_copyMethodList([NSUserDefaults class], &n);
		for (unsigned i = 0; i < n; i++) {
			const char *nm = sel_getName(method_getName(ms[i]));
			if (strstr(nm, "avaScript") || strstr(nm, "safari_browserDefaults") || strstr(nm, "_sf_")) {
				Dl_info di = {0}; dladdr((void *)method_getImplementation(ms[i]), &di);
				printf("-%s imp %p (%s+0x%lx)\n", nm, (void *)method_getImplementation(ms[i]),
				       di.dli_fname ? strrchr(di.dli_fname, '/') + 1 : "?", (long)((char *)method_getImplementation(ms[i]) - (char *)di.dli_fbase));
			}
		}
		Method *cms = class_copyMethodList(object_getClass([NSUserDefaults class]), &n);
		for (unsigned i = 0; i < n; i++) {
			const char *nm = sel_getName(method_getName(cms[i]));
			if (strstr(nm, "safari") || strstr(nm, "_sf")) {
				Dl_info di = {0}; dladdr((void *)method_getImplementation(cms[i]), &di);
				printf("+%s imp %s+0x%lx\n", nm, di.dli_fname ? strrchr(di.dli_fname, '/') + 1 : "?",
				       (long)((char *)method_getImplementation(cms[i]) - (char *)di.dli_fbase));
			}
		}
		printf("slide 0x%lx\n", (long)_dyld_get_image_vmaddr_slide(1));
		NSUserDefaults *d = [[NSUserDefaults alloc] initWithSuiteName:@"com.apple.mobilesafari"];
		printf("mobilesafari WebKitJavaScriptEnabled before register: %s\n", [[[d objectForKey:@"WebKitJavaScriptEnabled"] description] UTF8String]);
		((void (*)(id, SEL))objc_msgSend)(d, sel_registerName("_sf_registerSafariDefaults"));
		printf("after register: %s\n", [[[d objectForKey:@"WebKitJavaScriptEnabled"] description] UTF8String]);
		SEL s = sel_registerName("_sf_javaScriptEnabled");
		if ([d respondsToSelector:s]) printf("standard _sf_javaScriptEnabled = %d\n", ((BOOL (*)(id, SEL))objc_msgSend)(d, s));
		id sd = ((id (*)(id, SEL))objc_msgSend)([NSUserDefaults class], sel_registerName("_sf_safariDefaults"));
		printf("_sf_safariDefaults %s\n", [[sd description] UTF8String]);
		NSDictionary *all = [sd dictionaryRepresentation];
		for (NSString *k in all) if ([k rangeOfString:@"avaScript" options:NSCaseInsensitiveSearch].location != NSNotFound)
			printf("  %s = %s\n", k.UTF8String, [[all[k] description] UTF8String]);
		printf("safariDefaults _sf_javaScriptEnabled = %d\n", ((BOOL (*)(id, SEL))objc_msgSend)(sd, s));
		SEL bd = sel_registerName("safari_browserDefaults");
		if ([NSUserDefaults respondsToSelector:bd]) {
			id b = ((id (*)(id, SEL))objc_msgSend)([NSUserDefaults class], bd);
			printf("browserDefaults %p _sf_javaScriptEnabled = %d, WebKitJavaScriptEnabled obj %s\n", b,
			       ((BOOL (*)(id, SEL))objc_msgSend)(b, s), [[[b objectForKey:@"WebKitJavaScriptEnabled"] description] UTF8String]);
		}
	}
	return 0;
}
