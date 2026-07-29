//
//  ViewController.m
//  iSH
//
//  Created by Theodore Dubois on 10/17/17.
//

#import "TerminalViewController.h"
#import "AppDelegate.h"
#import "TerminalView.h"
#import "BarButton.h"
#import "ArrowBarButton.h"
#import "UserPreferences.h"
#import "AboutViewController.h"
#import "CurrentRoot.h"
#import "NSObject+SaneKVO.h"
#import "TerminalTabsState.h"
#import "LinuxInterop.h"
#include "kernel/init.h"
#include "kernel/task.h"
#include "kernel/calls.h"
#include "fs/devices.h"

@interface TerminalTabView : UIView

@property (nonatomic) UIButton *selectionButton;
@property (nonatomic) UIButton *closeButton;

@end

@implementation TerminalTabView

- (instancetype)initWithIndex:(NSUInteger)index
                         exited:(BOOL)exited {
    if (self = [super init]) {
        self.translatesAutoresizingMaskIntoConstraints = NO;
        self.layer.cornerRadius = 8;
        self.layer.borderWidth = 1.0 / UIScreen.mainScreen.scale;

        _selectionButton = [UIButton buttonWithType:UIButtonTypeSystem];
        _selectionButton.translatesAutoresizingMaskIntoConstraints = NO;
        _selectionButton.tag = index;
        _selectionButton.titleLabel.font =
                [UIFont preferredFontForTextStyle:UIFontTextStyleFootnote];
        _selectionButton.titleLabel.adjustsFontForContentSizeCategory = YES;
        _selectionButton.contentEdgeInsets =
                UIEdgeInsetsMake(0, 12, 0, 6);
        [_selectionButton setTitle:
                [NSString stringWithFormat:@"Shell %lu%@",
                 (unsigned long) index + 1, exited ? @" •" : @""]
                            forState:UIControlStateNormal];
        _selectionButton.accessibilityLabel =
                [NSString stringWithFormat:@"Shell %lu",
                 (unsigned long) index + 1];
        _selectionButton.accessibilityValue =
                exited ? @"Exited" : @"Running";

        _closeButton = [UIButton buttonWithType:UIButtonTypeSystem];
        _closeButton.translatesAutoresizingMaskIntoConstraints = NO;
        _closeButton.tag = index;
        _closeButton.titleLabel.font =
                [UIFont systemFontOfSize:19 weight:UIFontWeightRegular];
        _closeButton.titleLabel.adjustsFontForContentSizeCategory = YES;
        [_closeButton setTitle:@"×" forState:UIControlStateNormal];
        _closeButton.accessibilityLabel =
                [NSString stringWithFormat:@"Close Shell %lu",
                 (unsigned long) index + 1];

        [self addSubview:_selectionButton];
        [self addSubview:_closeButton];
        [NSLayoutConstraint activateConstraints:@[
            [self.heightAnchor constraintGreaterThanOrEqualToConstant:44],
            [_selectionButton.leadingAnchor constraintEqualToAnchor:
                    self.leadingAnchor],
            [_selectionButton.topAnchor constraintEqualToAnchor:
                    self.topAnchor],
            [_selectionButton.bottomAnchor constraintEqualToAnchor:
                    self.bottomAnchor],
            [_selectionButton.widthAnchor
                    constraintGreaterThanOrEqualToConstant:64],
            [_closeButton.leadingAnchor constraintEqualToAnchor:
                    _selectionButton.trailingAnchor],
            [_closeButton.trailingAnchor constraintEqualToAnchor:
                    self.trailingAnchor],
            [_closeButton.topAnchor constraintEqualToAnchor:self.topAnchor],
            [_closeButton.bottomAnchor constraintEqualToAnchor:
                    self.bottomAnchor],
            [_closeButton.widthAnchor constraintEqualToConstant:44],
        ]];
    }
    return self;
}

@end

@interface TerminalViewController () <UIGestureRecognizerDelegate>

@property UITapGestureRecognizer *tapRecognizer;
@property (weak, nonatomic) IBOutlet TerminalView *termView;
@property (weak, nonatomic) IBOutlet NSLayoutConstraint *bottomConstraint;
@property (weak, nonatomic) IBOutlet NSLayoutConstraint *termViewTopConstraint;

@property (weak, nonatomic) IBOutlet UIButton *tabKey;
@property (weak, nonatomic) IBOutlet UIButton *controlKey;
@property (weak, nonatomic) IBOutlet UIButton *escapeKey;
@property (strong, nonatomic) IBOutletCollection(id) NSArray *barButtons;
@property (strong, nonatomic) IBOutletCollection(id) NSArray *barControls;

@property (weak, nonatomic) IBOutlet UIInputView *barView;
@property (weak, nonatomic) IBOutlet UIStackView *bar;
@property (weak, nonatomic) IBOutlet NSLayoutConstraint *barTop;
@property (weak, nonatomic) IBOutlet NSLayoutConstraint *barBottom;
@property (weak, nonatomic) IBOutlet NSLayoutConstraint *barLeading;
@property (weak, nonatomic) IBOutlet NSLayoutConstraint *barTrailing;
@property (weak, nonatomic) IBOutlet NSLayoutConstraint *barButtonWidth;
@property (weak, nonatomic) IBOutlet NSLayoutConstraint *barHeight;
@property (weak, nonatomic) IBOutlet UIView *settingsBadge;

@property (weak, nonatomic) IBOutlet UIButton *infoButton;
@property (weak, nonatomic) IBOutlet UIButton *pasteButton;
@property (weak, nonatomic) IBOutlet UIButton *hideKeyboardButton;

@property (nonatomic) NSMutableArray<Terminal *> *sessionTerminals;
@property (nonatomic) NSUInteger activeSessionIndex;

@property (nonatomic) UIView *terminalTabsContainer;
@property (nonatomic) UIScrollView *terminalTabsScrollView;
@property (nonatomic) UIStackView *terminalTabsStackView;
@property (nonatomic) UIButton *addTerminalButton;
@property (nonatomic) UIButton *restartTerminalButton;
@property (nonatomic) UIView *terminalTabsSeparator;

@property BOOL ignoreKeyboardMotion;
@property (nonatomic) BOOL hasExternalKeyboard;

- (void)setupTerminalTabs;
- (void)reloadTerminalTabs;
- (void)activateSessionAtIndex:(NSUInteger)index;
- (Terminal *)createSessionWithError:(int *)error;

@end

@implementation TerminalViewController

static const NSUInteger MaximumTerminalSessions = 8;

- (void)viewDidLoad {
    [super viewDidLoad];
    [self setupTerminalTabs];

    int bootError = [AppDelegate bootError];
    if (bootError != 0) {
        NSString *message = [NSString stringWithFormat:@"could not boot"];
        NSString *subtitle = [NSString stringWithFormat:@"error code %d", bootError];
#if !ISH_LINUX
        if (bootError == _EINVAL)
            subtitle = [subtitle stringByAppendingString:@"\n(try reinstalling the app, see release notes for details)"];
#endif
        [self showMessage:message subtitle:subtitle];
        NSLog(@"boot failed with code %d", bootError);
    }

    self.terminal = self.terminal;
    [self reloadTerminalTabs];
    [self.termView becomeFirstResponder];

    NSNotificationCenter *center = [NSNotificationCenter defaultCenter];
    [center addObserver:self
               selector:@selector(keyboardDidSomething:)
                   name:UIKeyboardWillChangeFrameNotification
                 object:nil];
    [center addObserver:self
               selector:@selector(keyboardDidSomething:)
                   name:UIKeyboardDidChangeFrameNotification
                 object:nil];
    [center addObserver:self
               selector:@selector(_updateBadge)
                   name:FsUpdatedNotification
                 object:nil];


    [self _updateStyleFromPreferences:NO];
    
    if (UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPad) {
        [self.bar removeArrangedSubview:self.hideKeyboardButton];
        [self.hideKeyboardButton removeFromSuperview];
    }
    if (UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPhone) {
        self.barHeight.constant = 36;
    } else {
        self.barHeight.constant = 43;
    }
    
    // SF Symbols is cool
    if (@available(iOS 13, *)) {
        [self.infoButton setImage:[UIImage systemImageNamed:@"gear"] forState:UIControlStateNormal];
        [self.pasteButton setImage:[UIImage systemImageNamed:@"doc.on.clipboard"] forState:UIControlStateNormal];
        [self.hideKeyboardButton setImage:[UIImage systemImageNamed:@"keyboard.chevron.compact.down"] forState:UIControlStateNormal];
        
        [self.tabKey setTitle:nil forState:UIControlStateNormal];
        [self.tabKey setImage:[UIImage systemImageNamed:@"arrow.right.to.line.alt"] forState:UIControlStateNormal];
        [self.controlKey setTitle:nil forState:UIControlStateNormal];
        [self.controlKey setImage:[UIImage systemImageNamed:@"control"] forState:UIControlStateNormal];
        [self.escapeKey setTitle:nil forState:UIControlStateNormal];
        [self.escapeKey setImage:[UIImage systemImageNamed:@"escape"] forState:UIControlStateNormal];
    }
    
    [UserPreferences.shared observe:@[@"hideStatusBar"] options:0 owner:self usingBlock:^(typeof(self) self) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [self setNeedsStatusBarAppearanceUpdate];
        });
    }];
    [UserPreferences.shared observe:@[@"colorScheme", @"theme", @"hideExtraKeysWithExternalKeyboard"]
                            options:0 owner:self usingBlock:^(typeof(self) self) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [self _updateStyleFromPreferences:YES];
        });
    }];
    [self _updateBadge];
}

- (void)awakeFromNib {
    [super awakeFromNib];
    self.sessionTerminals = [NSMutableArray array];
    self.activeSessionIndex = NSNotFound;
#if !ISH_LINUX
    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(processExited:)
                                               name:ProcessExitedNotification
                                             object:nil];
#else
    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(kernelPanicked:)
                                               name:KernelPanicNotification
                                             object:nil];
#endif
}

- (void)viewDidAppear:(BOOL)animated {
    [AppDelegate maybePresentStartupMessageOnViewController:self];
    [super viewDidAppear:animated];
}

- (void)startNewSession {
    if ([AppDelegate bootError] != 0)
        return;
    if (self.sessionTerminals.count >= MaximumTerminalSessions) {
        [self showMessage:@"Shell limit reached"
                 subtitle:@"Close a shell before opening another one."];
        return;
    }
    int err = 0;
    Terminal *terminal = [self createSessionWithError:&err];
    if (terminal == nil) {
        [self showMessage:@"could not start session"
                 subtitle:[NSString stringWithFormat:@"error code %d", err]];
        return;
    }
    [self.sessionTerminals addObject:terminal];
    [self activateSessionAtIndex:self.sessionTerminals.count - 1];
}

- (NSArray<NSUUID *> *)sessionTerminalUUIDs {
    NSMutableArray<NSUUID *> *terminalUUIDs = [NSMutableArray array];
    for (Terminal *terminal in self.sessionTerminals)
        [terminalUUIDs addObject:terminal.uuid];
    return terminalUUIDs;
}

- (NSUUID *)activeSessionTerminalUUID {
    if (self.activeSessionIndex == NSNotFound ||
            self.activeSessionIndex >= self.sessionTerminals.count)
        return nil;
    return self.sessionTerminals[self.activeSessionIndex].uuid;
}

- (void)restoreSessionsFromTerminalUUIDs:(NSArray<NSUUID *> *)terminalUUIDs
                      activeTerminalUUID:(nullable NSUUID *)activeTerminalUUID {
    if ([AppDelegate bootError] != 0)
        return;

    [self.sessionTerminals removeAllObjects];
    NSMutableSet<NSUUID *> *seen = [NSMutableSet set];
    for (NSUUID *uuid in terminalUUIDs) {
        if ([seen containsObject:uuid])
            continue;
        [seen addObject:uuid];
        Terminal *terminal = [Terminal terminalWithUUID:uuid];
        // 只有通过普通 shell 启动链创建的 PTY 才能进入标签栏。
        if (terminal != nil &&
                (terminal.sessionProcessIdentifier != 0 ||
                 terminal.isSessionExited)) {
            [self.sessionTerminals addObject:terminal];
        }
    }

    if (self.sessionTerminals.count == 0) {
        self.activeSessionIndex = NSNotFound;
        [self startNewSession];
        return;
    }

    NSUInteger activeIndex = [self.sessionTerminals
            indexOfObjectPassingTest:^BOOL(
                    Terminal *terminal,
                    NSUInteger __unused index,
                    BOOL * __unused stop) {
        return activeTerminalUUID != nil &&
                [terminal.uuid isEqual:activeTerminalUUID];
    }];
    [self activateSessionAtIndex:
            activeIndex == NSNotFound ? 0 : activeIndex];
}

- (Terminal *)createSessionWithError:(int *)error {
    NSArray<NSString *> *command = UserPreferences.shared.launchCommand;

#if !ISH_LINUX
    int err = begin_new_init_child();
    if (err < 0) {
        *error = err;
        return nil;
    }
    struct tty *tty;
    Terminal *terminal = [Terminal createPseudoTerminal:&tty];
    if (terminal == nil) {
        NSAssert(IS_ERR(tty), @"tty should be error");
        *error = (int) PTR_ERR(tty);
        cancel_prepared_process();
        return nil;
    }
    NSString *stdioFile = [NSString stringWithFormat:@"/dev/pts/%d", tty->num];
    err = create_stdio(stdioFile.fileSystemRepresentation, TTY_PSEUDO_SLAVE_MAJOR, tty->num);
    lock(&ttys_lock);
    tty_release(tty);
    unlock(&ttys_lock);
    if (err < 0) {
        *error = err;
        [terminal destroy];
        cancel_prepared_process();
        return nil;
    }

    char argv[4096];
    [Terminal convertCommand:command toArgs:argv limitSize:sizeof(argv)];
    const char *envp = "TERM=xterm-256color\0";
    err = do_execve(command[0].UTF8String, command.count, argv, envp);
    if (err < 0) {
        *error = err;
        [terminal destroy];
        cancel_prepared_process();
        return nil;
    }
    int sessionPid = current->pid;
    err = commit_prepared_process();
    if (err < 0) {
        *error = err;
        [terminal destroy];
        cancel_prepared_process();
        return nil;
    }
    terminal.sessionProcessIdentifier = sessionPid;
    terminal.sessionExited = NO;
#else
    const char *argv_arr[command.count + 1];
    for (NSUInteger i = 0; i < command.count; i++)
        argv_arr[i] = command[i].UTF8String;
    argv_arr[command.count] = NULL;
    const char *envp_arr[] = {
        "TERM=xterm-256color",
        NULL,
    };
    const char *const *argv = argv_arr;
    const char *const *envp = envp_arr;
    __block Terminal *terminal = nil;
    __block int sessionPid = 0;
    __block int err = 1;
    sync_do_in_workqueue(^(void (^done)(void)) {
        linux_start_session(argv[0], argv, envp, ^(int retval, int pid, nsobj_t term) {
            err = retval;
            if (term)
                terminal = CFBridgingRelease(term);
            sessionPid = pid;
            done();
        });
    });
    NSAssert(err <= 0, @"session start did not finish??");
    if (err < 0) {
        *error = err;
        return nil;
    }
    terminal.sessionProcessIdentifier = sessionPid;
    terminal.sessionExited = NO;
#endif
    *error = 0;
    return terminal;
}

#if !ISH_LINUX
- (void)processExited:(NSNotification *)notif {
    Terminal *exitedTerminal = notif.userInfo[@"terminal"];
    int pid = [notif.userInfo[@"pid"] intValue];
    NSUInteger index = [self.sessionTerminals
            indexOfObjectPassingTest:^BOOL(
                    Terminal *terminal,
                    NSUInteger __unused index,
                    BOOL * __unused stop) {
        return exitedTerminal != nil ?
                terminal == exitedTerminal :
                terminal.sessionProcessIdentifier == pid;
    }];
    if (index == NSNotFound)
        return;

    Terminal *terminal = self.sessionTerminals[index];
    uint32_t waitStatus =
            [notif.userInfo[@"code"] unsignedIntValue];
    uint32_t signal = waitStatus & 0x7f;
    NSString *message;
    if (signal == 0) {
        uint32_t exitCode = (waitStatus >> 8) & 0xff;
        message = [NSString stringWithFormat:
                @"\r\n[Process exited with status %u]\r\n", exitCode];
    } else {
        message = [NSString stringWithFormat:
                @"\r\n[Process terminated by signal %u]\r\n", signal];
    }
    [terminal sendOutput:message.UTF8String
                  length:(int) [message lengthOfBytesUsingEncoding:
                          NSUTF8StringEncoding]];
    [self reloadTerminalTabs];
}
#endif

#if ISH_LINUX
- (void)kernelPanicked:(NSNotification *)notif {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"panik" message:notif.userInfo[@"message"] preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"k" style:UIAlertActionStyleDefault handler:nil]];
    [self presentViewController:alert animated:YES completion:nil];
}
#endif

#pragma mark Terminal tabs

- (void)setupTerminalTabs {
    if (self.terminalTabsContainer != nil)
        return;

    UIView *container = [UIView new];
    container.translatesAutoresizingMaskIntoConstraints = NO;
    container.accessibilityIdentifier = @"terminal-tab-bar";
    self.terminalTabsContainer = container;

    UIScrollView *scrollView = [UIScrollView new];
    scrollView.translatesAutoresizingMaskIntoConstraints = NO;
    scrollView.showsHorizontalScrollIndicator = NO;
    scrollView.alwaysBounceHorizontal = YES;
    scrollView.directionalLockEnabled = YES;
    self.terminalTabsScrollView = scrollView;

    UIStackView *tabs = [UIStackView new];
    tabs.translatesAutoresizingMaskIntoConstraints = NO;
    tabs.axis = UILayoutConstraintAxisHorizontal;
    tabs.alignment = UIStackViewAlignmentCenter;
    tabs.spacing = 4;
    self.terminalTabsStackView = tabs;

    UIButton *restart = [UIButton buttonWithType:UIButtonTypeSystem];
    restart.translatesAutoresizingMaskIntoConstraints = NO;
    restart.titleLabel.font =
            [UIFont systemFontOfSize:21 weight:UIFontWeightRegular];
    restart.titleLabel.adjustsFontForContentSizeCategory = YES;
    [restart setTitle:@"↻" forState:UIControlStateNormal];
    [restart addTarget:self
                action:@selector(restartCurrentTerminalTab:)
      forControlEvents:UIControlEventTouchUpInside];
    restart.accessibilityLabel = @"Restart Shell";
    restart.accessibilityIdentifier = @"restart-terminal-tab";
    restart.hidden = YES;
    self.restartTerminalButton = restart;

    UIButton *add = [UIButton buttonWithType:UIButtonTypeSystem];
    add.translatesAutoresizingMaskIntoConstraints = NO;
    add.titleLabel.font =
            [UIFont systemFontOfSize:23 weight:UIFontWeightRegular];
    add.titleLabel.adjustsFontForContentSizeCategory = YES;
    [add setTitle:@"+" forState:UIControlStateNormal];
    [add addTarget:self
                action:@selector(newTerminalTab:)
      forControlEvents:UIControlEventTouchUpInside];
    add.accessibilityLabel = @"New Shell";
    add.accessibilityHint = @"Opens another independent shell session";
    add.accessibilityIdentifier = @"new-terminal-tab";
    self.addTerminalButton = add;

    UIStackView *actions = [[UIStackView alloc]
            initWithArrangedSubviews:@[restart, add]];
    actions.translatesAutoresizingMaskIntoConstraints = NO;
    actions.axis = UILayoutConstraintAxisHorizontal;
    actions.alignment = UIStackViewAlignmentCenter;
    actions.spacing = 2;

    UIView *separator = [UIView new];
    separator.translatesAutoresizingMaskIntoConstraints = NO;
    self.terminalTabsSeparator = separator;

    [self.view addSubview:container];
    [container addSubview:scrollView];
    [scrollView addSubview:tabs];
    [container addSubview:actions];
    [container addSubview:separator];

    self.termViewTopConstraint.active = NO;
    [NSLayoutConstraint activateConstraints:@[
        [container.topAnchor constraintEqualToAnchor:
                self.view.safeAreaLayoutGuide.topAnchor],
        [container.leadingAnchor constraintEqualToAnchor:
                self.view.safeAreaLayoutGuide.leadingAnchor],
        [container.trailingAnchor constraintEqualToAnchor:
                self.view.safeAreaLayoutGuide.trailingAnchor],
        [container.heightAnchor constraintGreaterThanOrEqualToConstant:52],

        [scrollView.leadingAnchor constraintEqualToAnchor:
                container.leadingAnchor constant:4],
        [scrollView.topAnchor constraintEqualToAnchor:container.topAnchor],
        [scrollView.bottomAnchor constraintEqualToAnchor:
                container.bottomAnchor],
        [scrollView.trailingAnchor constraintEqualToAnchor:
                actions.leadingAnchor constant:-4],

        [tabs.leadingAnchor constraintEqualToAnchor:
                scrollView.contentLayoutGuide.leadingAnchor],
        [tabs.trailingAnchor constraintEqualToAnchor:
                scrollView.contentLayoutGuide.trailingAnchor],
        [tabs.topAnchor constraintEqualToAnchor:
                scrollView.contentLayoutGuide.topAnchor constant:4],
        [tabs.bottomAnchor constraintEqualToAnchor:
                scrollView.contentLayoutGuide.bottomAnchor constant:-4],
        [tabs.heightAnchor constraintGreaterThanOrEqualToConstant:44],
        [scrollView.contentLayoutGuide.heightAnchor constraintEqualToAnchor:
                scrollView.frameLayoutGuide.heightAnchor],

        [actions.trailingAnchor constraintEqualToAnchor:
                container.trailingAnchor constant:-4],
        [actions.centerYAnchor constraintEqualToAnchor:
                container.centerYAnchor],
        [restart.widthAnchor constraintEqualToConstant:44],
        [restart.heightAnchor constraintEqualToConstant:44],
        [add.widthAnchor constraintEqualToConstant:44],
        [add.heightAnchor constraintEqualToConstant:44],

        [separator.leadingAnchor constraintEqualToAnchor:
                container.leadingAnchor],
        [separator.trailingAnchor constraintEqualToAnchor:
                container.trailingAnchor],
        [separator.bottomAnchor constraintEqualToAnchor:
                container.bottomAnchor],
        [separator.heightAnchor constraintEqualToConstant:
                1.0 / UIScreen.mainScreen.scale],

        [self.termView.topAnchor constraintEqualToAnchor:
                container.bottomAnchor],
    ]];
}

- (void)reloadTerminalTabs {
    if (self.terminalTabsStackView == nil)
        return;

    for (UIView *view in self.terminalTabsStackView.arrangedSubviews.copy) {
        [self.terminalTabsStackView removeArrangedSubview:view];
        [view removeFromSuperview];
    }

    [self.sessionTerminals enumerateObjectsUsingBlock:
            ^(Terminal *terminal,
              NSUInteger index,
              BOOL * __unused stop) {
        TerminalTabView *tab = [[TerminalTabView alloc]
                initWithIndex:index exited:terminal.isSessionExited];
        [tab.selectionButton addTarget:self
                                action:@selector(selectTerminalTab:)
                      forControlEvents:UIControlEventTouchUpInside];
        [tab.closeButton addTarget:self
                            action:@selector(requestCloseTerminalTab:)
                  forControlEvents:UIControlEventTouchUpInside];
        tab.selectionButton.accessibilityIdentifier =
                [NSString stringWithFormat:@"terminal-tab-%@",
                 terminal.uuid.UUIDString];
        tab.closeButton.accessibilityIdentifier =
                [NSString stringWithFormat:@"close-terminal-tab-%@",
                 terminal.uuid.UUIDString];
        [self.terminalTabsStackView addArrangedSubview:tab];
    }];

    BOOL hasActiveSession =
            self.activeSessionIndex != NSNotFound &&
            self.activeSessionIndex < self.sessionTerminals.count;
    self.restartTerminalButton.hidden = !hasActiveSession ||
            !self.sessionTerminals[self.activeSessionIndex].isSessionExited;
    self.addTerminalButton.enabled =
            self.sessionTerminals.count < MaximumTerminalSessions;
    self.addTerminalButton.accessibilityHint =
            self.addTerminalButton.enabled ?
            @"Opens another independent shell session" :
            @"Close a shell before opening another one";
    [self updateTerminalTabsStyle];

    if (hasActiveSession) {
        dispatch_async(dispatch_get_main_queue(), ^{
            if (self.activeSessionIndex >=
                    self.terminalTabsStackView.arrangedSubviews.count)
                return;
            UIView *tab = self.terminalTabsStackView
                    .arrangedSubviews[self.activeSessionIndex];
            CGRect rect = [tab convertRect:tab.bounds
                                    toView:self.terminalTabsScrollView];
            [self.terminalTabsScrollView scrollRectToVisible:rect
                                                    animated:NO];
        });
    }
}

- (void)updateTerminalTabsStyle {
    if (self.terminalTabsContainer == nil)
        return;
    UIColor *background = [[UIColor alloc]
            ish_initWithHexString:UserPreferences.shared.palette
                    .backgroundColor];
    UIColor *foreground = [[UIColor alloc]
            ish_initWithHexString:UserPreferences.shared.palette
                    .foregroundColor];
    self.terminalTabsContainer.backgroundColor = background;
    self.terminalTabsSeparator.backgroundColor =
            [foreground colorWithAlphaComponent:0.22];
    self.addTerminalButton.tintColor = foreground;
    self.restartTerminalButton.tintColor = foreground;

    [self.terminalTabsStackView.arrangedSubviews
            enumerateObjectsUsingBlock:
            ^(TerminalTabView *tab,
              NSUInteger index,
              BOOL * __unused stop) {
        Terminal *terminal = self.sessionTerminals[index];
        BOOL selected = index == self.activeSessionIndex &&
                self.terminal == terminal;
        tab.backgroundColor = [foreground colorWithAlphaComponent:
                selected ? 0.18 : 0.06];
        tab.layer.borderColor = [foreground colorWithAlphaComponent:
                selected ? 0.42 : 0.15].CGColor;
        UIColor *titleColor = terminal.isSessionExited ?
                [foreground colorWithAlphaComponent:0.58] : foreground;
        [tab.selectionButton setTitleColor:titleColor
                                  forState:UIControlStateNormal];
        [tab.closeButton setTitleColor:
                [foreground colorWithAlphaComponent:0.72]
                              forState:UIControlStateNormal];
        tab.selectionButton.accessibilityTraits =
                UIAccessibilityTraitButton |
                (selected ? UIAccessibilityTraitSelected : 0);
    }];
}

- (void)activateSessionAtIndex:(NSUInteger)index {
    if (index >= self.sessionTerminals.count)
        return;
    // 输入法组合文本尚未进入 PTY；切换前提交，避免草稿落到下一个标签。
    if (self.terminal != self.sessionTerminals[index] &&
            self.isViewLoaded && self.termView.markedTextRange != nil)
        [self.termView unmarkText];
    self.activeSessionIndex = index;
    self.terminal = self.sessionTerminals[index];
    [self reloadTerminalTabs];
    if (self.isViewLoaded)
        [self.termView becomeFirstResponder];
}

- (void)selectTerminalTab:(UIButton *)sender {
    [self activateSessionAtIndex:(NSUInteger) sender.tag];
}

- (void)newTerminalTab:(id)sender {
    [self startNewSession];
}

- (void)requestCloseTerminalTab:(UIButton *)sender {
    NSUInteger index = (NSUInteger) sender.tag;
    if (index >= self.sessionTerminals.count)
        return;
    Terminal *terminal = self.sessionTerminals[index];
    NSUUID *terminalUUID = terminal.uuid;
    if (terminal.isSessionExited) {
        [self closeTerminalTabWithUUID:terminalUUID];
        return;
    }

    UIAlertController *alert = [UIAlertController
            alertControllerWithTitle:@"Close Shell?"
                             message:@"The running process and its scrollback "
                                     "will be discarded."
                      preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel"
                                              style:UIAlertActionStyleCancel
                                            handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Close"
                                              style:UIAlertActionStyleDestructive
                                            handler:^(
                                                    UIAlertAction * __unused
                                                            action) {
        [self closeTerminalTabWithUUID:terminalUUID];
    }]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)closeTerminalTabWithUUID:(NSUUID *)terminalUUID {
    NSUInteger index = [self.sessionTerminals
            indexOfObjectPassingTest:^BOOL(
                    Terminal *terminal,
                    NSUInteger __unused index,
                    BOOL * __unused stop) {
        return [terminal.uuid isEqual:terminalUUID];
    }];
    if (index == NSNotFound)
        return;

    Terminal *terminal = self.sessionTerminals[index];
    BOOL removedActiveSession = index == self.activeSessionIndex;
    NSUInteger newActiveIndex = ISHTerminalTabIndexAfterRemoval(
            index, self.activeSessionIndex,
            self.sessionTerminals.count - 1);
    [terminal destroy];
    [self.sessionTerminals removeObjectAtIndex:index];
    self.activeSessionIndex = newActiveIndex;

    if (self.sessionTerminals.count == 0) {
        self.terminal = nil;
        [self reloadTerminalTabs];
        [self startNewSession];
    } else if (removedActiveSession || self.terminal == terminal) {
        [self activateSessionAtIndex:newActiveIndex];
    } else {
        [self reloadTerminalTabs];
    }
}

- (void)restartCurrentTerminalTab:(id)sender {
    if (self.activeSessionIndex == NSNotFound ||
            self.activeSessionIndex >= self.sessionTerminals.count)
        return;
    Terminal *oldTerminal =
            self.sessionTerminals[self.activeSessionIndex];
    if (!oldTerminal.isSessionExited)
        return;

    int err = 0;
    Terminal *newTerminal = [self createSessionWithError:&err];
    if (newTerminal == nil) {
        [self showMessage:@"could not restart session"
                 subtitle:[NSString stringWithFormat:@"error code %d", err]];
        return;
    }

    [oldTerminal destroy];
    self.sessionTerminals[self.activeSessionIndex] = newTerminal;
    [self activateSessionAtIndex:self.activeSessionIndex];
}

- (void)cycleTerminalTab:(id)sender {
    NSUInteger next = ISHCycledTerminalTabIndex(
            self.activeSessionIndex, self.sessionTerminals.count, NO);
    if (next != NSNotFound)
        [self activateSessionAtIndex:next];
}

- (void)cycleTerminalTabBackward:(id)sender {
    NSUInteger next = ISHCycledTerminalTabIndex(
            self.activeSessionIndex, self.sessionTerminals.count, YES);
    if (next != NSNotFound)
        [self activateSessionAtIndex:next];
}

- (void)closeCurrentTerminalTab:(id)sender {
    if (self.activeSessionIndex == NSNotFound ||
            self.activeSessionIndex >= self.sessionTerminals.count)
        return;
    UIButton *close = [UIButton new];
    close.tag = self.activeSessionIndex;
    [self requestCloseTerminalTab:close];
}

- (void)showMessage:(NSString *)message subtitle:(NSString *)subtitle {
    dispatch_async(dispatch_get_main_queue(), ^{
        UIAlertController *alert = [UIAlertController alertControllerWithTitle:message message:subtitle preferredStyle:UIAlertControllerStyleAlert];
        [alert addAction:[UIAlertAction actionWithTitle:@"k"
                                                  style:UIAlertActionStyleDefault
                                                handler:nil]];
        [self presentViewController:alert animated:YES completion:nil];
    });
}

- (void)observeValueForKeyPath:(NSString *)keyPath ofObject:(id)object change:(NSDictionary *)change context:(void *)context {
    if (object == [UserPreferences shared]) {
        [self _updateStyleFromPreferences:YES];
    } else {
        [super observeValueForKeyPath:keyPath ofObject:object change:change context:context];
    }
}

- (void)_updateStyleFromPreferences:(BOOL)animated {
    NSAssert(NSThread.isMainThread, @"This method needs to be called on the main thread");
    NSTimeInterval duration = animated ? 0.1 : 0;
    [UIView animateWithDuration:duration animations:^{
        self.view.backgroundColor = [[UIColor alloc] ish_initWithHexString:UserPreferences.shared.palette.backgroundColor];
        UIKeyboardAppearance keyAppearance = UserPreferences.shared.keyboardAppearance;
        self.termView.keyboardAppearance = keyAppearance;
        for (BarButton *button in self.barButtons) {
            button.keyAppearance = keyAppearance;
        }
        UIColor *tintColor = keyAppearance == UIKeyboardAppearanceLight ? UIColor.blackColor : UIColor.whiteColor;
        for (UIControl *control in self.barControls) {
            control.tintColor = tintColor;
        }
        [self updateTerminalTabsStyle];
    }];
    UIView *oldBarView = self.termView.inputAccessoryView;
    if (UserPreferences.shared.hideExtraKeysWithExternalKeyboard && self.hasExternalKeyboard) {
        self.termView.inputAccessoryView = nil;
    } else {
        self.termView.inputAccessoryView = self.barView;
    }
    if (self.termView.inputAccessoryView != oldBarView && self.termView.isFirstResponder) {
        dispatch_async(dispatch_get_main_queue(), ^{
            self.ignoreKeyboardMotion = YES; // avoid infinite recursion
            [self.termView reloadInputViews];
            self.ignoreKeyboardMotion = NO;
        });
    }
}
- (void)_updateStyleAnimated {
    [self _updateStyleFromPreferences:YES];
}

- (void)_updateBadge {
    self.settingsBadge.hidden = !FsNeedsRepositoryUpdate();
}

- (UIStatusBarStyle)preferredStatusBarStyle {
    return UserPreferences.shared.statusBarStyle;
}

- (BOOL)prefersStatusBarHidden {
    return UserPreferences.shared.hideStatusBar;
}

- (void)keyboardDidSomething:(NSNotification *)notification {
    if (self.ignoreKeyboardMotion)
        return;

    CGRect screenKeyboardFrame = [notification.userInfo[UIKeyboardFrameEndUserInfoKey] CGRectValue];
    UIScreen *screen = UIScreen.mainScreen;
    // notification.object is nil before iOS 16.1 and the correct UIScreen after iOS 16.1
    if (notification.object != nil)
        screen = notification.object;
    CGRect keyboardFrame = [self.view convertRect:screenKeyboardFrame fromCoordinateSpace:screen.coordinateSpace];
    if (CGRectEqualToRect(keyboardFrame, CGRectZero))
        return;
    CGRect intersection = CGRectIntersection(keyboardFrame, self.view.bounds);
    keyboardFrame = intersection;
    NSLog(@"%@ %@", notification.name, @(keyboardFrame));
    self.hasExternalKeyboard = keyboardFrame.size.height < 100;
    CGFloat pad = CGRectGetMaxY(self.view.bounds) - CGRectGetMinY(keyboardFrame);
    // The keyboard appears to be undocked. This means it can either be split or
    // truly floating. In the former case we want to keep the pad, but in the
    // latter we should fall back to the input accessory view instead of the
    // keyboard.
    if (pad != keyboardFrame.size.height && keyboardFrame.size.width != UIScreen.mainScreen.bounds.size.width) {
        pad = MAX(self.view.safeAreaInsets.bottom, self.termView.inputAccessoryView.frame.size.height);
    }
    // NSLog(@"pad %f", pad);
    self.bottomConstraint.constant = pad;

    BOOL initialLayout = self.termView.needsUpdateConstraints;
    [self.view setNeedsUpdateConstraints];
    if (!initialLayout) {
        // if initial layout hasn't happened yet, the terminal view is going to be at a really weird place, so animating it is going to look really bad
        NSNumber *interval = notification.userInfo[UIKeyboardAnimationDurationUserInfoKey];
        NSNumber *curve = notification.userInfo[UIKeyboardAnimationCurveUserInfoKey];
        [UIView animateWithDuration:interval.doubleValue
                              delay:0
                            options:curve.integerValue << 16
                         animations:^{
                             [self.view layoutIfNeeded];
                         }
                         completion:nil];
    }
}

- (void)setHasExternalKeyboard:(BOOL)hasExternalKeyboard {
    _hasExternalKeyboard = hasExternalKeyboard;
    [self _updateStyleFromPreferences:YES];
}

- (void)prepareForSegue:(UIStoryboardSegue *)segue sender:(id)sender {
    if ([segue.identifier isEqualToString:@"embed"]) {
        // You might want to check if this is your embed segue here
        // in case there are other segues triggered from this view controller.
        segue.destinationViewController.view.translatesAutoresizingMaskIntoConstraints = NO;
    }
}

- (void)traitCollectionDidChange:(UITraitCollection *)previousTraitCollection {
    // Hack to resolve a layering mismatch between the UI and preferences.
    if (@available(iOS 12.0, *)) {
        if (previousTraitCollection.userInterfaceStyle != self.traitCollection.userInterfaceStyle) {
            // Ensure that the relevant things listening for this will update.
            UserPreferences.shared.colorScheme = UserPreferences.shared.colorScheme;
        }
    }
}

#pragma mark Bar

- (IBAction)showAbout:(id)sender {
    UINavigationController *navigationController = [[UIStoryboard storyboardWithName:@"About" bundle:nil] instantiateInitialViewController];
    if ([sender isKindOfClass:[UIGestureRecognizer class]]) {
        UIGestureRecognizer *recognizer = sender;
        if (recognizer.state == UIGestureRecognizerStateBegan) {
            AboutViewController *aboutViewController = (AboutViewController *) navigationController.topViewController;
            aboutViewController.includeDebugPanel = YES;
        } else {
            return;
        }
    }
    [self presentViewController:navigationController animated:YES completion:nil];
    [self.termView resignFirstResponder];
}

- (void)resizeBar {
    CGSize bar = self.barView.bounds.size;
    // set sizing parameters on bar
    // numbers stolen from iVim and modified somewhat
    if (UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPhone) {
        // phone
        [self setBarHorizontalPadding:6 verticalPadding:6 buttonWidth:32];
    } else if (bar.width >= 450) {
        // wide ipad
        [self setBarHorizontalPadding:15 verticalPadding:8 buttonWidth:43];
    } else {
        // narrow ipad (slide over)
        [self setBarHorizontalPadding:10 verticalPadding:8 buttonWidth:36];
    }
    [UIView performWithoutAnimation:^{
        [self.barView layoutIfNeeded];
    }];
}

- (void)setBarHorizontalPadding:(CGFloat)horizontal verticalPadding:(CGFloat)vertical buttonWidth:(CGFloat)buttonWidth {
    self.barLeading.constant = self.barTrailing.constant = horizontal;
    self.barTop.constant = self.barBottom.constant = vertical;
    self.barButtonWidth.constant = buttonWidth;
}

- (IBAction)pressEscape:(id)sender {
    [self pressKey:@"\x1b"];
}
- (IBAction)pressTab:(id)sender {
    [self pressKey:@"\t"];
}
- (void)pressKey:(NSString *)key {
    [self.termView insertText:key];
}

- (IBAction)pressControl:(id)sender {
    self.controlKey.selected = !self.controlKey.selected;
}
    
- (IBAction)pressArrow:(ArrowBarButton *)sender {
    switch (sender.direction) {
        case ArrowUp: [self pressKey:[self.terminal arrow:'A']]; break;
        case ArrowDown: [self pressKey:[self.terminal arrow:'B']]; break;
        case ArrowLeft: [self pressKey:[self.terminal arrow:'D']]; break;
        case ArrowRight: [self pressKey:[self.terminal arrow:'C']]; break;
        case ArrowNone: break;
    }
}

- (void)switchTerminal:(UIKeyCommand *)sender {
    unsigned i = (unsigned) sender.input.integerValue;
    if (i == 7) {
        if (self.activeSessionIndex != NSNotFound &&
                self.activeSessionIndex < self.sessionTerminals.count)
            self.terminal = self.sessionTerminals[self.activeSessionIndex];
    } else {
        if (self.termView.markedTextRange != nil)
            [self.termView unmarkText];
        self.terminal = [Terminal terminalWithType:TTY_CONSOLE_MAJOR number:i];
    }
}

- (void)increaseFontSize:(UIKeyCommand *)command {
    self.termView.overrideFontSize = self.termView.effectiveFontSize + 1;
}
- (void)decreaseFontSize:(UIKeyCommand *)command {
    self.termView.overrideFontSize = self.termView.effectiveFontSize - 1;
}
- (void)resetFontSize:(UIKeyCommand *)command {
    self.termView.overrideFontSize = 0;
}

- (NSArray<UIKeyCommand *> *)keyCommands {
    static NSMutableArray<UIKeyCommand *> *commands = nil;
    if (commands == nil) {
        commands = [NSMutableArray new];
        for (unsigned i = 1; i <= 7; i++) {
            [commands addObject:
             [UIKeyCommand keyCommandWithInput:[NSString stringWithFormat:@"%d", i]
                                 modifierFlags:UIKeyModifierCommand|UIKeyModifierAlternate|UIKeyModifierShift
                                        action:@selector(switchTerminal:)]];
        }
        NSArray<UIKeyCommand *> *tabCommands = @[
            [UIKeyCommand keyCommandWithInput:@"t"
                                modifierFlags:UIKeyModifierCommand
                                       action:@selector(newTerminalTab:)
                         discoverabilityTitle:@"New Shell"],
            [UIKeyCommand keyCommandWithInput:@"w"
                                modifierFlags:UIKeyModifierCommand
                                       action:@selector(closeCurrentTerminalTab:)
                         discoverabilityTitle:@"Close Shell"],
            [UIKeyCommand keyCommandWithInput:@"\t"
                                modifierFlags:UIKeyModifierControl
                                       action:@selector(cycleTerminalTab:)
                         discoverabilityTitle:@"Next Shell"],
            [UIKeyCommand keyCommandWithInput:@"\t"
                                modifierFlags:
                                        UIKeyModifierControl |
                                        UIKeyModifierShift
                                       action:@selector(
                                               cycleTerminalTabBackward:)
                         discoverabilityTitle:@"Previous Shell"],
        ];
        if (@available(iOS 15.0, *)) {
            for (UIKeyCommand *command in tabCommands)
                command.wantsPriorityOverSystemBehavior = YES;
        }
        [commands addObjectsFromArray:tabCommands];
        [commands addObject:
         [UIKeyCommand keyCommandWithInput:@"+"
                             modifierFlags:UIKeyModifierCommand
                                    action:@selector(increaseFontSize:)
                      discoverabilityTitle:@"Increase Font Size"]];
        [commands addObject:
         [UIKeyCommand keyCommandWithInput:@"="
                             modifierFlags:UIKeyModifierCommand
                                    action:@selector(increaseFontSize:)]];
        [commands addObject:
         [UIKeyCommand keyCommandWithInput:@"-"
                             modifierFlags:UIKeyModifierCommand
                                    action:@selector(decreaseFontSize:)
                      discoverabilityTitle:@"Decrease Font Size"]];
        [commands addObject:
         [UIKeyCommand keyCommandWithInput:@"0"
                             modifierFlags:UIKeyModifierCommand
                                    action:@selector(resetFontSize:)
                      discoverabilityTitle:@"Reset Font Size"]];
        [commands addObject:
         [UIKeyCommand keyCommandWithInput:@","
                             modifierFlags:UIKeyModifierCommand
                                    action:@selector(showAbout:)
                      discoverabilityTitle:@"Settings"]];
    }
    return commands;
}

- (void)setTerminal:(Terminal *)terminal {
    _terminal = terminal;
    self.termView.terminal = self.terminal;
    [self updateTerminalTabsStyle];
}

@end

@interface BarView : UIInputView
@property (weak) IBOutlet TerminalViewController *terminalViewController;
@property (nonatomic) IBInspectable BOOL allowsSelfSizing;
@end
@implementation BarView
@dynamic allowsSelfSizing;

- (void)layoutSubviews {
    [self.terminalViewController resizeBar];
}

@end
