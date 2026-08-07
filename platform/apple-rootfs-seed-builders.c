#include "platform/apple-rootfs-seed-internal.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <sqlite3.h>

#include "fs/fake-db.h"
#include "platform/apple-rootfs-storage-private.h"
#include "util/fchdir.h"

// 将种子、托管副本或导入目录组装成可发布 root。

int ish_apple_rootfs_finalize_seed_staging(
        int staging, struct seed_manifest *manifest_out) {
    struct seed_manifest manifest = {0};
    int error = ish_apple_rootfs_validate_seed_top(staging, &manifest);
    int staging_data = -1;
    if (error == 0) {
        staging_data = openat(staging, "data",
                O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (staging_data < 0)
            error = ish_apple_rootfs_errno_or_io();
    }
    if (error == 0)
        error = ish_apple_rootfs_validate_busybox_elf(staging_data);

    char *hardlink_bytes = NULL;
    size_t hardlink_length = 0;
    struct hardlink_manifest hardlinks = {0};
    if (error == 0)
        error = ish_apple_rootfs_read_regular_at(staging, ish_apple_rootfs_hardlink_manifest_name,
                HARDLINK_MANIFEST_LIMIT,
                &hardlink_bytes, &hardlink_length);
    if (error == 0) {
        error = ish_apple_rootfs_parse_hardlink_manifest(
                hardlink_bytes, hardlink_length, &hardlinks);
        hardlink_bytes = NULL;
    }
    if (error == 0)
        error = ish_apple_rootfs_validate_and_update_database(
                staging, staging_data, &hardlinks);
    ish_apple_rootfs_hardlink_manifest_destroy(&hardlinks);
    free(hardlink_bytes);

    if (staging_data >= 0 && close(staging_data) < 0 && error == 0)
        error = ish_apple_rootfs_errno_or_io();
    if (error == 0)
        error = ish_apple_rootfs_write_receipt_at(staging, &manifest);
    if (error == 0)
        error = ish_apple_rootfs_sync_directory_internal(staging);
    if (error == 0 && manifest_out != NULL)
        *manifest_out = manifest;
    return error;
}

int ish_apple_rootfs_build_staging_root(int seed, int staging) {
    struct seed_manifest source_manifest = {0};
    int error = ish_apple_rootfs_validate_seed_top(seed, &source_manifest);
    static const char *regular_resources[] = {
        "meta.db", ish_apple_rootfs_manifest_name,
        ish_apple_rootfs_hardlink_manifest_name,
    };
    for (size_t i = 0; error == 0 &&
            i < sizeof(regular_resources) / sizeof(regular_resources[0]); i++)
        error = ish_apple_rootfs_copy_regular_at(
                seed, staging, regular_resources[i]);

    if (error == 0 && mkdirat(staging, "data", 0700) < 0)
        error = ish_apple_rootfs_errno_or_io();
    int source_data = -1;
    int staging_data = -1;
    if (error == 0) {
        source_data = openat(seed, "data",
                O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (source_data < 0)
            error = ish_apple_rootfs_errno_or_io();
    }
    if (error == 0) {
        staging_data = openat(staging, "data",
                O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (staging_data < 0)
            error = ish_apple_rootfs_errno_or_io();
    }
    if (error == 0)
        error = ish_apple_rootfs_copy_directory_contents(
                source_data, staging_data, 0);
    if (source_data >= 0 && close(source_data) < 0 && error == 0)
        error = ish_apple_rootfs_errno_or_io();
    if (staging_data >= 0 && close(staging_data) < 0 && error == 0)
        error = ish_apple_rootfs_errno_or_io();

    struct seed_manifest copied_manifest = {0};
    if (error == 0)
        error = ish_apple_rootfs_finalize_seed_staging(
                staging, &copied_manifest);
    if (error == 0 && strcmp(source_manifest.archive_sha256,
            copied_manifest.archive_sha256) != 0)
        error = EINVAL;
    return error;
}

int ish_apple_rootfs_build_copied_root(
        int source,
        int staging,
        const char *source_name,
        const char *destination_name,
        const char *operation_token) {
    struct root_copy_context context = {
        .destination_root = staging,
    };
    int error = ish_apple_rootfs_copy_managed_root_contents(
            source, staging, "", 0, &context);
    ish_apple_rootfs_copy_context_destroy(&context);
    if (error == 0)
        error = ish_apple_rootfs_validate_opened_root(staging);
    if (error == 0)
        error = ish_apple_rootfs_prepare_copied_database(staging);
    if (error == 0)
        error = ish_apple_rootfs_validate_opened_root(staging);
    if (error == 0 && operation_token != NULL)
        error = ish_apple_rootfs_write_copy_operation_marker(
                staging, source_name,
                destination_name, operation_token);
    if (error == 0)
        error = ish_apple_rootfs_sync_directory_internal(staging);
    return error;
}

static int validate_imported_root_top(int root) {
    unsigned found = 0;
    int iterator_file = dup(root);
    if (iterator_file < 0)
        return ish_apple_rootfs_errno_or_io();
    DIR *iterator = fdopendir(iterator_file);
    if (iterator == NULL) {
        int error = ish_apple_rootfs_errno_or_io();
        close(iterator_file);
        return error;
    }

    int error = 0;
    errno = 0;
    struct dirent *entry;
    while ((entry = readdir(iterator)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 ||
                strcmp(entry->d_name, "..") == 0)
            continue;
        mode_t expected_type;
        unsigned bit = 0;
        if (strcmp(entry->d_name, "meta.db") == 0) {
            expected_type = S_IFREG;
            bit = 1u << 0;
        } else if (strcmp(entry->d_name, "data") == 0) {
            expected_type = S_IFDIR;
            bit = 1u << 1;
        } else if (strcmp(entry->d_name, "meta.db-wal") == 0 ||
                strcmp(entry->d_name, "meta.db-shm") == 0 ||
                strcmp(entry->d_name, "meta.db-journal") == 0) {
            expected_type = S_IFREG;
        } else {
            error = EINVAL;
            break;
        }
        struct stat metadata;
        if (fstatat(root, entry->d_name, &metadata,
                AT_SYMLINK_NOFOLLOW) < 0) {
            error = ish_apple_rootfs_errno_or_io();
            break;
        }
        if ((metadata.st_mode & S_IFMT) != expected_type ||
                metadata.st_uid != geteuid() ||
                (bit != 0 && (found & bit) != 0)) {
            error = EINVAL;
            break;
        }
        found |= bit;
        errno = 0;
    }
    if (entry == NULL && errno != 0 && error == 0)
        error = ish_apple_rootfs_errno_or_io();
    if (closedir(iterator) < 0 && error == 0)
        error = ish_apple_rootfs_errno_or_io();
    if (error == 0 && found != 0x03)
        error = EINVAL;
    return error;
}

static int count_imported_regular_bytes(
        int directory, unsigned depth, uintmax_t *total,
        struct progress progress) {
    if (depth > COPY_TREE_DEPTH_LIMIT)
        return ELOOP;
    struct stat directory_metadata;
    if (fstat(directory, &directory_metadata) < 0)
        return ish_apple_rootfs_errno_or_io();
    int iterator_file = openat(directory, ".",
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (iterator_file < 0)
        return ish_apple_rootfs_errno_or_io();
    DIR *iterator = fdopendir(iterator_file);
    if (iterator == NULL) {
        int error = ish_apple_rootfs_errno_or_io();
        close(iterator_file);
        return error;
    }
    int error = 0;
    errno = 0;
    struct dirent *entry;
    while ((entry = readdir(iterator)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 ||
                strcmp(entry->d_name, "..") == 0)
            continue;
        error = ish_apple_rootfs_report_copy_stage(
                progress, 0, "正在检查文件系统内容");
        if (error != 0)
            break;
        struct stat metadata;
        if (fstatat(directory, entry->d_name, &metadata,
                AT_SYMLINK_NOFOLLOW) < 0) {
            error = ish_apple_rootfs_errno_or_io();
            break;
        }
        if (S_ISREG(metadata.st_mode)) {
            if (metadata.st_size < 0 ||
                    UINTMAX_MAX - *total <
                    (uintmax_t) metadata.st_size) {
                error = EFBIG;
                break;
            }
            *total += (uintmax_t) metadata.st_size;
        } else if (S_ISFIFO(metadata.st_mode)) {
            // FIFO 没有内容字节，但仍在复制循环中轮询取消。
        } else if (S_ISDIR(metadata.st_mode)) {
            int child = openat(directory, entry->d_name,
                    O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            if (child < 0) {
                error = ish_apple_rootfs_errno_or_io();
            } else {
                error = count_imported_regular_bytes(
                        child, depth + 1, total, progress);
                if (close(child) < 0 && error == 0)
                    error = ish_apple_rootfs_errno_or_io();
            }
            if (error != 0)
                break;
        } else {
            error = EINVAL;
            break;
        }
        errno = 0;
    }
    if (entry == NULL && errno != 0 && error == 0)
        error = ish_apple_rootfs_errno_or_io();
    if (closedir(iterator) < 0 && error == 0)
        error = ish_apple_rootfs_errno_or_io();
    struct timespec directory_times[2];
    ish_apple_rootfs_stat_times(&directory_metadata, directory_times);
    if (futimens(directory, directory_times) < 0 && error == 0)
        error = ish_apple_rootfs_errno_or_io();
    return error;
}

int ish_apple_rootfs_build_imported_root(
        int seed, int imported, int staging,
        struct progress progress) {
    struct seed_manifest manifest = {0};
    int error = ish_apple_rootfs_validate_seed_top(seed, &manifest);
    if (error == 0)
        error = validate_imported_root_top(imported);

    struct root_copy_context context = {
        .destination_root = staging,
        .progress = progress,
        .progress_span = 0.85,
    };
    if (error == 0)
        error = count_imported_regular_bytes(
                imported, 0, &context.total_bytes, progress);
    if (error == 0)
        error = ish_apple_rootfs_report_root_copy_progress(
                &context, 0, "正在复制文件系统内容");
    if (error == 0)
        error = ish_apple_rootfs_copy_managed_root_contents(
                imported, staging, "", 0, &context);
    ish_apple_rootfs_copy_context_destroy(&context);
    if (error == 0)
        error = ish_apple_rootfs_report_copy_stage(
                progress, 0.86, "正在准备文件系统数据库");
    if (error == 0)
        error = ish_apple_rootfs_prepare_copied_database(staging);
    if (error == 0)
        error = ish_apple_rootfs_report_copy_stage(
                progress, 0.92, "正在写入文件系统收据");
    if (error == 0)
        error = ish_apple_rootfs_write_receipt_at(staging, &manifest);
    if (error == 0)
        error = ish_apple_rootfs_report_copy_stage(
                progress, 0.96, "正在验证文件系统");
    if (error == 0)
        error = ish_apple_rootfs_validate_opened_root(staging);
    if (error == 0)
        error = ish_apple_rootfs_report_copy_stage(
                progress, 0.99, "正在同步文件系统");
    if (error == 0)
        error = ish_apple_rootfs_sync_directory_internal(staging);
    if (error == 0)
        error = ish_apple_rootfs_report_copy_stage(
                progress, 1, "文件系统发布准备完成");
    return error;
}
