#import "ThirdPartyNoticesViewController.h"

@implementation ThirdPartyNoticesViewController

- (void)viewDidLoad {
    [super viewDidLoad];

    self.title = @"Alpine AArch64 Notices";

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

    // 仅在用户打开本页时读取，避免让设置页和 App 启动路径承担正文布局开销。
    NSURL *noticesURL = [NSBundle.mainBundle URLForResource:@"THIRD-PARTY-NOTICES"
                                              withExtension:@"txt"];
    if (noticesURL == nil) {
        textView.text = @"无法加载 Alpine AArch64 声明："
                         "应用包中缺少 THIRD-PARTY-NOTICES.txt。";
    } else {
        NSString *notices = [NSString stringWithContentsOfURL:noticesURL
                                                     encoding:NSUTF8StringEncoding
                                                        error:nil];
        textView.text = notices ?: @"无法加载 Alpine AArch64 声明："
                                    "文件无法读取或不是有效的 UTF-8 文本。";
    }

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
