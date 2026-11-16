/* Web test app (temporarily installed as Game2048): top half UIWebView (WebKit in-process), bottom half
 * WKWebView (WebContent process, like Safari), both loading the Mac's test page. Tells whether JavaScript
 * fails in WebKit itself, only in WebContent, or only in Safari. */
#import <UIKit/UIKit.h>
#import <WebKit/WebKit.h>

@interface AppDelegate : UIResponder <UIApplicationDelegate>
@property (strong, nonatomic) UIWindow *window;
@end

@implementation AppDelegate
- (BOOL)application:(UIApplication *)app didFinishLaunchingWithOptions:(NSDictionary *)opts {
	self.window = [[UIWindow alloc] initWithFrame:[UIScreen mainScreen].bounds];
	UIViewController *vc = [UIViewController new];
	vc.view.backgroundColor = UIColor.whiteColor;
	CGRect b = vc.view.bounds;
	CGFloat h = b.size.height / 2;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
	UIWebView *ui = [[UIWebView alloc] initWithFrame:CGRectMake(0, 20, b.size.width, h - 22)];
	[ui loadRequest:[NSURLRequest requestWithURL:[NSURL URLWithString:@"http://192.168.7.1:8080/?uiwebview"]]];
#pragma clang diagnostic pop
	WKWebView *wk = [[WKWebView alloc] initWithFrame:CGRectMake(0, h + 2, b.size.width, h - 2)
	                                  configuration:[WKWebViewConfiguration new]];
	[wk loadRequest:[NSURLRequest requestWithURL:[NSURL URLWithString:@"http://192.168.7.1:8080/?wkwebview"]]];
	[vc.view addSubview:ui];
	[vc.view addSubview:wk];
	self.window.rootViewController = vc;
	[self.window makeKeyAndVisible];
	return YES;
}
@end

int main(int argc, char *argv[]) {
	@autoreleasepool {
		return UIApplicationMain(argc, argv, nil, NSStringFromClass([AppDelegate class]));
	}
}
