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

- (void)test共享第三方声明入口 {
    XCUIElement *recoveryBar = self.app.navigationBars[@"Recovery Mode"];
    XCTAssertTrue([recoveryBar waitForExistenceWithTimeout:30],
                  @"Recovery 设置页没有在期限内出现");

    XCUIElement *table = self.app.tables.firstMatch;
    XCTAssertTrue([table waitForExistenceWithTimeout:10],
                  @"设置列表没有出现");

    XCUIElement *entry = table.cells[@"third-party-notices-entry"];
    for (NSUInteger attempt = 0; attempt < 4 && !entry.hittable; attempt++)
        [table swipeUp];
    XCTAssertTrue(entry.hittable, @"第三方声明入口不可点击");
    [entry tap];

    XCUIElement *navigationBar =
            self.app.navigationBars[@"Third-Party Notices"];
    XCTAssertTrue([navigationBar waitForExistenceWithTimeout:10],
                  @"第三方声明页面没有出现");

    XCUIElement *textView = self.app.textViews[@"third-party-notices-text"];
    XCTAssertTrue([textView waitForExistenceWithTimeout:10],
                  @"第三方声明正文没有出现");
    XCTAssertTrue([textView.value isKindOfClass:NSString.class],
                  @"第三方声明正文不是文本");
    NSString *text = textView.value;
    NSString *alpineMarker = @"===== BEGIN NOTICE: overview =====";
    NSString *appleHostMarker =
            @"===== BEGIN APPLE HOST NOTICE: overview =====";
    NSRange alpineRange = [text rangeOfString:alpineMarker];
    NSRange appleHostRange = [text rangeOfString:appleHostMarker];
    XCTAssertEqual(alpineRange.location, 0,
                   @"普通 iSH 的 Alpine 声明缺少固定起始标记");
    XCTAssertNotEqual(appleHostRange.location, NSNotFound,
                      @"普通 iSH 的 Apple 宿主声明缺少固定起始标记");
    XCTAssertLessThan(alpineRange.location, appleHostRange.location,
                      @"普通 iSH 的两份声明顺序漂移");

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
