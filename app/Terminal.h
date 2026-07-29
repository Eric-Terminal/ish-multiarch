//
//  Terminal.h
//  iSH
//
//  Created by Theodore Dubois on 10/18/17.
//

#import <UIKit/UIKit.h>
#import <WebKit/WebKit.h>

struct tty;

@interface Terminal : NSObject

+ (Terminal *)terminalWithType:(int)type number:(int)number;
#if !ISH_LINUX
// Returns a strong struct tty and a Terminal that has a weak reference to the same tty
+ (Terminal *)createPseudoTerminal:(struct tty **)tty;
#endif

+ (Terminal *)terminalWithUUID:(NSUUID *)uuid;
@property (readonly) NSUUID *uuid;

// 普通 shell PTY 用这两个字段把进程退出事件重新关联到 Scene 标签。
// tty1…tty6 等全局控制台不会设置它们，因此不会被恢复成普通标签。
@property (nonatomic) int sessionProcessIdentifier;
@property (nonatomic, getter=isSessionExited) BOOL sessionExited;
- (void)markSessionExited;

+ (void)convertCommand:(NSArray<NSString *> *)command toArgs:(char *)argv limitSize:(size_t)maxSize;

- (int)sendOutput:(const void *)buf length:(int)len;
- (void)sendInput:(NSData *)input;

- (NSString *)arrow:(char)direction;

// Make this terminal no longer be the singleton terminal with its type and number. Will happen eventually if all references go away, but sometimes you want it to happen now.
- (void)destroy;

@property (readonly) WKWebView *webView;
@property (nonatomic) BOOL enableVoiceOverAnnounce;
@property (nonatomic, getter=isPresentationActive) BOOL presentationActive;
// 原生滚动条由 TerminalView 复用；切换标签时按终端分别保存。
@property (nonatomic) CGSize savedScrollContentSize;
@property (nonatomic) CGPoint savedScrollContentOffset;
// Use KVO on this
@property (readonly) BOOL loaded;

@end

extern struct tty_driver ios_console_driver;
extern struct tty_driver ios_pty_driver;
