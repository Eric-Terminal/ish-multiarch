//
//  Roots.m
//  iSH
//
//  Created by Theodore Dubois on 6/7/20.
//

#import <FileProvider/FileProvider.h>
#import "Roots.h"
#import "AppGroup.h"
#import "NSObject+SaneKVO.h"
#include <errno.h>
#include <stdlib.h>
#include <sys/stat.h>
#if !ISH_LINUX
#include "platform/apple-rootfs-seed.h"
#endif
#include "tools/fakefs.h"

static NSURL *RootsDir(void) {
    static NSURL *rootsDir;
    static dispatch_once_t token;
    dispatch_once(&token, ^{
        rootsDir = [ContainerURL() URLByAppendingPathComponent:@"roots"];
        NSFileManager *manager = [NSFileManager defaultManager];
        [manager createDirectoryAtURL:rootsDir
          withIntermediateDirectories:YES
                           attributes:@{}
                                error:nil];
    });
    return rootsDir;
}

static NSString *const kDefaultRoot = @"Default Root";
static NSString *const kAArch64RootBase = @"aarch64";

static BOOL RootNameIsAArch64Candidate(NSString *name) {
    if ([name isEqualToString:kAArch64RootBase])
        return YES;

    NSString *prefix = [kAArch64RootBase stringByAppendingString:@"-"];
    if (![name hasPrefix:prefix])
        return NO;
    NSString *suffix = [name substringFromIndex:prefix.length];
    if (suffix.length == 0 || [suffix hasPrefix:@"0"])
        return NO;
    NSCharacterSet *nonDecimal = NSCharacterSet.decimalDigitCharacterSet.invertedSet;
    if ([suffix rangeOfCharacterFromSet:nonDecimal].location != NSNotFound)
        return NO;

    errno = 0;
    char *end = NULL;
    unsigned long index = strtoul(suffix.UTF8String, &end, 10);
    return errno == 0 && end != NULL && *end == '\0' && index >= 2;
}

static BOOL RootNameIsInstallerStaging(NSString *name) {
    if (![name hasPrefix:@"."])
        return NO;
    NSRange marker = [name rangeOfString:@".installing."
                                 options:NSBackwardsSearch];
    if (marker.location == NSNotFound || marker.location <= 1)
        return NO;
    NSString *rootName = [name substringWithRange:NSMakeRange(
            1, marker.location - 1)];
    if (!RootNameIsAArch64Candidate(rootName))
        return NO;
    NSString *token = [name substringFromIndex:NSMaxRange(marker)];
    if (token.length != 32)
        return NO;
    NSCharacterSet *nonHexadecimal = [[NSCharacterSet
            characterSetWithCharactersInString:@"0123456789abcdef"] invertedSet];
    return [token rangeOfCharacterFromSet:nonHexadecimal].location == NSNotFound;
}

static BOOL RootNameHasInstallerSuffix(NSString *name, NSString *suffix) {
    if (![name hasPrefix:@"."] || ![name hasSuffix:suffix] ||
            name.length <= suffix.length + 1)
        return NO;
    NSString *rootName = [name substringWithRange:NSMakeRange(
            1, name.length - suffix.length - 1)];
    return RootNameIsAArch64Candidate(rootName);
}

static BOOL RootNameIsInstallerPrivate(NSString *name) {
    return RootNameIsInstallerStaging(name) ||
            RootNameHasInstallerSuffix(name, @".install.lock") ||
            RootNameHasInstallerSuffix(name, @".installing.owner");
}

static int RootURLHasInstallationReceipt(NSURL *rootURL, BOOL *hasReceipt) {
    *hasReceipt = NO;
    struct stat rootMetadata;
    if (lstat(rootURL.fileSystemRepresentation, &rootMetadata) < 0)
        return errno == ENOENT ? 0 : errno;
    if (!S_ISDIR(rootMetadata.st_mode))
        return 0;

    NSURL *receipt = [rootURL URLByAppendingPathComponent:@"rootfs-installation.txt"];
    struct stat receiptMetadata;
    if (lstat(receipt.fileSystemRepresentation, &receiptMetadata) < 0)
        return errno == ENOENT ? 0 : errno;
    if (!S_ISREG(receiptMetadata.st_mode))
        return EINVAL;
    *hasReceipt = YES;
    return 0;
}

static BOOL RootURLIsUsable(NSURL *rootURL) {
    NSString *name = rootURL.lastPathComponent;
    if (RootNameIsInstallerPrivate(name))
        return NO;

    NSDictionary<NSURLResourceKey, id> *rootValues = [rootURL
            resourceValuesForKeys:@[NSURLIsDirectoryKey, NSURLIsSymbolicLinkKey]
            error:nil];
    NSDictionary<NSURLResourceKey, id> *databaseValues = [[rootURL
            URLByAppendingPathComponent:@"meta.db"]
            resourceValuesForKeys:@[NSURLIsRegularFileKey, NSURLIsSymbolicLinkKey]
            error:nil];
    NSDictionary<NSURLResourceKey, id> *dataValues = [[rootURL
            URLByAppendingPathComponent:@"data"]
            resourceValuesForKeys:@[NSURLIsDirectoryKey, NSURLIsSymbolicLinkKey]
            error:nil];
    return [rootValues[NSURLIsDirectoryKey] boolValue] &&
            ![rootValues[NSURLIsSymbolicLinkKey] boolValue] &&
            [databaseValues[NSURLIsRegularFileKey] boolValue] &&
            ![databaseValues[NSURLIsSymbolicLinkKey] boolValue] &&
            [dataValues[NSURLIsDirectoryKey] boolValue] &&
            ![dataValues[NSURLIsSymbolicLinkKey] boolValue];
}

@interface Roots ()
@property NSMutableOrderedSet<NSString *> *roots;
@property BOOL updatingDomains;
@property BOOL domainsNeedUpdate;
@property BOOL wantsVersionFile;
@property int startupError;
@end

@implementation Roots

- (instancetype)init {
    if (self = [super init]) {
        NSMutableOrderedSet<NSString *> *rootNames = [NSMutableOrderedSet orderedSet];
        self.roots = rootNames;

        NSFileManager *manager = NSFileManager.defaultManager;
        NSError *error = nil;
        NSArray<NSURL *> *rootURLs = [manager
                contentsOfDirectoryAtURL:RootsDir()
                includingPropertiesForKeys:nil
                options:0
                error:&error];
        if (rootURLs == nil) {
            self.startupError = EIO;
            NSLog(@"无法列出 rootfs：%@", error);
            return self;
        }

        NSString *managedAArch64Root = nil;
#if !ISH_LINUX
        NSURL *seed = [NSBundle.mainBundle URLForResource:@"AArch64Rootfs" withExtension:@"seed"];
        if (seed != nil) {
            NSString *selectedRoot = self.defaultRoot;
            rootURLs = [rootURLs sortedArrayUsingComparator:
                    ^NSComparisonResult(NSURL *first, NSURL *second) {
                BOOL firstIsSelected = [first.lastPathComponent isEqualToString:selectedRoot];
                BOOL secondIsSelected = [second.lastPathComponent isEqualToString:selectedRoot];
                if (firstIsSelected != secondIsSelected)
                    return firstIsSelected ? NSOrderedAscending : NSOrderedDescending;
                return [first.lastPathComponent compare:second.lastPathComponent];
            }];

            // 先复用任意既有候选，避免较低名称释放后重复复制 rootfs。
            int managedCandidateError = 0;
            for (NSURL *rootURL in rootURLs) {
                NSString *candidate = rootURL.lastPathComponent;
                if (!RootNameIsAArch64Candidate(candidate))
                    continue;
                BOOL hasReceipt;
                int receiptError = RootURLHasInstallationReceipt(rootURL, &hasReceipt);
                if (receiptError != 0) {
                    self.startupError = receiptError;
                    NSLog(@"无法检查 AArch64 rootfs 安装收据：%d", receiptError);
                    return self;
                }
                if (!hasReceipt)
                    continue;

                enum ish_apple_rootfs_seed_result result;
                int installError = ish_apple_rootfs_seed_install(
                        seed.fileSystemRepresentation,
                        RootsDir().fileSystemRepresentation,
                        candidate.UTF8String,
                        &result);
                if (installError == 0) {
                    managedAArch64Root = candidate;
                    break;
                }
                if (managedCandidateError == 0)
                    managedCandidateError = installError;
            }
            if (managedAArch64Root == nil && managedCandidateError != 0) {
                self.startupError = managedCandidateError;
                NSLog(@"AArch64 rootfs 复用失败：%d", managedCandidateError);
                return self;
            }

            // 没有可复用安装时，才从最低的未占用候选开始安装。
            for (NSUInteger index = 0;
                    managedAArch64Root == nil && index <= rootURLs.count;
                    index++) {
                NSString *candidate = index == 0 ? kAArch64RootBase :
                        [NSString stringWithFormat:@"%@-%lu", kAArch64RootBase,
                                                   (unsigned long) index + 1];
                NSURL *candidateURL = [RootsDir() URLByAppendingPathComponent:candidate];
                struct stat candidateMetadata;
                BOOL candidateExists = lstat(
                        candidateURL.fileSystemRepresentation, &candidateMetadata) == 0;
                if (!candidateExists && errno != ENOENT) {
                    self.startupError = errno;
                    NSLog(@"无法检查 AArch64 rootfs 候选名称：%d", errno);
                    return self;
                }
                if (candidateExists)
                    continue;

                enum ish_apple_rootfs_seed_result result;
                int installError = ish_apple_rootfs_seed_install(
                        seed.fileSystemRepresentation,
                        RootsDir().fileSystemRepresentation,
                        candidate.UTF8String,
                        &result);
                if (installError == 0) {
                    managedAArch64Root = candidate;
                    if (result == ISH_APPLE_ROOTFS_SEED_INSTALLED)
                        _wantsVersionFile = YES;
                    break;
                }
                // EEXIST 明确表示该名称不是可复用的托管 root，原数据保持不动。
                if (installError == EEXIST)
                    continue;
                self.startupError = installError;
                NSLog(@"AArch64 rootfs 安装失败：%d", installError);
                return self;
            }
            if (managedAArch64Root == nil) {
                self.startupError = EEXIST;
                NSLog(@"无法为 AArch64 rootfs 找到不冲突的名称");
                return self;
            }

            error = nil;
            rootURLs = [manager
                    contentsOfDirectoryAtURL:RootsDir()
                    includingPropertiesForKeys:nil
                    options:0
                    error:&error];
            if (rootURLs == nil) {
                self.startupError = EIO;
                NSLog(@"安装后无法列出 rootfs：%@", error);
                return self;
            }
        }
#endif

        for (NSURL *rootURL in rootURLs) {
            if (RootURLIsUsable(rootURL))
                [rootNames addObject:rootURL.lastPathComponent];
        }
        if (managedAArch64Root != nil && ![rootNames containsObject:managedAArch64Root]) {
            self.startupError = EIO;
            NSLog(@"安装后的 AArch64 rootfs 结构无效");
            return self;
        }
        if (!self.roots.count) {
            // import default root
            NSURL *archive = [NSBundle.mainBundle URLForResource:@"root" withExtension:@"tar.gz"];
            if (archive == nil) {
                self.startupError = ENOENT;
                NSLog(@"App 中既没有 AArch64 seed，也没有默认 rootfs 归档");
                return self;
            }
            NSError *importError;
            if (![self importRootFromArchive:archive
                                        name:@"default"
                                       error:&importError
                            progressReporter:nil]) {
                self.startupError = [importError.domain isEqualToString:NSPOSIXErrorDomain] ?
                        (int) importError.code : EIO;
                NSLog(@"默认 rootfs 导入失败：%@", importError);
                return self;
            }
            _wantsVersionFile = YES;
        }

        NSString *selectedRoot = self.defaultRoot;
        BOOL selectionIsValid = selectedRoot != nil && [self.roots containsObject:selectedRoot];
        if (!selectionIsValid && managedAArch64Root != nil) {
            self.defaultRoot = managedAArch64Root;
        } else if (!selectionIsValid && self.roots.count) {
            self.defaultRoot = self.roots.firstObject;
        }

        [self observe:@[@"roots"] options:0 owner:self usingBlock:^(typeof(self) self) {
            if (self.defaultRoot == nil && self.roots.count)
                self.defaultRoot = self.roots[0];
            [self syncFileProviderDomains];
        }];
        [self syncFileProviderDomains];
    }
    return self;
}

- (NSString *)defaultRoot {
    return [NSUserDefaults.standardUserDefaults stringForKey:kDefaultRoot];
}
- (void)setDefaultRoot:(NSString *)defaultRoot {
    [NSUserDefaults.standardUserDefaults setObject:defaultRoot forKey:kDefaultRoot];
}

- (NSURL *)rootUrl:(NSString *)name {
    return [RootsDir() URLByAppendingPathComponent:name];
}

- (void)syncFileProviderDomains {
    if (self.updatingDomains) {
        self.domainsNeedUpdate = YES;
        return;
    }
    self.updatingDomains = YES;
    self.domainsNeedUpdate = NO;

    [NSFileProviderManager getDomainsWithCompletionHandler:^(NSArray<NSFileProviderDomain *> *domains, NSError *error) {
        void (^onError)(NSError *error) = ^(NSError *error) {
            if (error != nil)
                NSLog(@"error adjusting domains: %@", error);
        };
        onError(error);
        NSMutableOrderedSet<NSString *> *missingRoots = [self.roots mutableCopy];
        for (NSFileProviderDomain *domain in domains) {
            if ([missingRoots containsObject:domain.identifier]) {
                [missingRoots removeObject:domain.identifier];
            } else {
                [NSFileManager.defaultManager removeItemAtURL:
                 [NSFileProviderManager.defaultManager.documentStorageURL
                  URLByAppendingPathComponent:domain.pathRelativeToDocumentStorage]
                                                        error:nil];
                [NSFileProviderManager removeDomain:domain completionHandler:onError];
            }
        }
        for (NSString *rootId in missingRoots) {
            [NSFileProviderManager addDomain:[[NSFileProviderDomain alloc] initWithIdentifier:rootId
                                                                                  displayName:rootId
                                                                pathRelativeToDocumentStorage:rootId]
                           completionHandler:onError];
        }
        if (self.domainsNeedUpdate)
            [self syncFileProviderDomains];
        self.updatingDomains = NO;
    }];
}

- (BOOL)accessInstanceVariablesDirectly {
    return YES;
}

void root_progress_callback(void *cookie, double progress, const char *message, bool *should_cancel) {
    id <ProgressReporter> reporter = (__bridge id<ProgressReporter>) cookie;
    [reporter updateProgress:progress message:[NSString stringWithUTF8String:message]];
    if ([reporter shouldCancel])
        *should_cancel = true;
}

- (BOOL)importRootFromArchive:(NSURL *)archive name:(NSString *)name error:(NSError **)error progressReporter:(id<ProgressReporter> _Nullable)progress {
    NSAssert(![self.roots containsObject:name], @"root already exists: %@", name);
    if (RootNameIsInstallerPrivate(name)) {
        *error = [NSError errorWithDomain:@"iSH" code:0 userInfo:@{
                NSLocalizedDescriptionKey: @"该文件系统名称由安装器保留"}];
        return NO;
    }
    struct fakefsify_error fs_err;
    NSURL *destination = [self rootUrl:name];
    NSURL *tempDestination = [NSFileManager.defaultManager.temporaryDirectory
                              URLByAppendingPathComponent:[NSProcessInfo.processInfo globallyUniqueString]];
    if (tempDestination == nil)
        return NO;
    if (!fakefs_import(archive.fileSystemRepresentation,
                       tempDestination.fileSystemRepresentation,
                       &fs_err, (struct progress) {(__bridge void *) progress, root_progress_callback})) {
        NSString *domain = NSPOSIXErrorDomain;
        if (fs_err.type == ERR_SQLITE)
            domain = @"SQLite";
        *error = [NSError errorWithDomain:domain
                                     code:fs_err.code
                                 userInfo:@{NSLocalizedDescriptionKey:
                                                [NSString stringWithFormat:@"%s, line %d", fs_err.message, fs_err.line]}];
        if (fs_err.type == ERR_CANCELLED)
            *error = nil;
        free(fs_err.message);
        [NSFileManager.defaultManager removeItemAtURL:tempDestination error:nil];
        return NO;
    }
    if (![NSFileManager.defaultManager moveItemAtURL:tempDestination toURL:destination error:error])
        return NO;

    void (^addRoot)(void) = ^{
        [[self mutableOrderedSetValueForKey:@"roots"] addObject:name];
    };
    if (!NSThread.isMainThread)
        dispatch_sync(dispatch_get_main_queue(), addRoot);
    else
        addRoot();
    return YES;
}

- (BOOL)exportRootNamed:(NSString *)name toArchive:(NSURL *)archive error:(NSError **)error progressReporter:(id<ProgressReporter> _Nullable)progress {
    NSAssert([self.roots containsObject:name], @"trying to export a root that doesn't exist: %@", name);
    struct fakefsify_error fs_err;
    if (!fakefs_export([self rootUrl:name].fileSystemRepresentation,
                       archive.fileSystemRepresentation,
                       &fs_err, (struct progress) {(__bridge void *) progress, root_progress_callback})) {
        // TODO: dedup with above method
        NSString *domain = NSPOSIXErrorDomain;
        if (fs_err.type == ERR_SQLITE)
            domain = @"SQLite";
        *error = [NSError errorWithDomain:domain
                                     code:fs_err.code
                                 userInfo:@{NSLocalizedDescriptionKey: [NSString stringWithUTF8String:fs_err.message]}];
        if (fs_err.type == ERR_CANCELLED)
            *error = nil;
        free(fs_err.message);
        return NO;
    }
    return YES;
}

- (BOOL)destroyRootNamed:(NSString *)name error:(NSError **)error {
    if ([name isEqualToString:self.defaultRoot]) {
        *error = [NSError errorWithDomain:@"iSH" code:0 userInfo:@{NSLocalizedDescriptionKey: @"Cannot delete the default filesystem"}];
        return NO;
    }
    NSAssert([self.roots containsObject:name], @"root does not exist: %@", name);
    if (![NSFileManager.defaultManager removeItemAtURL:[self rootUrl:name] error:error])
        return NO;
    [[self mutableOrderedSetValueForKey:@"roots"] removeObject:name];
    return YES;
}

- (BOOL)renameRoot:(NSString *)name toName:(NSString *)newName error:(NSError **)error {
    if (newName.length == 0) {
        *error = [NSError errorWithDomain:@"iSH" code:0 userInfo:@{NSLocalizedDescriptionKey: @"Filesystem name can't be empty"}];
        return NO;
    }
    if ([newName containsString:@"/"]) {
        *error = [NSError errorWithDomain:@"iSH" code:0 userInfo:@{NSLocalizedDescriptionKey: @"Filesystem name can't contain /"}];
        return NO;
    }
    if ([newName isEqualToString:@"."] || [newName isEqualToString:@".."]) {
        *error = [NSError errorWithDomain:@"iSH" code:0 userInfo:@{NSLocalizedDescriptionKey: @"Filesystem name can't be . or .."}];
        return NO;
    }
    if (RootNameIsInstallerPrivate(newName)) {
        *error = [NSError errorWithDomain:@"iSH" code:0 userInfo:@{
                NSLocalizedDescriptionKey: @"该文件系统名称由安装器保留"}];
        return NO;
    }
    if ([name isEqualToString:self.defaultRoot]) {
        *error = [NSError errorWithDomain:@"iSH" code:0 userInfo:@{NSLocalizedDescriptionKey: @"Cannot rename the default filesystem"}];
        return NO;
    }
    NSAssert([self.roots containsObject:name], @"root does not exist: %@", name);

    BOOL hasReceipt;
    int receiptError = RootURLHasInstallationReceipt([self rootUrl:name], &hasReceipt);
    if (receiptError != 0) {
        *error = [NSError errorWithDomain:NSPOSIXErrorDomain
                                     code:receiptError
                                 userInfo:nil];
        return NO;
    }
    if (hasReceipt) {
        *error = [NSError errorWithDomain:@"iSH" code:0 userInfo:@{
                NSLocalizedDescriptionKey: @"不能重命名由 App 管理的 AArch64 文件系统"}];
        return NO;
    }
    
    if (![NSFileManager.defaultManager moveItemAtURL:[self rootUrl:name] toURL:[self rootUrl:newName] error:error])
        return NO;
    NSUInteger index = [self.roots indexOfObject:name];
    [[self mutableOrderedSetValueForKey:@"roots"] replaceObjectAtIndex:index withObject:newName];
    return YES;
}

+ (instancetype)instance {
    static Roots *instance;
    static dispatch_once_t token;
    dispatch_once(&token, ^{
        instance = [Roots new];
    });
    return instance;
}

@end
