#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

// 这里只保存与 UIKit、PTY 生命周期无关的标签选择规则，
// 便于 Scene 恢复和键盘切换共用同一套边界行为。
NS_INLINE NSArray<NSUUID *> *ISHRestoreTerminalTabUUIDs(
        id _Nullable restorationObject,
        id _Nullable legacyUUIDValue) {
    NSArray *candidates = [restorationObject isKindOfClass:NSArray.class] ?
            restorationObject : @[];
    NSMutableArray<NSUUID *> *terminalUUIDs = [NSMutableArray array];
    NSMutableSet<NSUUID *> *seen = [NSMutableSet set];

    for (id candidate in candidates) {
        NSUUID *uuid = nil;
        if ([candidate isKindOfClass:NSUUID.class]) {
            uuid = candidate;
        } else if ([candidate isKindOfClass:NSString.class]) {
            uuid = [[NSUUID alloc] initWithUUIDString:candidate];
        }
        if (uuid != nil && ![seen containsObject:uuid]) {
            [terminalUUIDs addObject:uuid];
            [seen addObject:uuid];
        }
    }

    if (terminalUUIDs.count == 0 &&
            [legacyUUIDValue isKindOfClass:NSString.class]) {
        NSUUID *legacyUUID =
                [[NSUUID alloc] initWithUUIDString:legacyUUIDValue];
        if (legacyUUID != nil)
            [terminalUUIDs addObject:legacyUUID];
    }
    return terminalUUIDs;
}

NS_INLINE NSUUID * _Nullable ISHActiveTerminalTabUUID(
        id _Nullable activeUUIDValue,
        NSArray<NSUUID *> *terminalUUIDs) {
    NSUUID *activeUUID =
            [activeUUIDValue isKindOfClass:NSString.class] ?
            [[NSUUID alloc] initWithUUIDString:activeUUIDValue] : nil;
    if (activeUUID != nil && [terminalUUIDs containsObject:activeUUID])
        return activeUUID;
    return terminalUUIDs.firstObject;
}

NS_INLINE NSUInteger ISHTerminalTabIndexAfterRemoval(
        NSUInteger removedIndex,
        NSUInteger selectedIndex,
        NSUInteger remainingCount) {
    if (remainingCount == 0)
        return NSNotFound;
    if (removedIndex < selectedIndex)
        return selectedIndex - 1;
    if (removedIndex > selectedIndex)
        return selectedIndex;
    return MIN(removedIndex, remainingCount - 1);
}

NS_INLINE NSUInteger ISHCycledTerminalTabIndex(
        NSUInteger selectedIndex,
        NSUInteger count,
        BOOL reverse) {
    if (count == 0)
        return NSNotFound;
    if (selectedIndex == NSNotFound || selectedIndex >= count)
        return reverse ? count - 1 : 0;
    if (reverse)
        return selectedIndex == 0 ? count - 1 : selectedIndex - 1;
    return (selectedIndex + 1) % count;
}

NS_ASSUME_NONNULL_END
