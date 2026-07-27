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

- (void)test终端冷启动与重新连接 {
    [self.app terminate];
    [self.app launch];
    XCTAssertTrue([self.app.webViews.firstMatch waitForExistenceWithTimeout:180],
                  @"重新启动后终端界面没有在期限内出现");
    [self waitForPromptWithTimeout:300];
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

- (void)testAArch64SQLite持久化 {
    [self runGuestStage:@"SQLITE-INSTALL"
                command:@"if apk info -e 'sqlite=3.53.2-r0' "
                         ">/dev/null 2>&1; then :; else "
                         "timeout -k 15 900 apk --cache-max-age 10080 "
                         "add --no-progress 'sqlite=3.53.2-r0' "
                         ">\"$l\" 2>&1; fi && "
                         "apk info -e 'sqlite=3.53.2-r0' >/dev/null 2>>\"$l\" && "
                         "sqlite3 --version >>\"$l\" 2>&1 && "
                         "test \"$(sqlite3 --version | awk '{print $1}')\" = 3.53.2"
                timeout:1020];
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
                command:@"if apk info -e 'python3=3.14.5-r0' "
                         ">/dev/null 2>&1; then :; else "
                         "timeout -k 30 3600 apk --cache-max-age 10080 "
                         "add --no-progress 'python3=3.14.5-r0' "
                         ">\"$l\" 2>&1; fi && "
                         "apk info -e 'python3=3.14.5-r0' >>\"$l\" 2>&1 && "
                         "timeout -k 30 900 python3 -I -c "
                         "'import platform, sys; "
                         "assert platform.machine() == \"aarch64\"; "
                         "assert sys.version_info[:3] == (3, 14, 5)' "
                         ">>\"$l\" 2>&1"
                timeout:4680];
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
                command:@"if apk info -e 'build-base=0.5-r4' "
                         ">/dev/null 2>&1; then :; else "
                         "timeout -k 30 14400 apk --cache-max-age 10080 "
                         "add --no-progress 'build-base=0.5-r4' "
                         ">\"$l\" 2>&1; fi && "
                         "apk info -e 'build-base=0.5-r4' >>\"$l\" 2>&1 && "
                         "command -v cc >>\"$l\" 2>&1 && "
                         "machine=$(timeout -k 15 300 cc -dumpmachine 2>>\"$l\") && "
                         "test \"$machine\" = aarch64-alpine-linux-musl"
                timeout:15600];
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
                command:@"if apk info -e 'git=2.54.0-r0' >/dev/null 2>&1 && "
                         "apk info -e 'openssh-client-default=10.3_p1-r0' "
                         ">/dev/null 2>&1; then :; else "
                         "timeout -k 30 14400 apk --cache-max-age 10080 "
                         "add --no-progress 'git=2.54.0-r0' "
                         "'openssh-client-default=10.3_p1-r0' >\"$l\" 2>&1; fi && "
                         "apk info -e 'git=2.54.0-r0' >>\"$l\" 2>&1 && "
                         "apk info -e 'openssh-client-default=10.3_p1-r0' "
                         ">>\"$l\" 2>&1 && "
                         "test \"$(git --version)\" = 'git version 2.54.0' && "
                         "ssh -V 2>&1 | grep -F 'OpenSSH_10.3p1' >>\"$l\""
                timeout:15600];
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
