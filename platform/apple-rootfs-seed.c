#include "platform/apple-rootfs-seed.h"

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
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <sqlite3.h>

#include "fs/fake-db.h"
#include "platform/apple-rootfs-storage-private.h"
#include "platform/apple-rootfs-seed-internal.h"
#include "util/fchdir.h"

#ifndef __APPLE__
#error "Apple rootfs seed 安装器只能构建到 Apple 平台"
#endif


const char ish_apple_rootfs_manifest_name[] = "rootfs-manifest.txt";
const char ish_apple_rootfs_hardlink_manifest_name[] = "rootfs-hardlinks.tsv";
const char ish_apple_rootfs_install_receipt_name[] = "rootfs-installation.txt";
const char ish_apple_rootfs_copy_operation_marker_prefix[] =
        ".ish-copy-operation.";
const char ish_apple_rootfs_owner_format[] =
        "format=ish-rootfs-install-owner-v2";
const char ish_apple_rootfs_receipt_format[] =
        "format=ish-rootfs-install-v1\n";
const char ish_apple_rootfs_copy_operation_format[] =
        "format=ish-rootfs-copy-operation-v1";

#ifdef ISH_APPLE_ROOTFS_SEED_TESTING
int ish_apple_rootfs_seed_test_fail_phase;
int ish_apple_rootfs_seed_test_force_sparse_fallback;
size_t ish_apple_rootfs_seed_test_write_limit;
unsigned ish_apple_rootfs_seed_test_sparse_fallback_count;
unsigned ish_apple_rootfs_seed_test_limited_write_count;
#endif

int ish_apple_rootfs_errno_or_io(void) {
    return errno == 0 ? EIO : errno;
}

int ish_apple_rootfs_sync_directory_internal(int directory) {
    // 部分 Apple 文件系统不接受目录 fsync；文件内容仍需严格 fsync。
    if (fsync(directory) == 0)
        return 0;
    if (errno == EINVAL || errno == ENOTSUP)
        return 0;
    return ish_apple_rootfs_errno_or_io();
}

int ish_apple_rootfs_sync_directory(int directory) {
    return ish_apple_rootfs_sync_directory_internal(directory);
}

int ish_apple_rootfs_sync_directory_phase(int directory, int phase) {
#ifdef ISH_APPLE_ROOTFS_SEED_TESTING
    if (ish_apple_rootfs_seed_test_fail_phase == phase) {
        ish_apple_rootfs_seed_test_fail_phase =
                ISH_APPLE_ROOTFS_SEED_TEST_NONE;
        return EIO;
    }
#else
    (void) phase;
#endif
    return ish_apple_rootfs_sync_directory_internal(directory);
}

int ish_apple_rootfs_sqlite_error(sqlite3 *database) {
    int primary = sqlite3_extended_errcode(database) & 0xff;
    switch (primary) {
        case SQLITE_NOMEM:
            return ENOMEM;
        case SQLITE_FULL:
            return ENOSPC;
        case SQLITE_BUSY:
        case SQLITE_LOCKED:
            return EBUSY;
        case SQLITE_READONLY:
            return EROFS;
        case SQLITE_IOERR:
            return EIO;
        case SQLITE_CANTOPEN:
            return ENOENT;
        case SQLITE_PERM:
        case SQLITE_AUTH:
            return EPERM;
        default:
            return EINVAL;
    }
}

bool ish_apple_rootfs_name_is_valid(const char *name) {
    if (name == NULL)
        return false;
    size_t length = strlen(name);
    if (length == 0 || length > ROOT_NAME_LIMIT)
        return false;
    if (!((name[0] >= 'a' && name[0] <= 'z') ||
            (name[0] >= 'A' && name[0] <= 'Z') ||
            (name[0] >= '0' && name[0] <= '9')))
        return false;
    for (size_t i = 1; i < length; i++) {
        char byte = name[i];
        if (!((byte >= 'a' && byte <= 'z') ||
                (byte >= 'A' && byte <= 'Z') ||
                (byte >= '0' && byte <= '9') ||
                byte == '.' || byte == '_' || byte == '-'))
            return false;
    }
    return true;
}

bool ish_apple_rootfs_copy_operation_token_is_valid(const char *token) {
    if (token == NULL)
        return false;
    size_t length = strlen(token);
    if (length == 0 || length > COPY_OPERATION_TOKEN_LIMIT)
        return false;
    if (!((token[0] >= 'a' && token[0] <= 'z') ||
            (token[0] >= 'A' && token[0] <= 'Z') ||
            (token[0] >= '0' && token[0] <= '9')))
        return false;
    for (size_t index = 0; index < length; index++) {
        char byte = token[index];
        if (!((byte >= 'a' && byte <= 'z') ||
                (byte >= 'A' && byte <= 'Z') ||
                (byte >= '0' && byte <= '9') ||
                byte == '-' || byte == '_' || byte == '.'))
            return false;
    }
    return true;
}

int ish_apple_rootfs_format_copy_operation_marker_name(
        char output[NAME_MAX + 1],
        const char *operation_token) {
    if (!ish_apple_rootfs_copy_operation_token_is_valid(operation_token))
        return EINVAL;
    int length = snprintf(output, NAME_MAX + 1,
            "%s%s", ish_apple_rootfs_copy_operation_marker_prefix, operation_token);
    return length < 0 || length > NAME_MAX ? ENAMETOOLONG : 0;
}

int ish_apple_rootfs_format_private_name(
        char output[NAME_MAX + 1], const char *root_name,
        const char *suffix) {
    int length = snprintf(output, NAME_MAX + 1, ".%s%s", root_name, suffix);
    if (length < 0 || length > NAME_MAX)
        return ENAMETOOLONG;
    return 0;
}

static int lock_file(
        int parent, const char *lock_name,
        int operation, int *lock_out) {
    // lock 文件长期保留，避免 unlink 后不同进程锁住不同 vnode。
    int lock = -1;
    for (unsigned attempt = 0; attempt < 16; attempt++) {
        lock = openat(parent, lock_name,
                O_RDWR | O_CLOEXEC | O_NOFOLLOW);
        if (lock >= 0)
            break;
        if (errno != ENOENT)
            return ish_apple_rootfs_errno_or_io();
        lock = openat(parent, lock_name,
                O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (lock >= 0) {
            int error = fsync(lock) < 0 ? ish_apple_rootfs_errno_or_io() :
                    ish_apple_rootfs_sync_directory_internal(parent);
            if (error != 0) {
                close(lock);
                return error;
            }
            break;
        }
        if (errno != EEXIST && errno != ENOENT)
            return ish_apple_rootfs_errno_or_io();
    }
    if (lock < 0)
        return EAGAIN;
    struct stat metadata;
    int error = 0;
    if (fstat(lock, &metadata) < 0)
        error = ish_apple_rootfs_errno_or_io();
    else if (!S_ISREG(metadata.st_mode) ||
            metadata.st_uid != geteuid() || metadata.st_nlink != 1)
        error = EEXIST;
    while (error == 0 && flock(lock, operation) < 0) {
        if (errno != EINTR) {
            error = errno == EWOULDBLOCK ? EBUSY : ish_apple_rootfs_errno_or_io();
            break;
        }
    }
    if (error != 0) {
        close(lock);
        return error;
    }
    *lock_out = lock;
    return 0;
}

int ish_apple_rootfs_unlock_managed_root(int lock) {
    if (lock < 0)
        return EINVAL;
    int error = 0;
    while (flock(lock, LOCK_UN) < 0) {
        if (errno == EINTR)
            continue;
        error = ish_apple_rootfs_errno_or_io();
        break;
    }
    if (close(lock) < 0 && error == 0)
        error = ish_apple_rootfs_errno_or_io();
    return error;
}

int ish_apple_rootfs_lock_managed_root(
        const char *persistent_parent,
        const char *root_name,
        bool exclusive,
        bool require_valid_root,
        int *lock_out) {
    if (persistent_parent == NULL || persistent_parent[0] == '\0' ||
            !ish_apple_rootfs_name_is_valid(root_name) || lock_out == NULL)
        return EINVAL;
    *lock_out = -1;

    char lock_name[NAME_MAX + 1];
    int error = ish_apple_rootfs_format_private_name(
            lock_name, root_name, ".lifecycle.lock");
    if (error != 0)
        return error;
    int parent = open(persistent_parent,
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (parent < 0)
        return ish_apple_rootfs_errno_or_io();

    int lock = -1;
    int operation = exclusive ? LOCK_EX | LOCK_NB : LOCK_SH;
    error = lock_file(parent, lock_name, operation, &lock);
    bool present = false;
    if (error == 0 && require_valid_root)
        error = ish_apple_rootfs_open_existing_root(parent, root_name, &present, NULL);
    if (error == 0 && !require_valid_root) {
        struct stat metadata;
        error = ish_apple_rootfs_entry_metadata(parent, root_name, &present, &metadata);
    }
    if (error == 0 && !present)
        error = ENOENT;
    if (close(parent) < 0 && error == 0)
        error = ish_apple_rootfs_errno_or_io();
    if (error != 0) {
        if (lock >= 0)
            (void) ish_apple_rootfs_unlock_managed_root(lock);
        return error;
    }
    *lock_out = lock;
    return 0;
}

int ish_apple_rootfs_lock_copy_catalog(
        const char *persistent_parent,
        int *lock_out) {
    if (persistent_parent == NULL || persistent_parent[0] == '\0' ||
            lock_out == NULL)
        return EINVAL;
    *lock_out = -1;
    static const char lock_name[] = ".copy-operation.lock";

    int parent = open(persistent_parent,
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (parent < 0)
        return ish_apple_rootfs_errno_or_io();
    int lock = -1;
    int error = lock_file(parent, lock_name, LOCK_EX, &lock);
    if (close(parent) < 0 && error == 0)
        error = ish_apple_rootfs_errno_or_io();
    if (error != 0) {
        if (lock >= 0)
            (void) ish_apple_rootfs_unlock_managed_root(lock);
        return error;
    }
    *lock_out = lock;
    return 0;
}

static int install_locked(
        const char *seed_root, int parent, const char *root_name,
        const char *owner_name, enum ish_apple_rootfs_seed_result *result) {
    bool root_present;
    int error = ish_apple_rootfs_inspect_existing_root(parent, root_name, &root_present);
    if (error != 0)
        return error;
    if (root_present) {
        error = ish_apple_rootfs_cleanup_staging_if_owned(parent, owner_name, root_name);
        if (error != 0)
            return error;
        *result = ISH_APPLE_ROOTFS_SEED_ALREADY_PRESENT;
        return 0;
    }

    error = ish_apple_rootfs_recover_staging(parent, owner_name, root_name);
    if (error != 0)
        return error;
    int seed = open(seed_root,
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (seed < 0)
        return ish_apple_rootfs_errno_or_io();

    int staging = -1;
    struct staging_owner owner = {0};
    error = ish_apple_rootfs_create_staging(parent, root_name,
            owner_name, &staging, &owner);
    if (error == 0)
        error = ish_apple_rootfs_build_staging_root(seed, staging);
    if (close(seed) < 0 && error == 0)
        error = ish_apple_rootfs_errno_or_io();
    if (staging >= 0 && close(staging) < 0 && error == 0)
        error = ish_apple_rootfs_errno_or_io();
    if (error != 0) {
        if (owner.marker_inode != 0)
            (void) ish_apple_rootfs_remove_owned_staging(parent, owner_name, &owner);
        return error;
    }

    if (renameatx_np(parent, owner.staging_name,
            parent, root_name, RENAME_EXCL) < 0) {
        int rename_error = ish_apple_rootfs_errno_or_io();
        int cleanup_error = ish_apple_rootfs_remove_owned_staging(
                parent, owner_name, &owner);
        if (cleanup_error != 0)
            return cleanup_error;
        if (rename_error != EEXIST)
            return rename_error;
        error = ish_apple_rootfs_inspect_existing_root(parent, root_name, &root_present);
        if (error != 0)
            return error;
        if (!root_present)
            return EAGAIN;
        *result = ISH_APPLE_ROOTFS_SEED_ALREADY_PRESENT;
        return 0;
    }

    error = ish_apple_rootfs_sync_directory_phase(parent,
            ISH_APPLE_ROOTFS_SEED_TEST_PUBLISH_ROOT_SYNC);
    if (error != 0) {
        int rollback_error = ish_apple_rootfs_rollback_unsynchronized_publish(
                parent, root_name, &owner);
        return rollback_error == 0 ? error : rollback_error;
    }
    /*
     * final 的父目录项已经持久化；owner 清理失败只会留下可识别的恢复凭据，
     * 不能把已提交安装重新报告成失败。
     */
    *result = ISH_APPLE_ROOTFS_SEED_INSTALLED;
    (void) ish_apple_rootfs_unlink_owner_marker(parent, owner_name, &owner,
            ISH_APPLE_ROOTFS_SEED_TEST_PUBLISH_OWNER_SYNC);
    return 0;
}

static int copy_managed_root_locked(
        const char *seed_root,
        int parent,
        const char *source_name,
        const char *destination_name,
        const char *owner_name,
        const char *operation_token) {
    bool source_present;
    int source = -1;
    int error = ish_apple_rootfs_open_existing_root(
            parent, source_name, &source_present, &source);
    if (error == EEXIST)
        return EINVAL;
    if (error != 0)
        return error;
    if (!source_present)
        return ENOENT;

    bool destination_present;
    int destination = -1;
    error = ish_apple_rootfs_open_existing_root(
            parent, destination_name,
            &destination_present, &destination);
    bool already_completed = false;
    if (error == 0 && destination_present) {
        if (operation_token == NULL) {
            error = EEXIST;
        } else {
            bool marker_present;
            bool marker_matches;
            error = ish_apple_rootfs_inspect_copy_operation_marker(
                    destination, source_name, destination_name,
                    operation_token, &marker_present, &marker_matches);
            if (error == 0 && (!marker_present || !marker_matches))
                error = EEXIST;
            if (error == 0)
                error = ish_apple_rootfs_verify_named_root_identity(
                        parent, destination_name, destination);
            if (error == 0)
                error = ish_apple_rootfs_sync_directory_internal(parent);
            if (error == 0) {
                already_completed = true;
                (void) ish_apple_rootfs_cleanup_staging_if_owned(
                        parent, owner_name, destination_name);
            }
        }
    }
    if (destination >= 0 && close(destination) < 0 && error == 0)
        error = ish_apple_rootfs_errno_or_io();
    if (already_completed) {
        (void) close(source);
        return 0;
    }

    int seed = -1;
    if (error == 0) {
        seed = open(seed_root,
                O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (seed < 0)
            error = ish_apple_rootfs_errno_or_io();
    }
    struct seed_manifest manifest;
    if (error == 0)
        error = ish_apple_rootfs_validate_seed_top(seed, &manifest);
    if (seed >= 0 && close(seed) < 0 && error == 0)
        error = ish_apple_rootfs_errno_or_io();
    if (error == 0)
        error = ish_apple_rootfs_recover_staging(
                parent, owner_name, destination_name);

    int staging = -1;
    struct staging_owner owner = {0};
    if (error == 0)
        error = ish_apple_rootfs_create_staging(parent, destination_name,
                owner_name, &staging, &owner);
    if (error == 0)
        error = ish_apple_rootfs_build_copied_root(
                source, staging, source_name,
                destination_name, operation_token);
    if (close(source) < 0 && error == 0)
        error = ish_apple_rootfs_errno_or_io();
    if (staging >= 0 && close(staging) < 0 && error == 0)
        error = ish_apple_rootfs_errno_or_io();
    if (error != 0) {
        if (owner.marker_inode != 0) {
            int cleanup_error = ish_apple_rootfs_remove_owned_staging(
                    parent, owner_name, &owner);
            if (cleanup_error != 0)
                return cleanup_error;
        }
        return error;
    }

    // 与 seed 安装共用排他发布，目录目标无论何种类型都不会被覆盖。
    if (renameatx_np(parent, owner.staging_name,
            parent, destination_name, RENAME_EXCL) < 0) {
        int rename_error = ish_apple_rootfs_errno_or_io();
        int cleanup_error = ish_apple_rootfs_remove_owned_staging(
                parent, owner_name, &owner);
        return cleanup_error != 0 ? cleanup_error : rename_error;
    }
    error = ish_apple_rootfs_sync_directory_phase(parent,
            ISH_APPLE_ROOTFS_SEED_TEST_PUBLISH_ROOT_SYNC);
    if (error != 0) {
        int rollback_error = ish_apple_rootfs_rollback_unsynchronized_publish(
                parent, destination_name, &owner);
        return rollback_error == 0 ? error : rollback_error;
    }
    // final 已持久化；后续 catalog 操作可以安全收敛残留 owner。
    (void) ish_apple_rootfs_unlink_owner_marker(parent, owner_name, &owner,
            ISH_APPLE_ROOTFS_SEED_TEST_PUBLISH_OWNER_SYNC);
    return 0;
}

int ish_apple_rootfs_seed_install(
        const char *seed_root,
        const char *persistent_parent,
        const char *root_name,
        enum ish_apple_rootfs_seed_result *result) {
    if (seed_root == NULL || persistent_parent == NULL ||
            result == NULL || !ish_apple_rootfs_name_is_valid(root_name))
        return EINVAL;

    char lock_name[NAME_MAX + 1];
    char owner_name[NAME_MAX + 1];
    int error = ish_apple_rootfs_format_private_name(
            lock_name, root_name, ".install.lock");
    if (error == 0)
        error = ish_apple_rootfs_format_private_name(
                owner_name, root_name, ".installing.owner");
    if (error != 0)
        return error;

    int parent = open(persistent_parent,
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (parent < 0)
        return ish_apple_rootfs_errno_or_io();
    int lock = -1;
    error = lock_file(parent, lock_name, LOCK_EX, &lock);
    if (error == 0)
        error = install_locked(seed_root, parent, root_name,
                owner_name, result);
    bool completed = error == 0;
    if (lock >= 0) {
        while (flock(lock, LOCK_UN) < 0 && errno == EINTR) {}
        close(lock);
    }
    if (close(parent) < 0 && error == 0)
        error = ish_apple_rootfs_errno_or_io();
    return completed ? 0 : error;
}

int ish_apple_rootfs_publish_imported_root(
        const char *seed_root,
        const char *persistent_parent,
        const char *imported_root,
        const char *destination_name,
        struct progress progress) {
    if (seed_root == NULL || seed_root[0] == '\0' ||
            persistent_parent == NULL || persistent_parent[0] == '\0' ||
            imported_root == NULL || imported_root[0] == '\0' ||
            !ish_apple_rootfs_name_is_valid(destination_name))
        return EINVAL;

    char lock_name[NAME_MAX + 1];
    char owner_name[NAME_MAX + 1];
    int error = ish_apple_rootfs_format_private_name(
            lock_name, destination_name, ".install.lock");
    if (error == 0)
        error = ish_apple_rootfs_format_private_name(
                owner_name, destination_name, ".installing.owner");
    if (error != 0)
        return error;

    int parent = open(persistent_parent,
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (parent < 0)
        return ish_apple_rootfs_errno_or_io();
    int lock = -1;
    error = lock_file(parent, lock_name, LOCK_EX, &lock);

    bool destination_present = false;
    if (error == 0)
        error = ish_apple_rootfs_inspect_existing_root(
                parent, destination_name, &destination_present);
    if (error == 0 && destination_present)
        error = EEXIST;
    if (error == 0)
        error = ish_apple_rootfs_recover_staging(
                parent, owner_name, destination_name);

    int seed = -1;
    int imported = -1;
    if (error == 0) {
        seed = open(seed_root,
                O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (seed < 0)
            error = ish_apple_rootfs_errno_or_io();
    }
    if (error == 0) {
        imported = open(imported_root,
                O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (imported < 0)
            error = ish_apple_rootfs_errno_or_io();
    }

    int staging = -1;
    struct staging_owner owner = {0};
    if (error == 0)
        error = ish_apple_rootfs_create_staging(parent, destination_name,
                owner_name, &staging, &owner);
    if (error == 0)
        error = ish_apple_rootfs_build_imported_root(
                seed, imported, staging, progress);
    if (seed >= 0 && close(seed) < 0 && error == 0)
        error = ish_apple_rootfs_errno_or_io();
    if (imported >= 0 && close(imported) < 0 && error == 0)
        error = ish_apple_rootfs_errno_or_io();
    if (staging >= 0 && close(staging) < 0 && error == 0)
        error = ish_apple_rootfs_errno_or_io();
    if (error != 0 && owner.marker_inode != 0) {
        int cleanup_error = ish_apple_rootfs_remove_owned_staging(
                parent, owner_name, &owner);
        if (cleanup_error != 0)
            error = cleanup_error;
    }

    if (error == 0 && renameatx_np(parent, owner.staging_name,
            parent, destination_name, RENAME_EXCL) < 0) {
        error = ish_apple_rootfs_errno_or_io();
        int cleanup_error = ish_apple_rootfs_remove_owned_staging(
                parent, owner_name, &owner);
        if (cleanup_error != 0)
            error = cleanup_error;
    }
    if (error == 0) {
        error = ish_apple_rootfs_sync_directory_phase(parent,
                ISH_APPLE_ROOTFS_SEED_TEST_PUBLISH_ROOT_SYNC);
        if (error != 0) {
            int rollback_error = ish_apple_rootfs_rollback_unsynchronized_publish(
                    parent, destination_name, &owner);
            if (rollback_error != 0)
                error = rollback_error;
        }
    }
    if (error == 0)
        (void) ish_apple_rootfs_unlink_owner_marker(parent, owner_name, &owner,
                ISH_APPLE_ROOTFS_SEED_TEST_PUBLISH_OWNER_SYNC);

    bool completed = error == 0;
    if (lock >= 0) {
        while (flock(lock, LOCK_UN) < 0 && errno == EINTR) {}
        close(lock);
    }
    if (close(parent) < 0 && error == 0)
        error = ish_apple_rootfs_errno_or_io();
    return completed ? 0 : error;
}

static int copy_claimed_managed_root(
        const char *seed_root,
        const char *persistent_parent,
        const char *source_name,
        const char *destination_name,
        const char *operation_token) {
    if (seed_root == NULL || seed_root[0] == '\0' ||
            persistent_parent == NULL || persistent_parent[0] == '\0' ||
            !ish_apple_rootfs_name_is_valid(source_name) ||
            !ish_apple_rootfs_name_is_valid(destination_name) ||
            (operation_token != NULL &&
             !ish_apple_rootfs_copy_operation_token_is_valid(
                     operation_token)) ||
            strcmp(source_name, destination_name) == 0)
        return EINVAL;

    char lock_name[NAME_MAX + 1];
    char owner_name[NAME_MAX + 1];
    int error = ish_apple_rootfs_format_private_name(
            lock_name, destination_name, ".install.lock");
    if (error == 0)
        error = ish_apple_rootfs_format_private_name(
                owner_name, destination_name, ".installing.owner");
    if (error != 0)
        return error;

    int parent = open(persistent_parent,
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (parent < 0)
        return ish_apple_rootfs_errno_or_io();
    int lock = -1;
    error = lock_file(parent, lock_name, LOCK_EX, &lock);
    if (error == 0)
        error = copy_managed_root_locked(seed_root, parent,
                source_name, destination_name,
                owner_name, operation_token);
    bool completed = error == 0;
    if (lock >= 0) {
        while (flock(lock, LOCK_UN) < 0 && errno == EINTR) {}
        close(lock);
    }
    if (close(parent) < 0 && error == 0)
        error = ish_apple_rootfs_errno_or_io();
    return completed ? 0 : error;
}

int ish_apple_rootfs_copy_claimed_managed_root_for_operation(
        const char *seed_root,
        const char *persistent_parent,
        const char *source_name,
        const char *destination_name,
        const char *operation_token) {
    if (!ish_apple_rootfs_copy_operation_token_is_valid(
                operation_token))
        return EINVAL;
    return copy_claimed_managed_root(
            seed_root, persistent_parent,
            source_name, destination_name, operation_token);
}

int ish_apple_rootfs_copy_managed_root(
        const char *seed_root,
        const char *persistent_parent,
        const char *source_name,
        const char *destination_name) {
    if (seed_root == NULL || seed_root[0] == '\0' ||
            persistent_parent == NULL || persistent_parent[0] == '\0' ||
            !ish_apple_rootfs_name_is_valid(source_name) ||
            !ish_apple_rootfs_name_is_valid(destination_name) ||
            strcmp(source_name, destination_name) == 0)
        return EINVAL;

    int source_lock = -1;
    int error = ish_apple_rootfs_lock_managed_root(
            persistent_parent, source_name, true, true, &source_lock);
    if (error == EEXIST)
        error = EINVAL;
    if (error != 0)
        return error;
    error = copy_claimed_managed_root(
            seed_root, persistent_parent,
            source_name, destination_name, NULL);
    bool completed = error == 0;
    int unlock_error =
            ish_apple_rootfs_unlock_managed_root(source_lock);
    if (error == 0)
        error = unlock_error;
    return completed ? 0 : error;
}

int ish_apple_rootfs_find_managed_copy_operation(
        const char *persistent_parent,
        const char *source_name,
        const char *operation_token,
        char *destination_name,
        size_t destination_capacity,
        bool *found) {
    if (persistent_parent == NULL || persistent_parent[0] == '\0' ||
            !ish_apple_rootfs_name_is_valid(source_name) ||
            !ish_apple_rootfs_copy_operation_token_is_valid(
                    operation_token) ||
            destination_name == NULL || destination_capacity == 0 ||
            found == NULL)
        return EINVAL;
    destination_name[0] = '\0';
    *found = false;

    int parent = open(persistent_parent,
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (parent < 0)
        return ish_apple_rootfs_errno_or_io();
    int iterator_file = dup(parent);
    if (iterator_file < 0) {
        int error = ish_apple_rootfs_errno_or_io();
        close(parent);
        return error;
    }
    DIR *iterator = fdopendir(iterator_file);
    if (iterator == NULL) {
        int error = ish_apple_rootfs_errno_or_io();
        close(iterator_file);
        close(parent);
        return error;
    }

    int error = 0;
    errno = 0;
    struct dirent *entry;
    while ((entry = readdir(iterator)) != NULL) {
        if (!ish_apple_rootfs_name_is_valid(entry->d_name)) {
            errno = 0;
            continue;
        }

        bool root_present;
        int root = -1;
        error = ish_apple_rootfs_open_existing_root(
                parent, entry->d_name, &root_present, &root);
        if (error == EEXIST) {
            error = 0;
            errno = 0;
            continue;
        }
        if (error != 0)
            break;
        if (!root_present) {
            errno = 0;
            continue;
        }

        bool marker_present;
        bool marker_matches;
        error = ish_apple_rootfs_inspect_copy_operation_marker(
                root, source_name, entry->d_name,
                operation_token, &marker_present, &marker_matches);
        if (error == 0)
            error = ish_apple_rootfs_verify_named_root_identity(
                    parent, entry->d_name, root);
        if (close(root) < 0 && error == 0)
            error = ish_apple_rootfs_errno_or_io();
        if (error != 0)
            break;
        if (!marker_present) {
            errno = 0;
            continue;
        }
        if (!marker_matches ||
                strcmp(entry->d_name, source_name) == 0) {
            error = EEXIST;
            break;
        }
        if (*found &&
                strcmp(destination_name, entry->d_name) != 0) {
            error = EEXIST;
            break;
        }
        size_t name_length = strlen(entry->d_name);
        if (name_length >= destination_capacity) {
            error = ERANGE;
            break;
        }
        memcpy(destination_name, entry->d_name, name_length + 1);
        *found = true;
        errno = 0;
    }
    if (entry == NULL && errno != 0 && error == 0)
        error = ish_apple_rootfs_errno_or_io();
    if (closedir(iterator) < 0 && error == 0)
        error = ish_apple_rootfs_errno_or_io();
    if (close(parent) < 0 && error == 0)
        error = ish_apple_rootfs_errno_or_io();
    return error;
}
