//
//  ViewController.h
//  iSH
//
//  Created by Theodore Dubois on 10/17/17.
//

#import <UIKit/UIKit.h>
#import "Terminal.h"

NS_ASSUME_NONNULL_BEGIN

@interface TerminalViewController : UIViewController

@property (nonatomic, nullable) Terminal *terminal;

- (void)startNewSession;
// Scene 恢复只保存普通 shell PTY；全局控制台不会出现在这个列表中。
- (void)restoreSessionsFromTerminalUUIDs:(NSArray<NSUUID *> *)terminalUUIDs
                      activeTerminalUUID:(nullable NSUUID *)activeTerminalUUID;
@property (nonatomic, readonly) NSArray<NSUUID *> *sessionTerminalUUIDs;
@property (nonatomic, readonly, nullable) NSUUID *activeSessionTerminalUUID;

@end

extern struct tty_driver ios_tty_driver;

NS_ASSUME_NONNULL_END
