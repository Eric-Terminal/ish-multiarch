//
//  UITests.m
//  UITests
//
//  Created by Theodore Dubois on 11/13/20.
//

#import <CommonCrypto/CommonDigest.h>
#import <XCTest/XCTest.h>

typedef NS_ENUM(NSUInteger, ISHGuestTransportResult) {
    ISHGuestTransportResultPass,
    ISHGuestTransportResultRetryableFail,
    ISHGuestTransportResultFail,
    ISHGuestTransportResultTimeout,
};

@interface UITests : XCTestCase

@property XCUIApplication *app;
@property BOOL guestRecoveryRequired;

- (BOOL)recoverGuestStateWithTimeout:(NSTimeInterval)timeout;

@end

@implementation UITests

- (void)setUp {
    self.continueAfterFailure = NO;
    self.app = [XCUIApplication new];
    self.guestRecoveryRequired = YES;
    // 产品门禁不允许一次性引导打断已经开始输入的物理命令。
    self.app.launchArguments = @[@"-Skip Startup Message", @"1"];
    [self.app launch];

    XCTAssertTrue([self.app.webViews.firstMatch waitForExistenceWithTimeout:180],
                  @"终端界面没有在期限内出现");
    [self waitForPromptWithTimeout:300];

    BOOL recovered = [self recoverGuestStateWithTimeout:90];
    self.guestRecoveryRequired = !recovered;
    XCTAssertTrue(recovered, @"上轮 guest 测试状态没有恢复");
}

- (void)tearDown {
    @try {
        if (self.guestRecoveryRequired) {
            [self.app terminate];
            [self.app launch];
            XCTAssertTrue(
                    [self.app.webViews.firstMatch waitForExistenceWithTimeout:180],
                    @"异常退出后无法重新打开终端执行恢复");
            [self waitForPromptWithTimeout:300];
            BOOL recovered = [self recoverGuestStateWithTimeout:90];
            self.guestRecoveryRequired = !recovered;
            XCTAssertTrue(recovered, @"异常退出后 guest 测试状态恢复失败");
        }
    } @finally {
        [self.app terminate];
        self.app = nil;
        [super tearDown];
    }
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

- (NSString *)commandUsingCDNIPv4:(NSString *)command {
    // 公网软件门禁仍使用原 hostname、SNI 与证书，只稳定选择 DNS 当前的 A 记录。
    NSString *prefix =
            @"(hosts=/etc/hosts; host=dl-cdn.alpinelinux.org; "
             "b=/root/.ish-ios-ipv4-gate-hosts; n=$b.new; g=; m=; "
             "restore_hosts() { r=$1; trap - 0 HUP INT TERM; "
             "if test -n \"$g\" && ! rm -f \"$g\"; then r=125; fi; "
             "if test -n \"$m\" && ! rm -f \"$m\"; then r=125; fi; "
             "if cp -p \"$b\" \"$hosts\" && cmp -s \"$b\" \"$hosts\" && "
             "test \"$(stat -c '%u:%g:%a' \"$b\")\" = "
             "\"$(stat -c '%u:%g:%a' \"$hosts\")\"; then "
             "if ! rm -f \"$b\" \"$n\"; then r=125; fi; "
             "else r=125; rm -f \"$n\" || :; fi; exit \"$r\"; }; "
             "if test -f \"$b\"; then "
             "if cp -p \"$b\" \"$hosts\" && cmp -s \"$b\" \"$hosts\" && "
             "test \"$(stat -c '%u:%g:%a' \"$b\")\" = "
             "\"$(stat -c '%u:%g:%a' \"$hosts\")\" && rm -f \"$b\" \"$n\"; "
             "then :; else rm -f \"$n\" || :; exit 125; fi; fi; "
             "rm -f \"$n\" || exit 125; "
             "unset http_proxy https_proxy HTTP_PROXY HTTPS_PROXY "
             "ALL_PROXY all_proxy SSL_NO_VERIFY_HOSTNAME; "
             "NO_PROXY=\"$host\"; no_proxy=\"$host\"; export NO_PROXY no_proxy; "
             "ip=$(timeout -k 15 90 nslookup -type=A \"$host\" 2>>\"$l\" | "
             "awk '$1 == \"Name:\" { answer=1; next } "
             "answer && $1 == \"Address:\" && $2 ~ /^[0-9.]+$/ "
             "{ print $2; exit }'); test -n \"$ip\" || exit; "
             "printf 'PINNED_IPV4=%s\\n' \"$ip\" >>\"$l\" || exit; "
             "cp -p \"$hosts\" \"$n\" && mv \"$n\" \"$b\" || exit; "
             "trap 'restore_hosts $?' 0; trap 'restore_hosts 129' HUP; "
             "trap 'restore_hosts 130' INT; trap 'restore_hosts 143' TERM; "
             "printf '\\n%s\\t%s\\n' \"$ip\" \"$host\" >>\"$hosts\" || exit; "
             "g=/tmp/ish-ipv4-getent-$t; rm -f \"$g\" || exit; "
             "timeout -k 15 90 getent ahostsv4 \"$host\" "
             ">\"$g\" 2>>\"$l\" || exit; "
             "awk -v ip=\"$ip\" "
             "'NF { seen=1; if ($1 != ip) bad=1 } "
             "END { exit !(seen && !bad) }' \"$g\" || exit; "
             "rm -f \"$g\" || exit; { ";
    NSString *suffix =
            @"; }; r=$?; restore_hosts \"$r\")";
    return [NSString stringWithFormat:@"%@%@%@", prefix, command, suffix];
}

- (void)typeGuestLine:(NSString *)line {
    // 分批合成键盘事件，期间不做 AX 查询或重新检查焦点。
    const NSUInteger chunkLength = 512;
    for (NSUInteger offset = 0; offset < line.length; offset += chunkLength) {
        NSRange range = NSMakeRange(offset, MIN(chunkLength, line.length - offset));
        [self.app typeText:[line substringWithRange:range]];
    }
    [self.app typeText:@"\n"];
}

- (ISHGuestTransportResult)waitForTransportPass:(NSString *)pass
                                            fail:(NSString *)fail
                                         timeout:(NSTimeInterval)timeout {
    NSPredicate *predicate = [NSPredicate
            predicateWithFormat:@"label CONTAINS %@ OR label CONTAINS %@", pass, fail];
    XCUIElement *finished = [self.app.webViews.staticTexts
            matchingPredicate:predicate].firstMatch;
    NSTimeInterval pollInterval = 1;
    if (timeout >= 3600)
        pollInterval = 60;
    else if (timeout >= 600)
        pollInterval = 30;
    NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:timeout];
    BOOL didFinish = finished.exists;
    while (!didFinish) {
        NSTimeInterval remaining = deadline.timeIntervalSinceNow;
        if (remaining <= 0)
            break;
        [NSThread sleepForTimeInterval:MIN(pollInterval, remaining)];
        didFinish = finished.exists;
    }
    if (!didFinish)
        return ISHGuestTransportResultTimeout;
    NSString *label = finished.label;
    if ([label containsString:pass])
        return ISHGuestTransportResultPass;
    NSString *retryableFail = [fail stringByAppendingString:@":1"];
    NSRange retryableRange = [label rangeOfString:retryableFail];
    if (retryableRange.location != NSNotFound) {
        NSUInteger end = NSMaxRange(retryableRange);
        if (end == label.length ||
                ![NSCharacterSet.decimalDigitCharacterSet
                        characterIsMember:[label characterAtIndex:end]])
            return ISHGuestTransportResultRetryableFail;
    }
    return ISHGuestTransportResultFail;
}

- (ISHGuestTransportResult)submitGuestLineResult:(NSString *)line
                                             pass:(NSString *)pass
                                             fail:(NSString *)fail
                                          timeout:(NSTimeInterval)timeout {
    NSData *lineData = [line dataUsingEncoding:NSUTF8StringEncoding];
    // BusyBox ash 的交互编辑缓冲最多接收 2046 个 ASCII 载荷字符。
    XCTAssertLessThanOrEqual(lineData.length + 1, 1800u,
                             @"guest 传输命令超过安全行长");
    if (lineData.length + 1 > 1800)
        return ISHGuestTransportResultFail;

    // AX 查询会令 WebKit 丢失 first responder，每条物理命令前都重新聚焦。
    [self.app.webViews.firstMatch tap];
    [self typeGuestLine:line];
    return [self waitForTransportPass:pass fail:fail timeout:timeout];
}

- (BOOL)submitGuestLine:(NSString *)line
                   pass:(NSString *)pass
                   fail:(NSString *)fail
                timeout:(NSTimeInterval)timeout {
    ISHGuestTransportResult result =
            [self submitGuestLineResult:line pass:pass fail:fail timeout:timeout];
    BOOL didFinish = result != ISHGuestTransportResultTimeout;
    XCTAssertTrue(didFinish,
                  @"guest 命令传输或执行未在期限内确认：\n%@",
                  self.terminalTail);
    if (!didFinish)
        return NO;
    BOOL didPass = result == ISHGuestTransportResultPass;
    XCTAssertTrue(didPass, @"guest 脚本传输失败：\n%@", self.terminalTail);
    return didPass;
}

- (BOOL)recoverGuestStateWithTimeout:(NSTimeInterval)timeout {
    NSString *token = [NSUUID.UUID.UUIDString substringToIndex:8];
    NSString *pass = [NSString stringWithFormat:@"ISH-RECOVER:%@:PASS", token];
    NSString *fail = [NSString stringWithFormat:@"ISH-RECOVER:%@:FAIL", token];
    NSString *line = [NSString stringWithFormat:
            @"h=/etc/hosts; b=/root/.ish-ios-ipv4-gate-hosts; n=$b.new; r=0; "
             "if test -f \"$b\"; then "
             "if cp -p \"$b\" \"$h\" && cmp -s \"$b\" \"$h\" && "
             "test \"$(stat -c '%%u:%%g:%%a' \"$b\")\" = "
             "\"$(stat -c '%%u:%%g:%%a' \"$h\")\"; then "
             "if ! rm -f \"$b\" \"$n\"; then r=125; fi; "
             "else r=125; rm -f \"$n\" || :; fi; "
             "elif ! rm -f \"$n\"; then r=125; fi; "
             "if ! rm -f /tmp/.ish-ios-uitest-stage.b64 "
             "/tmp/.ish-ios-uitest-stage.sh "
             "/tmp/ish-ipv4-getent-* /tmp/ish-apkindex-*.list "
             "/tmp/ish-apkindex.tar.gz; "
             "then r=125; fi; "
             "if test \"$r\" -eq 0; then "
             "printf 'ISH-RECOVER:%%s:PASS\\n' '%@'; else "
             "printf 'ISH-RECOVER:%%s:FAIL:%%s\\n' '%@' \"$r\"; fi",
            token, token];
    return [self submitGuestLine:line pass:pass fail:fail timeout:timeout];
}

- (BOOL)submitGuestScript:(NSString *)script
                    token:(NSString *)token
                  timeout:(NSTimeInterval)timeout {
    NSData *scriptData = [script dataUsingEncoding:NSUTF8StringEncoding];
    NSString *encoded = [scriptData base64EncodedStringWithOptions:0];
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(scriptData.bytes, (CC_LONG) scriptData.length, digest);
    NSMutableString *expectedSHA256 =
            [NSMutableString stringWithCapacity:CC_SHA256_DIGEST_LENGTH * 2];
    for (NSUInteger index = 0; index < CC_SHA256_DIGEST_LENGTH; index++)
        [expectedSHA256 appendFormat:@"%02x", digest[index]];
    NSString *base64Path = @"/tmp/.ish-ios-uitest-stage.b64";
    NSString *scriptPath = @"/tmp/.ish-ios-uitest-stage.sh";
    const NSUInteger payloadLength = 1024;
    const NSUInteger maximumAttempts = 2;

    for (NSUInteger attempt = 1; attempt <= maximumAttempts; attempt++) {
        NSString *transportToken =
                [NSString stringWithFormat:@"%@-%lu", token, (unsigned long)attempt];
        NSUInteger fragment = 0;
        BOOL shouldRetry = NO;

        for (NSUInteger offset = 0; offset < encoded.length;
             offset += payloadLength) {
            fragment++;
            NSRange range =
                    NSMakeRange(offset, MIN(payloadLength, encoded.length - offset));
            NSString *payload = [encoded substringWithRange:range];
            NSString *number = [NSString stringWithFormat:@"%04lu",
                                                          (unsigned long)fragment];
            NSString *pass = [NSString
                    stringWithFormat:@"ISH-XFER:%@:%@:ACK", transportToken, number];
            NSString *fail = [NSString
                    stringWithFormat:@"ISH-XFER:%@:%@:FAIL", transportToken, number];
            NSString *prepare =
                    fragment == 1 ? @"rm -f \"$b\" \"$s\" && " : @"";
            NSString *redirect = fragment == 1 ? @">" : @">>";
            NSUInteger cumulative = NSMaxRange(range);
            NSString *line = [NSString stringWithFormat:
                    @"b='%@'; s='%@'; if %@printf '%%s' '%@' %@\"$b\" && "
                     "test \"$(/bin/busybox wc -c <\"$b\")\" -eq %lu; then "
                     "printf 'ISH-XFER:%%s:%%s:ACK\\n' '%@' '%@'; else r=$?; "
                     "if ! rm -f \"$b\" \"$s\"; then r=125; fi; "
                     "printf 'ISH-XFER:%%s:%%s:FAIL:%%s\\n' '%@' '%@' \"$r\"; fi",
                    base64Path, scriptPath, prepare, payload, redirect,
                    (unsigned long)cumulative, transportToken, number,
                    transportToken, number];
            ISHGuestTransportResult result = [self
                    submitGuestLineResult:line pass:pass fail:fail timeout:60];
            if (result == ISHGuestTransportResultTimeout) {
                XCTFail(@"guest 脚本分片状态不确定，禁止盲目重试：\n%@",
                        self.terminalTail);
                return NO;
            }
            if (result == ISHGuestTransportResultRetryableFail) {
                shouldRetry = YES;
                break;
            }
            if (result == ISHGuestTransportResultFail) {
                XCTFail(@"guest 脚本分片清理状态不安全，禁止重试：\n%@",
                        self.terminalTail);
                return NO;
            }
        }
        if (shouldRetry)
            continue;

        NSString *done =
                [NSString stringWithFormat:@"ISH-XFER:%@:FINAL:DONE", transportToken];
        NSString *fail =
                [NSString stringWithFormat:@"ISH-XFER:%@:FINAL:FAIL", transportToken];
        NSString *execute = [NSString stringWithFormat:
                @"b='%@'; s='%@'; t='%@'; r=0; "
                 "if /bin/busybox base64 -d \"$b\" >\"$s\" && "
                 "test \"$(/bin/busybox wc -c <\"$s\")\" -eq %lu && "
                 "actual=$(/bin/busybox sha256sum \"$s\") && "
                 "actual=${actual%%%% *} && test \"$actual\" = '%@' && "
                 "rm -f \"$b\"; then /bin/busybox sh \"$s\"; r=$?; "
                 "else r=125; fi; "
                 "if ! rm -f \"$b\" \"$s\"; then r=125; fi; "
                 "if test \"$r\" -eq 0; then "
                 "printf 'ISH-XFER:%%s:FINAL:DONE\\n' \"$t\"; else "
                 "printf 'ISH-XFER:%%s:FINAL:FAIL:%%s\\n' \"$t\" \"$r\"; fi",
                base64Path, scriptPath, transportToken,
                (unsigned long)scriptData.length, expectedSHA256];
        return [self submitGuestLine:execute
                                pass:done
                                fail:fail
                             timeout:timeout];
    }

    XCTFail(@"guest 脚本分片在一次安全重试后仍传输失败：\n%@",
            self.terminalTail);
    return NO;
}

- (void)runGuestStage:(NSString *)stage
               command:(NSString *)command
               timeout:(NSTimeInterval)timeout {
    // 在下一条业务命令前确认 shell 就绪；最后一阶段无需等待无后继用途的提示符。
    [self waitForPromptWithTimeout:180];

    NSString *token = [NSUUID.UUID.UUIDString substringToIndex:8];
    NSString *pass = [NSString stringWithFormat:@"ISH-IOS:%@:%@:PASS", token, stage];
    NSString *fail = [NSString stringWithFormat:@"ISH-IOS:%@:%@:FAIL", token, stage];
    NSString *log = [NSString stringWithFormat:@"/tmp/ish-ios-%@.log",
                                               stage.lowercaseString];

    // 标记在输入行中保持分段，只有 guest 实际执行 printf 后才会出现完整结果。
    NSString *script = [NSString stringWithFormat:
            @"t='%@'; l='%@'; if rm -f \"$l\" && { %@; }; then "
             "printf 'ISH-IOS:%%s:' \"$t\"; printf '%@:PASS\\n'; "
             "else r=$?; tail -c 4096 \"$l\" 2>/dev/null; "
             "printf '\\nISH-IOS:%%s:' \"$t\"; "
             "printf '%@:FAIL:%%s\\n' \"$r\"; fi",
            token, log, command, stage, stage];
    BOOL usesIPv4Scope =
            [command containsString:@"/root/.ish-ios-ipv4-gate-hosts"];
    // 任一传输中断都可能留下固定 stage 文件；业务通过后再解除恢复责任。
    self.guestRecoveryRequired = YES;
    if (![self submitGuestScript:script token:token timeout:timeout])
        return;
    if (!usesIPv4Scope)
        self.guestRecoveryRequired = NO;

    NSPredicate *finishedPredicate = [NSPredicate
            predicateWithFormat:@"label CONTAINS %@ OR label CONTAINS %@", pass, fail];
    XCUIElement *finished = [self.app.webViews.staticTexts
            matchingPredicate:finishedPredicate].firstMatch;
    BOOL didFinish = [finished waitForExistenceWithTimeout:30];
    XCTAssertTrue(didFinish, @"%@ 阶段没有生成结果：\n%@", stage, self.terminalTail);
    if (!didFinish)
        return;
    BOOL didPass = [finished.label containsString:pass];
    XCTAssertTrue(didPass, @"%@ 阶段失败：\n%@", stage, self.terminalTail);
    if (didPass && usesIPv4Scope)
        self.guestRecoveryRequired = NO;
}

- (void)test终端冷启动与重新连接 {
    [self.app terminate];
    [self.app launch];
    XCTAssertTrue([self.app.webViews.firstMatch waitForExistenceWithTimeout:180],
                  @"重新启动后终端界面没有在期限内出现");
    [self waitForPromptWithTimeout:300];
}

- (void)test多终端标签隔离切换与关闭 {
    XCUIElement *firstTab = self.app.buttons[@"Shell 1"];
    XCTAssertTrue([firstTab waitForExistenceWithTimeout:30],
                  @"冷启动后没有创建首个终端标签");
    NSString *firstTabIdentifier = firstTab.identifier;
    XCTAssertTrue([firstTabIdentifier hasPrefix:@"terminal-tab-"],
                  @"首个终端标签没有稳定会话标识");

    NSString *token = [NSUUID.UUID.UUIDString substringToIndex:8];
    NSString *firstToken = [NSString
            stringWithFormat:@"ISH-TAB:%@:FIRST", token];
    NSString *firstFail = [NSString
            stringWithFormat:@"ISH-TAB-FAIL:%@:FIRST", token];
    NSString *firstLine = [NSString stringWithFormat:
            @"printf 'ISH-TAB:%@:'; printf 'FIRST\\n'", token];
    XCTAssertTrue([self submitGuestLine:firstLine
                                   pass:firstToken
                                   fail:firstFail
                                timeout:30],
                  @"首个终端没有执行唯一标记命令");

    XCUIElement *newShell = self.app.buttons[@"new-terminal-tab"];
    XCTAssertTrue([newShell waitForExistenceWithTimeout:30],
                  @"终端标签栏没有提供新建 shell 按钮");
    [newShell tap];

    XCUIElement *secondTab = self.app.buttons[@"Shell 2"];
    XCTAssertTrue([secondTab waitForExistenceWithTimeout:30],
                  @"点击新建后没有出现第二个终端标签");
    [self waitForPromptWithTimeout:180];

    NSString *secondToken = [NSString
            stringWithFormat:@"ISH-TAB:%@:SECOND", token];
    NSString *secondFail = [NSString
            stringWithFormat:@"ISH-TAB-FAIL:%@:SECOND", token];
    NSString *secondLine = [NSString stringWithFormat:
            @"printf 'ISH-TAB:%@:'; printf 'SECOND\\n'", token];
    XCTAssertTrue([self submitGuestLine:secondLine
                                   pass:secondToken
                                   fail:secondFail
                                timeout:30],
                  @"第二个终端没有执行唯一标记命令");

    [firstTab tap];
    NSPredicate *firstTokenPredicate =
            [NSPredicate predicateWithFormat:@"label CONTAINS %@", firstToken];
    XCUIElement *firstTokenOutput = [self.app.webViews.staticTexts
            matchingPredicate:firstTokenPredicate].firstMatch;
    XCTAssertTrue([firstTokenOutput waitForExistenceWithTimeout:30],
                  @"切回首标签后没有恢复它自己的输出：\n%@",
                  self.terminalTail);
    NSPredicate *secondTokenPredicate =
            [NSPredicate predicateWithFormat:@"label CONTAINS %@", secondToken];
    XCTAssertFalse([self.app.webViews.staticTexts
            matchingPredicate:secondTokenPredicate].firstMatch.exists,
                   @"首标签错误显示了第二个 shell 的输出：\n%@",
                   self.terminalTail);

    [secondTab tap];
    XCUIElement *secondTokenOutput = [self.app.webViews.staticTexts
            matchingPredicate:secondTokenPredicate].firstMatch;
    XCTAssertTrue([secondTokenOutput waitForExistenceWithTimeout:30],
                  @"切回第二个标签后没有恢复它自己的输出：\n%@",
                  self.terminalTail);
    XCTAssertFalse([self.app.webViews.staticTexts
            matchingPredicate:firstTokenPredicate].firstMatch.exists,
                   @"第二个标签错误显示了首个 shell 的输出：\n%@",
                   self.terminalTail);

    XCUIElement *closeSecond = self.app.buttons[@"Close Shell 2"];
    XCTAssertTrue([closeSecond waitForExistenceWithTimeout:30],
                  @"第二个标签没有提供关闭按钮");
    [closeSecond tap];

    XCUIElement *confirmation = self.app.alerts[@"Close Shell?"];
    XCTAssertTrue([confirmation waitForExistenceWithTimeout:30],
                  @"关闭运行中的 shell 时没有显示确认框");
    [confirmation.buttons[@"Close"] tap];
    XCTAssertTrue([secondTab waitForNonExistenceWithTimeout:30],
                  @"确认关闭后第二个终端标签仍然存在");
    XCUIElement *remainingFirstTab =
            self.app.buttons[firstTabIdentifier];
    XCTAssertTrue([remainingFirstTab waitForExistenceWithTimeout:30],
                  @"关闭第二个标签时误删或替换了剩余标签");
    XCTAssertTrue([firstTokenOutput waitForExistenceWithTimeout:30],
                  @"关闭相邻标签后没有保留首个 shell 的滚屏内容");

    NSString *remainingToken = [NSString
            stringWithFormat:@"ISH-TAB:%@:REMAINING", token];
    NSString *remainingFail = [NSString
            stringWithFormat:@"ISH-TAB-FAIL:%@:REMAINING", token];
    NSString *remainingLine = [NSString stringWithFormat:
            @"printf 'ISH-TAB:%@:'; printf 'REMAINING\\n'", token];
    XCTAssertTrue([self submitGuestLine:remainingLine
                                   pass:remainingToken
                                   fail:remainingFail
                                timeout:30],
                  @"关闭相邻标签后，剩余 shell 无法继续执行命令");

    NSString *firstCloseIdentifier = [firstTabIdentifier
            stringByReplacingOccurrencesOfString:@"terminal-tab-"
                                      withString:@"close-terminal-tab-"];
    XCUIElement *closeFirst = self.app.buttons[firstCloseIdentifier];
    XCTAssertTrue([closeFirst waitForExistenceWithTimeout:30],
                  @"剩余标签没有提供稳定标识的关闭按钮");
    [closeFirst tap];
    confirmation = self.app.alerts[@"Close Shell?"];
    XCTAssertTrue([confirmation waitForExistenceWithTimeout:30],
                  @"关闭唯一运行中的 shell 时没有显示确认框");
    [confirmation.buttons[@"Close"] tap];
    XCTAssertTrue([remainingFirstTab waitForNonExistenceWithTimeout:30],
                  @"关闭唯一标签后仍保留旧会话");

    XCUIElement *replacementFirstTab = self.app.buttons[@"Shell 1"];
    XCTAssertTrue([replacementFirstTab waitForExistenceWithTimeout:30],
                  @"关闭唯一标签后没有自动创建替代 shell");
    NSString *replacementIdentifier = replacementFirstTab.identifier;
    XCTAssertTrue([replacementIdentifier hasPrefix:@"terminal-tab-"],
                  @"替代 shell 没有稳定会话标识");
    XCTAssertNotEqualObjects(replacementIdentifier, firstTabIdentifier,
                             @"关闭唯一标签后错误复用了旧会话");
    [self waitForPromptWithTimeout:180];

    NSString *replacementToken = [NSString
            stringWithFormat:@"ISH-TAB:%@:REPLACEMENT", token];
    NSString *replacementFail = [NSString
            stringWithFormat:@"ISH-TAB-FAIL:%@:REPLACEMENT", token];
    NSString *replacementLine = [NSString stringWithFormat:
            @"printf 'ISH-TAB:%@:'; printf 'REPLACEMENT\\n'", token];
    XCTAssertTrue([self submitGuestLine:replacementLine
                                   pass:replacementToken
                                   fail:replacementFail
                                timeout:30],
                  @"关闭唯一标签后，替代 shell 无法执行命令");

    // XCUIApplication 的强制终止会连同内核、PTY 和 Terminal 注册表一起退出，
    // 无法验证只发生 Scene 断开/重连时的内存会话恢复；该路径留给多窗口人工门禁。
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
                command:@"timeout -k 15 90 getent hosts dl-cdn.alpinelinux.org "
                         ">\"$l\" 2>&1"
                timeout:150];
    [self runGuestStage:@"NSLOOKUP"
                command:@"timeout -k 15 90 nslookup dl-cdn.alpinelinux.org "
                         ">\"$l\" 2>&1"
                timeout:150];
    [self runGuestStage:@"HTTP-IPV4"
                command:@"ip=$(timeout -k 15 90 "
                         "nslookup dl-cdn.alpinelinux.org | "
                         "awk '$1 == \"Address:\" && $2 ~ /^[0-9.]+$/ "
                         "{ print $2; exit }'); test -n \"$ip\" && "
                         "printf 'GET / HTTP/1.0\\r\\nHost: "
                         "dl-cdn.alpinelinux.org\\r\\nConnection: close\\r\\n\\r\\n' | "
                         "timeout -k 15 90 nc \"$ip\" 80 >\"$l\" 2>&1 && "
                         "grep -Eq '^HTTP/1\\.[01] [0-9]{3}' \"$l\""
                timeout:270];
    [self runGuestStage:@"HTTP"
                command:@"printf 'GET / HTTP/1.0\\r\\nHost: "
                         "dl-cdn.alpinelinux.org\\r\\nConnection: close\\r\\n\\r\\n' | "
                         "timeout -k 15 90 nc dl-cdn.alpinelinux.org 80 "
                         ">\"$l\" 2>&1 && "
                         "grep -Eq '^HTTP/1\\.[01] [0-9]{3}' \"$l\""
                timeout:150];
    [self runGuestStage:@"HTTPS"
                command:[self commandUsingCDNIPv4:
                         @"a=/tmp/ish-apkindex.tar.gz; "
                          "m=/tmp/ish-apkindex-$t.list; "
                          "url=https://dl-cdn.alpinelinux.org/alpine/v3.24/"
                          "main/aarch64/APKINDEX.tar.gz; wget_ok=; "
                          "for attempt in 1 2; do rm -f \"$a\" \"$m\"; "
                          "if timeout -k 15 300 wget -Y off -T 90 -O \"$a\" "
                          "\"$url\" >>\"$l\" 2>&1 && test -s \"$a\" && "
                          "timeout -k 15 120 tar -tzf \"$a\" "
                          ">\"$m\" 2>>\"$l\" && "
                          "grep -qx APKINDEX \"$m\"; then "
                          "wget_ok=1; break; fi; "
                          "printf 'HTTPS_IPV4_ATTEMPT_%s_FAILED\\n' "
                          "\"$attempt\" >>\"$l\"; done; "
                          "test \"$wget_ok\" = 1 && "
                          "grep -F \"Connecting to $host ($ip:\" "
                          "\"$l\" >/dev/null && rm -f \"$m\" \"$a\""]
                timeout:1260];
    [self runGuestStage:@"APK-CONFIG"
                command:@"test \"$(apk --print-arch 2>\"$l\")\" = aarch64 && "
                         "grep -Fx 'https://dl-cdn.alpinelinux.org/alpine/"
                         "v3.24/main' /etc/apk/repositories >/dev/null 2>>\"$l\" && "
                         "grep -Fx 'https://dl-cdn.alpinelinux.org/alpine/"
                         "v3.24/community' /etc/apk/repositories "
                         ">/dev/null 2>>\"$l\""
                timeout:30];
    [self runGuestStage:@"APK-UPDATE"
                command:[self commandUsingCDNIPv4:
                         @"apk_ok=; for attempt in 1 2; do "
                          "if timeout -k 15 900 apk --timeout 120 update "
                          ">>\"$l\" 2>&1; then apk_ok=1; break; fi; "
                          "printf 'APK_UPDATE_IPV4_ATTEMPT_%s_FAILED\\n' "
                          "\"$attempt\" >>\"$l\"; done; test \"$apk_ok\" = 1"]
                timeout:2220];
    [self runGuestStage:@"APK-SEARCH"
                command:@"o=/tmp/ish-apk-search.log; rm -f \"$o\"; "
                         "timeout -k 15 1200 apk --network=no search -x busybox "
                         ">\"$o\" 2>>\"$l\" && "
                         "grep -Eq '^busybox-[0-9]' \"$o\""
                timeout:1320];
}

- (void)testAArch64SQLite持久化 {
    [self runGuestStage:@"SQLITE-INSTALL"
                command:[self commandUsingCDNIPv4:
                         @"if apk info -e 'sqlite=3.53.2-r0' "
                          ">/dev/null 2>&1; then :; else "
                          "apk_ok=; for attempt in 1 2; do "
                          "if timeout -k 15 900 apk --timeout 120 "
                          "--cache-max-age 10080 add --no-progress "
                          "'sqlite=3.53.2-r0' >>\"$l\" 2>&1; then "
                          "apk_ok=1; break; fi; "
                          "printf 'APK_INSTALL_IPV4_ATTEMPT_%s_FAILED\\n' "
                          "\"$attempt\" >>\"$l\"; done; "
                          "test \"$apk_ok\" = 1; fi && "
                          "apk info -e 'sqlite=3.53.2-r0' >/dev/null 2>>\"$l\" && "
                          "sqlite3 --version >>\"$l\" 2>&1 && "
                          "test \"$(sqlite3 --version | "
                          "awk '{print $1}')\" = 3.53.2"]
                timeout:2220];
    [self runGuestStage:@"SQLITE-WAL"
                command:@"d=/root/.ish-ios-sqlite-gate; "
                         "db=$d/matrix.db; o=$d/result; "
                         "rm -rf \"$d\"; mkdir -p \"$d\" && "
                         "sqlite3 \"$db\" 'PRAGMA journal_mode=WAL; "
                         "CREATE TABLE values_under_test(value INTEGER); "
                         "BEGIN; INSERT INTO values_under_test VALUES(40),(2); "
                         "COMMIT; SELECT sum(value) FROM values_under_test; "
                         "PRAGMA integrity_check;' >\"$o\" 2>\"$l\" && "
                         "test \"$(sed -n '1p' \"$o\")\" = wal && "
                         "test \"$(sed -n '2p' \"$o\")\" = 42 && "
                         "test \"$(sed -n '3p' \"$o\")\" = ok && "
                         "test -s \"$db\" && sync"
                timeout:180];

    // 终止宿主进程后再读同一数据库，避免把进程内缓存误当作持久化成功。
    [self.app terminate];
    [self.app launch];
    XCTAssertTrue([self.app.webViews.firstMatch waitForExistenceWithTimeout:180],
                  @"重启后终端界面没有在期限内出现");
    [self waitForPromptWithTimeout:300];

    [self runGuestStage:@"SQLITE-RESTART"
                command:@"d=/root/.ish-ios-sqlite-gate; db=$d/matrix.db; "
                         "command -v sqlite3 >\"$l\" 2>&1 && "
                         "test \"$(sqlite3 \"$db\" 'SELECT sum(value) "
                         "FROM values_under_test;')\" = 42 && "
                         "test \"$(sqlite3 \"$db\" 'PRAGMA integrity_check;')\" = ok && "
                         "rm -rf \"$d\""
                timeout:180];
}

- (void)testAArch64Python运行时 {
    [self runGuestStage:@"PYTHON-INSTALL"
                command:[self commandUsingCDNIPv4:
                         @"if apk info -e 'python3=3.14.5-r0' "
                          ">/dev/null 2>&1; then :; else "
                          "apk_ok=; for attempt in 1 2; do "
                          "if timeout -k 30 3600 apk --timeout 120 "
                          "--cache-max-age 10080 add --no-progress "
                          "'python3=3.14.5-r0' >>\"$l\" 2>&1; then "
                          "apk_ok=1; break; fi; "
                          "printf 'APK_INSTALL_IPV4_ATTEMPT_%s_FAILED\\n' "
                          "\"$attempt\" >>\"$l\"; done; "
                          "test \"$apk_ok\" = 1; fi && "
                          "apk info -e 'python3=3.14.5-r0' >>\"$l\" 2>&1 && "
                          "timeout -k 30 900 python3 -I -c "
                          "'import platform, sys; "
                          "assert platform.machine() == \"aarch64\"; "
                          "assert sys.version_info[:3] == (3, 14, 5)' "
                          ">>\"$l\" 2>&1"]
                timeout:8700];
    [self runGuestStage:@"PYTHON-STDLIB"
                command:@"timeout -k 15 300 python3 -I -c "
                         "'import hashlib, json, math, sqlite3, ssl, zlib; "
                         "payload = {\"arch\": \"aarch64\", \"value\": 42}; "
                         "assert json.loads(json.dumps(payload)) == payload; "
                         "assert hashlib.sha256(b\"iSH-iOS\").hexdigest() == "
                         "\"5a2119976555f8eca995f30ba421f17be94704cd44450ee37c32227efcedfe3a\"; "
                         "assert math.factorial(10) == 3628800; "
                         "assert sqlite3.connect(\":memory:\").execute("
                         "\"select 6 * 7\").fetchone()[0] == 42; "
                         "assert ssl.OPENSSL_VERSION.startswith(\"OpenSSL \"); "
                         "assert zlib.decompress(zlib.compress(b\"python\")) "
                         "== b\"python\"' >\"$l\" 2>&1"
                timeout:420];
    [self runGuestStage:@"PYTHON-FILE"
                command:@"d=/root/.ish-ios-python-gate-$t; export d; "
                         "rm -rf \"$d\"; mkdir -p \"$d\" && "
                         "timeout -k 15 300 python3 -I -c "
                         "'from pathlib import Path; import os; "
                         "p = Path(os.environ[\"d\"]); data = b\"python-ios\\n\" * 64; "
                         "q = p / \"state\"; f = q.open(\"wb\"); "
                         "assert f.write(data) == len(data); "
                         "f.flush(); os.fsync(f.fileno()); f.close(); "
                         "q.rename(p / \"renamed\")' >\"$l\" 2>&1 && "
                         "timeout -k 15 300 python3 -I -c "
                         "'from pathlib import Path; import os; "
                         "p = Path(os.environ[\"d\"]); q = p / \"renamed\"; "
                         "data = b\"python-ios\\n\" * 64; "
                         "assert q.read_bytes() == data; "
                         "assert q.stat().st_size == len(data)' "
                         ">>\"$l\" 2>&1 && sync && rm -rf \"$d\""
                timeout:720];
    [self runGuestStage:@"PYTHON-THREAD"
                command:@"timeout -k 15 300 python3 -I -c "
                         "'from concurrent.futures import ThreadPoolExecutor; "
                         "import threading; "
                         "gate = threading.Barrier(4, timeout=120); "
                         "main_id = threading.get_native_id(); "
                         "pool = ThreadPoolExecutor(max_workers=4); "
                         "futures = [pool.submit(lambda n=n: "
                         "(gate.wait(), threading.get_native_id(), n * n)[1:]) "
                         "for n in range(4)]; "
                         "pairs = [future.result(timeout=180) for future in futures]; "
                         "pool.shutdown(wait=True); ids = [pair[0] for pair in pairs]; "
                         "assert len(set(ids)) == 4 and main_id not in ids; "
                         "assert sorted(pair[1] for pair in pairs) == [0, 1, 4, 9]' "
                         ">\"$l\" 2>&1"
                timeout:420];
}

- (void)testAArch64本地编译与Pthread线程 {
    [self runGuestStage:@"BUILD-INSTALL"
                command:[self commandUsingCDNIPv4:
                         @"if apk info -e 'build-base=0.5-r4' "
                          ">/dev/null 2>&1; then :; else "
                          "apk_ok=; for attempt in 1 2; do "
                          "if timeout -k 30 14400 apk --timeout 120 "
                          "--cache-max-age 10080 add --no-progress "
                          "'build-base=0.5-r4' >>\"$l\" 2>&1; then "
                          "apk_ok=1; break; fi; "
                          "printf 'APK_INSTALL_IPV4_ATTEMPT_%s_FAILED\\n' "
                          "\"$attempt\" >>\"$l\"; done; "
                          "test \"$apk_ok\" = 1; fi && "
                          "apk info -e 'build-base=0.5-r4' >>\"$l\" 2>&1 && "
                          "command -v cc >>\"$l\" 2>&1 && "
                          "machine=$(timeout -k 15 300 cc -dumpmachine "
                          "2>>\"$l\") && "
                          "test \"$machine\" = aarch64-alpine-linux-musl"]
                timeout:30000];
    [self runGuestStage:@"BUILD-C"
                command:@"d=/root/.ish-ios-build-c-$t; rm -rf \"$d\"; "
                         "mkdir -p \"$d\" && printf '%s\\n' "
                         "'#include <stdint.h>' '#include <stdio.h>' "
                         "'int main(void) {' "
                         "'    uint64_t previous = 0, current = 1;' "
                         "'    for (unsigned step = 0; step < 20; step++) {' "
                         "'        uint64_t next = previous + current;' "
                         "'        previous = current;' '        current = next;' "
                         "'    }' '    if (previous != 6765) return 1;' "
                         "'    printf(\"C_OK:%llu\\n\", "
                         "(unsigned long long)previous);' "
                         "'    return 0;' '}' >\"$d/main.c\" && "
                         "timeout -k 30 1800 cc -std=c11 -O2 -Wall -Wextra -Werror "
                         "\"$d/main.c\" -o \"$d/main\" >>\"$l\" 2>&1 && "
                         "timeout -k 15 300 \"$d/main\" >\"$d/result\" 2>>\"$l\" && "
                         "cat \"$d/result\" >>\"$l\" && "
                         "grep -qx 'C_OK:6765' \"$d/result\" && rm -rf \"$d\""
                timeout:2400];
    [self runGuestStage:@"BUILD-PTHREAD"
                command:@"d=/root/.ish-ios-build-pthread-$t; rm -rf \"$d\"; "
                         "mkdir -p \"$d\" && printf '%s\\n' "
                         "'#define _POSIX_C_SOURCE 200809L' "
                         "'#include <pthread.h>' '#include <stdint.h>' "
                         "'#include <stdio.h>' "
                         "'struct job { unsigned index; uint64_t result; };' "
                         "'static void *worker(void *opaque) {' "
                         "'    struct job *job = opaque;' "
                         "'    uint64_t value = (uint64_t)job->index + 1U;' "
                         "'    job->result = value * value;' "
                         "'    return job;' '}' "
                         "'int main(void) {' '    pthread_t threads[4];' "
                         "'    struct job jobs[4] = "
                         "{{0, 0}, {1, 0}, {2, 0}, {3, 0}};' "
                         "'    for (unsigned i = 0; i < 4; i++) {' "
                         "'        if (pthread_create(&threads[i], 0, worker, "
                         "&jobs[i]) != 0) return 10 + (int)i;' '    }' "
                         "'    uint64_t total = 0;' "
                         "'    for (unsigned i = 0; i < 4; i++) {' "
                         "'        void *returned = 0;' "
                         "'        if (pthread_join(threads[i], &returned) != 0 || "
                         "returned != &jobs[i]) return 20 + (int)i;' "
                         "'        total += jobs[i].result;' '    }' "
                         "'    if (total != 30) return 30;' "
                         "'    printf(\"PTHREAD_OK:%llu\\n\", "
                         "(unsigned long long)total);' "
                         "'    return 0;' '}' >\"$d/main.c\" && "
                         "timeout -k 30 1800 cc -std=c11 -O2 -Wall -Wextra -Werror "
                         "-pthread \"$d/main.c\" -o \"$d/main\" >>\"$l\" 2>&1 && "
                         "timeout -k 15 300 \"$d/main\" >\"$d/result\" 2>>\"$l\" && "
                         "cat \"$d/result\" >>\"$l\" && "
                         "grep -qx 'PTHREAD_OK:30' \"$d/result\" && rm -rf \"$d\""
                timeout:2400];
}

- (void)testAArch64Git与SSH客户端 {
    [self runGuestStage:@"GITSSH-INSTALL"
                command:[self commandUsingCDNIPv4:
                         @"if apk info -e 'git=2.54.0-r0' >/dev/null 2>&1 && "
                          "apk info -e 'openssh-client-default=10.3_p1-r0' "
                          ">/dev/null 2>&1; then :; else "
                          "apk_ok=; for attempt in 1 2; do "
                          "if timeout -k 30 14400 apk --timeout 120 "
                          "--cache-max-age 10080 add --no-progress "
                          "'git=2.54.0-r0' "
                          "'openssh-client-default=10.3_p1-r0' "
                          ">>\"$l\" 2>&1; then apk_ok=1; break; fi; "
                          "printf 'APK_INSTALL_IPV4_ATTEMPT_%s_FAILED\\n' "
                          "\"$attempt\" >>\"$l\"; done; "
                          "test \"$apk_ok\" = 1; fi && "
                          "apk info -e 'git=2.54.0-r0' >>\"$l\" 2>&1 && "
                          "apk info -e 'openssh-client-default=10.3_p1-r0' "
                          ">>\"$l\" 2>&1 && "
                          "test \"$(git --version)\" = 'git version 2.54.0' && "
                          "ssh -V 2>&1 | grep -F 'OpenSSH_10.3p1' >>\"$l\""]
                timeout:30000];
    [self runGuestStage:@"GIT"
                command:@"d=/root/.ish-ios-git-gate-$t; rm -rf \"$d\"; "
                         "mkdir -p \"$d/repo\" && git -C \"$d/repo\" init -q && "
                         "git -C \"$d/repo\" config user.name 'iSH iPhone Gate' && "
                         "git -C \"$d/repo\" config user.email 'ios@localhost' && "
                         "printf 'alpha\\n' >\"$d/repo/state.txt\" && "
                         "git -C \"$d/repo\" add state.txt && "
                         "git -C \"$d/repo\" commit -q -m initial && "
                         "printf 'beta\\n' >>\"$d/repo/state.txt\" && "
                         "git -C \"$d/repo\" commit -qam update && sync && "
                         "test \"$(git -C \"$d/repo\" rev-list --count HEAD)\" = 2 && "
                         "test \"$(git -C \"$d/repo\" cat-file -t HEAD)\" = commit && "
                         "git -C \"$d/repo\" show HEAD:state.txt >\"$d/readback\" && "
                         "test \"$(sed -n '1p' \"$d/readback\")\" = alpha && "
                         "test \"$(sed -n '2p' \"$d/readback\")\" = beta && "
                         "test -z \"$(sed -n '3p' \"$d/readback\")\" && "
                         "test -z \"$(git -C \"$d/repo\" status --porcelain)\" && "
                         "git -C \"$d/repo\" fsck --strict --full --no-dangling "
                         ">>\"$l\" 2>&1 && rm -rf \"$d\""
                timeout:900];
    [self runGuestStage:@"SSH"
                command:@"d=/root/.ish-ios-ssh-gate-$t; rm -rf \"$d\"; "
                         "mkdir -m 700 \"$d\" && "
                         "timeout -k 15 600 ssh-keygen -q -t ed25519 -N '' "
                         "-C ish-ios -f \"$d/id_ed25519\" >>\"$l\" 2>&1 && "
                         "test \"$(stat -c '%a' \"$d/id_ed25519\")\" = 600 && "
                         "test \"$(stat -c '%a' \"$d/id_ed25519.pub\")\" = 644 && "
                         "ssh-keygen -y -f \"$d/id_ed25519\" >\"$d/derived.pub\" && "
                         "test \"$(cut -d ' ' -f 1-2 \"$d/derived.pub\")\" = "
                         "\"$(cut -d ' ' -f 1-2 \"$d/id_ed25519.pub\")\" && "
                         "ssh-keygen -lf \"$d/id_ed25519.pub\" >>\"$l\" 2>&1 && "
                         "ssh -Q key | grep -qx ssh-ed25519 && "
                         "ssh -G -F /dev/null -o BatchMode=yes -p 2222 "
                         "example.invalid >\"$d/config\" 2>>\"$l\" && "
                         "grep -qx 'port 2222' \"$d/config\" && "
                         "grep -qx 'batchmode yes' \"$d/config\" && rm -rf \"$d\""
                timeout:900];
}

@end
