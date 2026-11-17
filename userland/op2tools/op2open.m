/* op2open URL: open a URL like a tap would (SpringBoard via FrontBoardServices / LSApplicationWorkspace), unlocking. */
#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <objc/message.h>
int main(int argc, char **argv) {
	@autoreleasepool {
		if (argc < 2) { fprintf(stderr, "usage: op2open URL\n"); return 2; }
		NSURL *u = [NSURL URLWithString:@(argv[1])];
		void *sbs = dlopen("/System/Library/PrivateFrameworks/SpringBoardServices.framework/SpringBoardServices", RTLD_NOW);
		bool (*openSensitive)(CFURLRef, Boolean) = sbs ? dlsym(sbs, "SBSOpenSensitiveURLAndUnlock") : NULL;
		if (openSensitive) {
			bool ok = openSensitive((__bridge CFURLRef)u, 1);
			printf("SBSOpenSensitiveURLAndUnlock -> %d\n", ok);
			if (ok) return 0;
		}
		dlopen("/System/Library/Frameworks/MobileCoreServices.framework/MobileCoreServices", RTLD_NOW);
		id ws = ((id (*)(id, SEL))objc_msgSend)(NSClassFromString(@"LSApplicationWorkspace"), sel_registerName("defaultWorkspace"));
		BOOL ok = ((BOOL (*)(id, SEL, id))objc_msgSend)(ws, sel_registerName("openURL:"), u);
		printf("LSApplicationWorkspace openURL -> %d\n", ok);
		return ok ? 0 : 1;
	}
}
