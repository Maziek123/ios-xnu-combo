/*
 * op2probe.dylib: loaded into MobileSafari by op2inject (debugging only). Writes to
 * /private/var/tmp/op2probe.txt what Safari actually uses for JavaScript: its defaults answer and every
 * WKWebView's preferences (public and private script switches). If a web view has JavaScript off, it is
 * switched on and reloaded, to prove the cause.
 */
#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import <WebKit/WebKit.h>
#include <objc/message.h>
#include <stdio.h>

static FILE *gOut;

static void
say(NSString *s)
{
	fprintf(gOut, "%s\n", s.UTF8String);
	fflush(gOut);
}

static NSString *
val(id obj, NSString *key)
{
	@try {
		return [[obj valueForKey:key] description] ?: @"nil";
	} @catch (NSException *e) {
		return @"(no such key)";
	}
}

static void
walk(UIView *v, NSMutableArray *found)
{
	if ([v isKindOfClass:[WKWebView class]])
		[found addObject:v];
	for (UIView *s in v.subviews)
		walk(s, found);
}

static void
probe(void)
{
	id sd = ((id (*)(id, SEL))objc_msgSend)([NSUserDefaults class], sel_registerName("_sf_safariDefaults"));
	BOOL js = ((BOOL (*)(id, SEL))objc_msgSend)(sd, sel_registerName("_sf_javaScriptEnabled"));
	say([NSString stringWithFormat:@"pid %d uid %d: _sf_safariDefaults %@ _sf_javaScriptEnabled=%d JavaScriptEnabled=%@",
	        getpid(), getuid(), sd, js, [sd objectForKey:@"JavaScriptEnabled"]]);

	NSMutableArray *views = [NSMutableArray array];
	for (UIWindow *w in [UIApplication sharedApplication].windows)
		walk(w, views);
	say([NSString stringWithFormat:@"%lu WKWebView(s)", (unsigned long)views.count]);
	for (WKWebView *wv in views) {
		WKPreferences *p = wv.configuration.preferences;
		say([NSString stringWithFormat:@"  %p url=%@", wv, wv.URL]);
		say([NSString stringWithFormat:@"    preferences.javaScriptEnabled=%d _javaScriptMarkupEnabled=%@ "
		        "_allowsJavaScriptMarkup(config)=%@ _javaScriptRuntimeFlags=%@",
		        p.javaScriptEnabled, val(p, @"_javaScriptMarkupEnabled"),
		        val(wv.configuration, @"_allowsJavaScriptMarkup"), val(p, @"_javaScriptRuntimeFlags")]);
		say([NSString stringWithFormat:@"    contentBlockersEnabled? userContentController=%@ processPool=%@",
		        wv.configuration.userContentController, wv.configuration.processPool]);
		if (!p.javaScriptEnabled) {
			p.javaScriptEnabled = YES;
			[wv reload];
			say(@"    -> switched javaScriptEnabled ON and reloaded");
		}
	}
}

__attribute__((constructor)) static void
op2probe_init(void)
{
	gOut = fopen("/private/var/tmp/op2probe.txt", "a");
	if (!gOut)
		return;
	say(@"--- op2probe loaded");
	dispatch_async(dispatch_get_main_queue(), ^{
		@try {
			probe();
		} @catch (NSException *e) {
			say([NSString stringWithFormat:@"exception %@", e]);
		}
	});
}
