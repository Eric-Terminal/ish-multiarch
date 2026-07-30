#include "platform/apple-watch-runtime.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "fs/fd.h"
#include "fs/path.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "kernel/init.h"
#include "misc.h"
#include "platform/apple-watch-guest-files-private.h"
#include "platform/apple-watch-runtime-private.h"

#define WATCH_TEMPORARY_SCAN_LIMIT 256
#define WATCH_TEMPORARY_CLEANUP_LIMIT 64

struct watch_guest_file {
    int32_t id;
    const char *parent_directory;
    const char *name;
    const char *temporary_prefix;
    size_t maximum_length;
    bool needs_ish_directory;
};

static _Atomic uint64_t temporary_sequence = 1;

#ifdef ISH_APPLE_WATCH_GUEST_FILES_TESTING
static size_t test_maximum_chunk;
static size_t test_fail_after;
static size_t test_bytes_written;
static int test_write_error;
static ish_watch_guest_file_test_event_hook test_event_hook;
#endif

static const struct watch_guest_file *watch_guest_file_for_id(
        int32_t file_id) {
    static const struct watch_guest_file repositories = {
        .id = ISH_WATCH_GUEST_FILE_REPOSITORIES,
        .parent_directory = "/etc/apk",
        .name = "repositories",
        .temporary_prefix = ".ish-repositories.tmp",
        .maximum_length = ISH_WATCH_REPOSITORIES_LIMIT,
    };
    static const struct watch_guest_file apk_version = {
        .id = ISH_WATCH_GUEST_FILE_APK_VERSION,
        .parent_directory = "/ish",
        .name = "apk-version",
        .temporary_prefix = ".ish-apk-version.tmp",
        .maximum_length = ISH_WATCH_APK_VERSION_LIMIT,
        .needs_ish_directory = true,
    };

    switch (file_id) {
        case ISH_WATCH_GUEST_FILE_REPOSITORIES:
            return &repositories;
        case ISH_WATCH_GUEST_FILE_APK_VERSION:
            return &apk_version;
        default:
            return NULL;
    }
}

static int watch_guest_directory_sync_fd(struct fd *directory) {
    int error = file_sync_fd(directory, false);
    // 部分 Apple 文件系统不接受目录 fsync；文件自身仍已完整 fsync。
    if (error == _EINVAL || error == _ENOTSUP)
        return 0;
    return error;
}

static int watch_guest_directory_sync_path(const char *path) {
    struct fd *directory = generic_open(
            path, O_RDONLY_ | O_DIRECTORY_ | O_NOFOLLOW_, 0);
    if (IS_ERR(directory))
        return (int) PTR_ERR(directory);

    int error = watch_guest_directory_sync_fd(directory);
    int close_error = fd_close(directory);
    if (error >= 0 && close_error < 0)
        error = close_error;
    return error;
}

static struct fd *watch_guest_file_open_parent(
        const struct watch_guest_file *file,
        bool create_if_missing) {
    struct fd *parent = generic_open(
            file->parent_directory,
            O_RDONLY_ | O_DIRECTORY_ | O_NOFOLLOW_,
            0);
    if (!IS_ERR(parent) || !create_if_missing ||
            PTR_ERR(parent) != _ENOENT)
        return parent;

    int error = generic_mkdirat(
            AT_PWD, file->parent_directory, 0755);
    if (error < 0 && error != _EEXIST)
        return ERR_PTR(error);
    if (error == 0) {
        error = watch_guest_directory_sync_path("/");
        /*
         * fakefs 没有按目录身份回滚 rmdir 的入口。同步失败时宁可留下空 /ish，
         * 也不能按路径删除并发替换进来的目录。
         */
        if (error < 0)
            return ERR_PTR(error);
    }

    return generic_open(
            file->parent_directory,
            O_RDONLY_ | O_DIRECTORY_ | O_NOFOLLOW_,
            0);
}

#ifdef ISH_APPLE_WATCH_GUEST_FILES_TESTING
static void watch_guest_file_test_event(
        const struct watch_guest_file *file,
        int event,
        const char *temporary_name) {
    if (test_event_hook != NULL)
        test_event_hook(file->id, event, temporary_name);
}
#else
static void watch_guest_file_test_event(
        const struct watch_guest_file *file,
        int event,
        const char *temporary_name) {
    (void) file;
    (void) event;
    (void) temporary_name;
}
#endif

static ssize_t watch_guest_file_read_current(
        const struct watch_guest_file *file,
        void *buffer,
        size_t capacity) {
    struct fd *parent = watch_guest_file_open_parent(file, false);
    if (IS_ERR(parent))
        return PTR_ERR(parent);
    watch_guest_file_test_event(
            file, ISH_WATCH_GUEST_FILE_TEST_PARENT_OPENED, NULL);

    struct fd *guest_fd = generic_openat(
            parent, file->name, O_RDONLY_ | O_NOFOLLOW_, 0);
    if (IS_ERR(guest_fd)) {
        ssize_t error = PTR_ERR(guest_fd);
        (void) fd_close(parent);
        return error;
    }

    unsigned char *contents = malloc(file->maximum_length + 1);
    if (contents == NULL) {
        (void) fd_close(guest_fd);
        (void) fd_close(parent);
        return _ENOMEM;
    }

    size_t total = 0;
    ssize_t result = 0;
    while (total <= file->maximum_length) {
        ssize_t count = file_read_fd(
                guest_fd,
                contents + total,
                file->maximum_length + 1 - total);
        if (count < 0) {
            result = count;
            break;
        }
        if (count == 0)
            break;
        total += (size_t) count;
    }

    int close_error = fd_close(guest_fd);
    if (result >= 0 && close_error < 0)
        result = close_error;
    if (result >= 0 && total > file->maximum_length)
        result = _EFBIG;
    if (result >= 0 && total > capacity)
        result = _E2BIG;
    if (result >= 0) {
        if (total != 0)
            memcpy(buffer, contents, total);
        result = (ssize_t) total;
    }
    close_error = fd_close(parent);
    if (result >= 0 && close_error < 0)
        result = close_error;
    free(contents);
    return result;
}

static ssize_t watch_guest_file_write(
        struct fd *guest_fd,
        const unsigned char *bytes,
        size_t length) {
#ifdef ISH_APPLE_WATCH_GUEST_FILES_TESTING
    if (test_write_error < 0 &&
            test_bytes_written >= test_fail_after)
        return test_write_error;
    if (test_maximum_chunk != 0 &&
            length > test_maximum_chunk)
        length = test_maximum_chunk;
    if (test_write_error < 0 &&
            test_bytes_written < test_fail_after &&
            length > test_fail_after - test_bytes_written)
        length = test_fail_after - test_bytes_written;
#endif

    ssize_t written = file_write_fd(guest_fd, bytes, length);
#ifdef ISH_APPLE_WATCH_GUEST_FILES_TESTING
    if (written > 0)
        test_bytes_written += (size_t) written;
#endif
    return written;
}

static int watch_guest_file_write_all(
        struct fd *guest_fd,
        const void *bytes,
        size_t length) {
    const unsigned char *next = bytes;
    size_t remaining = length;
    while (remaining != 0) {
        ssize_t written = watch_guest_file_write(
                guest_fd, next, remaining);
        if (written < 0)
            return (int) written;
        if (written == 0)
            return _EIO;
        next += (size_t) written;
        remaining -= (size_t) written;
    }
    return 0;
}

static int watch_guest_file_open_temporary(
        const struct watch_guest_file *file,
        struct fd *parent,
        char name[MAX_PATH],
        struct fd **guest_fd) {
    for (unsigned attempt = 0; attempt < 128; attempt++) {
        uint64_t random[2];
        arc4random_buf(random, sizeof(random));
        uint64_t sequence = atomic_fetch_add_explicit(
                &temporary_sequence, 1, memory_order_relaxed);
        int length = snprintf(
                name,
                MAX_PATH,
                "%s.%016llx%016llx.%016llx",
                file->temporary_prefix,
                (unsigned long long) random[0],
                (unsigned long long) random[1],
                (unsigned long long) sequence);
        if (length < 0 || length >= MAX_PATH)
            return _ENAMETOOLONG;

        struct fd *opened = generic_openat(
                parent,
                name,
                O_WRONLY_ | O_CREAT_ | O_EXCL_ | O_NOFOLLOW_,
                0644);
        if (!IS_ERR(opened)) {
            *guest_fd = opened;
            return 0;
        }
        int error = (int) PTR_ERR(opened);
        if (error != _EEXIST)
            return error;
    }
    return _EEXIST;
}

static int watch_guest_file_cleanup_stale_temporary(
        const struct watch_guest_file *file,
        struct fd *parent) {
    struct fd *directory = generic_openat(
            parent, ".", O_RDONLY_ | O_DIRECTORY_ | O_NOFOLLOW_, 0);
    if (IS_ERR(directory))
        return (int) PTR_ERR(directory);

    struct stale_names {
        char value[WATCH_TEMPORARY_CLEANUP_LIMIT][NAME_MAX + 1];
    };
    struct stale_names *names = malloc(sizeof(*names));
    if (names == NULL) {
        (void) fd_close(directory);
        return _ENOMEM;
    }

    size_t count = 0;
    size_t scanned = 0;
    int error = 0;
    size_t prefix_length = strlen(file->temporary_prefix);
    lock(&directory->lock);
    while (scanned < WATCH_TEMPORARY_SCAN_LIMIT &&
            count < WATCH_TEMPORARY_CLEANUP_LIMIT) {
        struct dir_entry entry;
        int result = directory->ops->readdir(directory, &entry);
        if (result <= 0) {
            if (result < 0)
                error = result;
            break;
        }
        scanned++;
        if (strncmp(
                entry.name,
                file->temporary_prefix,
                prefix_length) != 0 ||
                entry.name[prefix_length] != '.')
            continue;
        strcpy(names->value[count], entry.name);
        count++;
    }
    unlock(&directory->lock);
    int close_error = fd_close(directory);
    if (error >= 0 && close_error < 0)
        error = close_error;

    for (size_t index = 0; error >= 0 && index < count; index++) {
        struct statbuf stat = {};
        int stat_error = generic_statat(
                parent, names->value[index], &stat, false);
        if (stat_error == _ENOENT)
            continue;
        if (stat_error < 0) {
            error = stat_error;
            break;
        }
        // 该前缀由桥接层保留；目录等非临时文件类型仍保守跳过。
        if (!S_ISREG(stat.mode))
            continue;
        int unlink_error = generic_unlinkat(
                parent, names->value[index]);
        if (unlink_error < 0 && unlink_error != _ENOENT)
            error = unlink_error;
    }
    free(names);
    return error;
}

static int watch_guest_file_replace_current(
        const struct watch_guest_file *file,
        const void *bytes,
        size_t length,
        bool remove_file) {
    struct fd *parent = watch_guest_file_open_parent(
            file, file->needs_ish_directory && !remove_file);
    if (IS_ERR(parent))
        return (int) PTR_ERR(parent);
    watch_guest_file_test_event(
            file, ISH_WATCH_GUEST_FILE_TEST_PARENT_OPENED, NULL);

    if (remove_file) {
        int error = generic_unlinkat(parent, file->name);
        if (error < 0) {
            (void) fd_close(parent);
            return error;
        }
        // unlink 已经发布后不能再以同步失败伪报“目标未改变”。
        (void) watch_guest_directory_sync_fd(parent);
        (void) fd_close(parent);
        return 0;
    }

#ifdef ISH_APPLE_WATCH_GUEST_FILES_TESTING
    test_bytes_written = 0;
#endif

    int error = watch_guest_file_cleanup_stale_temporary(
            file, parent);
    if (error < 0) {
        (void) fd_close(parent);
        return error;
    }

    char temporary_name[MAX_PATH];
    struct fd *guest_fd = NULL;
    error = watch_guest_file_open_temporary(
            file, parent, temporary_name, &guest_fd);
    if (error < 0) {
        (void) fd_close(parent);
        return error;
    }
    watch_guest_file_test_event(
            file,
            ISH_WATCH_GUEST_FILE_TEST_TEMPORARY_OPENED,
            temporary_name);

    error = watch_guest_file_write_all(
            guest_fd, bytes, length);
    if (error >= 0)
        error = file_sync_fd(guest_fd, false);
    int close_error = fd_close(guest_fd);
    if (error >= 0 && close_error < 0)
        error = close_error;
    if (error < 0) {
        // 只清理由本次 O_EXCL 创建的精确名字，不枚举或删除其他前缀文件。
        (void) generic_unlinkat(parent, temporary_name);
        (void) fd_close(parent);
        return error;
    }

    error = generic_renameat(
            parent, temporary_name, parent, file->name);
    if (error < 0) {
        (void) generic_unlinkat(parent, temporary_name);
        (void) fd_close(parent);
        return error;
    }
    /*
     * 同一固定 parent 下的 guest rename 对活进程观察者是不可分的；fakefs 的
     * SQLite 元数据、宿主目录项与掉电持久化跨越多层，不承诺崩溃原子性。
     */
    // rename 已经发布后，目录同步只能增强掉电持久性，不能改变成功语义。
    (void) watch_guest_directory_sync_fd(parent);
    (void) fd_close(parent);
    return 0;
}

static int watch_guest_file_validate_replace(
        const struct watch_guest_file *file,
        const void *bytes,
        size_t length,
        int remove_file) {
    if (file == NULL || (remove_file != 0 && remove_file != 1))
        return _EINVAL;
    if (remove_file != 0)
        return bytes == NULL && length == 0 ? 0 : _EINVAL;
    if (length > file->maximum_length)
        return _E2BIG;
    if (bytes == NULL && length != 0)
        return _EINVAL;
    return 0;
}

ssize_t ish_watch_guest_file_read(
        int32_t file_id, void *buffer, size_t capacity) {
    const struct watch_guest_file *file =
            watch_guest_file_for_id(file_id);
    if (file == NULL || (buffer == NULL && capacity != 0))
        return _EINVAL;

    int error = ish_watch_runtime_operation_availability();
    if (error < 0)
        return error;

    lock(&ish_watch_prepared_task_lock);
    error = ish_watch_runtime_operation_availability();
    if (error < 0) {
        unlock(&ish_watch_prepared_task_lock);
        return error;
    }

    error = begin_new_init_child();
    if (error < 0) {
        unlock(&ish_watch_prepared_task_lock);
        return error;
    }
    ssize_t result = watch_guest_file_read_current(
            file, buffer, capacity);
    cancel_prepared_process();
    unlock(&ish_watch_prepared_task_lock);
    return result;
}

int ish_watch_guest_file_replace(
        int32_t file_id,
        const void *bytes,
        size_t length,
        int remove_file) {
    const struct watch_guest_file *file =
            watch_guest_file_for_id(file_id);
    int error = watch_guest_file_validate_replace(
            file, bytes, length, remove_file);
    if (error < 0)
        return error;

    error = ish_watch_runtime_operation_availability();
    if (error < 0)
        return error;

    lock(&ish_watch_prepared_task_lock);
    error = ish_watch_runtime_operation_availability();
    if (error < 0) {
        unlock(&ish_watch_prepared_task_lock);
        return error;
    }

    error = begin_new_init_child();
    if (error < 0) {
        unlock(&ish_watch_prepared_task_lock);
        return error;
    }
    error = watch_guest_file_replace_current(
            file, bytes, length, remove_file != 0);
    cancel_prepared_process();
    unlock(&ish_watch_prepared_task_lock);
    return error;
}

#ifdef ISH_APPLE_WATCH_GUEST_FILES_TESTING
ssize_t ish_watch_guest_file_test_read_current(
        int32_t file_id, void *buffer, size_t capacity) {
    const struct watch_guest_file *file =
            watch_guest_file_for_id(file_id);
    if (file == NULL || (buffer == NULL && capacity != 0))
        return _EINVAL;
    return watch_guest_file_read_current(file, buffer, capacity);
}

int ish_watch_guest_file_test_replace_current(
        int32_t file_id,
        const void *bytes,
        size_t length,
        int remove_file) {
    const struct watch_guest_file *file =
            watch_guest_file_for_id(file_id);
    int error = watch_guest_file_validate_replace(
            file, bytes, length, remove_file);
    if (error < 0)
        return error;
    return watch_guest_file_replace_current(
            file, bytes, length, remove_file != 0);
}

void ish_watch_guest_file_test_set_write_behavior(
        size_t maximum_chunk,
        size_t fail_after,
        int error) {
    test_maximum_chunk = maximum_chunk;
    test_fail_after = fail_after;
    test_write_error = error;
}

void ish_watch_guest_file_test_set_event_hook(
        ish_watch_guest_file_test_event_hook hook) {
    test_event_hook = hook;
}
#endif
