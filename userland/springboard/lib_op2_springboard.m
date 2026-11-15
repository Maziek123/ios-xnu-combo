#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>
#include <fcntl.h>
#include <objc/runtime.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <spawn.h>

#define DYLD_INTERPOSE(_replacement, _replacee) \
    __attribute__((used)) static struct { \
        const void *replacement; \
        const void *replacee; \
    } _interpose_##_replacee \
    __attribute__((section("__DATA,__interpose"))) = { \
        (const void *)(unsigned long)&_replacement, \
        (const void *)(unsigned long)&_replacee \
    };

static int g_log_fd = -1;

static void sb_log(const char *fmt, ...) {
    if (g_log_fd < 0) {
        g_log_fd = open("/tmp/sb_agent.log", O_WRONLY | O_CREAT | O_APPEND, 0666);
    }
    if (g_log_fd >= 0) {
        char buf[512];
        va_list ap;
        va_start(ap, fmt);
        int n = vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        if (n > 0) {
            write(g_log_fd, buf, n);
        }
    }
}

// DYLD Interposing for posix_spawn to inject lib_op2_app_fix.dylib into all child apps
static int my_posix_spawn(pid_t *pid, const char *path,
                          const posix_spawn_file_actions_t *file_actions,
                          const posix_spawnattr_t *attrp,
                          char *const argv[], char *const envp[]) {
    sb_log("[OP2SB] posix_spawn called for %s\n", path ? path : "null");
    int envc = 0;
    BOOL has_dyld = NO;
    if (envp) {
        while (envp[envc]) {
            if (strncmp(envp[envc], "DYLD_INSERT_LIBRARIES=", 22) == 0) {
                has_dyld = YES;
            }
            envc++;
        }
    }
    char **new_env = (char **)malloc(sizeof(char *) * (envc + 3));
    for (int i = 0; i < envc; i++) {
        new_env[i] = envp[i];
    }
    if (!has_dyld) {
        new_env[envc++] = "DYLD_INSERT_LIBRARIES=/usr/local/lib_op2_app_fix.dylib";
    }
    new_env[envc] = NULL;
    int ret = posix_spawn(pid, path, file_actions, attrp, argv, new_env);
    free(new_env);
    return ret;
}
DYLD_INTERPOSE(my_posix_spawn, posix_spawn);

static int my_posix_spawnp(pid_t *pid, const char *path,
                           const posix_spawn_file_actions_t *file_actions,
                           const posix_spawnattr_t *attrp,
                           char *const argv[], char *const envp[]) {
    sb_log("[OP2SB] posix_spawnp called for %s\n", path ? path : "null");
    int envc = 0;
    BOOL has_dyld = NO;
    if (envp) {
        while (envp[envc]) {
            if (strncmp(envp[envc], "DYLD_INSERT_LIBRARIES=", 22) == 0) {
                has_dyld = YES;
            }
            envc++;
        }
    }
    char **new_env = (char **)malloc(sizeof(char *) * (envc + 3));
    for (int i = 0; i < envc; i++) {
        new_env[i] = envp[i];
    }
    if (!has_dyld) {
        new_env[envc++] = "DYLD_INSERT_LIBRARIES=/usr/local/lib_op2_app_fix.dylib";
    }
    new_env[envc] = NULL;
    int ret = posix_spawnp(pid, path, file_actions, attrp, argv, new_env);
    free(new_env);
    return ret;
}
DYLD_INTERPOSE(my_posix_spawnp, posix_spawnp);

extern uint32_t CARenderServerCreateSnapshots(mach_port_t, uint32_t, void *);
static uint32_t my_CARenderServerCreateSnapshots(mach_port_t port, uint32_t count, void *snapshots) {
    sb_log("[OP2SB] Intercepted CARenderServerCreateSnapshots! Returning 0 immediately.\n");
    return 0;
}
DYLD_INTERPOSE(my_CARenderServerCreateSnapshots, CARenderServerCreateSnapshots);

// Mock AVSystemController to satisfy volume and audio route queries instantly
@interface OP2MockAVSystemController : NSObject
@end

@implementation OP2MockAVSystemController

- (BOOL)getActiveCategoryVolume:(float *)vol andName:(id *)name {
    if (vol) *vol = 0.5f;
    if (name) *name = @"Speaker";
    return YES;
}

- (BOOL)getActiveCategoryVolume:(float *)vol andName:(id *)name fallbackCategory:(id)fb {
    if (vol) *vol = 0.5f;
    if (name) *name = @"Speaker";
    return YES;
}

- (BOOL)getActiveCategoryVolume:(float *)vol andName:(id *)name forRoute:(id)route andDeviceIdentifier:(id)dev {
    if (vol) *vol = 0.5f;
    if (name) *name = @"Speaker";
    return YES;
}

- (BOOL)getActiveCategoryMuted:(BOOL *)muted {
    if (muted) *muted = NO;
    return YES;
}

- (BOOL)getActiveCategoryMuted:(BOOL *)muted forRoute:(id)route andDeviceIdentifier:(id)dev {
    if (muted) *muted = NO;
    return YES;
}

- (id)routeForCategory:(id)cat {
    return @"Speaker";
}

- (id)pickableRoutesForCategory:(id)cat {
    return @[];
}

- (id)pickableRoutesForCategory:(id)cat andMode:(id)mode {
    return @[];
}

- (BOOL)okToNotifyFromThisThread {
    return YES;
}

- (NSMethodSignature *)methodSignatureForSelector:(SEL)aSelector {
    return [NSMethodSignature signatureWithObjCTypes:"v@:"];
}

- (void)forwardInvocation:(NSInvocation *)anInvocation {
    // Safely no-op any other volume/routing requests
}

@end

static id swizzled_sharedAVSystemController(id self, SEL _cmd) {
    static id s_mock = nil;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        s_mock = [[OP2MockAVSystemController alloc] init];
        sb_log("[OP2SB] Created OP2MockAVSystemController instance %p\n", s_mock);
    });
    return s_mock;
}

static BOOL swizzled_shouldPlayLockSound(id self, SEL _cmd) {
    return NO;
}

static id op2_identifier(id self, SEL _cmd) {
    if ([self respondsToSelector:@selector(bundleIdentifier)]) {
        return [self performSelector:@selector(bundleIdentifier)];
    }
    if ([self respondsToSelector:@selector(applicationBundleID)]) {
        return [self performSelector:@selector(applicationBundleID)];
    }
    if ([self respondsToSelector:@selector(leafIdentifier)]) {
        return [self performSelector:@selector(leafIdentifier)];
    }
    return nil;
}

static BOOL op2_boolForActivationSetting(id self, SEL _cmd, unsigned int setting) {
    return NO;
}

static long long op2_flagForActivationSetting(id self, SEL _cmd, unsigned int setting) {
    return 0;
}

static id op2_objectForActivationSetting(id self, SEL _cmd, unsigned int setting) {
    return nil;
}

static NSDictionary *(*orig_fb_environment)(id, SEL) = NULL;
static NSDictionary *swizzled_fb_environment(id self, SEL _cmd) {
    NSDictionary *env = nil;
    if (orig_fb_environment) {
        env = orig_fb_environment(self, _cmd);
    }
    NSMutableDictionary *mut = env ? [env mutableCopy] : [NSMutableDictionary dictionary];
    mut[@"DYLD_INSERT_LIBRARIES"] = @"/usr/local/lib_op2_app_fix.dylib";
    return mut;
}

static void (*orig_fb_setEnvironment)(id, SEL, NSDictionary *) = NULL;
static void swizzled_fb_setEnvironment(id self, SEL _cmd, NSDictionary *env) {
    NSMutableDictionary *mut = env ? [env mutableCopy] : [NSMutableDictionary dictionary];
    mut[@"DYLD_INSERT_LIBRARIES"] = @"/usr/local/lib_op2_app_fix.dylib";
    if (orig_fb_setEnvironment) {
        orig_fb_setEnvironment(self, _cmd, mut);
    }
}

static NSDictionary *swizzled_fb_internalDebugEnvironmentVariables(id self, SEL _cmd) {
    return @{@"DYLD_INSERT_LIBRARIES": @"/usr/local/lib_op2_app_fix.dylib"};
}

static void show_game_window(void);
static void hide_game_window(void);

static void (*orig_launchIcon)(id, SEL, id, long long, id, id, id) = NULL;
static void swizzled_launchIcon(id self, SEL _cmd, id icon, long long loc, id ctx, id act, id actions) {
    id bundleId = nil;
    if ([icon respondsToSelector:@selector(applicationBundleID)]) {
        bundleId = [icon performSelector:@selector(applicationBundleID)];
    }
    sb_log("[OP2SB] launchIcon intercepted for %s\n", bundleId ? [bundleId UTF8String] : "unknown");
    if (bundleId && [bundleId isEqualToString:@"com.apple.game2048"]) {
        show_game_window();
        return;
    }
    if (orig_launchIcon) {
        orig_launchIcon(self, _cmd, icon, loc, ctx, act, actions);
    }
}

static void install_swizzles(void) {
    dlopen("/System/Library/PrivateFrameworks/Celestial.framework/Celestial", RTLD_NOW);
    Class avsCls = objc_getClass("AVSystemController");
    if (avsCls) {
        Method m = class_getClassMethod(avsCls, @selector(sharedAVSystemController));
        if (m) {
            method_setImplementation(m, (IMP)swizzled_sharedAVSystemController);
            sb_log("[OP2SB] Swizzled +[AVSystemController sharedAVSystemController]\n");
        }
    }

    Class lsmCls = objc_getClass("SBLockScreenManager");
    if (lsmCls) {
        Method m1 = class_getInstanceMethod(lsmCls, @selector(shouldPlayLockSound));
        if (m1) {
            method_setImplementation(m1, (IMP)swizzled_shouldPlayLockSound);
            sb_log("[OP2SB] Swizzled shouldPlayLockSound\n");
        }
        SEL selUnlock = sel_registerName("shouldPlayUnlockSound");
        Method m2 = class_getInstanceMethod(lsmCls, selUnlock);
        if (m2) {
            method_setImplementation(m2, (IMP)swizzled_shouldPlayLockSound);
            sb_log("[OP2SB] Swizzled shouldPlayUnlockSound\n");
        }
    }

    Class sbAppCls = objc_getClass("SBApplication");
    if (sbAppCls) {
        class_addMethod(sbAppCls, @selector(identifier), (IMP)op2_identifier, "@@:");
        class_addMethod(sbAppCls, @selector(boolForActivationSetting:), (IMP)op2_boolForActivationSetting, "B@:I");
        class_addMethod(sbAppCls, @selector(flagForActivationSetting:), (IMP)op2_flagForActivationSetting, "q@:I");
        class_addMethod(sbAppCls, @selector(objectForActivationSetting:), (IMP)op2_objectForActivationSetting, "@@:I");
        sb_log("[OP2SB] Added activation settings and identifier methods to SBApplication\n");
    }

    Class sbIconCls = objc_getClass("SBIcon");
    if (sbIconCls) {
        class_addMethod(sbIconCls, @selector(identifier), (IMP)op2_identifier, "@@:");
        sb_log("[OP2SB] Added -[SBIcon identifier]\n");
    }

    dlopen("/System/Library/PrivateFrameworks/FrontBoard.framework/FrontBoard", RTLD_NOW);
    dlopen("/System/Library/PrivateFrameworks/FrontBoardServices.framework/FrontBoardServices", RTLD_NOW);

    Class ctxCls = objc_getClass("FBProcessExecutionContext");
    if (ctxCls) {
        Method mEnv = class_getInstanceMethod(ctxCls, @selector(environment));
        if (mEnv) {
            orig_fb_environment = (NSDictionary *(*)(id, SEL))method_getImplementation(mEnv);
            method_setImplementation(mEnv, (IMP)swizzled_fb_environment);
            sb_log("[OP2SB] Swizzled -[FBProcessExecutionContext environment]\n");
        }
        Method mSetEnv = class_getInstanceMethod(ctxCls, @selector(setEnvironment:));
        if (mSetEnv) {
            orig_fb_setEnvironment = (void (*)(id, SEL, NSDictionary *))method_getImplementation(mSetEnv);
            method_setImplementation(mSetEnv, (IMP)swizzled_fb_setEnvironment);
            sb_log("[OP2SB] Swizzled -[FBProcessExecutionContext setEnvironment:]\n");
        }
    }

    Class appProcCls = objc_getClass("FBApplicationProcess");
    if (appProcCls) {
        Method mDeb = class_getInstanceMethod(appProcCls, sel_registerName("_queue_internalDebugEnvironmentVariables"));
        if (mDeb) {
            method_setImplementation(mDeb, (IMP)swizzled_fb_internalDebugEnvironmentVariables);
            sb_log("[OP2SB] Swizzled -[FBApplicationProcess _queue_internalDebugEnvironmentVariables]\n");
        }
    }

    Class uiCtrlCls = objc_getClass("SBUIController");
    if (uiCtrlCls) {
        SEL selLaunch = @selector(launchIcon:fromLocation:context:activationSettings:actions:);
        Method mLaunch = class_getInstanceMethod(uiCtrlCls, selLaunch);
        if (mLaunch) {
            orig_launchIcon = (void (*)(id, SEL, id, long long, id, id, id))method_getImplementation(mLaunch);
            method_setImplementation(mLaunch, (IMP)swizzled_launchIcon);
            sb_log("[OP2SB] Swizzled -[SBUIController launchIcon:fromLocation:context:activationSettings:actions:]\n");
        }
    }
}

#import "../games/2048/GameViewController.h"

static UIWindow *g_gameWindow = nil;
static GameViewController *g_gameVC = nil;
static void apply_homescreen_layout(void);

static void show_game_window(void) {
    if (!g_gameWindow) {
        CGRect screenBounds = [[UIScreen mainScreen] bounds];
        if (screenBounds.size.width <= 0) screenBounds = CGRectMake(0, 0, 414, 736);
        g_gameWindow = [[UIWindow alloc] initWithFrame:screenBounds];
        g_gameWindow.windowLevel = 2100.0;
        g_gameWindow.opaque = YES;
        g_gameWindow.backgroundColor = [UIColor colorWithRed:0.980 green:0.973 blue:0.937 alpha:1.0];
        g_gameVC = [[GameViewController alloc] init];
        __weak UIWindow *weakWin = g_gameWindow;
        g_gameVC.onExitBlock = ^{
            sb_log("[OP2SB] onExitBlock triggered\n");
            dispatch_async(dispatch_get_main_queue(), ^{
                if (weakWin) {
                    weakWin.hidden = YES;
                    weakWin.alpha = 0.0;
                    [CATransaction flush];
                }
                apply_homescreen_layout();
            });
        };
        g_gameWindow.rootViewController = g_gameVC;
    }
    g_gameWindow.hidden = NO;
    g_gameWindow.alpha = 1.0;
    [g_gameWindow makeKeyAndVisible];
    [g_gameWindow.layer setNeedsDisplay];
    [g_gameWindow.layer setNeedsLayout];
    [g_gameWindow layoutIfNeeded];
    [CATransaction commit];
    [CATransaction flush];
    sb_log("[OP2SB] Game window shown and made key! (frame: [%.1f, %.1f, %.1f, %.1f])\n",
        g_gameWindow.frame.origin.x, g_gameWindow.frame.origin.y,
        g_gameWindow.frame.size.width, g_gameWindow.frame.size.height);
}

static void hide_game_window(void) {
    if (g_gameWindow) {
        g_gameWindow.hidden = YES;
        g_gameWindow.alpha = 0.0;
        [CATransaction commit];
        [CATransaction flush];
        sb_log("[OP2SB] Game window hidden\n");
    }
}

static void apply_homescreen_layout(void) {
    [CATransaction begin];
    hide_game_window();
    NSArray *windows = [[UIApplication sharedApplication] windows];
    for (UIWindow *w in windows) {
        const char *cname = object_getClassName(w);
        if (strstr(cname, "CoverSheet") || strstr(cname, "DashBoard")) {
            w.hidden = YES;
            w.alpha = 0.0;
        } else if (strstr(cname, "Wallpaper")) {
            w.hidden = NO;
            w.alpha = 1.0;
            w.windowLevel = 50.0;
            [w.layer setNeedsDisplay];
            [w.layer setNeedsLayout];
        } else if (strstr(cname, "HomeScreen")) {
            w.hidden = NO;
            w.alpha = 1.0;
            w.windowLevel = 100.0;
            [w.layer setNeedsDisplay];
            [w.layer setNeedsLayout];
            [w layoutIfNeeded];
        } else if (strstr(cname, "MainDisplaySceneLayoutWindow")) {
            w.hidden = YES;
            w.alpha = 0.0;
        } else if (strstr(cname, "FBRootWindow") || strstr(cname, "MainSwitcher") || strstr(cname, "Screenshots")) {
            w.hidden = YES;
        }
    }
    [CATransaction commit];
    [CATransaction flush];
    sb_log("[OP2SB] Applied homescreen layout (Wallpaper=50.0, HomeScreen=100.0)\n");
}

static void apply_app_layout(void) {
    [CATransaction begin];
    NSArray *windows = [[UIApplication sharedApplication] windows];
    for (UIWindow *w in windows) {
        const char *cname = object_getClassName(w);
        if (strstr(cname, "CoverSheet") || strstr(cname, "DashBoard")) {
            w.hidden = YES;
            w.alpha = 0.0;
        } else if (strstr(cname, "MainDisplaySceneLayoutWindow")) {
            w.hidden = NO;
            w.alpha = 1.0;
            w.windowLevel = 200.0;
            [w.layer setNeedsDisplay];
            [w.layer setNeedsLayout];
            [w layoutIfNeeded];
        } else if (strstr(cname, "FBRootWindow")) {
            w.hidden = NO;
            w.alpha = 1.0;
            w.windowLevel = 150.0;
        } else if (strstr(cname, "Wallpaper")) {
            w.hidden = NO;
            w.alpha = 1.0;
            w.windowLevel = 50.0;
        } else if (strstr(cname, "HomeScreen")) {
            w.hidden = NO;
            w.alpha = 1.0;
            w.windowLevel = 100.0;
        } else if (strstr(cname, "MainSwitcher") || strstr(cname, "Screenshots")) {
            w.hidden = YES;
        }
    }
    [CATransaction commit];
    [CATransaction flush];
    sb_log("[OP2SB] Applied app layout (Wallpaper=50.0, HomeScreen=100.0, App=200.0)\n");
}

static void do_unlock(void) {
    @try {
        hide_game_window();
        Class lsmCls = objc_getClass("SBLockScreenManager");
        id mgr = lsmCls ? [lsmCls performSelector:@selector(sharedInstance)] : nil;
        sb_log("[OP2SB] Unlock dispatching, mgr=%p\n", mgr);
        if (mgr) {
            if ([mgr respondsToSelector:@selector(noteMenuButtonSinglePress)]) {
                [mgr performSelector:@selector(noteMenuButtonSinglePress)];
            }
            if ([mgr respondsToSelector:@selector(lockScreenViewControllerRequestsUnlock)]) {
                [mgr performSelector:@selector(lockScreenViewControllerRequestsUnlock)];
            }
            if ([mgr respondsToSelector:@selector(unlockUIFromSource:withOptions:)]) {
                typedef void (*UnlockFn)(id, SEL, int, id);
                UnlockFn fn = (UnlockFn)[mgr methodForSelector:@selector(unlockUIFromSource:withOptions:)];
                fn(mgr, @selector(unlockUIFromSource:withOptions:), 1, nil);
            }
            if ([mgr respondsToSelector:@selector(_attemptUnlockWithPasscode:finishUIUnlock:)]) {
                typedef void (*PassFn)(id, SEL, id, BOOL);
                PassFn fn = (PassFn)[mgr methodForSelector:@selector(_attemptUnlockWithPasscode:finishUIUnlock:)];
                fn(mgr, @selector(_attemptUnlockWithPasscode:finishUIUnlock:), @"", YES);
            }
            if ([mgr respondsToSelector:@selector(_finishUIUnlockFromSource:withOptions:)]) {
                typedef void (*FinishFn)(id, SEL, int, id);
                FinishFn fn = (FinishFn)[mgr methodForSelector:@selector(_finishUIUnlockFromSource:withOptions:)];
                fn(mgr, @selector(_finishUIUnlockFromSource:withOptions:), 1, nil);
            }
            if ([mgr respondsToSelector:@selector(_setUILocked:)]) {
                typedef void (*SetLockedFn)(id, SEL, BOOL);
                SetLockedFn fn = (SetLockedFn)[mgr methodForSelector:@selector(_setUILocked:)];
                fn(mgr, @selector(_setUILocked:), NO);
            }
            if ([mgr respondsToSelector:@selector(lockScreenViewControllerWillDismiss)]) {
                [mgr performSelector:@selector(lockScreenViewControllerWillDismiss)];
            }
            if ([mgr respondsToSelector:@selector(lockScreenViewControllerDidDismiss)]) {
                [mgr performSelector:@selector(lockScreenViewControllerDidDismiss)];
            }
        }

        Class cspCls = objc_getClass("SBCoverSheetPresentationManager");
        id csp = cspCls ? [cspCls performSelector:@selector(sharedInstance)] : nil;
        if (csp) {
            if ([csp respondsToSelector:@selector(setHasBeenDismissedSinceBoot:)]) {
                typedef void (*SetBoolFn)(id, SEL, BOOL);
                SetBoolFn fn = (SetBoolFn)[csp methodForSelector:@selector(setHasBeenDismissedSinceBoot:)];
                fn(csp, @selector(setHasBeenDismissedSinceBoot:), YES);
            }
            if ([csp respondsToSelector:@selector(_setCoverSheetWindowVisible:forReason:)]) {
                typedef void (*SetVisFn)(id, SEL, BOOL, id);
                SetVisFn fn = (SetVisFn)[csp methodForSelector:@selector(_setCoverSheetWindowVisible:forReason:)];
                fn(csp, @selector(_setCoverSheetWindowVisible:forReason:), NO, @"OP2Unlock");
            }
            if ([csp respondsToSelector:@selector(setCoverSheetPresented:animated:withCompletion:)]) {
                typedef void (*DismissFn)(id, SEL, BOOL, BOOL, id);
                DismissFn fn = (DismissFn)[csp methodForSelector:@selector(setCoverSheetPresented:animated:withCompletion:)];
                fn(csp, @selector(setCoverSheetPresented:animated:withCompletion:), NO, NO, nil);
            }
        }

        Class icCls = objc_getClass("SBIconController");
        id ic = icCls ? [icCls performSelector:@selector(sharedInstance)] : nil;
        if (ic) {
            if ([ic respondsToSelector:@selector(scrollToIconListAtIndex:animate:)]) {
                typedef void (*ScrollFn)(id, SEL, long long, BOOL);
                ScrollFn fn = (ScrollFn)[ic methodForSelector:@selector(scrollToIconListAtIndex:animate:)];
                fn(ic, @selector(scrollToIconListAtIndex:animate:), 0, NO);
            }
        }

        Class wpcCls = objc_getClass("SBWallpaperController");
        id wpc = wpcCls ? [wpcCls performSelector:@selector(sharedInstance)] : nil;
        if (wpc) {
            if ([wpc respondsToSelector:@selector(setVariant:withOutAnimationFactory:inAnimationFactory:completion:)]) {
                typedef void (*SetVarFn)(id, SEL, long long, id, id, id);
                SetVarFn fn = (SetVarFn)[wpc methodForSelector:@selector(setVariant:withOutAnimationFactory:inAnimationFactory:completion:)];
                fn(wpc, @selector(setVariant:withOutAnimationFactory:inAnimationFactory:completion:), 0, nil, nil, nil);
            }
        }

        id app = [UIApplication sharedApplication];
        if ([app respondsToSelector:@selector(_simulateHomeButtonPress)]) {
            [app performSelector:@selector(_simulateHomeButtonPress)];
        }
        if ([app respondsToSelector:@selector(_returnToHomeScreenWithCompletion:)]) {
            typedef void (*HomeFn)(id, SEL, id);
            HomeFn fn = (HomeFn)[app methodForSelector:@selector(_returnToHomeScreenWithCompletion:)];
            fn(app, @selector(_returnToHomeScreenWithCompletion:), ^{
                sb_log("[OP2SB] _returnToHomeScreen completion called\n");
            });
        }

        apply_homescreen_layout();
    } @catch (NSException *e) {
        sb_log("[OP2SB] Exception in unlock: %s\n", [[e description] UTF8String]);
    }
}

static void do_launch(NSString *bundleId) {
    @try {
        sb_log("[OP2SB] do_launch called for '%s'\n", [bundleId UTF8String]);
        if ([bundleId isEqualToString:@"com.apple.game2048"]) {
            sb_log("[OP2SB] Launching embedded 2048 Game Window!\n");
            show_game_window();
            return;
        }
        
        Class appCtrlCls = objc_getClass("SBApplicationController");
        id appCtrl = appCtrlCls ? [appCtrlCls performSelector:@selector(sharedInstance)] : nil;
        id sbApp = appCtrl ? [appCtrl performSelector:@selector(applicationWithBundleIdentifier:) withObject:bundleId] : nil;
        sb_log("[OP2SB] appCtrl=%p, sbApp=%p\n", appCtrl, sbApp);

        Class icCls = objc_getClass("SBIconController");
        id ic = icCls ? [icCls performSelector:@selector(sharedInstance)] : nil;
        id model = ic ? [ic performSelector:@selector(model)] : nil;
        id icon = model ? [model performSelector:@selector(applicationIconForBundleIdentifier:) withObject:bundleId] : nil;
        sb_log("[OP2SB] ic=%p, model=%p, icon=%p\n", ic, model, icon);

        Class actSettingsCls = objc_getClass("SBActivationSettings");
        id actSettings = actSettingsCls ? [[actSettingsCls alloc] init] : nil;

        if (icon && [icon respondsToSelector:@selector(launchFromLocation:context:activationSettings:actions:)]) {
            typedef void (*LaunchIconFn)(id, SEL, long long, id, id, id);
            LaunchIconFn fn = (LaunchIconFn)[icon methodForSelector:@selector(launchFromLocation:context:activationSettings:actions:)];
            fn(icon, @selector(launchFromLocation:context:activationSettings:actions:), 0, nil, actSettings, nil);
            sb_log("[OP2SB] Successfully invoked icon launchFromLocation\n");
        } else {
            Class uiCtrlCls = objc_getClass("SBUIController");
            id uiCtrl = uiCtrlCls ? [uiCtrlCls performSelector:@selector(sharedInstance)] : nil;
            if (icon && uiCtrl && [uiCtrl respondsToSelector:@selector(launchIcon:fromLocation:context:activationSettings:actions:)]) {
                typedef void (*LaunchUIFn)(id, SEL, id, long long, id, id, id);
                LaunchUIFn fn = (LaunchUIFn)[uiCtrl methodForSelector:@selector(launchIcon:fromLocation:context:activationSettings:actions:)];
                fn(uiCtrl, @selector(launchIcon:fromLocation:context:activationSettings:actions:), icon, 0, nil, actSettings, nil);
                sb_log("[OP2SB] Successfully invoked uiCtrl launchIcon\n");
            } else if (sbApp && uiCtrl && [uiCtrl respondsToSelector:@selector(_activateApplicationFromAccessibility:)]) {
                [uiCtrl performSelector:@selector(_activateApplicationFromAccessibility:) withObject:sbApp];
                sb_log("[OP2SB] Successfully invoked uiCtrl _activateApplicationFromAccessibility\n");
            }
        }

        id app = [UIApplication sharedApplication];
        if ([app respondsToSelector:@selector(launchApplicationWithIdentifier:suspended:)]) {
            typedef void (*LaunchFn)(id, SEL, id, BOOL);
            LaunchFn fn = (LaunchFn)[app methodForSelector:@selector(launchApplicationWithIdentifier:suspended:)];
            fn(app, @selector(launchApplicationWithIdentifier:suspended:), bundleId, NO);
            sb_log("[OP2SB] launchApplicationWithIdentifier called\n");
        }
    } @catch (NSException *e) {
        sb_log("[OP2SB] Exception in do_launch: %s\n", [[e description] UTF8String]);
    } @finally {
        apply_app_layout();
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.5 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
            apply_app_layout();
        });
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(1.2 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
            apply_app_layout();
        });
    }
}

static void dump_view_hierarchy(UIView *v, FILE *f, int indent) {
    if (!v) return;
    for (int i = 0; i < indent; i++) fprintf(f, "  ");
    fprintf(f, "%s (frame=[%.1f,%.1f,%.1f,%.1f], hidden=%d, alpha=%.2f, subs=%lu)\n",
        object_getClassName(v), v.frame.origin.x, v.frame.origin.y, v.frame.size.width, v.frame.size.height,
        v.hidden, v.alpha, (unsigned long)[v.subviews count]);
    for (UIView *sub in v.subviews) {
        dump_view_hierarchy(sub, f, indent + 1);
    }
}

static void handle_command(int client_fd, const char *cmd) {
    sb_log("[OP2SB] Command received: %s\n", cmd);

    if (strncmp(cmd, "ping", 4) == 0) {
        const char *resp = "PONG\n";
        write(client_fd, resp, strlen(resp));
        return;
    }

    if (strncmp(cmd, "dump", 4) == 0) {
        dispatch_semaphore_t sem = dispatch_semaphore_create(0);
        dispatch_async(dispatch_get_main_queue(), ^{
            FILE *f = fopen("/tmp/sb_diag.txt", "w");
            if (f) {
                NSArray *windows = [[UIApplication sharedApplication] windows];
                fprintf(f, "=== Windows (%lu) ===\n", (unsigned long)[windows count]);
                for (UIWindow *w in windows) {
                    fprintf(f, "\nWindow: %s (hidden=%d, alpha=%.2f, windowLevel=%.1f, frame=(%.1f,%.1f,%.1f,%.1f), rootVC=%s)\n",
                        object_getClassName(w), w.hidden, w.alpha, w.windowLevel,
                        w.frame.origin.x, w.frame.origin.y, w.frame.size.width, w.frame.size.height,
                        w.rootViewController ? object_getClassName(w.rootViewController) : "nil");
                    dump_view_hierarchy(w, f, 1);
                }
                fclose(f);
            }
            dispatch_semaphore_signal(sem);
        });
        CFRunLoopWakeUp(CFRunLoopGetMain());
        dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC));
        const char *resp = "OK dumped to /tmp/sb_diag.txt\n";
        write(client_fd, resp, strlen(resp));
        return;
    }

    if (strncmp(cmd, "lock", 4) == 0) {
        const char *resp = "OK lock dispatched\n";
        write(client_fd, resp, strlen(resp));
        dispatch_async(dispatch_get_main_queue(), ^{
            Class cspCls = objc_getClass("SBCoverSheetPresentationManager");
            id csp = cspCls ? [cspCls performSelector:@selector(sharedInstance)] : nil;
            if (csp && [csp respondsToSelector:@selector(setCoverSheetPresented:animated:withCompletion:)]) {
                typedef void (*DismissFn)(id, SEL, BOOL, BOOL, id);
                DismissFn fn = (DismissFn)[csp methodForSelector:@selector(setCoverSheetPresented:animated:withCompletion:)];
                fn(csp, @selector(setCoverSheetPresented:animated:withCompletion:), YES, NO, nil);
            }
            if (csp && [csp respondsToSelector:@selector(_setCoverSheetWindowVisible:forReason:)]) {
                typedef void (*SetVisFn)(id, SEL, BOOL, id);
                SetVisFn fn = (SetVisFn)[csp methodForSelector:@selector(_setCoverSheetWindowVisible:forReason:)];
                fn(csp, @selector(_setCoverSheetWindowVisible:forReason:), YES, @"OP2Lock");
            }
            Class lsmCls = objc_getClass("SBLockScreenManager");
            id mgr = lsmCls ? [lsmCls performSelector:@selector(sharedInstance)] : nil;
            if (mgr && [mgr respondsToSelector:@selector(lockUIFromSource:withOptions:)]) {
                typedef void (*LockFn)(id, SEL, int, id);
                LockFn fn = (LockFn)[mgr methodForSelector:@selector(lockUIFromSource:withOptions:)];
                fn(mgr, @selector(lockUIFromSource:withOptions:), 1, nil);
            }
        });
        CFRunLoopWakeUp(CFRunLoopGetMain());
        return;
    }

    if (strncmp(cmd, "game", 4) == 0) {
        const char *resp = "OK game dispatched\n";
        write(client_fd, resp, strlen(resp));
        dispatch_async(dispatch_get_main_queue(), ^{
            show_game_window();
        });
        CFRunLoopWakeUp(CFRunLoopGetMain());
        return;
    }

    if (strncmp(cmd, "move ", 5) == 0) {
        const char *dir = cmd + 5;
        char d[32] = {0};
        strncpy(d, dir, sizeof(d)-1);
        char *nl = strchr(d, '\n'); if (nl) *nl = 0;
        char *cr = strchr(d, '\r'); if (cr) *cr = 0;
        char *sp = strchr(d, ' '); if (sp) *sp = 0;
        NSString *dirStr = [NSString stringWithUTF8String:d];

        char resp[128];
        snprintf(resp, sizeof(resp), "OK move %s dispatched\n", d);
        write(client_fd, resp, strlen(resp));
        dispatch_async(dispatch_get_main_queue(), ^{
            if (g_gameVC) {
                if ([dirStr isEqualToString:@"up"]) [g_gameVC actionUp];
                else if ([dirStr isEqualToString:@"down"]) [g_gameVC actionDown];
                else if ([dirStr isEqualToString:@"left"]) [g_gameVC actionLeft];
                else if ([dirStr isEqualToString:@"right"]) [g_gameVC actionRight];
                else if ([dirStr isEqualToString:@"new"]) [g_gameVC startNewGame];
                [CATransaction flush];
            }
        });
        CFRunLoopWakeUp(CFRunLoopGetMain());
        return;
    }

    if (strncmp(cmd, "unlock", 6) == 0 || strncmp(cmd, "home", 4) == 0) {
        const char *resp = "OK unlock/home dispatched\n";
        write(client_fd, resp, strlen(resp));
        dispatch_async(dispatch_get_main_queue(), ^{
            do_unlock();
        });
        CFRunLoopWakeUp(CFRunLoopGetMain());
        return;
    }

    if (strncmp(cmd, "launch ", 7) == 0) {
        const char *bundleIdStr = cmd + 7;
        char bId[128] = {0};
        strncpy(bId, bundleIdStr, sizeof(bId)-1);
        char *nl = strchr(bId, '\n'); if (nl) *nl = 0;
        char *cr = strchr(bId, '\r'); if (cr) *cr = 0;
        char *sp = strchr(bId, ' '); if (sp) *sp = 0;
        NSString *bundleId = [[NSString alloc] initWithUTF8String:bId];

        char resp[256];
        snprintf(resp, sizeof(resp), "OK launch %s dispatched\n", bId);
        write(client_fd, resp, strlen(resp));

        dispatch_async(dispatch_get_main_queue(), ^{
            do_launch(bundleId);
        });
        CFRunLoopWakeUp(CFRunLoopGetMain());
        return;
    }

    if (strncmp(cmd, "diag", 4) == 0) {
        char *resp = (char *)malloc(1024);
        memset(resp, 0, 1024);
        dispatch_semaphore_t sem = dispatch_semaphore_create(0);
        dispatch_async(dispatch_get_main_queue(), ^{
            Class lsmCls = objc_getClass("SBLockScreenManager");
            id mgr = lsmCls ? [lsmCls performSelector:@selector(sharedInstance)] : nil;
            int uiLocked = mgr ? (int)[mgr performSelector:@selector(isUILocked)] : -1;
            int lsActive = mgr ? (int)[mgr performSelector:@selector(isLockScreenActive)] : -1;
            int lsVisible = mgr ? (int)[mgr performSelector:@selector(isLockScreenVisible)] : -1;
            
            Class cspCls = objc_getClass("SBCoverSheetPresentationManager");
            id csp = cspCls ? [cspCls performSelector:@selector(sharedInstance)] : nil;
            int dismissedBoot = csp ? (int)[csp performSelector:@selector(hasBeenDismissedSinceBoot)] : -1;
            
            id app = [UIApplication sharedApplication];
            int showingHome = app ? (int)[app performSelector:@selector(isShowingHomescreen)] : -1;
            int appLocked = app ? (int)[app performSelector:@selector(isLocked)] : -1;
            
            snprintf(resp, 1024, 
                "DIAG: uiLocked=%d lsActive=%d lsVisible=%d dismissedBoot=%d showingHome=%d appLocked=%d mgr=%p csp=%p app=%p\n",
                uiLocked, lsActive, lsVisible, dismissedBoot, showingHome, appLocked, mgr, csp, app);
            dispatch_semaphore_signal(sem);
        });
        CFRunLoopWakeUp(CFRunLoopGetMain());
        long r = dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC));
        if (r != 0) {
            snprintf(resp, 1024, "DIAG: TIMEOUT waiting for main queue block\n");
        }
        write(client_fd, resp, strlen(resp));
        free(resp);
        return;
    }

    const char *err = "ERR unknown command\n";
    write(client_fd, err, strlen(err));
}

static void *server_thread(void *arg) {
    const char *sock_path = "/var/run/sb.sock";
    unlink(sock_path);

    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) {
        sb_log("[OP2SB] socket(AF_UNIX) failed: %d\n", errno);
        return NULL;
    }

    struct sockaddr_un sun;
    memset(&sun, 0, sizeof(sun));
    sun.sun_family = AF_UNIX;
    strncpy(sun.sun_path, sock_path, sizeof(sun.sun_path)-1);

    if (bind(s, (struct sockaddr *)&sun, sizeof(sun)) < 0) {
        sb_log("[OP2SB] bind(%s) failed: %d\n", sock_path, errno);
        close(s);
        return NULL;
    }

    chmod(sock_path, 0777);

    if (listen(s, 10) < 0) {
        sb_log("[OP2SB] listen() failed: %d\n", errno);
        close(s);
        return NULL;
    }

    sb_log("[OP2SB] UNIX domain socket server listening on %s (pid %d)\n", sock_path, getpid());

    while (1) {
        int client = accept(s, NULL, NULL);
        if (client < 0) continue;

        char buf[256] = {0};
        ssize_t n = read(client, buf, sizeof(buf)-1);
        if (n > 0) {
            buf[n] = 0;
            handle_command(client, buf);
        }
        close(client);
    }

    close(s);
    return NULL;
}

__attribute__((constructor))
static void op2_springboard_init(void) {
    const char *prog = getprogname();
    if (!prog || strcmp(prog, "SpringBoard") != 0) {
        return;
    }
    sb_log("[OP2SB] *** op2_springboard injected into pid %d (%s) ***\n", getpid(), prog);
    install_swizzles();
    pthread_t th;
    pthread_create(&th, NULL, server_thread, NULL);
    pthread_detach(th);
}
