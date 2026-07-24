//
//  UITests.m
//  UITests
//
//  Created by Theodore Dubois on 11/13/20.
//

#import <XCTest/XCTest.h>

@interface UITests : XCTestCase

@property XCUIApplication *app;

@end

@implementation UITests

- (void)setUp {
    self.continueAfterFailure = NO;
    self.app = [XCUIApplication new];
    [self.app launch];

    XCTAssertTrue([self.app.webViews.firstMatch waitForExistenceWithTimeout:180],
                  @"终端界面没有在期限内出现");
    [self waitForPromptWithTimeout:300];
}

- (void)tearDown {
    [self.app terminate];
    self.app = nil;
}

- (NSString *)terminalTail {
    NSArray<XCUIElement *> *lines = self.app.webViews.staticTexts.allElementsBoundByIndex;
    NSUInteger first = lines.count > 20 ? lines.count - 20 : 0;
    NSMutableArray<NSString *> *labels = [NSMutableArray array];
    for (NSUInteger index = first; index < lines.count; index++) {
        NSString *label = lines[index].label;
        if (label.length != 0)
            [labels addObject:label];
    }
    return [labels componentsJoinedByString:@"\n"];
}

- (void)waitForPromptWithTimeout:(NSTimeInterval)timeout {
    XCUIElementQuery *terminalLines = self.app.webViews.staticTexts;
    NSPredicate *predicate = [NSPredicate predicateWithBlock:
            ^BOOL(id _, NSDictionary *__) {
        XCUIElement *lastLine = terminalLines.allElementsBoundByIndex.lastObject;
        return lastLine.exists && [lastLine.label hasSuffix:@":~#"];
    }];
    XCTestExpectation *prompt = [self expectationForPredicate:predicate
                                              evaluatedWithObject:nil
                                                          handler:nil];
    [self waitForExpectations:@[prompt] timeout:timeout];
}

- (void)runGuestStage:(NSString *)stage
               command:(NSString *)command
               timeout:(NSTimeInterval)timeout {
    NSString *token = [NSUUID.UUID.UUIDString substringToIndex:8];
    NSString *pass = [NSString stringWithFormat:@"ISH-IOS:%@:%@:PASS", token, stage];
    NSString *fail = [NSString stringWithFormat:@"ISH-IOS:%@:%@:FAIL", token, stage];
    NSString *log = [NSString stringWithFormat:@"/tmp/ish-ios-%@.log",
                                               stage.lowercaseString];

    // 标记在输入行中保持分段，只有 guest 实际执行 printf 后才会出现完整结果。
    NSString *script = [NSString stringWithFormat:
            @"t='%@'; l='%@'; rm -f \"$l\"; if %@; then "
             "printf 'ISH-IOS:%%s:' \"$t\"; printf '%@:PASS\\n'; "
             "else r=$?; tail -c 4096 \"$l\" 2>/dev/null; "
             "printf '\\nISH-IOS:%%s:' \"$t\"; "
             "printf '%@:FAIL:%%s\\n' \"$r\"; fi",
            token, log, command, stage, stage];
    [self.app typeText:[script stringByAppendingString:@"\n"]];

    NSPredicate *finishedPredicate = [NSPredicate
            predicateWithFormat:@"label CONTAINS %@ OR label CONTAINS %@", pass, fail];
    XCUIElement *finished = [self.app.webViews.staticTexts
            matchingPredicate:finishedPredicate].firstMatch;
    BOOL didFinish = [finished waitForExistenceWithTimeout:timeout];
    XCTAssertTrue(didFinish, @"%@ 阶段超时：\n%@", stage, self.terminalTail);
    if (!didFinish)
        return;
    XCTAssertTrue([finished.label containsString:pass],
                  @"%@ 阶段失败：\n%@", stage, self.terminalTail);
    [self waitForPromptWithTimeout:30];
}

- (void)testAArch64基础网络与软件源 {
    [self runGuestStage:@"ARCH"
                command:@"uname -m >\"$l\" 2>&1 && grep -qx aarch64 \"$l\""
                timeout:30];
    [self runGuestStage:@"RESOLVER"
                command:@"grep -Eq '^nameserver[[:space:]]+[^[:space:]]+' "
                         "/etc/resolv.conf >\"$l\" 2>&1"
                timeout:30];
    [self runGuestStage:@"GETENT"
                command:@"timeout 90 getent hosts dl-cdn.alpinelinux.org "
                         ">\"$l\" 2>&1"
                timeout:120];
    [self runGuestStage:@"NSLOOKUP"
                command:@"timeout 90 nslookup dl-cdn.alpinelinux.org "
                         ">\"$l\" 2>&1"
                timeout:120];
    [self runGuestStage:@"HTTP-IPV4"
                command:@"ip=$(nslookup dl-cdn.alpinelinux.org | "
                         "awk '$1 == \"Address:\" && $2 ~ /^[0-9.]+$/ "
                         "{ print $2; exit }'); test -n \"$ip\" && "
                         "printf 'GET / HTTP/1.0\\r\\nHost: "
                         "dl-cdn.alpinelinux.org\\r\\nConnection: close\\r\\n\\r\\n' | "
                         "timeout 90 nc \"$ip\" 80 >\"$l\" 2>&1 && "
                         "grep -Eq '^HTTP/1\\.[01] [0-9]{3}' \"$l\""
                timeout:120];
    [self runGuestStage:@"HTTP"
                command:@"printf 'GET / HTTP/1.0\\r\\nHost: "
                         "dl-cdn.alpinelinux.org\\r\\nConnection: close\\r\\n\\r\\n' | "
                         "timeout 90 nc dl-cdn.alpinelinux.org 80 "
                         ">\"$l\" 2>&1 && "
                         "grep -Eq '^HTTP/1\\.[01] [0-9]{3}' \"$l\""
                timeout:120];
    [self runGuestStage:@"HTTPS"
                command:@"a=/tmp/ish-apkindex.tar.gz; rm -f \"$a\"; "
                         "timeout -k 15 300 wget -q -T 60 -t 2 -O \"$a\" "
                         "https://dl-cdn.alpinelinux.org/alpine/v3.24/main/"
                         "aarch64/APKINDEX.tar.gz >\"$l\" 2>&1 && "
                         "test -s \"$a\" && "
                         "tar -tzf \"$a\" 2>>\"$l\" | grep -qx APKINDEX"
                timeout:360];
    [self runGuestStage:@"APK-CONFIG"
                command:@"test \"$(apk --print-arch 2>\"$l\")\" = aarch64 && "
                         "grep -Fx 'https://dl-cdn.alpinelinux.org/alpine/"
                         "v3.24/main' /etc/apk/repositories >/dev/null 2>>\"$l\" && "
                         "grep -Fx 'https://dl-cdn.alpinelinux.org/alpine/"
                         "v3.24/community' /etc/apk/repositories "
                         ">/dev/null 2>>\"$l\""
                timeout:30];
    [self runGuestStage:@"APK-UPDATE"
                command:@"timeout -k 15 900 apk update >\"$l\" 2>&1"
                timeout:1020];
    [self runGuestStage:@"APK-SEARCH"
                command:@"o=/tmp/ish-apk-search.log; rm -f \"$o\"; "
                         "timeout -k 15 1200 apk search -x busybox "
                         ">\"$o\" 2>>\"$l\" && "
                         "grep -Eq '^busybox-[0-9]' \"$o\""
                timeout:1320];
}

@end
