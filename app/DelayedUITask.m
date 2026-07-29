//
//  DelayedUITask.m
//  iSH
//
//  Created by Theodore Dubois on 11/8/17.
//

#import "DelayedUITask.h"

@interface DelayedUITask ()

@property (weak) id target;
@property SEL action;
@property NSTimer *timer;

@end

@implementation DelayedUITask

- (instancetype)initWithTarget:(id)target action:(SEL)action {
    if (self = [super init]) {
        self.target = target;
        self.action = action;
    }
    return self;
}

- (void)schedule {
    if (!self.timer.valid) {
        self.timer = [NSTimer timerWithTimeInterval:1./60 repeats:NO block:^(NSTimer * _Nonnull timer) {
            self.timer = nil;
            id target = self.target;
            if (target != nil)
                ((void (*)(id, SEL)) [target methodForSelector:self.action])(
                        target, self.action);
        }];
        [NSRunLoop.mainRunLoop addTimer:self.timer forMode:NSDefaultRunLoopMode];
    }
}

- (void)invalidate {
    [self.timer invalidate];
    self.timer = nil;
}

@end
