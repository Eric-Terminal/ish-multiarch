#include "platform/apple-watch-root-archive.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "tools/fakefs.h"

static int import_calls;
static int export_calls;
static int cancel_callbacks;
static int sync_calls;
static int fail_sync_call;
static bool export_lock_was_exclusive;
static bool archive_was_mutated;
static bool export_missing_host;

static void require(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "失败：%s（errno=%d）\n", message, errno);
        exit(1);
    }
}

static void write_file(const char *path, const char *bytes) {
    int file = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    require(file >= 0, "创建测试文件");
    size_t length = strlen(bytes);
    require(write(file, bytes, length) == (ssize_t) length,
            "写入测试文件");
    require(close(file) == 0, "关闭测试文件");
}

static void write_repeated_file(
        const char *path, unsigned char byte, size_t length) {
    int file = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    require(file >= 0, "创建大归档测试文件");
    unsigned char bytes[64 * 1024];
    memset(bytes, byte, sizeof(bytes));
    while (length > 0) {
        size_t chunk = length < sizeof(bytes) ? length : sizeof(bytes);
        require(write(file, bytes, chunk) == (ssize_t) chunk,
                "写入大归档测试文件");
        length -= chunk;
    }
    require(close(file) == 0, "关闭大归档测试文件");
}

static void format_child(
        char output[PATH_MAX], const char *parent, const char *name) {
    int length = snprintf(output, PATH_MAX, "%s/%s", parent, name);
    require(length > 0 && length < PATH_MAX, "拼接测试路径");
}

bool fakefs_import(
        const char *archive_path,
        const char *fs,
        struct fakefsify_error *error,
        struct progress progress) {
    (void) error;
    bool cancelled = false;
    if (progress.callback != NULL)
        progress.callback(
                progress.cookie, 0.5, "import", &cancelled);
    if (cancelled) {
        error->type = ERR_CANCELLED;
        error->message = strdup("");
        return false;
    }
    struct stat metadata;
    if (lstat(archive_path, &metadata) < 0 ||
            !S_ISREG(metadata.st_mode))
        return false;
    import_calls++;
    if (mkdir(fs, 0700) < 0)
        return false;
    char path[PATH_MAX];
    format_child(path, fs, "data");
    if (mkdir(path, 0700) < 0)
        return false;
    format_child(path, fs, "meta.db");
    write_file(path, "database");
    return true;
}

bool fakefs_export(
        const char *fs,
        const char *archive_path,
        struct fakefsify_error *error,
        struct progress progress) {
    (void) fs;
    (void) error;
    bool cancelled = false;
    if (progress.callback != NULL)
        progress.callback(
                progress.cookie, 0.5, "export", &cancelled);
    if (cancelled) {
        error->type = ERR_CANCELLED;
        error->message = strdup("");
        return false;
    }
    export_calls++;
    write_file(archive_path, "exported");
    if (export_missing_host) {
        error->type = ERR_POSIX;
        error->code = ENOENT;
        error->message = strdup("missing host object");
        return false;
    }
    return true;
}

bool ish_apple_root_catalog_is_managed_name(const char *name) {
    return name != NULL &&
            (strcmp(name, "aarch64") == 0 ||
             strcmp(name, "aarch64-2") == 0);
}

int ish_apple_root_catalog_import_fakefs(
        const char *seed_root,
        const char *persistent_parent,
        const char *imported_root,
        char name[ISH_APPLE_ROOT_NAME_CAPACITY],
        struct progress progress) {
    (void) seed_root;
    (void) persistent_parent;
    bool cancelled = false;
    if (progress.callback != NULL)
        progress.callback(
                progress.cookie, 0.5, "publish", &cancelled);
    if (cancelled)
        return ECANCELED;
    struct stat data;
    char data_path[PATH_MAX];
    format_child(data_path, imported_root, "data");
    if (lstat(data_path, &data) < 0 || !S_ISDIR(data.st_mode))
        return EINVAL;
    snprintf(name, ISH_APPLE_ROOT_NAME_CAPACITY, "aarch64-2");
    return 0;
}

int ish_apple_rootfs_lock_managed_root(
        const char *persistent_parent,
        const char *root_name,
        bool exclusive,
        bool require_valid_root,
        int *lock_out) {
    (void) persistent_parent;
    (void) require_valid_root;
    if (!ish_apple_root_catalog_is_managed_name(root_name))
        return EINVAL;
    export_lock_was_exclusive = exclusive;
    *lock_out = open("/dev/null", O_RDONLY);
    return *lock_out < 0 ? errno : 0;
}

int ish_apple_rootfs_unlock_managed_root(int lock) {
    return close(lock) < 0 ? errno : 0;
}

int ish_apple_rootfs_sync_directory(int directory) {
    (void) directory;
    sync_calls++;
    if (fail_sync_call != 0 && sync_calls == fail_sync_call)
        return EIO;
    return 0;
}

static int remove_tree_at(int parent, const char *name) {
    struct stat metadata;
    if (fstatat(parent, name, &metadata, AT_SYMLINK_NOFOLLOW) < 0)
        return errno == ENOENT ? 0 : errno;
    if (!S_ISDIR(metadata.st_mode))
        return unlinkat(parent, name, 0) < 0 ? errno : 0;
    int child = openat(parent, name,
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (child < 0)
        return errno;
    DIR *iterator = fdopendir(dup(child));
    if (iterator == NULL) {
        close(child);
        return errno;
    }
    int error = 0;
    struct dirent *entry;
    while (error == 0 && (entry = readdir(iterator)) != NULL) {
        if (strcmp(entry->d_name, ".") != 0 &&
                strcmp(entry->d_name, "..") != 0)
            error = remove_tree_at(child, entry->d_name);
    }
    closedir(iterator);
    close(child);
    if (error == 0 && unlinkat(parent, name, AT_REMOVEDIR) < 0)
        error = errno;
    return error;
}

int ish_apple_rootfs_remove_entry_at(int parent, const char *name) {
    return remove_tree_at(parent, name);
}

static bool has_partial(const char *directory) {
    DIR *iterator = opendir(directory);
    require(iterator != NULL, "枚举测试目录");
    bool found = false;
    struct dirent *entry;
    while ((entry = readdir(iterator)) != NULL) {
        if (strstr(entry->d_name, ".watch-root-") ==
                entry->d_name) {
            found = true;
            break;
        }
    }
    closedir(iterator);
    return found;
}

static void cancel_progress(
        void *cookie,
        double fraction,
        const char *message,
        bool *cancel_out) {
    (void) cookie;
    (void) fraction;
    (void) message;
    cancel_callbacks++;
    *cancel_out = true;
}

static void cancel_publish_progress(
        void *cookie,
        double fraction,
        const char *message,
        bool *cancel_out) {
    (void) cookie;
    (void) message;
    if (fraction >= 0.96) {
        cancel_callbacks++;
        *cancel_out = true;
    }
}

static void mutate_archive_progress(
        void *cookie,
        double fraction,
        const char *message,
        bool *cancel_out) {
    (void) message;
    (void) cancel_out;
    if (archive_was_mutated || fraction <= 0 || fraction >= 0.10)
        return;
    const char *path = cookie;
    int file = open(path, O_WRONLY | O_CLOEXEC);
    require(file >= 0, "打开并发改写归档");
    unsigned char changed = 'B';
    require(pwrite(file, &changed, 1, 0) == 1,
            "并发原地改写归档");
    require(close(file) == 0, "关闭并发改写归档");
    archive_was_mutated = true;
}

int main(void) {
    require(ish_apple_watch_root_archive_is_supported_name("root.tar"),
            "应接受 tar");
    require(ish_apple_watch_root_archive_is_supported_name("root.tar.gz"),
            "应接受 tar.gz");
    require(ish_apple_watch_root_archive_is_supported_name("ROOT.TGZ"),
            "应接受大小写不敏感的 tgz");
    require(!ish_apple_watch_root_archive_is_supported_name("../root.tar"),
            "必须拒绝路径穿越");
    require(!ish_apple_watch_root_archive_is_supported_name("root.zip"),
            "必须拒绝其他扩展名");

    char temporary[] = "/tmp/ish-watch-root-archive.XXXXXX";
    char *base = mkdtemp(temporary);
    require(base != NULL, "创建临时目录");
    char parent[PATH_MAX];
    char shared[PATH_MAX];
    char seed[PATH_MAX];
    format_child(parent, base, "roots");
    format_child(shared, base, "shared");
    format_child(seed, base, "seed");
    require(mkdir(parent, 0700) == 0, "创建 roots");
    require(mkdir(shared, 0700) == 0, "创建 shared");
    require(mkdir(seed, 0700) == 0, "创建 seed");

    char stale_partial[PATH_MAX];
    char unrelated_partial[PATH_MAX];
    format_child(stale_partial, parent,
            ".watch-root-fakefs-0123456789abcdef0123456789abcdef.partial");
    format_child(unrelated_partial, parent,
            ".watch-root-fakefs-not-a-token.partial");
    require(mkdir(stale_partial, 0700) == 0, "创建崩溃遗留 partial");
    require(mkdir(unrelated_partial, 0700) == 0, "创建非协议私有目录");
    require(ish_apple_watch_root_archive_cleanup(parent) == 0 &&
            access(stale_partial, F_OK) < 0 && errno == ENOENT &&
            access(unrelated_partial, F_OK) == 0,
            "启动清扫只能删除严格匹配协议的归档 partial");
    require(rmdir(unrelated_partial) == 0, "清理非协议私有目录");

    char archive[PATH_MAX];
    format_child(archive, shared, "backup.tar.gz");
    write_file(archive, "archive");
    char root_name[ISH_APPLE_ROOT_NAME_CAPACITY];
    struct ish_apple_watch_root_archive_error archive_error;
    int error = ish_apple_watch_root_archive_import(
            seed, parent, shared, "backup.tar.gz",
            root_name, (struct progress) {0}, &archive_error);
    require(error == 0 && strcmp(root_name, "aarch64-2") == 0,
            "普通归档应恢复为新 root");
    require(import_calls == 1 && !has_partial(parent),
            "导入成功后必须清理全部私有 partial");

    error = ish_apple_watch_root_archive_import(
            seed, parent, shared, "backup.tar.gz",
            root_name,
            (struct progress) {
                .callback = cancel_publish_progress,
            },
            &archive_error);
    require(error == ECANCELED && import_calls == 2 &&
            !has_partial(parent),
            "发布复制阶段取消必须返回 ECANCELED 并清理私有 root");

    error = ish_apple_watch_root_archive_import(
            seed, parent, shared, "backup.tar.gz",
            root_name,
            (struct progress) {
                .callback = cancel_progress,
            },
            &archive_error);
    require(error == ECANCELED && cancel_callbacks > 0 &&
            import_calls == 2 && !has_partial(parent),
            "取消快照必须返回 ECANCELED 且不进入 fakefs");

    char symlink_path[PATH_MAX];
    format_child(symlink_path, shared, "link.tar");
    require(symlink("backup.tar.gz", symlink_path) == 0,
            "创建归档符号链接");
    error = ish_apple_watch_root_archive_import(
            seed, parent, shared, "link.tar",
            root_name, (struct progress) {0}, &archive_error);
    require(error != 0 && import_calls == 2 && !has_partial(parent),
            "符号链接归档必须在 fakefs 前拒绝并清理");

    char fifo_path[PATH_MAX];
    format_child(fifo_path, shared, "pipe.tar");
    require(mkfifo(fifo_path, 0600) == 0,
            "创建 Shared FIFO 归档候选");
    alarm(3);
    error = ish_apple_watch_root_archive_import(
            seed, parent, shared, "pipe.tar",
            root_name, (struct progress) {0}, &archive_error);
    alarm(0);
    require(error == EINVAL && import_calls == 2 &&
            !has_partial(parent),
            "Shared FIFO 必须非阻塞拒绝");

    char mutable_archive[PATH_MAX];
    format_child(mutable_archive, shared, "mutable.tar");
    write_repeated_file(mutable_archive, 'A', 256 * 1024);
    archive_was_mutated = false;
    error = ish_apple_watch_root_archive_import(
            seed, parent, shared, "mutable.tar",
            root_name,
            (struct progress) {
                .cookie = mutable_archive,
                .callback = mutate_archive_progress,
            },
            &archive_error);
    require(error == EAGAIN && archive_was_mutated &&
            import_calls == 2 && !has_partial(parent),
            "并发原地改写 Shared 归档必须拒绝混合快照");

    error = ish_apple_watch_root_archive_export(
            parent, "aarch64", "aarch64",
            shared, "active.tar.gz",
            (struct progress) {0}, &archive_error);
    require(error == EBUSY && export_calls == 0,
            "活动 root 必须拒绝宿主导出");

    sync_calls = 0;
    export_lock_was_exclusive = false;
    error = ish_apple_watch_root_archive_export(
            parent, "aarch64-2", "aarch64",
            shared, "inactive.tar.gz",
            (struct progress) {0}, &archive_error);
    require(error == 0 && export_calls == 1 &&
            export_lock_was_exclusive && sync_calls >= 2 &&
            !has_partial(parent),
            "非活动 root 应持有排他锁并同步双目录后原子导出");
    char output[PATH_MAX];
    format_child(output, shared, "inactive.tar.gz");
    struct stat output_metadata;
    require(lstat(output, &output_metadata) == 0 &&
            S_ISREG(output_metadata.st_mode),
            "导出结果必须是普通文件");

    sync_calls = 0;
    fail_sync_call = 2;
    error = ish_apple_watch_root_archive_export(
            parent, "aarch64-2", "aarch64",
            shared, "rollback.tar.gz",
            (struct progress) {0}, &archive_error);
    char rolled_back_output[PATH_MAX];
    format_child(rolled_back_output, shared, "rollback.tar.gz");
    require(error == EIO && access(rolled_back_output, F_OK) < 0 &&
            errno == ENOENT && sync_calls >= 4 && !has_partial(parent),
            "双目录同步失败必须回滚发布并持久化清理");
    fail_sync_call = 0;

    error = ish_apple_watch_root_archive_export(
            parent, "aarch64-2", "aarch64",
            shared, "inactive.tar.gz",
            (struct progress) {0}, &archive_error);
    require(error == EEXIST && export_calls == 3 &&
            !has_partial(parent),
            "不得覆盖 Shared 现有文件，失败后仍须清理 partial");

    export_missing_host = true;
    error = ish_apple_watch_root_archive_export(
            parent, "aarch64-2", "aarch64",
            shared, "missing-host.tar.gz",
            (struct progress) {0}, &archive_error);
    char missing_output[PATH_MAX];
    format_child(missing_output, shared, "missing-host.tar.gz");
    require(error == EINVAL &&
            archive_error.code == ENOENT &&
            access(missing_output, F_OK) < 0 &&
            errno == ENOENT && !has_partial(parent),
            "fakefs 缺少宿主对象时不得发布或残留导出 partial");
    export_missing_host = false;

    int base_directory = open(base, O_RDONLY | O_DIRECTORY);
    require(base_directory >= 0, "打开临时根目录");
    require(remove_tree_at(base_directory, "roots") == 0,
            "清理 roots");
    require(remove_tree_at(base_directory, "shared") == 0,
            "清理 shared");
    require(remove_tree_at(base_directory, "seed") == 0,
            "清理 seed");
    close(base_directory);
    require(rmdir(base) == 0, "清理临时根目录");
    puts("Watch 宿主 Root 归档 C 边界回归通过");
    return 0;
}
