/* op2mc [setting...]: what ManagedConfiguration (profiled) answers for restriction settings, as Safari asks it.
 * 1 = allowed (MCRestrictedBoolExplicitYes), 2 = not allowed, 0 = no data. */
#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <unistd.h>
int main(int argc, char **argv) {
	@autoreleasepool {
		if (getenv("OP2_UID")) { int u = atoi(getenv("OP2_UID")); setgid(u); setuid(u); }
		if (!dlopen("/System/Library/PrivateFrameworks/ManagedConfiguration.framework/ManagedConfiguration", RTLD_NOW)) {
			printf("dlopen: %s\n", dlerror()); return 1;
		}
		id conn = [NSClassFromString(@"MCProfileConnection") performSelector:@selector(sharedConnection)];
		printf("connection %s\n", conn ? "ok" : "nil");
		NSArray *keys = argc > 1 ? [[NSProcessInfo processInfo].arguments subarrayWithRange:NSMakeRange(1, argc - 1)]
		                         : @[ @"safariAllowJavaScript", @"safariAllowPopups", @"allowSafari" ];
		for (NSString *k in keys) {
			int (*f)(id, SEL, id) = (int (*)(id, SEL, id))[conn methodForSelector:@selector(effectiveBoolValueForSetting:)];
			printf("%s = %d\n", k.UTF8String, f ? f(conn, @selector(effectiveBoolValueForSetting:), k) : -1);
		}
	}
	return 0;
}
