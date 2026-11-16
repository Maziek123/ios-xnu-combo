/* op2url URL: fetch with NSURLSession (CFNetwork + Network.framework, the stack Safari uses). */
#import <Foundation/Foundation.h>
#include <unistd.h>
#include <stdlib.h>
int main(int argc, char **argv) {
	@autoreleasepool {
		if (getenv("OP2_UID")) {   /* run as another user (501 = mobile), like apps */
			int u = atoi(getenv("OP2_UID"));
			if (setgid(u) || setuid(u)) perror("setuid");
		}
		NSURL *u = [NSURL URLWithString:[NSString stringWithUTF8String:argc > 1 ? argv[1] : "http://example.com/"]];
		dispatch_semaphore_t sem = dispatch_semaphore_create(0);
		__block int rc = 1;
		NSURLSessionConfiguration *c = [NSURLSessionConfiguration ephemeralSessionConfiguration];
		c.timeoutIntervalForRequest = 20;
		[[[NSURLSession sessionWithConfiguration:c] dataTaskWithURL:u completionHandler:^(NSData *d, NSURLResponse *r, NSError *e) {
			if (e) printf("error %ld %s: %s\n", (long)e.code, e.domain.UTF8String, e.description.UTF8String);
			else { printf("status %ld, %lu bytes\n", (long)((NSHTTPURLResponse *)r).statusCode, (unsigned long)d.length); rc = 0; }
			dispatch_semaphore_signal(sem);
		}] resume];
		dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC));
		return rc;
	}
}
