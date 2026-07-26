#import <XCTest/XCTest.h>

@interface ThirdPartyNoticesUITests : XCTestCase

@property (nonatomic, strong) XCUIApplication *app;

@end

@implementation ThirdPartyNoticesUITests

- (void)setUp {
    self.continueAfterFailure = NO;
    self.app = [XCUIApplication new];
    // 声明页不依赖 guest；Recovery 直接加载同一份 About storyboard，
    // 避免本用例触发 rootfs 安装和 Linux 启动。
    self.app.launchArguments = @[@"-recovery", @"YES"];
    [self.app launch];
}

- (void)tearDown {
    [self.app terminate];
    self.app = nil;
}

- (void)testAlpineAArch64声明入口 {
    XCUIElement *recoveryBar = self.app.navigationBars[@"Recovery Mode"];
    XCTAssertTrue([recoveryBar waitForExistenceWithTimeout:30],
                  @"Recovery 设置页没有在期限内出现");

    XCUIElement *table = self.app.tables.firstMatch;
    XCTAssertTrue([table waitForExistenceWithTimeout:10],
                  @"设置列表没有出现");

    XCUIElement *entry = table.cells[@"third-party-notices-entry"];
    for (NSUInteger attempt = 0; attempt < 4 && !entry.hittable; attempt++)
        [table swipeUp];
    XCTAssertTrue(entry.hittable, @"Alpine AArch64 声明入口不可点击");
    [entry tap];

    XCUIElement *navigationBar =
            self.app.navigationBars[@"Alpine AArch64 Notices"];
    XCTAssertTrue([navigationBar waitForExistenceWithTimeout:10],
                  @"Alpine AArch64 声明页面没有出现");

    XCUIElement *textView = self.app.textViews[@"third-party-notices-text"];
    XCTAssertTrue([textView waitForExistenceWithTimeout:10],
                  @"Alpine AArch64 声明正文没有出现");
    XCTAssertTrue([textView.value isKindOfClass:NSString.class],
                  @"Alpine AArch64 声明正文不是文本");
    NSString *text = textView.value;
    XCTAssertTrue([text hasPrefix:@"===== BEGIN NOTICE: overview ====="],
                  @"Alpine AArch64 声明正文缺少固定起始标记");

    XCUIElement *back = navigationBar.buttons[@"Recovery Mode"];
    XCTAssertTrue(back.hittable, @"声明页面的返回按钮不可点击");
    [back tap];
    XCTAssertTrue([recoveryBar waitForExistenceWithTimeout:10],
                  @"从声明页面返回后 Recovery 设置页没有出现");

    // Exit 会先把持久化 recovery 开关复位，再结束 App。
    XCUIElement *exit = recoveryBar.buttons[@"Exit"];
    XCTAssertTrue(exit.hittable, @"Recovery 设置页退出按钮不可点击");
    [exit tap];
    NSPredicate *stopped = [NSPredicate predicateWithBlock:
            ^BOOL(id object, NSDictionary *_) {
        XCUIApplication *app = object;
        return app.state == XCUIApplicationStateNotRunning;
    }];
    XCTestExpectation *appStopped =
            [self expectationForPredicate:stopped evaluatedWithObject:self.app
                                  handler:nil];
    [self waitForExpectations:@[appStopped] timeout:10];
}

@end
