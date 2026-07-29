#import <Foundation/Foundation.h>

#import "../../app/TerminalTabsState.h"

static NSUInteger failures;

static void expect(BOOL condition, NSString *message) {
    if (condition)
        return;
    NSLog(@"失败：%@", message);
    failures++;
}

int main(void) {
    @autoreleasepool {
        NSString *first =
                @"11111111-1111-1111-1111-111111111111";
        NSString *second =
                @"22222222-2222-2222-2222-222222222222";
        NSArray<NSUUID *> *restored = ISHRestoreTerminalTabUUIDs(
                @[first, @"invalid", first, second], nil);
        expect(restored.count == 2,
               @"恢复列表应丢弃无效值和重复 UUID");
        expect([restored.firstObject.UUIDString isEqualToString:
                first.uppercaseString],
               @"恢复列表应保持原顺序");

        NSArray<NSUUID *> *legacy =
                ISHRestoreTerminalTabUUIDs(nil, second);
        expect(legacy.count == 1 &&
                [legacy.firstObject.UUIDString isEqualToString:
                        second.uppercaseString],
               @"旧版单 UUID 状态应继续恢复");

        NSUUID *active = ISHActiveTerminalTabUUID(second, restored);
        expect([active.UUIDString isEqualToString:second.uppercaseString],
               @"有效的活动 UUID 应保持选中");
        active = ISHActiveTerminalTabUUID(
                @"33333333-3333-3333-3333-333333333333", restored);
        expect([active isEqual:restored.firstObject],
               @"无效的活动 UUID 应回落首个标签");
        active = ISHActiveTerminalTabUUID(@[@"malformed"], restored);
        expect([active isEqual:restored.firstObject],
               @"非字符串活动状态应安全回落首个标签");

        expect(ISHTerminalTabIndexAfterRemoval(0, 2, 2) == 1,
               @"删除活动标签之前的标签后应修正索引");
        expect(ISHTerminalTabIndexAfterRemoval(2, 0, 2) == 0,
               @"删除活动标签之后的标签不应改变选择");
        expect(ISHTerminalTabIndexAfterRemoval(1, 1, 2) == 1,
               @"删除活动标签后应优先选择右侧标签");
        expect(ISHTerminalTabIndexAfterRemoval(2, 2, 2) == 1,
               @"删除末尾活动标签后应选择左侧标签");
        expect(ISHTerminalTabIndexAfterRemoval(0, 0, 0) == NSNotFound,
               @"删除最后一个标签后应返回空选择");

        expect(ISHCycledTerminalTabIndex(2, 3, NO) == 0,
               @"向后切换应在末尾回绕");
        expect(ISHCycledTerminalTabIndex(0, 3, YES) == 2,
               @"向前切换应在开头回绕");
        expect(ISHCycledTerminalTabIndex(NSNotFound, 3, NO) == 0,
               @"缺失选择时向后切换应落到首个标签");

        if (failures != 0) {
            NSLog(@"iOS 终端标签状态回归失败：%lu 项",
                  (unsigned long) failures);
            return 1;
        }
        NSLog(@"iOS 终端标签状态回归通过");
    }
    return 0;
}
