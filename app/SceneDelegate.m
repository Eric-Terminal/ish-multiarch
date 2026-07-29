//
//  SceneDelegate.m
//  iSH
//
//  Created by Theodore Dubois on 10/26/19.
//

#import "SceneDelegate.h"
#import "AboutViewController.h"
#import "AppDelegate.h"
#import "TerminalTabsState.h"

TerminalViewController *currentTerminalViewController = NULL;

static NSString *const TerminalUUID = @"TerminalUUID";
static NSString *const TerminalUUIDs = @"TerminalUUIDs";

@implementation SceneDelegate

- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)connectionOptions {
    if ([NSUserDefaults.standardUserDefaults boolForKey:@"recovery"]) {
        UINavigationController *vc = [[UIStoryboard storyboardWithName:@"About" bundle:nil] instantiateInitialViewController];
        AboutViewController *avc = (AboutViewController *) vc.topViewController;
        avc.recoveryMode = YES;
        self.window.rootViewController = vc;
        return;
    }
    if ([AppDelegate bootError] != 0)
        return;

    TerminalViewController *vc = (TerminalViewController *) self.window.rootViewController;
    NSDictionary *restoration = session.stateRestorationActivity.userInfo;
    NSArray<NSUUID *> *terminalUUIDs = ISHRestoreTerminalTabUUIDs(
            restoration[TerminalUUIDs], restoration[TerminalUUID]);
    NSUUID *activeUUID = ISHActiveTerminalTabUUID(
            restoration[TerminalUUID], terminalUUIDs);
    if (terminalUUIDs.count == 0) {
        [vc startNewSession];
    } else {
        [vc restoreSessionsFromTerminalUUIDs:terminalUUIDs
                          activeTerminalUUID:activeUUID];
    }
}

- (NSUserActivity *)stateRestorationActivityForScene:(UIScene *)scene {
    NSUserActivity *activity = [[NSUserActivity alloc] initWithActivityType:@"app.ish.scene"];
    TerminalViewController *vc = (TerminalViewController *) self.window.rootViewController;
    if ([vc isKindOfClass:TerminalViewController.class]) {
        NSMutableArray<NSString *> *terminalUUIDs = [NSMutableArray array];
        for (NSUUID *uuid in vc.sessionTerminalUUIDs)
            [terminalUUIDs addObject:uuid.UUIDString];
        NSString *activeUUID = vc.activeSessionTerminalUUID.UUIDString;
        if (terminalUUIDs.count != 0 && activeUUID != nil) {
            [activity addUserInfoEntriesFromDictionary:@{
                TerminalUUID: activeUUID,
                TerminalUUIDs: terminalUUIDs,
            }];
        }
    }
    return activity;
}

- (void)sceneDidBecomeActive:(UIScene *)scene {
    TerminalViewController *terminalViewController = (TerminalViewController *) self.window.rootViewController;;
    currentTerminalViewController = terminalViewController;
}

- (void)sceneWillResignActive:(UIScene *)scene {
    TerminalViewController *terminalViewController = (TerminalViewController *) self.window.rootViewController;

    if (currentTerminalViewController == terminalViewController) {
        currentTerminalViewController = NULL;
    }
}

@end
