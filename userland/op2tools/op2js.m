/* op2js [script]: run JavaScript in JavaScriptCore (the engine Safari uses) and print the result. */
#import <Foundation/Foundation.h>
#import <JavaScriptCore/JavaScriptCore.h>
int main(int argc, char **argv) {
	@autoreleasepool {
		JSContext *c = [JSContext new];
		c.exceptionHandler = ^(JSContext *ctx, JSValue *e) { printf("exception: %s\n", e.toString.UTF8String); };
		NSString *src = argc > 1 ? @(argv[1]) : @"var s=0; for (var i=0;i<3000000;i++) s+=i%7; 'sum '+s+' '+typeof WebAssembly";
		NSDate *t = [NSDate date];
		JSValue *v = [c evaluateScript:src];
		printf("result: %s (%.0f ms)\n", v.toString.UTF8String, -[t timeIntervalSinceNow] * 1000);
	}
	return 0;
}
