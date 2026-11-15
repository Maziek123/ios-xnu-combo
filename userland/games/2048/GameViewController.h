#import <UIKit/UIKit.h>

@interface GameViewController : UIViewController
- (void)startNewGame;
- (void)actionLeft;
- (void)actionRight;
- (void)actionUp;
- (void)actionDown;
- (void)actionExit;
@property (nonatomic, copy) void (^onExitBlock)(void);
@end

