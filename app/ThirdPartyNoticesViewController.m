#import "ThirdPartyNoticesViewController.h"

@implementation ThirdPartyNoticesViewController

- (void)viewDidLoad {
    [super viewDidLoad];

    self.title = @"Licenses and Source";

    UITextView *textView = [[UITextView alloc] initWithFrame:CGRectZero];
    textView.translatesAutoresizingMaskIntoConstraints = NO;
    textView.editable = NO;
    textView.selectable = YES;
    textView.alwaysBounceVertical = YES;
    textView.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    textView.adjustsFontForContentSizeCategory = YES;
    textView.textContainerInset = UIEdgeInsetsMake(16, 12, 16, 12);
    textView.accessibilityIdentifier = @"third-party-notices-text";

    if (@available(iOS 13.0, *)) {
        self.view.backgroundColor = UIColor.systemBackgroundColor;
        textView.backgroundColor = UIColor.systemBackgroundColor;
        textView.textColor = UIColor.labelColor;
    } else {
        self.view.backgroundColor = UIColor.whiteColor;
        textView.backgroundColor = UIColor.whiteColor;
        textView.textColor = UIColor.blackColor;
    }

    // 固定资源顺序与产品组成无关；每个产品只展示自身实际打包的正文。
    NSArray<NSString *> *resourceNames = @[
        @"PROJECT-LICENSES",
        @"THIRD-PARTY-NOTICES",
        @"APPLE-HOST-NOTICES",
    ];
    NSMutableArray<NSString *> *sections = [NSMutableArray array];
    for (NSString *resourceName in resourceNames) {
        NSURL *noticesURL =
                [NSBundle.mainBundle URLForResource:resourceName
                                      withExtension:@"txt"];
        if (noticesURL == nil)
            continue;
        NSString *notices = [NSString stringWithContentsOfURL:noticesURL
                                                     encoding:NSUTF8StringEncoding
                                                        error:nil];
        if (notices == nil) {
            [sections addObject:
                    [NSString stringWithFormat:
                            @"无法加载 %@.txt："
                             "文件无法读取或不是有效的 UTF-8 文本。",
                            resourceName]];
        } else {
            [sections addObject:notices];
        }
    }
    textView.text = sections.count == 0
            ? @"无法加载许可与源码信息：应用包中没有可用的许可资源。"
            : [sections componentsJoinedByString:@"\n\n"];

    [self.view addSubview:textView];
    UILayoutGuide *safeArea = self.view.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [textView.topAnchor constraintEqualToAnchor:safeArea.topAnchor],
        [textView.leadingAnchor constraintEqualToAnchor:safeArea.leadingAnchor],
        [textView.trailingAnchor constraintEqualToAnchor:safeArea.trailingAnchor],
        [textView.bottomAnchor constraintEqualToAnchor:safeArea.bottomAnchor],
    ]];
}

@end
