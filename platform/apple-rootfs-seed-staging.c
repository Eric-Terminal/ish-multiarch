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

// 负责收据、复制凭据与可崩溃恢复的 staging 生命周期。

static bool valid_receipt(const char *bytes, size_t length) {
    static const char digest_prefix[] = "seed_archive_sha256=";
    size_t format_length = sizeof(ish_apple_rootfs_receipt_format) - 1;
    size_t prefix_length = sizeof(digest_prefix) - 1;
    size_t expected_length = format_length + prefix_length + 64 + 1;
    return length == expected_length &&
            memcmp(bytes, ish_apple_rootfs_receipt_format, format_length) == 0 &&
            memcmp(bytes + format_length,
                    digest_prefix, prefix_length) == 0 &&
            ish_apple_rootfs_sha256_is_valid(bytes + format_length + prefix_length, 64) &&
            bytes[expected_length - 1] == '\n';
}

int ish_apple_rootfs_write_receipt_at(
        int directory, const struct seed_manifest *manifest) {
    static const char digest_prefix[] = "seed_archive_sha256=";
    char receipt[(sizeof(ish_apple_rootfs_receipt_format) - 1) +
            (sizeof(digest_prefix) - 1) + 64 + 1];
    size_t offset = 0;
    memcpy(receipt + offset, ish_apple_rootfs_receipt_format,
            sizeof(ish_apple_rootfs_receipt_format) - 1);
    offset += sizeof(ish_apple_rootfs_receipt_format) - 1;
    memcpy(receipt + offset, digest_prefix, sizeof(digest_prefix) - 1);
    offset += sizeof(digest_prefix) - 1;
    memcpy(receipt + offset, manifest->archive_sha256, 64);
    offset += 64;
    receipt[offset++] = '\n';
    return ish_apple_rootfs_create_regular_at(directory, ish_apple_rootfs_install_receipt_name,
            receipt, offset, true);
}

int ish_apple_rootfs_validate_opened_root(int root) {
    char *receipt = NULL;
    size_t receipt_length = 0;
    int error = ish_apple_rootfs_read_regular_at(root, ish_apple_rootfs_install_receipt_name,
            MANIFEST_LIMIT, &receipt, &receipt_length);
    if (error == 0 && !valid_receipt(receipt, receipt_length))
        error = EEXIST;
    free(receipt);

    static const struct {
        const char *name;
        mode_t type;
    } required[] = {
        {"meta.db", S_IFREG},
        {"data", S_IFDIR},
    };
    for (size_t i = 0;
            error == 0 && i < sizeof(required) / sizeof(required[0]); i++) {
        struct stat metadata;
        if (fstatat(root, required[i].name, &metadata,
                AT_SYMLINK_NOFOLLOW) < 0) {
            error = ish_apple_rootfs_errno_or_io();
        } else if ((metadata.st_mode & S_IFMT) != required[i].type ||
                metadata.st_uid != geteuid()) {
            error = EEXIST;
        }
    }
    return error;
}

int ish_apple_rootfs_open_existing_root(
        int parent, const char *root_name,
        bool *present, int *root_out) {
    // 已运行 root 允许 guest 改写 SQLite 内容，只验证凭据与必要宿主对象。
    *present = false;
    if (root_out != NULL)
        *root_out = -1;
    struct stat root_metadata;
    if (fstatat(parent, root_name, &root_metadata,
            AT_SYMLINK_NOFOLLOW) < 0) {
        if (errno == ENOENT)
            return 0;
        return ish_apple_rootfs_errno_or_io();
    }
    *present = true;
    if (!S_ISDIR(root_metadata.st_mode) ||
            root_metadata.st_uid != geteuid())
        return EEXIST;

    int root = openat(parent, root_name,
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (root < 0)
        return ish_apple_rootfs_errno_or_io();
    struct stat opened_metadata;
    int error = 0;
    if (fstat(root, &opened_metadata) < 0)
        error = ish_apple_rootfs_errno_or_io();
    else if (opened_metadata.st_dev != root_metadata.st_dev ||
            opened_metadata.st_ino != root_metadata.st_ino)
        error = EAGAIN;
    if (error == 0)
        error = ish_apple_rootfs_validate_opened_root(root);
    if (error != 0 || root_out == NULL) {
        if (close(root) < 0 && error == 0)
            error = ish_apple_rootfs_errno_or_io();
    } else {
        *root_out = root;
    }
    return error;
}

int ish_apple_rootfs_inspect_existing_root(
        int parent, const char *root_name, bool *present) {
    return ish_apple_rootfs_open_existing_root(parent, root_name, present, NULL);
}

int ish_apple_rootfs_write_copy_operation_marker(
        int root,
        const char *source_name,
        const char *destination_name,
        const char *operation_token) {
    char marker_name[NAME_MAX + 1];
    char record[COPY_OPERATION_RECORD_LIMIT + 1];
    int error = ish_apple_rootfs_format_copy_operation_marker_name(
            marker_name, operation_token);
    if (error == 0)
        error = ish_apple_rootfs_format_copy_operation_record(
            record, source_name, destination_name, operation_token);
    if (error != 0)
        return error;
    return ish_apple_rootfs_create_regular_at(root, marker_name,
            record, strlen(record), true);
}

int ish_apple_rootfs_inspect_copy_operation_marker(
        int root,
        const char *source_name,
        const char *destination_name,
        const char *operation_token,
        bool *present,
        bool *matches) {
    *present = false;
    *matches = false;
    char marker_name[NAME_MAX + 1];
    char expected[COPY_OPERATION_RECORD_LIMIT + 1];
    int error = ish_apple_rootfs_format_copy_operation_marker_name(
            marker_name, operation_token);
    if (error == 0)
        error = ish_apple_rootfs_format_copy_operation_record(
                expected, source_name,
                destination_name, operation_token);
    if (error != 0)
        return error;
    size_t expected_length = strlen(expected);

    struct stat named;
    if (fstatat(root, marker_name,
            &named, AT_SYMLINK_NOFOLLOW) < 0) {
        if (errno == ENOENT)
            return 0;
        return ish_apple_rootfs_errno_or_io();
    }
    *present = true;
    if (!S_ISREG(named.st_mode) ||
            named.st_uid != geteuid() ||
            named.st_nlink != 1 ||
            named.st_size != (off_t) expected_length)
        return 0;

    int file = openat(root, marker_name,
            O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (file < 0)
        return errno == ELOOP ? 0 : ish_apple_rootfs_errno_or_io();
    struct stat metadata;
    if (fstat(file, &metadata) < 0)
        error = ish_apple_rootfs_errno_or_io();
    else if (!S_ISREG(metadata.st_mode) ||
            metadata.st_uid != geteuid() ||
            metadata.st_nlink != 1 ||
            metadata.st_size != (off_t) expected_length ||
            metadata.st_dev != named.st_dev ||
            metadata.st_ino != named.st_ino)
        error = EAGAIN;

    char bytes[COPY_OPERATION_RECORD_LIMIT];
    size_t offset = 0;
    while (error == 0 && offset < expected_length) {
        ssize_t count = read(
                file, bytes + offset, expected_length - offset);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            error = ish_apple_rootfs_errno_or_io();
        } else if (count == 0) {
            error = EIO;
        } else {
            offset += (size_t) count;
        }
    }
    if (error == 0) {
        char extra;
        ssize_t count;
        do {
            count = read(file, &extra, 1);
        } while (count < 0 && errno == EINTR);
        if (count < 0)
            error = ish_apple_rootfs_errno_or_io();
        else if (count != 0)
            error = EAGAIN;
    }

    struct stat verified;
    if (error == 0 &&
            fstatat(root, marker_name,
                    &verified, AT_SYMLINK_NOFOLLOW) < 0)
        error = ish_apple_rootfs_errno_or_io();
    if (error == 0 && (!S_ISREG(verified.st_mode) ||
            verified.st_uid != geteuid() ||
            verified.st_nlink != 1 ||
            verified.st_size != metadata.st_size ||
            verified.st_dev != metadata.st_dev ||
            verified.st_ino != metadata.st_ino))
        error = EAGAIN;
    if (close(file) < 0 && error == 0)
        error = ish_apple_rootfs_errno_or_io();
    if (error != 0)
        return error;
    *matches = memcmp(bytes, expected, expected_length) == 0;
    return 0;
}

int ish_apple_rootfs_verify_named_root_identity(
        int parent, const char *root_name, int root) {
    struct stat opened;
    struct stat named;
    if (fstat(root, &opened) < 0)
        return ish_apple_rootfs_errno_or_io();
    if (fstatat(parent, root_name,
            &named, AT_SYMLINK_NOFOLLOW) < 0)
        return ish_apple_rootfs_errno_or_io();
    if (!S_ISDIR(named.st_mode) ||
            named.st_uid != geteuid() ||
            opened.st_dev != named.st_dev ||
            opened.st_ino != named.st_ino)
        return EAGAIN;
    return 0;
}

static void generate_owner_token(char token[OWNER_TOKEN_HEX_LENGTH + 1]) {
    unsigned char random[OWNER_TOKEN_BYTES];
    static const char hexadecimal[] = "0123456789abcdef";
    arc4random_buf(random, sizeof(random));
    for (size_t i = 0; i < sizeof(random); i++) {
        token[i * 2] = hexadecimal[random[i] >> 4u];
        token[i * 2 + 1] = hexadecimal[random[i] & 0x0fu];
    }
    token[OWNER_TOKEN_HEX_LENGTH] = '\0';
}

static int format_staging_name(
        char output[NAME_MAX + 1], const char *root_name,
        const char token[OWNER_TOKEN_HEX_LENGTH + 1]) {
    int length = snprintf(output, NAME_MAX + 1,
            ".%s.installing.%s", root_name, token);
    return length < 0 || length > NAME_MAX ? ENAMETOOLONG : 0;
}

static int format_owner_record(
        char output[OWNER_RECORD_LIMIT + 1],
        const struct staging_owner *owner) {
    int length = snprintf(output, OWNER_RECORD_LIMIT + 1,
            "%s\nstaging=%s\ndevice=%" PRIxMAX "\ninode=%" PRIxMAX "\n",
            ish_apple_rootfs_owner_format, owner->staging_name,
            owner->staging_device, owner->staging_inode);
    if (length < 0 || length > OWNER_RECORD_LIMIT)
        return EOVERFLOW;
    return 0;
}

static bool parse_hex_uintmax(
        const char *bytes, size_t length, uintmax_t *value_out) {
    if (length == 0 || length > sizeof(uintmax_t) * 2)
        return false;
    uintmax_t value = 0;
    for (size_t i = 0; i < length; i++) {
        unsigned digit;
        if (bytes[i] >= '0' && bytes[i] <= '9')
            digit = (unsigned) (bytes[i] - '0');
        else if (bytes[i] >= 'a' && bytes[i] <= 'f')
            digit = (unsigned) (bytes[i] - 'a') + 10u;
        else
            return false;
        if (value > (UINTMAX_MAX - digit) / 16u)
            return false;
        value = value * 16u + digit;
    }
    *value_out = value;
    return true;
}

static bool parse_owner_record(
        char *bytes, size_t length, const char *root_name,
        struct staging_owner *owner) {
    if (memchr(bytes, '\0', length) != NULL)
        return false;
    char *cursor = bytes;
    char *end = bytes + length;
    char *line;
    size_t line_length;
    if (ish_apple_rootfs_take_line(&cursor, end, &line, &line_length) != 0 ||
            !ish_apple_rootfs_line_equals(line, line_length, ish_apple_rootfs_owner_format))
        return false;

    static const char staging_prefix[] = "staging=";
    if (ish_apple_rootfs_take_line(&cursor, end, &line, &line_length) != 0 ||
            line_length <= sizeof(staging_prefix) - 1 ||
            memcmp(line, staging_prefix, sizeof(staging_prefix) - 1) != 0)
        return false;
    const char *staging = line + sizeof(staging_prefix) - 1;
    size_t staging_length = line_length - (sizeof(staging_prefix) - 1);
    char expected_prefix[NAME_MAX + 1];
    int prefix_error = ish_apple_rootfs_format_private_name(
            expected_prefix, root_name, ".installing.");
    size_t expected_length = prefix_error == 0 ?
            strlen(expected_prefix) : 0;
    if (prefix_error != 0 ||
            staging_length != expected_length + OWNER_TOKEN_HEX_LENGTH ||
            memcmp(staging, expected_prefix, expected_length) != 0)
        return false;
    for (size_t i = expected_length; i < staging_length; i++) {
        if (!((staging[i] >= '0' && staging[i] <= '9') ||
                (staging[i] >= 'a' && staging[i] <= 'f')))
            return false;
    }
    memcpy(owner->staging_name, staging, staging_length);
    owner->staging_name[staging_length] = '\0';

    static const char device_prefix[] = "device=";
    if (ish_apple_rootfs_take_line(&cursor, end, &line, &line_length) != 0 ||
            line_length <= sizeof(device_prefix) - 1 ||
            memcmp(line, device_prefix, sizeof(device_prefix) - 1) != 0 ||
            !parse_hex_uintmax(line + sizeof(device_prefix) - 1,
                    line_length - (sizeof(device_prefix) - 1),
                    &owner->staging_device))
        return false;
    static const char inode_prefix[] = "inode=";
    if (ish_apple_rootfs_take_line(&cursor, end, &line, &line_length) != 0 ||
            line_length <= sizeof(inode_prefix) - 1 ||
            memcmp(line, inode_prefix, sizeof(inode_prefix) - 1) != 0 ||
            !parse_hex_uintmax(line + sizeof(inode_prefix) - 1,
                    line_length - (sizeof(inode_prefix) - 1),
                    &owner->staging_inode) ||
            owner->staging_inode == 0 || cursor != end)
        return false;
    return true;
}

static int inspect_owner(
        int parent, const char *owner_name, const char *root_name,
        enum owner_state *state, struct staging_owner *owner) {
    *state = OWNER_MISSING;
    *owner = (struct staging_owner) {0};
    struct stat metadata;
    if (fstatat(parent, owner_name, &metadata,
            AT_SYMLINK_NOFOLLOW) < 0) {
        if (errno == ENOENT)
            return 0;
        return ish_apple_rootfs_errno_or_io();
    }
    *state = OWNER_UNKNOWN;
    if (!S_ISLNK(metadata.st_mode) || metadata.st_uid != geteuid() ||
            metadata.st_nlink != 1 || metadata.st_size <= 0 ||
            (uintmax_t) metadata.st_size > OWNER_RECORD_LIMIT)
        return 0;

    char bytes[OWNER_RECORD_LIMIT + 1];
    ssize_t count = readlinkat(parent, owner_name,
            bytes, sizeof(bytes));
    if (count < 0)
        return ish_apple_rootfs_errno_or_io();
    if ((size_t) count > OWNER_RECORD_LIMIT)
        return 0;
    bytes[count] = '\0';
    if (!parse_owner_record(bytes, (size_t) count, root_name, owner))
        return 0;

    struct stat verified;
    if (fstatat(parent, owner_name, &verified,
            AT_SYMLINK_NOFOLLOW) < 0)
        return ish_apple_rootfs_errno_or_io();
    if (!S_ISLNK(verified.st_mode) ||
            verified.st_dev != metadata.st_dev ||
            verified.st_ino != metadata.st_ino)
        return EAGAIN;
    owner->marker_device = (uintmax_t) verified.st_dev;
    owner->marker_inode = (uintmax_t) verified.st_ino;
    *state = OWNER_VALID;
    return 0;
}

int ish_apple_rootfs_entry_metadata(
        int parent, const char *name,
        bool *exists, struct stat *metadata) {
    *exists = false;
    if (fstatat(parent, name, metadata, AT_SYMLINK_NOFOLLOW) < 0) {
        if (errno == ENOENT)
            return 0;
        return ish_apple_rootfs_errno_or_io();
    }
    *exists = true;
    return 0;
}

static int verify_staging_identity(
        int parent, const struct staging_owner *owner, bool *exists) {
    struct stat metadata;
    int error = ish_apple_rootfs_entry_metadata(
            parent, owner->staging_name, exists, &metadata);
    if (error != 0 || !*exists)
        return error;
    if (!S_ISDIR(metadata.st_mode) || metadata.st_uid != geteuid() ||
            (uintmax_t) metadata.st_dev != owner->staging_device ||
            (uintmax_t) metadata.st_ino != owner->staging_inode)
        return EEXIST;
    int directory = openat(parent, owner->staging_name,
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (directory < 0)
        return ish_apple_rootfs_errno_or_io();
    struct stat opened;
    if (fstat(directory, &opened) < 0)
        error = ish_apple_rootfs_errno_or_io();
    else if (opened.st_dev != metadata.st_dev ||
            opened.st_ino != metadata.st_ino)
        error = EAGAIN;
    if (close(directory) < 0 && error == 0)
        error = ish_apple_rootfs_errno_or_io();
    return error;
}

static int remove_unpublished_staging(
        int parent, const struct staging_owner *owner) {
    bool exists;
    int error = verify_staging_identity(parent, owner, &exists);
    if (error != 0)
        return error;
    if (exists)
        error = ish_apple_rootfs_remove_entry_at(parent, owner->staging_name);
    if (error == 0)
        error = ish_apple_rootfs_sync_directory_internal(parent);
    return error;
}

int ish_apple_rootfs_unlink_owner_marker(
        int parent, const char *owner_name,
        const struct staging_owner *owner, int sync_phase) {
    struct stat marker;
    if (fstatat(parent, owner_name, &marker,
            AT_SYMLINK_NOFOLLOW) < 0)
        return ish_apple_rootfs_errno_or_io();
    if (!S_ISLNK(marker.st_mode) ||
            (uintmax_t) marker.st_dev != owner->marker_device ||
            (uintmax_t) marker.st_ino != owner->marker_inode)
        return EAGAIN;
#ifdef ISH_APPLE_ROOTFS_SEED_TESTING
    if (sync_phase == ISH_APPLE_ROOTFS_SEED_TEST_PUBLISH_OWNER_SYNC &&
            ish_apple_rootfs_seed_test_fail_phase ==
                    ISH_APPLE_ROOTFS_SEED_TEST_PUBLISH_OWNER_UNLINK) {
        ish_apple_rootfs_seed_test_fail_phase =
                ISH_APPLE_ROOTFS_SEED_TEST_NONE;
        return EIO;
    }
#endif
    if (unlinkat(parent, owner_name, 0) < 0)
        return ish_apple_rootfs_errno_or_io();
    if (sync_phase == ISH_APPLE_ROOTFS_SEED_TEST_NONE)
        return ish_apple_rootfs_sync_directory_internal(parent);
    return ish_apple_rootfs_sync_directory_phase(parent, sync_phase);
}

int ish_apple_rootfs_remove_owned_staging(
        int parent, const char *owner_name,
        const struct staging_owner *owner) {
    bool exists;
    int error = verify_staging_identity(parent, owner, &exists);
    if (error != 0)
        return error;
    if (exists) {
        error = ish_apple_rootfs_remove_entry_at(parent, owner->staging_name);
        if (error != 0)
            return error;
    }
    // 先持久化 staging 消失，再删除恢复凭据；两个阶段不能合并。
    if (error == 0)
        error = ish_apple_rootfs_sync_directory_phase(parent,
                ISH_APPLE_ROOTFS_SEED_TEST_CLEANUP_STAGING_SYNC);
    if (error == 0)
        error = ish_apple_rootfs_unlink_owner_marker(parent, owner_name, owner,
                ISH_APPLE_ROOTFS_SEED_TEST_CLEANUP_OWNER_SYNC);
    return error;
}

int ish_apple_rootfs_recover_staging(
        int parent, const char *owner_name, const char *root_name) {
    enum owner_state state;
    struct staging_owner owner;
    int error = inspect_owner(
            parent, owner_name, root_name, &state, &owner);
    if (error != 0)
        return error;
    if (state == OWNER_UNKNOWN)
        return EEXIST;
    return state == OWNER_VALID ?
            ish_apple_rootfs_remove_owned_staging(parent, owner_name, &owner) : 0;
}

int ish_apple_rootfs_cleanup_staging_if_owned(
        int parent, const char *owner_name, const char *root_name) {
    enum owner_state state;
    struct staging_owner owner;
    int error = inspect_owner(
            parent, owner_name, root_name, &state, &owner);
    if (error != 0)
        return error;
    if (state == OWNER_UNKNOWN)
        return EEXIST;
    return state == OWNER_VALID ?
            ish_apple_rootfs_remove_owned_staging(parent, owner_name, &owner) : 0;
}

int ish_apple_rootfs_create_staging(
        int parent, const char *root_name, const char *owner_name,
        int *staging_out, struct staging_owner *owner_out) {
    // 动态名字让 owner 发布前的孤儿可保守保留，又不会阻塞下一次安装。
    struct staging_owner owner = {0};
    int error = EAGAIN;
    for (unsigned attempt = 0; attempt < 16; attempt++) {
        char token[OWNER_TOKEN_HEX_LENGTH + 1];
        generate_owner_token(token);
        error = format_staging_name(owner.staging_name, root_name, token);
        if (error != 0)
            return error;
        if (mkdirat(parent, owner.staging_name, 0700) == 0) {
            error = 0;
            break;
        }
        if (errno != EEXIST)
            return ish_apple_rootfs_errno_or_io();
    }
    if (error != 0)
        return error;
    int staging = -1;
    staging = openat(parent, owner.staging_name,
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (staging < 0)
        error = ish_apple_rootfs_errno_or_io();
    if (error == 0 && fchmod(staging, 0700) < 0)
        error = ish_apple_rootfs_errno_or_io();
    struct stat metadata;
    if (error == 0 && fstat(staging, &metadata) < 0)
        error = ish_apple_rootfs_errno_or_io();
    if (error == 0 && (!S_ISDIR(metadata.st_mode) ||
            metadata.st_uid != geteuid()))
        error = EEXIST;
    if (error == 0) {
        owner.staging_device = (uintmax_t) metadata.st_dev;
        owner.staging_inode = (uintmax_t) metadata.st_ino;
    }
    if (error == 0)
        error = ish_apple_rootfs_sync_directory_internal(parent);

    char owner_record[OWNER_RECORD_LIMIT + 1];
    if (error == 0)
        error = format_owner_record(owner_record, &owner);
    if (error == 0 && symlinkat(owner_record, parent, owner_name) < 0)
        error = ish_apple_rootfs_errno_or_io();
    bool owner_created = error == 0;
    if (error == 0)
        error = ish_apple_rootfs_sync_directory_internal(parent);
    if (error == 0) {
        enum owner_state state;
        error = inspect_owner(parent, owner_name,
                root_name, &state, &owner);
        if (error == 0 && state != OWNER_VALID)
            error = EAGAIN;
    }
    if (error != 0) {
        int original_error = error;
        if (staging >= 0)
            close(staging);
        if (owner_created)
            (void) ish_apple_rootfs_recover_staging(parent, owner_name, root_name);
        else
            (void) remove_unpublished_staging(parent, &owner);
        return original_error;
    }
    *staging_out = staging;
    *owner_out = owner;
    return 0;
}

int ish_apple_rootfs_rollback_unsynchronized_publish(
        int parent, const char *root_name,
        const struct staging_owner *owner) {
    struct stat metadata;
    if (fstatat(parent, root_name, &metadata,
            AT_SYMLINK_NOFOLLOW) < 0)
        return ish_apple_rootfs_errno_or_io();
    if (!S_ISDIR(metadata.st_mode) ||
            metadata.st_uid != geteuid() ||
            (uintmax_t) metadata.st_dev != owner->staging_device ||
            (uintmax_t) metadata.st_ino != owner->staging_inode)
        return EAGAIN;
    if (renameatx_np(parent, root_name,
            parent, owner->staging_name, RENAME_EXCL) < 0)
        return ish_apple_rootfs_errno_or_io();
    return ish_apple_rootfs_sync_directory_internal(parent);
}
