#import "GameViewController.h"

#define GRID_SIZE 4

@interface GameViewController () {
    int board[GRID_SIZE][GRID_SIZE];
    NSInteger score;
    NSInteger bestScore;
    UILabel *scoreLabel;
    UILabel *bestScoreLabel;
    UIView *boardContainerView;
    UIView *cellBackgrounds[GRID_SIZE][GRID_SIZE];
    UILabel *tileViews[GRID_SIZE][GRID_SIZE];
    UILabel *gameOverLabel;
}
@end

@implementation GameViewController

- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = [UIColor colorWithRed:0.980 green:0.973 blue:0.937 alpha:1.0]; /* #FAF8EF */

    bestScore = [[NSUserDefaults standardUserDefaults] integerForKey:@"bestScore2048"];
    if (bestScore < 0) bestScore = 0;

    [self setupUI];
    [self setupGestures];
    [self startNewGame];
}

- (void)setupUI {
    CGFloat screenW = self.view.bounds.size.width;
    CGFloat screenH = self.view.bounds.size.height;
    if (screenW <= 0) screenW = 414; /* Default iPhone 6 Plus width */
    if (screenH <= 0) screenH = 736;

    CGFloat margin = 16.0;
    CGFloat topY = 40.0;

    /* Title Label "2048" */
    UILabel *titleLabel = [[UILabel alloc] initWithFrame:CGRectMake(margin, topY, 140, 50)];
    titleLabel.text = @"2048";
    titleLabel.font = [UIFont boldSystemFontOfSize:46];
    titleLabel.textColor = [UIColor colorWithRed:0.467 green:0.431 blue:0.396 alpha:1.0]; /* #776E65 */
    [self.view addSubview:titleLabel];

    /* Subtitle */
    UILabel *subLabel = [[UILabel alloc] initWithFrame:CGRectMake(margin, topY + 52, screenW - 2 * margin, 20)];
    subLabel.text = @"Join the numbers to get to 2048!";
    subLabel.font = [UIFont systemFontOfSize:14];
    subLabel.textColor = [UIColor colorWithRed:0.55 green:0.50 blue:0.45 alpha:1.0];
    [self.view addSubview:subLabel];

    /* Score Boxes */
    CGFloat boxW = 85.0;
    CGFloat boxH = 50.0;
    CGFloat boxSpacing = 8.0;
    CGFloat rightX = screenW - margin;

    /* Best Box */
    UIView *bestBox = [[UIView alloc] initWithFrame:CGRectMake(rightX - boxW, topY, boxW, boxH)];
    bestBox.backgroundColor = [UIColor colorWithRed:0.733 green:0.678 blue:0.627 alpha:1.0]; /* #BBADA0 */
    bestBox.layer.cornerRadius = 6.0;
    bestBox.clipsToBounds = YES;
    [self.view addSubview:bestBox];

    UILabel *bestTitle = [[UILabel alloc] initWithFrame:CGRectMake(0, 4, boxW, 14)];
    bestTitle.text = @"BEST";
    bestTitle.font = [UIFont boldSystemFontOfSize:11];
    bestTitle.textColor = [UIColor colorWithRed:0.933 green:0.894 blue:0.855 alpha:1.0];
    bestTitle.textAlignment = NSTextAlignmentCenter;
    [bestBox addSubview:bestTitle];

    bestScoreLabel = [[UILabel alloc] initWithFrame:CGRectMake(0, 18, boxW, 28)];
    bestScoreLabel.text = [NSString stringWithFormat:@"%ld", (long)bestScore];
    bestScoreLabel.font = [UIFont boldSystemFontOfSize:18];
    bestScoreLabel.textColor = [UIColor whiteColor];
    bestScoreLabel.textAlignment = NSTextAlignmentCenter;
    [bestBox addSubview:bestScoreLabel];

    /* Score Box */
    UIView *scoreBox = [[UIView alloc] initWithFrame:CGRectMake(rightX - boxW * 2 - boxSpacing, topY, boxW, boxH)];
    scoreBox.backgroundColor = [UIColor colorWithRed:0.733 green:0.678 blue:0.627 alpha:1.0];
    scoreBox.layer.cornerRadius = 6.0;
    scoreBox.clipsToBounds = YES;
    [self.view addSubview:scoreBox];

    UILabel *scoreTitle = [[UILabel alloc] initWithFrame:CGRectMake(0, 4, boxW, 14)];
    scoreTitle.text = @"SCORE";
    scoreTitle.font = [UIFont boldSystemFontOfSize:11];
    scoreTitle.textColor = [UIColor colorWithRed:0.933 green:0.894 blue:0.855 alpha:1.0];
    scoreTitle.textAlignment = NSTextAlignmentCenter;
    [scoreBox addSubview:scoreTitle];

    scoreLabel = [[UILabel alloc] initWithFrame:CGRectMake(0, 18, boxW, 28)];
    scoreLabel.text = @"0";
    scoreLabel.font = [UIFont boldSystemFontOfSize:18];
    scoreLabel.textColor = [UIColor whiteColor];
    scoreLabel.textAlignment = NSTextAlignmentCenter;
    [scoreBox addSubview:scoreLabel];

    /* Exit / Home Button */
    UIButton *closeBtn = [UIButton buttonWithType:UIButtonTypeCustom];
    closeBtn.frame = CGRectMake(margin, topY + 54, 85, 34);
    closeBtn.backgroundColor = [UIColor colorWithRed:0.85 green:0.25 blue:0.20 alpha:1.0]; /* Red #D94033 */
    closeBtn.layer.cornerRadius = 6.0;
    [closeBtn setTitle:@"Exit" forState:UIControlStateNormal];
    [closeBtn setTitleColor:[UIColor whiteColor] forState:UIControlStateNormal];
    closeBtn.titleLabel.font = [UIFont boldSystemFontOfSize:14];
    [closeBtn addTarget:self action:@selector(actionExit) forControlEvents:UIControlEventTouchUpInside];
    [self.view addSubview:closeBtn];

    /* New Game Button */
    UIButton *newGameBtn = [UIButton buttonWithType:UIButtonTypeCustom];
    newGameBtn.frame = CGRectMake(rightX - 110, topY + 54, 110, 34);
    newGameBtn.backgroundColor = [UIColor colorWithRed:0.557 green:0.475 blue:0.404 alpha:1.0]; /* #8F7A66 */
    newGameBtn.layer.cornerRadius = 6.0;
    [newGameBtn setTitle:@"New Game" forState:UIControlStateNormal];
    [newGameBtn setTitleColor:[UIColor whiteColor] forState:UIControlStateNormal];
    newGameBtn.titleLabel.font = [UIFont boldSystemFontOfSize:14];
    [newGameBtn addTarget:self action:@selector(startNewGame) forControlEvents:UIControlEventTouchUpInside];
    [self.view addSubview:newGameBtn];

    /* Board Container */
    CGFloat boardSide = screenW - (margin * 2);
    if (boardSide > 380) boardSide = 380;
    CGFloat boardX = (screenW - boardSide) / 2.0;
    CGFloat boardY = topY + 98.0;

    boardContainerView = [[UIView alloc] initWithFrame:CGRectMake(boardX, boardY, boardSide, boardSide)];
    boardContainerView.backgroundColor = [UIColor colorWithRed:0.733 green:0.678 blue:0.627 alpha:1.0]; /* #BBADA0 */
    boardContainerView.layer.cornerRadius = 8.0;
    boardContainerView.clipsToBounds = YES;
    [self.view addSubview:boardContainerView];

    /* Calculate Tile Dimensions */
    CGFloat tileSpacing = 10.0;
    CGFloat totalSpacing = tileSpacing * (GRID_SIZE + 1);
    CGFloat tileSize = (boardSide - totalSpacing) / GRID_SIZE;

    for (int r = 0; r < GRID_SIZE; r++) {
        for (int c = 0; c < GRID_SIZE; c++) {
            CGFloat cx = tileSpacing + c * (tileSize + tileSpacing);
            CGFloat cy = tileSpacing + r * (tileSize + tileSpacing);
            CGRect cellFrame = CGRectMake(cx, cy, tileSize, tileSize);

            /* Empty background cell slot */
            UIView *bgCell = [[UIView alloc] initWithFrame:cellFrame];
            bgCell.backgroundColor = [UIColor colorWithRed:0.804 green:0.757 blue:0.706 alpha:1.0]; /* #CDC1B4 */
            bgCell.layer.cornerRadius = 6.0;
            [boardContainerView addSubview:bgCell];
            cellBackgrounds[r][c] = bgCell;

            /* Active Tile Label */
            UILabel *tile = [[UILabel alloc] initWithFrame:cellFrame];
            tile.layer.cornerRadius = 6.0;
            tile.clipsToBounds = YES;
            tile.textAlignment = NSTextAlignmentCenter;
            tile.hidden = YES;
            [boardContainerView addSubview:tile];
            tileViews[r][c] = tile;
        }
    }

    /* Game Over Overlay Label */
    gameOverLabel = [[UILabel alloc] initWithFrame:CGRectMake(0, 0, boardSide, boardSide)];
    gameOverLabel.backgroundColor = [UIColor colorWithWhite:1.0 alpha:0.75];
    gameOverLabel.text = @"Game Over!\nTap New Game";
    gameOverLabel.numberOfLines = 2;
    gameOverLabel.font = [UIFont boldSystemFontOfSize:28];
    gameOverLabel.textColor = [UIColor colorWithRed:0.467 green:0.431 blue:0.396 alpha:1.0];
    gameOverLabel.textAlignment = NSTextAlignmentCenter;
    gameOverLabel.hidden = YES;
    [boardContainerView addSubview:gameOverLabel];

    /* On-Screen Arrow Controls (for direct, 100% reliable tactile tapping!) */
    CGFloat arrowCenterY = boardY + boardSide + 60.0;
    CGFloat arrowCenterX = screenW / 2.0;
    CGFloat btnW = 68.0;
    CGFloat btnH = 46.0;

    UIButton *upBtn = [self makeArrowButton:@"▲" frame:CGRectMake(arrowCenterX - btnW/2, arrowCenterY - btnH - 6, btnW, btnH) action:@selector(actionUp)];
    UIButton *downBtn = [self makeArrowButton:@"▼" frame:CGRectMake(arrowCenterX - btnW/2, arrowCenterY + 6, btnW, btnH) action:@selector(actionDown)];
    UIButton *leftBtn = [self makeArrowButton:@"◀" frame:CGRectMake(arrowCenterX - btnW/2 - btnW - 14, arrowCenterY - btnH/2, btnW, btnH) action:@selector(actionLeft)];
    UIButton *rightBtn = [self makeArrowButton:@"▶" frame:CGRectMake(arrowCenterX + btnW/2 + 14, arrowCenterY - btnH/2, btnW, btnH) action:@selector(actionRight)];

    [self.view addSubview:upBtn];
    [self.view addSubview:downBtn];
    [self.view addSubview:leftBtn];
    [self.view addSubview:rightBtn];
}

- (UIButton *)makeArrowButton:(NSString *)title frame:(CGRect)frame action:(SEL)action {
    UIButton *btn = [UIButton buttonWithType:UIButtonTypeCustom];
    btn.frame = frame;
    btn.backgroundColor = [UIColor colorWithRed:0.733 green:0.678 blue:0.627 alpha:1.0];
    btn.layer.cornerRadius = 8.0;
    [btn setTitle:title forState:UIControlStateNormal];
    [btn setTitleColor:[UIColor whiteColor] forState:UIControlStateNormal];
    btn.titleLabel.font = [UIFont boldSystemFontOfSize:22];
    [btn addTarget:self action:action forControlEvents:UIControlEventTouchUpInside];
    return btn;
}

- (void)setupGestures {
    UISwipeGestureRecognizer *up = [[UISwipeGestureRecognizer alloc] initWithTarget:self action:@selector(actionUp)];
    up.direction = UISwipeGestureRecognizerDirectionUp;
    [self.view addGestureRecognizer:up];

    UISwipeGestureRecognizer *down = [[UISwipeGestureRecognizer alloc] initWithTarget:self action:@selector(actionDown)];
    down.direction = UISwipeGestureRecognizerDirectionDown;
    [self.view addGestureRecognizer:down];

    UISwipeGestureRecognizer *left = [[UISwipeGestureRecognizer alloc] initWithTarget:self action:@selector(actionLeft)];
    left.direction = UISwipeGestureRecognizerDirectionLeft;
    [self.view addGestureRecognizer:left];

    UISwipeGestureRecognizer *right = [[UISwipeGestureRecognizer alloc] initWithTarget:self action:@selector(actionRight)];
    right.direction = UISwipeGestureRecognizerDirectionRight;
    [self.view addGestureRecognizer:right];
}

- (void)startNewGame {
    memset(board, 0, sizeof(board));
    score = 0;
    scoreLabel.text = @"0";
    gameOverLabel.hidden = YES;

    [self spawnRandomTile];
    [self spawnRandomTile];
    [self renderBoard];
}

- (void)spawnRandomTile {
    NSMutableArray *emptySlots = [NSMutableArray array];
    for (int r = 0; r < GRID_SIZE; r++) {
        for (int c = 0; c < GRID_SIZE; c++) {
            if (board[r][c] == 0) {
                [emptySlots addObject:@[@(r), @(c)]];
            }
        }
    }
    if ([emptySlots count] == 0) return;

    int idx = arc4random_uniform((uint32_t)[emptySlots count]);
    NSArray *slot = emptySlots[idx];
    int r = [slot[0] intValue];
    int c = [slot[1] intValue];

    int val = (arc4random_uniform(10) == 0) ? 4 : 2;
    board[r][c] = val;
}

- (void)renderBoard {
    for (int r = 0; r < GRID_SIZE; r++) {
        for (int c = 0; c < GRID_SIZE; c++) {
            int val = board[r][c];
            UILabel *tile = tileViews[r][c];
            if (val > 0) {
                tile.hidden = NO;
                tile.text = [NSString stringWithFormat:@"%d", val];
                tile.backgroundColor = [self colorForValue:val];
                tile.textColor = [self textColorForValue:val];
                tile.font = [UIFont boldSystemFontOfSize:[self fontSizeForValue:val]];
            } else {
                tile.hidden = YES;
            }
        }
    }

    scoreLabel.text = [NSString stringWithFormat:@"%ld", (long)score];
    if (score > bestScore) {
        bestScore = score;
        bestScoreLabel.text = [NSString stringWithFormat:@"%ld", (long)bestScore];
        [[NSUserDefaults standardUserDefaults] setInteger:bestScore forKey:@"bestScore2048"];
    }

    if (![self canMove]) {
        gameOverLabel.hidden = NO;
    }
}

- (BOOL)canMove {
    for (int r = 0; r < GRID_SIZE; r++) {
        for (int c = 0; c < GRID_SIZE; c++) {
            if (board[r][c] == 0) return YES;
            if (c + 1 < GRID_SIZE && board[r][c] == board[r][c + 1]) return YES;
            if (r + 1 < GRID_SIZE && board[r][c] == board[r + 1][c]) return YES;
        }
    }
    return NO;
}

#pragma mark - Movement Logic

- (void)actionExit {
    if (self.onExitBlock) {
        self.onExitBlock();
    } else {
        [self dismissViewControllerAnimated:YES completion:nil];
    }
}

- (void)actionLeft {
    if ([self moveLeft]) {
        [self spawnRandomTile];
        [self renderBoard];
    }
}

- (void)actionRight {
    if ([self moveRight]) {
        [self spawnRandomTile];
        [self renderBoard];
    }
}

- (void)actionUp {
    if ([self moveUp]) {
        [self spawnRandomTile];
        [self renderBoard];
    }
}

- (void)actionDown {
    if ([self moveDown]) {
        [self spawnRandomTile];
        [self renderBoard];
    }
}

- (BOOL)slideAndMergeRow:(int *)row {
    BOOL changed = NO;
    int temp[GRID_SIZE] = {0};
    int k = 0;

    /* Compress non-zeros */
    for (int i = 0; i < GRID_SIZE; i++) {
        if (row[i] != 0) {
            temp[k++] = row[i];
        }
    }

    /* Merge pairs */
    for (int i = 0; i < GRID_SIZE - 1; i++) {
        if (temp[i] != 0 && temp[i] == temp[i + 1]) {
            temp[i] *= 2;
            score += temp[i];
            temp[i + 1] = 0;
            i++; /* skip merged item */
        }
    }

    /* Compress again */
    int result[GRID_SIZE] = {0};
    k = 0;
    for (int i = 0; i < GRID_SIZE; i++) {
        if (temp[i] != 0) {
            result[k++] = temp[i];
        }
    }

    /* Check if changed and copy back */
    for (int i = 0; i < GRID_SIZE; i++) {
        if (row[i] != result[i]) {
            changed = YES;
            row[i] = result[i];
        }
    }
    return changed;
}

- (BOOL)moveLeft {
    BOOL changed = NO;
    for (int r = 0; r < GRID_SIZE; r++) {
        int row[GRID_SIZE];
        for (int c = 0; c < GRID_SIZE; c++) row[c] = board[r][c];
        if ([self slideAndMergeRow:row]) {
            changed = YES;
            for (int c = 0; c < GRID_SIZE; c++) board[r][c] = row[c];
        }
    }
    return changed;
}

- (BOOL)moveRight {
    BOOL changed = NO;
    for (int r = 0; r < GRID_SIZE; r++) {
        int row[GRID_SIZE];
        for (int c = 0; c < GRID_SIZE; c++) row[c] = board[r][GRID_SIZE - 1 - c];
        if ([self slideAndMergeRow:row]) {
            changed = YES;
            for (int c = 0; c < GRID_SIZE; c++) board[r][GRID_SIZE - 1 - c] = row[c];
        }
    }
    return changed;
}

- (BOOL)moveUp {
    BOOL changed = NO;
    for (int c = 0; c < GRID_SIZE; c++) {
        int col[GRID_SIZE];
        for (int r = 0; r < GRID_SIZE; r++) col[r] = board[r][c];
        if ([self slideAndMergeRow:col]) {
            changed = YES;
            for (int r = 0; r < GRID_SIZE; r++) board[r][c] = col[r];
        }
    }
    return changed;
}

- (BOOL)moveDown {
    BOOL changed = NO;
    for (int c = 0; c < GRID_SIZE; c++) {
        int col[GRID_SIZE];
        for (int r = 0; r < GRID_SIZE; r++) col[r] = board[GRID_SIZE - 1 - r][c];
        if ([self slideAndMergeRow:col]) {
            changed = YES;
            for (int r = 0; r < GRID_SIZE; r++) board[GRID_SIZE - 1 - r][c] = col[r];
        }
    }
    return changed;
}

#pragma mark - Color & Typography Styling

- (UIColor *)colorForValue:(int)val {
    switch (val) {
        case 2:    return [UIColor colorWithRed:0.933 green:0.894 blue:0.855 alpha:1.0]; /* #EEE4DA */
        case 4:    return [UIColor colorWithRed:0.929 green:0.878 blue:0.784 alpha:1.0]; /* #EDE0C8 */
        case 8:    return [UIColor colorWithRed:0.949 green:0.694 blue:0.475 alpha:1.0]; /* #F2B179 */
        case 16:   return [UIColor colorWithRed:0.961 green:0.584 blue:0.388 alpha:1.0]; /* #F59563 */
        case 32:   return [UIColor colorWithRed:0.965 green:0.486 blue:0.373 alpha:1.0]; /* #F67C5F */
        case 64:   return [UIColor colorWithRed:0.965 green:0.369 blue:0.231 alpha:1.0]; /* #F65E3B */
        case 128:  return [UIColor colorWithRed:0.929 green:0.812 blue:0.447 alpha:1.0]; /* #EDCF72 */
        case 256:  return [UIColor colorWithRed:0.929 green:0.800 blue:0.380 alpha:1.0]; /* #EDCC61 */
        case 512:  return [UIColor colorWithRed:0.929 green:0.784 blue:0.314 alpha:1.0]; /* #EDC850 */
        case 1024: return [UIColor colorWithRed:0.929 green:0.773 blue:0.247 alpha:1.0]; /* #EDC53F */
        case 2048: return [UIColor colorWithRed:0.929 green:0.761 blue:0.180 alpha:1.0]; /* #EDC22E */
        default:   return [UIColor colorWithRed:0.235 green:0.227 blue:0.196 alpha:1.0]; /* Super tile */
    }
}

- (UIColor *)textColorForValue:(int)val {
    if (val <= 4) {
        return [UIColor colorWithRed:0.467 green:0.431 blue:0.396 alpha:1.0]; /* #776E65 */
    }
    return [UIColor whiteColor];
}

- (CGFloat)fontSizeForValue:(int)val {
    if (val >= 1024) return 22.0;
    if (val >= 128)  return 26.0;
    return 32.0;
}

@end
