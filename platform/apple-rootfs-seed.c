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
#include "util/fchdir.h"

#ifndef __APPLE__
#error "Apple rootfs seed 安装器只能构建到 Apple 平台"
#endif

#define ROOT_NAME_LIMIT 128
#define MANIFEST_LIMIT (64 * 1024)
#define HARDLINK_MANIFEST_LIMIT (16 * 1024 * 1024)
#define COPY_BUFFER_SIZE (16 * 1024)
#define COPY_TREE_DEPTH_LIMIT 256
#define REMOVE_TREE_DEPTH_LIMIT 512
#define OWNER_TOKEN_BYTES 16
#define OWNER_TOKEN_HEX_LENGTH (OWNER_TOKEN_BYTES * 2)
#define OWNER_RECORD_LIMIT 512
#define COPY_OPERATION_TOKEN_LIMIT 128
#define COPY_OPERATION_RECORD_LIMIT 512

static const char rootfs_manifest_name[] = "rootfs-manifest.txt";
static const char hardlink_manifest_name[] = "rootfs-hardlinks.tsv";
static const char install_receipt_name[] = "rootfs-installation.txt";
static const char copy_operation_marker_prefix[] =
        ".ish-copy-operation.";
static const char owner_format[] = "format=ish-rootfs-install-owner-v2";
static const char receipt_format[] = "format=ish-rootfs-install-v1\n";
static const char copy_operation_format[] =
        "format=ish-rootfs-copy-operation-v1";

struct seed_manifest {
    char archive_sha256[65];
};

struct hardlink_entry {
    char *canonical;
    char *member;
    sqlite3_int64 database_inode;
};

struct hardlink_manifest {
    char *storage;
    struct hardlink_entry *entries;
    size_t count;
};

struct relative_parent {
    int directory;
    char leaf[NAME_MAX + 1];
};

struct copied_hardlink {
    dev_t device;
    ino_t inode;
    char *destination_path;
};

struct root_copy_context {
    int destination_root;
    struct copied_hardlink *hardlinks;
    size_t hardlink_count;
    size_t hardlink_capacity;
    struct progress progress;
    uintmax_t total_bytes;
    uintmax_t copied_bytes;
    double progress_span;
};

struct staging_owner {
    char staging_name[NAME_MAX + 1];
    uintmax_t staging_device;
    uintmax_t staging_inode;
    uintmax_t marker_device;
    uintmax_t marker_inode;
};

enum owner_state {
    OWNER_MISSING,
    OWNER_VALID,
    OWNER_UNKNOWN,
};

enum ish_apple_rootfs_seed_test_phase {
    ISH_APPLE_ROOTFS_SEED_TEST_NONE,
    ISH_APPLE_ROOTFS_SEED_TEST_CLEANUP_STAGING_SYNC,
    ISH_APPLE_ROOTFS_SEED_TEST_CLEANUP_OWNER_SYNC,
    ISH_APPLE_ROOTFS_SEED_TEST_PUBLISH_ROOT_SYNC,
    ISH_APPLE_ROOTFS_SEED_TEST_PUBLISH_OWNER_UNLINK,
    ISH_APPLE_ROOTFS_SEED_TEST_PUBLISH_OWNER_SYNC,
};

#ifdef ISH_APPLE_ROOTFS_SEED_TESTING
int ish_apple_rootfs_seed_test_fail_phase;
int ish_apple_rootfs_seed_test_force_sparse_fallback;
size_t ish_apple_rootfs_seed_test_write_limit;
unsigned ish_apple_rootfs_seed_test_sparse_fallback_count;
unsigned ish_apple_rootfs_seed_test_limited_write_count;
#endif

static int errno_or_io(void) {
    return errno == 0 ? EIO : errno;
}

static int sync_directory(int directory) {
    // 部分 Apple 文件系统不接受目录 fsync；文件内容仍需严格 fsync。
    if (fsync(directory) == 0)
        return 0;
    if (errno == EINVAL || errno == ENOTSUP)
        return 0;
    return errno_or_io();
}

int ish_apple_rootfs_sync_directory(int directory) {
    return sync_directory(directory);
}

static int sync_directory_phase(int directory, int phase) {
#ifdef ISH_APPLE_ROOTFS_SEED_TESTING
    if (ish_apple_rootfs_seed_test_fail_phase == phase) {
        ish_apple_rootfs_seed_test_fail_phase =
                ISH_APPLE_ROOTFS_SEED_TEST_NONE;
        return EIO;
    }
#else
    (void) phase;
#endif
    return sync_directory(directory);
}

static int sqlite_error(sqlite3 *database) {
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

static bool valid_root_name(const char *name) {
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

static int format_copy_operation_marker_name(
        char output[NAME_MAX + 1],
        const char *operation_token) {
    if (!ish_apple_rootfs_copy_operation_token_is_valid(operation_token))
        return EINVAL;
    int length = snprintf(output, NAME_MAX + 1,
            "%s%s", copy_operation_marker_prefix, operation_token);
    return length < 0 || length > NAME_MAX ? ENAMETOOLONG : 0;
}

static int format_private_name(
        char output[NAME_MAX + 1], const char *root_name,
        const char *suffix) {
    int length = snprintf(output, NAME_MAX + 1, ".%s%s", root_name, suffix);
    if (length < 0 || length > NAME_MAX)
        return ENAMETOOLONG;
    return 0;
}

static int write_all(int file, const void *bytes, size_t length) {
    const unsigned char *cursor = bytes;
    while (length != 0) {
        size_t request = length;
#ifdef ISH_APPLE_ROOTFS_SEED_TESTING
        if (ish_apple_rootfs_seed_test_write_limit != 0 &&
                request > ish_apple_rootfs_seed_test_write_limit) {
            request = ish_apple_rootfs_seed_test_write_limit;
            ish_apple_rootfs_seed_test_limited_write_count++;
        }
#endif
        ssize_t written = write(file, cursor, request);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            return errno_or_io();
        }
        if (written == 0)
            return EIO;
        cursor += (size_t) written;
        length -= (size_t) written;
    }
    return 0;
}

static int seek_file(
        int file, off_t offset, int operation, off_t *result_out) {
    off_t result;
    do {
        result = lseek(file, offset, operation);
    } while (result < 0 && errno == EINTR);
    if (result < 0)
        return errno_or_io();
    *result_out = result;
    return 0;
}

static int report_root_copy_progress(
        struct root_copy_context *context,
        uintmax_t copied,
        const char *path) {
    if (UINTMAX_MAX - context->copied_bytes < copied)
        context->copied_bytes = UINTMAX_MAX;
    else
        context->copied_bytes += copied;
    if (context->progress.callback == NULL)
        return 0;
    double fraction = context->total_bytes == 0 ? 1 :
            (double) context->copied_bytes /
            (double) context->total_bytes;
    if (fraction > 1)
        fraction = 1;
    if (context->progress_span > 0)
        fraction *= context->progress_span;
    bool cancelled = false;
    context->progress.callback(
            context->progress.cookie, fraction, path, &cancelled);
    return cancelled ? ECANCELED : 0;
}

static int report_copy_stage(
        struct progress progress,
        double fraction,
        const char *message) {
    if (progress.callback == NULL)
        return 0;
    bool cancelled = false;
    progress.callback(
            progress.cookie, fraction, message, &cancelled);
    return cancelled ? ECANCELED : 0;
}

static int copy_file_range(
        int source, int destination, off_t length,
        const char *path,
        struct root_copy_context *context) {
    unsigned char buffer[COPY_BUFFER_SIZE];
    while (length > 0) {
        size_t request = length < (off_t) sizeof(buffer) ?
                (size_t) length : sizeof(buffer);
        ssize_t count = read(source, buffer, request);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            return errno_or_io();
        }
        if (count == 0)
            return EIO;
        int error = write_all(destination, buffer, (size_t) count);
        if (error != 0)
            return error;
        error = report_root_copy_progress(
                context, (uintmax_t) count, path);
        if (error != 0)
            return error;
        length -= count;
    }
    return 0;
}

static bool bytes_are_zero(const unsigned char *bytes, size_t length) {
    for (size_t index = 0; index < length; index++) {
        if (bytes[index] != 0)
            return false;
    }
    return true;
}

static int copy_file_sparse_fallback(
        int source, int destination, off_t logical_size,
        const char *path,
        struct root_copy_context *context) {
#ifdef ISH_APPLE_ROOTFS_SEED_TESTING
    ish_apple_rootfs_seed_test_sparse_fallback_count++;
#endif
    if (ftruncate(destination, 0) < 0)
        return errno_or_io();
    off_t position;
    int error = seek_file(source, 0, SEEK_SET, &position);
    if (error == 0)
        error = seek_file(destination, 0, SEEK_SET, &position);

    unsigned char buffer[COPY_BUFFER_SIZE];
    off_t copied = 0;
    while (error == 0 && copied < logical_size) {
        off_t remaining = logical_size - copied;
        size_t request = remaining < (off_t) sizeof(buffer) ?
                (size_t) remaining : sizeof(buffer);
        ssize_t count = read(source, buffer, request);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            error = errno_or_io();
        } else if (count == 0) {
            error = EIO;
        } else if (bytes_are_zero(buffer, (size_t) count)) {
            error = seek_file(
                    destination, count, SEEK_CUR, &position);
        } else {
            error = write_all(destination, buffer, (size_t) count);
        }
        if (count > 0)
            copied += count;
        if (error == 0 && count > 0)
            error = report_root_copy_progress(
                    context, (uintmax_t) count, path);
    }
    if (error == 0 && ftruncate(destination, logical_size) < 0)
        error = errno_or_io();
    return error;
}

static int copy_file_preserving_holes(
        int source, int destination, off_t logical_size,
        const char *path,
        struct root_copy_context *context) {
    if (logical_size < 0)
        return EINVAL;
#ifdef ISH_APPLE_ROOTFS_SEED_TESTING
    if (ish_apple_rootfs_seed_test_force_sparse_fallback)
        return copy_file_sparse_fallback(
                source, destination, logical_size, path, context);
#endif
    off_t cursor = 0;
    while (cursor < logical_size) {
        off_t data;
        int error = seek_file(source, cursor, SEEK_DATA, &data);
        if (error == ENXIO)
            break;
        if (error == EINVAL || error == ENOTSUP)
            return copy_file_sparse_fallback(
                    source, destination, logical_size, path, context);
        if (error != 0)
            return error;
        if (data >= logical_size)
            break;

        off_t hole;
        error = seek_file(source, data, SEEK_HOLE, &hole);
        if (error == EINVAL || error == ENOTSUP)
            return copy_file_sparse_fallback(
                    source, destination, logical_size, path, context);
        if (error == ENXIO)
            hole = logical_size;
        else if (error != 0)
            return error;
        if (hole > logical_size)
            hole = logical_size;
        if (hole <= data)
            return EIO;

        off_t position;
        error = seek_file(source, data, SEEK_SET, &position);
        if (error == 0)
            error = seek_file(destination, data, SEEK_SET, &position);
        if (error == 0)
            error = copy_file_range(
                    source, destination, hole - data,
                    path, context);
        if (error != 0)
            return error;
        cursor = hole;
    }
    return ftruncate(destination, logical_size) < 0 ?
            errno_or_io() : 0;
}

static int create_regular_at(
        int directory, const char *name,
        const void *bytes, size_t length, bool synchronize) {
    int file = openat(directory, name,
            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (file < 0)
        return errno_or_io();
    int error = write_all(file, bytes, length);
    if (error == 0 && synchronize && fsync(file) < 0)
        error = errno_or_io();
    if (close(file) < 0 && error == 0)
        error = errno_or_io();
    if (error != 0)
        unlinkat(directory, name, 0);
    return error;
}

static int read_regular_at(
        int directory, const char *name, size_t limit,
        char **bytes_out, size_t *length_out) {
    int file = openat(directory, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (file < 0)
        return errno_or_io();

    struct stat metadata = {0};
    int error = 0;
    if (fstat(file, &metadata) < 0)
        error = errno_or_io();
    else if (!S_ISREG(metadata.st_mode))
        error = EINVAL;
    else if (metadata.st_size < 0 || (uintmax_t) metadata.st_size > limit)
        error = EFBIG;

    size_t length = 0;
    char *bytes = NULL;
    if (error == 0) {
        length = (size_t) metadata.st_size;
        bytes = malloc(length + 1);
        if (bytes == NULL)
            error = ENOMEM;
    }
    size_t offset = 0;
    while (error == 0 && offset < length) {
        ssize_t count = read(file, bytes + offset, length - offset);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            error = errno_or_io();
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
            error = errno_or_io();
        else if (count != 0)
            error = EFBIG;
    }
    if (close(file) < 0 && error == 0)
        error = errno_or_io();
    if (error != 0) {
        free(bytes);
        return error;
    }
    bytes[length] = '\0';
    *bytes_out = bytes;
    *length_out = length;
    return 0;
}

static int remove_entry_at(int parent, const char *name, unsigned depth) {
    if (depth > REMOVE_TREE_DEPTH_LIMIT)
        return ELOOP;
    struct stat metadata;
    if (fstatat(parent, name, &metadata, AT_SYMLINK_NOFOLLOW) < 0)
        return errno_or_io();
    if (!S_ISDIR(metadata.st_mode)) {
        if (unlinkat(parent, name, 0) < 0)
            return errno_or_io();
        return 0;
    }

    int directory = openat(parent, name,
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (directory < 0)
        return errno_or_io();
    int iterator_fd = dup(directory);
    if (iterator_fd < 0) {
        int error = errno_or_io();
        close(directory);
        return error;
    }
    DIR *iterator = fdopendir(iterator_fd);
    if (iterator == NULL) {
        int error = errno_or_io();
        close(iterator_fd);
        close(directory);
        return error;
    }

    int error = 0;
    errno = 0;
    struct dirent *entry;
    while ((entry = readdir(iterator)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 ||
                strcmp(entry->d_name, "..") == 0)
            continue;
        error = remove_entry_at(directory, entry->d_name, depth + 1);
        if (error != 0)
            break;
        errno = 0;
    }
    if (entry == NULL && errno != 0 && error == 0)
        error = errno_or_io();
    if (closedir(iterator) < 0 && error == 0)
        error = errno_or_io();
    if (close(directory) < 0 && error == 0)
        error = errno_or_io();
    if (error == 0 && unlinkat(parent, name, AT_REMOVEDIR) < 0)
        error = errno_or_io();
    return error;
}

int ish_apple_rootfs_remove_entry_at(int parent, const char *name) {
    return remove_entry_at(parent, name, 0);
}

static int copy_regular_at(
        int source_directory, int destination_directory,
        const char *name) {
    int source = openat(source_directory, name,
            O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (source < 0)
        return errno_or_io();
    struct stat metadata = {0};
    int error = 0;
    if (fstat(source, &metadata) < 0)
        error = errno_or_io();
    else if (!S_ISREG(metadata.st_mode))
        error = EINVAL;

    int destination = -1;
    if (error == 0) {
        destination = openat(destination_directory, name,
                O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                0600);
        if (destination < 0)
            error = errno_or_io();
    }
    unsigned char buffer[COPY_BUFFER_SIZE];
    while (error == 0) {
        ssize_t count = read(source, buffer, sizeof(buffer));
        if (count < 0) {
            if (errno == EINTR)
                continue;
            error = errno_or_io();
        } else if (count == 0) {
            break;
        } else {
            error = write_all(destination, buffer, (size_t) count);
        }
    }
    if (destination >= 0 && error == 0 && fsync(destination) < 0)
        error = errno_or_io();
    if (destination >= 0 && close(destination) < 0 && error == 0)
        error = errno_or_io();
    if (close(source) < 0 && error == 0)
        error = errno_or_io();
    if (error != 0 && destination >= 0)
        unlinkat(destination_directory, name, 0);
    return error;
}

static int copy_directory_contents(
        int source, int destination, unsigned depth) {
    if (depth > COPY_TREE_DEPTH_LIMIT)
        return ELOOP;
    int iterator_fd = dup(source);
    if (iterator_fd < 0)
        return errno_or_io();
    DIR *iterator = fdopendir(iterator_fd);
    if (iterator == NULL) {
        int error = errno_or_io();
        close(iterator_fd);
        return error;
    }

    int error = 0;
    errno = 0;
    struct dirent *entry;
    while ((entry = readdir(iterator)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 ||
                strcmp(entry->d_name, "..") == 0)
            continue;
        struct stat metadata;
        if (fstatat(source, entry->d_name, &metadata,
                AT_SYMLINK_NOFOLLOW) < 0) {
            error = errno_or_io();
            break;
        }
        if (S_ISREG(metadata.st_mode)) {
            error = copy_regular_at(source, destination, entry->d_name);
        } else if (S_ISDIR(metadata.st_mode)) {
            // 在创建下一层前拒绝深树，确保失败 staging 总能被删除器收回。
            if (depth >= COPY_TREE_DEPTH_LIMIT) {
                error = ELOOP;
                break;
            }
            if (mkdirat(destination, entry->d_name, 0700) < 0) {
                error = errno_or_io();
            } else {
                int source_child = openat(source, entry->d_name,
                        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
                int destination_child = -1;
                if (source_child < 0) {
                    error = errno_or_io();
                } else {
                    destination_child = openat(destination, entry->d_name,
                            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
                    if (destination_child < 0)
                        error = errno_or_io();
                    else
                        error = copy_directory_contents(source_child,
                                destination_child, depth + 1);
                }
                if (source_child >= 0 && close(source_child) < 0 && error == 0)
                    error = errno_or_io();
                if (destination_child >= 0 &&
                        close(destination_child) < 0 && error == 0)
                    error = errno_or_io();
            }
        } else {
            error = EINVAL;
        }
        if (error != 0)
            break;
        errno = 0;
    }
    if (entry == NULL && errno != 0 && error == 0)
        error = errno_or_io();
    if (closedir(iterator) < 0 && error == 0)
        error = errno_or_io();
    if (error == 0)
        error = sync_directory(destination);
    return error;
}

static void root_copy_context_destroy(struct root_copy_context *context) {
    for (size_t index = 0; index < context->hardlink_count; index++)
        free(context->hardlinks[index].destination_path);
    free(context->hardlinks);
    *context = (struct root_copy_context) {0};
}

static const char *copied_hardlink_path(
        const struct root_copy_context *context,
        const struct stat *metadata) {
    for (size_t index = 0; index < context->hardlink_count; index++) {
        const struct copied_hardlink *entry = &context->hardlinks[index];
        if (entry->device == metadata->st_dev &&
                entry->inode == metadata->st_ino)
            return entry->destination_path;
    }
    return NULL;
}

static int remember_copied_hardlink(
        struct root_copy_context *context,
        const struct stat *metadata,
        const char *destination_path) {
    if (context->hardlink_count == context->hardlink_capacity) {
        size_t next_capacity = context->hardlink_capacity == 0 ?
                8 : context->hardlink_capacity * 2;
        if (next_capacity < context->hardlink_capacity ||
                next_capacity > SIZE_MAX / sizeof(*context->hardlinks))
            return ENOMEM;
        void *grown = realloc(context->hardlinks,
                next_capacity * sizeof(*context->hardlinks));
        if (grown == NULL)
            return ENOMEM;
        context->hardlinks = grown;
        context->hardlink_capacity = next_capacity;
    }
    char *path = strdup(destination_path);
    if (path == NULL)
        return ENOMEM;
    context->hardlinks[context->hardlink_count++] =
            (struct copied_hardlink) {
                .device = metadata->st_dev,
                .inode = metadata->st_ino,
                .destination_path = path,
            };
    return 0;
}

static void stat_times(
        const struct stat *metadata,
        struct timespec times[2]) {
    times[0] = metadata->st_atimespec;
    times[1] = metadata->st_mtimespec;
}

static int restore_times_at(
        int directory,
        const char *name,
        const struct stat *metadata) {
    struct timespec times[2];
    stat_times(metadata, times);
    return utimensat(
            directory, name, times, AT_SYMLINK_NOFOLLOW) < 0 ?
            errno_or_io() : 0;
}

static int copy_regular_preserving_hardlinks(
        int source_directory,
        int destination_directory,
        const char *name,
        const char *destination_path,
        struct root_copy_context *context) {
    int source = openat(source_directory, name,
            O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (source < 0)
        return errno_or_io();
    struct stat metadata;
    int error = 0;
    if (fstat(source, &metadata) < 0)
        error = errno_or_io();
    else if (!S_ISREG(metadata.st_mode))
        error = EINVAL;

    const char *existing_path = error == 0 && metadata.st_nlink > 1 ?
            copied_hardlink_path(context, &metadata) : NULL;
    if (existing_path != NULL) {
        if (linkat(context->destination_root, existing_path,
                destination_directory, name, 0) < 0)
            error = errno_or_io();
        if (error == 0)
            error = report_root_copy_progress(
                    context, (uintmax_t) metadata.st_size,
                    destination_path);
        if (close(source) < 0 && error == 0)
            error = errno_or_io();
        return error;
    }

    int destination = -1;
    if (error == 0) {
        destination = openat(destination_directory, name,
                O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                0600);
        if (destination < 0)
            error = errno_or_io();
    }
    /*
     * fakefs 的 guest 权限、属主和时间保存在 meta.db；宿主对象只承担内容
     * 与进程访问控制，因此只复制 POSIX rwx 位，不传播 ACL、flags 或 xattr。
     */
    uintmax_t copied_before = context->copied_bytes;
    if (error == 0)
        error = copy_file_preserving_holes(
                source, destination, metadata.st_size,
                destination_path, context);
    uintmax_t copied_for_file =
            context->copied_bytes - copied_before;
    if (error == 0 &&
            copied_for_file < (uintmax_t) metadata.st_size)
        error = report_root_copy_progress(
                context,
                (uintmax_t) metadata.st_size - copied_for_file,
                destination_path);
    if (destination >= 0 && error == 0 &&
            fchmod(destination, metadata.st_mode & 0777) < 0)
        error = errno_or_io();
    if (destination >= 0 && error == 0) {
        struct timespec times[2];
        stat_times(&metadata, times);
        if (futimens(destination, times) < 0)
            error = errno_or_io();
    }
    if (destination >= 0 && error == 0 && fsync(destination) < 0)
        error = errno_or_io();
    if (destination >= 0 && close(destination) < 0 && error == 0)
        error = errno_or_io();
    if (close(source) < 0 && error == 0)
        error = errno_or_io();
    if (error == 0 && metadata.st_nlink > 1)
        error = remember_copied_hardlink(
                context, &metadata, destination_path);
    if (error != 0 && destination >= 0)
        unlinkat(destination_directory, name, 0);
    return error;
}

static int copy_fifo_preserving_hardlinks(
        int destination_directory,
        const char *name,
        const char *destination_path,
        const struct stat *metadata,
        struct root_copy_context *context) {
    const char *existing_path = metadata->st_nlink > 1 ?
            copied_hardlink_path(context, metadata) : NULL;
    int error = 0;
    if (existing_path != NULL) {
        if (linkat(context->destination_root, existing_path,
                destination_directory, name, 0) < 0)
            error = errno_or_io();
        if (error == 0)
            error = restore_times_at(
                    destination_directory, name, metadata);
        return error;
    }
    lock_fchdir(destination_directory);
    int result = mkfifo(name, metadata->st_mode & 0777);
    int saved_errno = errno;
    unlock_fchdir();
    errno = saved_errno;
    if (result < 0)
        return errno_or_io();
    if (metadata->st_nlink > 1)
        error = remember_copied_hardlink(
                context, metadata, destination_path);
    if (error == 0)
        error = restore_times_at(
                destination_directory, name, metadata);
    if (error != 0)
        unlinkat(destination_directory, name, 0);
    return error;
}

static int format_copy_path(
        char output[PATH_MAX],
        const char *parent_path,
        const char *name) {
    int length = parent_path[0] == '\0' ?
            snprintf(output, PATH_MAX, "%s", name) :
            snprintf(output, PATH_MAX, "%s/%s", parent_path, name);
    return length < 0 || length >= PATH_MAX ? ENAMETOOLONG : 0;
}

static int copy_managed_root_contents(
        int source,
        int destination,
        const char *destination_path,
        unsigned depth,
        struct root_copy_context *context) {
    if (depth > COPY_TREE_DEPTH_LIMIT)
        return ELOOP;
    int iterator_file = openat(source, ".",
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (iterator_file < 0)
        return errno_or_io();
    DIR *iterator = fdopendir(iterator_file);
    if (iterator == NULL) {
        int error = errno_or_io();
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
        // 复制操作凭据属于宿主事务元数据，不能继承到下一代副本。
        if (depth == 0 && strncmp(
                entry->d_name, copy_operation_marker_prefix,
                sizeof(copy_operation_marker_prefix) - 1) == 0)
            continue;
        char child_path[PATH_MAX];
        error = format_copy_path(
                child_path, destination_path, entry->d_name);
        if (error != 0)
            break;
        error = report_root_copy_progress(
                context, 0, child_path);
        if (error != 0)
            break;

        struct stat metadata;
        if (fstatat(source, entry->d_name, &metadata,
                AT_SYMLINK_NOFOLLOW) < 0) {
            error = errno_or_io();
            break;
        }
        if (S_ISREG(metadata.st_mode)) {
            error = copy_regular_preserving_hardlinks(
                    source, destination, entry->d_name,
                    child_path, context);
        } else if (S_ISFIFO(metadata.st_mode)) {
            error = copy_fifo_preserving_hardlinks(
                    destination, entry->d_name,
                    child_path, &metadata, context);
        } else if (S_ISDIR(metadata.st_mode)) {
            if (depth >= COPY_TREE_DEPTH_LIMIT) {
                error = ELOOP;
                break;
            }
            if (mkdirat(destination, entry->d_name, 0700) < 0) {
                error = errno_or_io();
            } else {
                int source_child = openat(source, entry->d_name,
                        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
                int destination_child = -1;
                struct stat opened_metadata;
                if (source_child < 0) {
                    error = errno_or_io();
                } else if (fstat(source_child, &opened_metadata) < 0) {
                    error = errno_or_io();
                } else if (opened_metadata.st_dev != metadata.st_dev ||
                        opened_metadata.st_ino != metadata.st_ino) {
                    error = EAGAIN;
                } else {
                    destination_child = openat(destination, entry->d_name,
                            O_RDONLY | O_DIRECTORY |
                            O_CLOEXEC | O_NOFOLLOW);
                    if (destination_child < 0)
                        error = errno_or_io();
                    else
                        error = copy_managed_root_contents(
                                source_child, destination_child,
                                child_path, depth + 1, context);
                }
                if (destination_child >= 0 && error == 0 &&
                        fchmod(destination_child,
                                metadata.st_mode & 0777) < 0)
                    error = errno_or_io();
                if (destination_child >= 0 && error == 0) {
                    struct timespec times[2];
                    stat_times(&metadata, times);
                    if (futimens(destination_child, times) < 0)
                        error = errno_or_io();
                }
                if (destination_child >= 0 && error == 0)
                    error = sync_directory(destination_child);
                if (source_child >= 0 &&
                        close(source_child) < 0 && error == 0)
                    error = errno_or_io();
                if (destination_child >= 0 &&
                        close(destination_child) < 0 && error == 0)
                    error = errno_or_io();
            }
        } else {
            // fakefs 仅用宿主 FIFO 表示 guest FIFO；其他 guest 特殊对象是普通占位文件。
            error = EINVAL;
        }
        if (error != 0)
            break;
        errno = 0;
    }
    if (entry == NULL && errno != 0 && error == 0)
        error = errno_or_io();
    if (closedir(iterator) < 0 && error == 0)
        error = errno_or_io();
    if (error == 0)
        error = sync_directory(destination);
    return error;
}

static int take_line(
        char **cursor, char *end, char **line, size_t *length) {
    if (*cursor == end)
        return EINVAL;
    char *newline = memchr(*cursor, '\n', (size_t) (end - *cursor));
    if (newline == NULL)
        return EINVAL;
    *line = *cursor;
    *length = (size_t) (newline - *cursor);
    *newline = '\0';
    *cursor = newline + 1;
    return 0;
}

static bool line_equals(const char *line, size_t length, const char *expected) {
    return strlen(expected) == length && memcmp(line, expected, length) == 0;
}

static bool line_has_nonempty_value(
        const char *line, size_t length, const char *prefix) {
    size_t prefix_length = strlen(prefix);
    return length > prefix_length &&
            memcmp(line, prefix, prefix_length) == 0;
}

static int format_copy_operation_record(
        char output[COPY_OPERATION_RECORD_LIMIT + 1],
        const char *source_name,
        const char *destination_name,
        const char *operation_token) {
    if (!valid_root_name(source_name) ||
            !valid_root_name(destination_name) ||
            !ish_apple_rootfs_copy_operation_token_is_valid(
                    operation_token))
        return EINVAL;
    int length = snprintf(output, COPY_OPERATION_RECORD_LIMIT + 1,
            "%s\ntoken=%s\nsource=%s\ndestination=%s\n",
            copy_operation_format, operation_token,
            source_name, destination_name);
    return length < 0 || length > COPY_OPERATION_RECORD_LIMIT ?
            EOVERFLOW : 0;
}

static bool valid_sha256(const char *digest, size_t length) {
    if (length != 64)
        return false;
    for (size_t i = 0; i < length; i++) {
        if (!((digest[i] >= '0' && digest[i] <= '9') ||
                (digest[i] >= 'a' && digest[i] <= 'f')))
            return false;
    }
    return true;
}

static int parse_seed_manifest(
        char *bytes, size_t length, struct seed_manifest *manifest) {
    if (memchr(bytes, '\0', length) != NULL)
        return EINVAL;
    char *cursor = bytes;
    char *end = bytes + length;
    char *line;
    size_t line_length;
    int error = take_line(&cursor, end, &line, &line_length);
    if (error != 0 || !line_equals(line, line_length,
            "format=ish-fakefs-v3"))
        return EINVAL;
    error = take_line(&cursor, end, &line, &line_length);
    if (error != 0 || !line_equals(line, line_length,
            "packager=apple-aarch64-rootfs-v1"))
        return EINVAL;
    error = take_line(&cursor, end, &line, &line_length);
    if (error != 0 || !line_equals(line, line_length,
            "guest_arch=aarch64"))
        return EINVAL;
    error = take_line(&cursor, end, &line, &line_length);
    if (error != 0 || !line_equals(line, line_length,
            "source_kind=official"))
        return EINVAL;
    error = take_line(&cursor, end, &line, &line_length);
    if (error != 0 || !line_has_nonempty_value(line, line_length,
            "alpine_version="))
        return EINVAL;
    error = take_line(&cursor, end, &line, &line_length);
    static const char digest_prefix[] = "archive_sha256=";
    size_t digest_prefix_length = sizeof(digest_prefix) - 1;
    if (error != 0 || line_length != digest_prefix_length + 64 ||
            memcmp(line, digest_prefix, digest_prefix_length) != 0 ||
            !valid_sha256(line + digest_prefix_length, 64))
        return EINVAL;
    memcpy(manifest->archive_sha256, line + digest_prefix_length, 64);
    manifest->archive_sha256[64] = '\0';
    error = take_line(&cursor, end, &line, &line_length);
    if (error != 0 || !line_has_nonempty_value(line, line_length,
            "source_url=https://"))
        return EINVAL;
    error = take_line(&cursor, end, &line, &line_length);
    if (error != 0 || !line_equals(line, line_length,
            "hardlinks=rootfs-hardlinks.tsv") || cursor != end)
        return EINVAL;
    return 0;
}

static int validate_seed_top(int seed, struct seed_manifest *manifest) {
    unsigned found = 0;
    int iterator_fd = dup(seed);
    if (iterator_fd < 0)
        return errno_or_io();
    DIR *iterator = fdopendir(iterator_fd);
    if (iterator == NULL) {
        int error = errno_or_io();
        close(iterator_fd);
        return error;
    }

    int error = 0;
    errno = 0;
    struct dirent *entry;
    while ((entry = readdir(iterator)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 ||
                strcmp(entry->d_name, "..") == 0)
            continue;
        unsigned bit;
        bool expects_directory = false;
        if (strcmp(entry->d_name, "meta.db") == 0) {
            bit = 1u << 0;
        } else if (strcmp(entry->d_name, "data") == 0) {
            bit = 1u << 1;
            expects_directory = true;
        } else if (strcmp(entry->d_name, rootfs_manifest_name) == 0) {
            bit = 1u << 2;
        } else if (strcmp(entry->d_name, hardlink_manifest_name) == 0) {
            bit = 1u << 3;
        } else {
            error = EINVAL;
            break;
        }
        if ((found & bit) != 0) {
            error = EINVAL;
            break;
        }
        struct stat metadata;
        if (fstatat(seed, entry->d_name, &metadata,
                AT_SYMLINK_NOFOLLOW) < 0) {
            error = errno_or_io();
            break;
        }
        if ((expects_directory && !S_ISDIR(metadata.st_mode)) ||
                (!expects_directory && !S_ISREG(metadata.st_mode))) {
            error = EINVAL;
            break;
        }
        found |= bit;
        errno = 0;
    }
    if (entry == NULL && errno != 0 && error == 0)
        error = errno_or_io();
    if (closedir(iterator) < 0 && error == 0)
        error = errno_or_io();
    if (error != 0)
        return error;
    if (found != 0x0f)
        return EINVAL;

    char *manifest_bytes = NULL;
    size_t manifest_length = 0;
    error = read_regular_at(seed, rootfs_manifest_name, MANIFEST_LIMIT,
            &manifest_bytes, &manifest_length);
    if (error == 0)
        error = parse_seed_manifest(manifest_bytes,
                manifest_length, manifest);
    free(manifest_bytes);
    return error;
}

static bool valid_relative_path(const char *path) {
    if (path == NULL || path[0] == '\0' || path[0] == '/')
        return false;
    size_t path_length = strlen(path);
    if (path_length > PATH_MAX || path[path_length - 1] == '/')
        return false;
    const char *component = path;
    while (*component != '\0') {
        const char *separator = strchr(component, '/');
        size_t length = separator == NULL ? strlen(component) :
                (size_t) (separator - component);
        if (length == 0 || length > NAME_MAX ||
                (length == 1 && component[0] == '.') ||
                (length == 2 && component[0] == '.' && component[1] == '.'))
            return false;
        if (separator == NULL)
            break;
        component = separator + 1;
    }
    return true;
}

static int open_relative_parent(
        int root, const char *path, struct relative_parent *parent) {
    if (!valid_relative_path(path))
        return EINVAL;
    int directory = dup(root);
    if (directory < 0)
        return errno_or_io();

    const char *component = path;
    const char *separator;
    while ((separator = strchr(component, '/')) != NULL) {
        size_t length = (size_t) (separator - component);
        char name[NAME_MAX + 1];
        memcpy(name, component, length);
        name[length] = '\0';
        int child = openat(directory, name,
                O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (child < 0) {
            int error = errno_or_io();
            close(directory);
            return error;
        }
        if (close(directory) < 0) {
            int error = errno_or_io();
            close(child);
            return error;
        }
        directory = child;
        component = separator + 1;
    }
    size_t leaf_length = strlen(component);
    memcpy(parent->leaf, component, leaf_length + 1);
    parent->directory = directory;
    return 0;
}

static int close_relative_parent(
        struct relative_parent *parent, int error) {
    if (close(parent->directory) < 0 && error == 0)
        error = errno_or_io();
    parent->directory = -1;
    return error;
}

static int open_regular_relative(
        int root, const char *path, int flags, int *file_out) {
    struct relative_parent parent = {.directory = -1};
    int error = open_relative_parent(root, path, &parent);
    int file = -1;
    if (error == 0) {
        file = openat(parent.directory, parent.leaf,
                flags | O_CLOEXEC | O_NOFOLLOW);
        if (file < 0)
            error = errno_or_io();
    }
    if (error == 0) {
        struct stat metadata;
        if (file < 0)
            error = EIO;
        else if (fstat(file, &metadata) < 0)
            error = errno_or_io();
        else if (!S_ISREG(metadata.st_mode))
            error = EINVAL;
    }
    if (parent.directory >= 0)
        error = close_relative_parent(&parent, error);
    if (error != 0) {
        if (file >= 0)
            close(file);
        return error;
    }
    *file_out = file;
    return 0;
}

static int compare_string_pointers(const void *left, const void *right) {
    const char *const *left_string = left;
    const char *const *right_string = right;
    return strcmp(*left_string, *right_string);
}

static void hardlink_manifest_destroy(struct hardlink_manifest *manifest) {
    free(manifest->entries);
    free(manifest->storage);
    *manifest = (struct hardlink_manifest) {0};
}

static int parse_hardlink_manifest(
        char *bytes, size_t length, struct hardlink_manifest *manifest) {
    manifest->storage = bytes;
    if (memchr(bytes, '\0', length) != NULL)
        return EINVAL;
    if (length == 0)
        return 0;
    if (bytes[length - 1] != '\n')
        return EINVAL;
    size_t line_count = 0;
    for (size_t i = 0; i < length; i++) {
        if (bytes[i] == '\n') {
            if (line_count == SIZE_MAX / sizeof(*manifest->entries))
                return EOVERFLOW;
            line_count++;
        }
    }
    if (line_count == 0)
        return EINVAL;
    manifest->entries = calloc(line_count, sizeof(*manifest->entries));
    if (manifest->entries == NULL)
        return ENOMEM;

    char *cursor = bytes;
    char *end = bytes + length;
    while (cursor < end) {
        char *newline = memchr(cursor, '\n', (size_t) (end - cursor));
        if (newline == NULL)
            return EINVAL;
        char *tab = memchr(cursor, '\t', (size_t) (newline - cursor));
        if (tab == NULL || memchr(tab + 1, '\t',
                (size_t) (newline - tab - 1)) != NULL)
            return EINVAL;
        *tab = '\0';
        *newline = '\0';
        if (!valid_relative_path(cursor) || !valid_relative_path(tab + 1))
            return EINVAL;
        struct hardlink_entry *current =
                &manifest->entries[manifest->count];
        *current = (struct hardlink_entry) {
            .canonical = cursor,
            .member = tab + 1,
        };
        if (manifest->count == 0 || strcmp(current->canonical,
                manifest->entries[manifest->count - 1].canonical) != 0) {
            if (manifest->count != 0 && strcmp(current->canonical,
                    manifest->entries[manifest->count - 1].canonical) <= 0)
                return EINVAL;
            if (strcmp(current->canonical, current->member) != 0)
                return EINVAL;
        } else if (strcmp(current->member,
                manifest->entries[manifest->count - 1].member) <= 0) {
            return EINVAL;
        }
        manifest->count++;
        cursor = newline + 1;
    }
    if (manifest->count != line_count)
        return EINVAL;

    char **members = malloc(manifest->count * sizeof(*members));
    if (members == NULL)
        return ENOMEM;
    for (size_t i = 0; i < manifest->count; i++)
        members[i] = manifest->entries[i].member;
    qsort(members, manifest->count, sizeof(*members), compare_string_pointers);
    int error = 0;
    for (size_t i = 1; i < manifest->count; i++) {
        if (strcmp(members[i - 1], members[i]) == 0) {
            error = EINVAL;
            break;
        }
    }
    free(members);
    return error;
}

static int validate_busybox_elf(int data_directory) {
    int busybox = -1;
    int error = open_regular_relative(
            data_directory, "bin/busybox", O_RDONLY, &busybox);
    if (error != 0)
        return error;
    unsigned char header[20];
    size_t offset = 0;
    while (offset < sizeof(header)) {
        ssize_t count = read(busybox, header + offset, sizeof(header) - offset);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            error = errno_or_io();
            break;
        }
        if (count == 0) {
            error = EINVAL;
            break;
        }
        offset += (size_t) count;
    }
    if (error == 0 && !(header[0] == 0x7f && header[1] == 'E' &&
            header[2] == 'L' && header[3] == 'F' && header[4] == 2 &&
            header[5] == 1 && header[18] == 183 && header[19] == 0))
        error = EINVAL;
    if (close(busybox) < 0 && error == 0)
        error = errno_or_io();
    return error;
}

static int sqlite_scalar_int64(
        sqlite3 *database, const char *sql, sqlite3_int64 *value) {
    sqlite3_stmt *statement = NULL;
    int result = sqlite3_prepare_v2(database, sql, -1, &statement, NULL);
    if (result != SQLITE_OK)
        return sqlite_error(database);
    int error = 0;
    result = sqlite3_step(statement);
    if (result != SQLITE_ROW) {
        error = sqlite_error(database);
    } else {
        *value = sqlite3_column_int64(statement, 0);
        result = sqlite3_step(statement);
        if (result != SQLITE_DONE)
            error = result == SQLITE_ROW ? EINVAL : sqlite_error(database);
    }
    if (sqlite3_finalize(statement) != SQLITE_OK && error == 0)
        error = sqlite_error(database);
    return error;
}

static int sqlite_scalar_text(
        sqlite3 *database, const char *sql, const char *expected) {
    sqlite3_stmt *statement = NULL;
    int result = sqlite3_prepare_v2(database, sql, -1, &statement, NULL);
    if (result != SQLITE_OK)
        return sqlite_error(database);
    int error = 0;
    result = sqlite3_step(statement);
    if (result != SQLITE_ROW) {
        error = sqlite_error(database);
    } else {
        const unsigned char *value = sqlite3_column_text(statement, 0);
        if (value == NULL || strcmp((const char *) value, expected) != 0) {
            error = EINVAL;
        } else {
            result = sqlite3_step(statement);
            if (result != SQLITE_DONE)
                error = result == SQLITE_ROW ? EINVAL : sqlite_error(database);
        }
    }
    if (sqlite3_finalize(statement) != SQLITE_OK && error == 0)
        error = sqlite_error(database);
    return error;
}

static int database_inode_for_path(
        sqlite3 *database, sqlite3_stmt *statement,
        const char *relative_path, sqlite3_int64 *inode) {
    size_t path_length = strlen(relative_path);
    if (path_length > (size_t) INT_MAX - 1)
        return EOVERFLOW;
    char *database_path = malloc(path_length + 2);
    if (database_path == NULL)
        return ENOMEM;
    database_path[0] = '/';
    memcpy(database_path + 1, relative_path, path_length + 1);
    sqlite3_reset(statement);
    sqlite3_clear_bindings(statement);
    int result = sqlite3_bind_blob(statement, 1, database_path,
            (int) (path_length + 1), SQLITE_TRANSIENT);
    free(database_path);
    if (result != SQLITE_OK)
        return sqlite_error(database);
    result = sqlite3_step(statement);
    if (result != SQLITE_ROW)
        return result == SQLITE_DONE ? EINVAL : sqlite_error(database);
    *inode = sqlite3_column_int64(statement, 0);
    result = sqlite3_step(statement);
    if (result != SQLITE_DONE)
        return result == SQLITE_ROW ? EINVAL : sqlite_error(database);
    return 0;
}

static int database_path_count(
        sqlite3 *database, sqlite3_stmt *statement,
        sqlite3_int64 inode, sqlite3_int64 *count) {
    sqlite3_reset(statement);
    sqlite3_clear_bindings(statement);
    int result = sqlite3_bind_int64(statement, 1, inode);
    if (result != SQLITE_OK)
        return sqlite_error(database);
    result = sqlite3_step(statement);
    if (result != SQLITE_ROW)
        return sqlite_error(database);
    *count = sqlite3_column_int64(statement, 0);
    result = sqlite3_step(statement);
    if (result != SQLITE_DONE)
        return result == SQLITE_ROW ? EINVAL : sqlite_error(database);
    return 0;
}

static int validate_database_hardlinks(
        sqlite3 *database, struct hardlink_manifest *manifest) {
    sqlite3_stmt *inode_statement = NULL;
    sqlite3_stmt *count_statement = NULL;
    int result = sqlite3_prepare_v2(database,
            "select inode from paths where path = ?", -1,
            &inode_statement, NULL);
    if (result != SQLITE_OK)
        return sqlite_error(database);
    result = sqlite3_prepare_v2(database,
            "select count(*) from paths where inode = ?", -1,
            &count_statement, NULL);
    int error = result == SQLITE_OK ? 0 : sqlite_error(database);

    size_t group_count = 0;
    size_t index = 0;
    while (error == 0 && index < manifest->count) {
        size_t group_start = index;
        const char *canonical = manifest->entries[index].canonical;
        while (index < manifest->count && strcmp(
                manifest->entries[index].canonical, canonical) == 0)
            index++;
        size_t group_size = index - group_start;
        if (group_size < 2) {
            error = EINVAL;
            break;
        }
        sqlite3_int64 expected_inode = 0;
        for (size_t member = group_start; member < index; member++) {
            sqlite3_int64 inode;
            error = database_inode_for_path(database, inode_statement,
                    manifest->entries[member].member, &inode);
            if (error != 0)
                break;
            if (member == group_start)
                expected_inode = inode;
            else if (inode != expected_inode) {
                error = EINVAL;
                break;
            }
            manifest->entries[member].database_inode = inode;
        }
        sqlite3_int64 database_count = 0;
        if (error == 0)
            error = database_path_count(database, count_statement,
                    expected_inode, &database_count);
        if (error == 0 && (database_count < 0 ||
                (uintmax_t) database_count != (uintmax_t) group_size))
            error = EINVAL;
        group_count++;
    }

    if (sqlite3_finalize(inode_statement) != SQLITE_OK && error == 0)
        error = sqlite_error(database);
    if (count_statement != NULL &&
            sqlite3_finalize(count_statement) != SQLITE_OK && error == 0)
        error = sqlite_error(database);
    if (error != 0)
        return error;

    sqlite3_int64 database_group_count;
    error = sqlite_scalar_int64(database,
            "select count(*) from (select inode from paths "
            "group by inode having count(*) > 1)",
            &database_group_count);
    if (error == 0 && (database_group_count < 0 ||
            (uintmax_t) database_group_count != (uintmax_t) group_count))
        error = EINVAL;
    return error;
}

static int pread_all(
        int file, void *bytes, size_t length, off_t offset) {
    unsigned char *cursor = bytes;
    while (length != 0) {
        ssize_t count = pread(file, cursor, length, offset);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            return errno_or_io();
        }
        if (count == 0)
            return EIO;
        cursor += (size_t) count;
        length -= (size_t) count;
        if ((uintmax_t) offset > (uintmax_t) OFF_MAX - (uintmax_t) count)
            return EOVERFLOW;
        offset += (off_t) count;
    }
    return 0;
}

static int compare_regular_files_at(
        int directory, const char *left_path, const char *right_path) {
    int left = -1;
    int right = -1;
    int error = open_regular_relative(
            directory, left_path, O_RDONLY, &left);
    if (error != 0)
        return error;
    error = open_regular_relative(directory, right_path, O_RDONLY, &right);
    if (error != 0) {
        close(left);
        return error;
    }
    struct stat left_metadata;
    struct stat right_metadata;
    if (fstat(left, &left_metadata) < 0 || fstat(right, &right_metadata) < 0)
        error = errno_or_io();
    else if (!S_ISREG(left_metadata.st_mode) ||
            !S_ISREG(right_metadata.st_mode) ||
            left_metadata.st_size < 0 ||
            left_metadata.st_size != right_metadata.st_size)
        error = EINVAL;

    unsigned char left_buffer[COPY_BUFFER_SIZE];
    unsigned char right_buffer[COPY_BUFFER_SIZE];
    off_t offset = 0;
    while (error == 0 && offset < left_metadata.st_size) {
        off_t remaining = left_metadata.st_size - offset;
        size_t chunk = remaining > (off_t) sizeof(left_buffer) ?
                sizeof(left_buffer) : (size_t) remaining;
        error = pread_all(left, left_buffer, chunk, offset);
        if (error == 0)
            error = pread_all(right, right_buffer, chunk, offset);
        if (error == 0 && memcmp(left_buffer, right_buffer, chunk) != 0)
            error = EINVAL;
        if ((uintmax_t) offset > (uintmax_t) OFF_MAX - (uintmax_t) chunk)
            error = EOVERFLOW;
        else
            offset += (off_t) chunk;
    }
    if (close(left) < 0 && error == 0)
        error = errno_or_io();
    if (close(right) < 0 && error == 0)
        error = errno_or_io();
    return error;
}

static int rebuild_hardlinks(
        int data_directory, const struct hardlink_manifest *manifest) {
    size_t index = 0;
    while (index < manifest->count) {
        size_t group_start = index;
        const char *canonical = manifest->entries[index].canonical;
        while (index < manifest->count && strcmp(
                manifest->entries[index].canonical, canonical) == 0)
            index++;
        size_t group_size = index - group_start;
        struct relative_parent canonical_parent = {.directory = -1};
        int error = open_relative_parent(
                data_directory, canonical, &canonical_parent);
        if (error != 0)
            return error;
        struct stat canonical_metadata;
        if (fstatat(canonical_parent.directory, canonical_parent.leaf,
                &canonical_metadata, AT_SYMLINK_NOFOLLOW) < 0)
            error = errno_or_io();
        else if (!S_ISREG(canonical_metadata.st_mode))
            error = EINVAL;

        for (size_t member = group_start + 1;
                error == 0 && member < index; member++) {
            const char *member_path = manifest->entries[member].member;
            struct relative_parent member_parent = {.directory = -1};
            error = open_relative_parent(
                    data_directory, member_path, &member_parent);
            struct stat member_metadata;
            if (error == 0 && fstatat(member_parent.directory,
                    member_parent.leaf, &member_metadata,
                    AT_SYMLINK_NOFOLLOW) < 0)
                error = errno_or_io();
            if (error == 0 && !S_ISREG(member_metadata.st_mode))
                error = EINVAL;
            if (error == 0)
                error = compare_regular_files_at(
                    data_directory, canonical, member_path);
            bool already_linked = error == 0 &&
                    canonical_metadata.st_dev == member_metadata.st_dev &&
                    canonical_metadata.st_ino == member_metadata.st_ino;
            if (error == 0 && !already_linked && unlinkat(
                    member_parent.directory, member_parent.leaf, 0) < 0)
                error = errno_or_io();
            if (error == 0 && !already_linked && linkat(
                    canonical_parent.directory, canonical_parent.leaf,
                    member_parent.directory, member_parent.leaf, 0) < 0)
                error = errno_or_io();
            if (error == 0 && !already_linked)
                error = sync_directory(member_parent.directory);
            if (member_parent.directory >= 0)
                error = close_relative_parent(&member_parent, error);
        }

        if (error == 0 && fstatat(canonical_parent.directory,
                canonical_parent.leaf, &canonical_metadata,
                AT_SYMLINK_NOFOLLOW) < 0)
            error = errno_or_io();
        if (error == 0 && (uintmax_t) canonical_metadata.st_nlink !=
                (uintmax_t) group_size)
            error = EINVAL;
        for (size_t member = group_start;
                error == 0 && member < index; member++) {
            struct relative_parent member_parent = {.directory = -1};
            error = open_relative_parent(data_directory,
                    manifest->entries[member].member, &member_parent);
            struct stat member_metadata;
            if (error == 0 && fstatat(member_parent.directory,
                    member_parent.leaf, &member_metadata,
                    AT_SYMLINK_NOFOLLOW) < 0)
                error = errno_or_io();
            if (error == 0 && (!S_ISREG(member_metadata.st_mode) ||
                    member_metadata.st_dev != canonical_metadata.st_dev ||
                    member_metadata.st_ino != canonical_metadata.st_ino ||
                    member_metadata.st_nlink != canonical_metadata.st_nlink))
                error = EINVAL;
            if (member_parent.directory >= 0)
                error = close_relative_parent(&member_parent, error);
        }
        if (canonical_parent.directory >= 0)
            error = close_relative_parent(&canonical_parent, error);
        if (error != 0)
            return error;
    }
    return 0;
}

static int update_database_inode(
        sqlite3 *database, sqlite3_int64 database_inode) {
    char *message = NULL;
    int result = sqlite3_exec(database, "begin immediate", NULL, NULL, &message);
    sqlite3_free(message);
    if (result != SQLITE_OK)
        return sqlite_error(database);
    sqlite3_stmt *statement = NULL;
    result = sqlite3_prepare_v2(database,
            "update meta set db_inode = ?", -1, &statement, NULL);
    int error = result == SQLITE_OK ? 0 : sqlite_error(database);
    if (error == 0 && sqlite3_bind_int64(
            statement, 1, database_inode) != SQLITE_OK)
        error = sqlite_error(database);
    if (error == 0 && sqlite3_step(statement) != SQLITE_DONE)
        error = sqlite_error(database);
    if (error == 0 && sqlite3_changes(database) != 1)
        error = EINVAL;
    if (statement != NULL && sqlite3_finalize(statement) != SQLITE_OK &&
            error == 0)
        error = sqlite_error(database);

    const char *transaction = error == 0 ? "commit" : "rollback";
    message = NULL;
    result = sqlite3_exec(database, transaction, NULL, NULL, &message);
    sqlite3_free(message);
    if (result != SQLITE_OK && error == 0)
        error = sqlite_error(database);
    return error;
}

static int database_path_for_file(int database_file, char path[PATH_MAX]) {
    if (fcntl(database_file, F_GETPATH, path) < 0)
        return errno_or_io();
    size_t length = strnlen(path, PATH_MAX);
    return length == 0 || length == PATH_MAX ? ENAMETOOLONG : 0;
}

static int verify_database_name_identity(
        int staging_directory, const char *database_path,
        const struct stat *expected) {
    struct stat directory_entry;
    if (fstatat(staging_directory, "meta.db", &directory_entry,
            AT_SYMLINK_NOFOLLOW) < 0)
        return errno_or_io();
    struct stat path_entry;
    if (lstat(database_path, &path_entry) < 0)
        return errno_or_io();
    if (!S_ISREG(directory_entry.st_mode) ||
            !S_ISREG(path_entry.st_mode) ||
            directory_entry.st_dev != expected->st_dev ||
            directory_entry.st_ino != expected->st_ino ||
            path_entry.st_dev != expected->st_dev ||
            path_entry.st_ino != expected->st_ino)
        return EAGAIN;
    return 0;
}

static int verify_sqlite_database_identity(
        sqlite3 *database, int staging_directory,
        const char *database_path, const struct stat *expected) {
    int error = verify_database_name_identity(
            staging_directory, database_path, expected);
    int moved = 1;
    int result = error == 0 ? sqlite3_file_control(database, "main",
            SQLITE_FCNTL_HAS_MOVED, &moved) : SQLITE_OK;
    if (error == 0 && (result != SQLITE_OK || moved != 0))
        error = result == SQLITE_OK ? EAGAIN : sqlite_error(database);
    return error;
}

static int materialize_relative_directory(
        int data_directory, const char *path) {
    struct relative_parent parent = {.directory = -1};
    int error = open_relative_parent(data_directory, path, &parent);
    int directory = -1;
    bool created = false;
    if (error == 0) {
        directory = openat(parent.directory, parent.leaf,
                O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (directory < 0 && errno == ENOENT) {
            if (mkdirat(parent.directory, parent.leaf, 0700) < 0) {
                error = errno_or_io();
            } else {
                created = true;
                directory = openat(parent.directory, parent.leaf,
                        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
                if (directory < 0)
                    error = errno_or_io();
            }
        } else if (directory < 0) {
            error = errno_or_io();
        }
    }

    struct stat metadata;
    if (error == 0 && fstat(directory, &metadata) < 0)
        error = errno_or_io();
    if (error == 0 && (!S_ISDIR(metadata.st_mode) ||
            metadata.st_uid != geteuid()))
        error = EINVAL;
    if (directory >= 0 && close(directory) < 0 && error == 0)
        error = errno_or_io();
    if (created && error == 0)
        error = sync_directory(parent.directory);
    if (parent.directory >= 0)
        error = close_relative_parent(&parent, error);
    return error;
}

static int materialize_database_directories(
        sqlite3 *database, int data_directory) {
    sqlite3_stmt *statement = NULL;
    int result = sqlite3_prepare_v2(database,
            "select paths.path, stats.stat from paths "
            "join stats using (inode) "
            "order by length(paths.path), paths.path",
            -1, &statement, NULL);
    if (result != SQLITE_OK)
        return sqlite_error(database);

    int error = 0;
    bool found_root = false;
    while (error == 0 && (result = sqlite3_step(statement)) == SQLITE_ROW) {
        if (sqlite3_column_type(statement, 0) != SQLITE_BLOB ||
                sqlite3_column_type(statement, 1) != SQLITE_BLOB) {
            error = EINVAL;
            break;
        }
        const void *path_bytes = sqlite3_column_blob(statement, 0);
        int path_length = sqlite3_column_bytes(statement, 0);
        const void *stat_bytes = sqlite3_column_blob(statement, 1);
        int stat_length = sqlite3_column_bytes(statement, 1);
        if (path_length < 0 || stat_length != (int) sizeof(struct ish_stat) ||
                stat_bytes == NULL) {
            error = EINVAL;
            break;
        }

        struct ish_stat guest_stat;
        memcpy(&guest_stat, stat_bytes, sizeof(guest_stat));
        bool is_directory = (guest_stat.mode & S_IFMT) == S_IFDIR;
        if (path_length == 0) {
            if (found_root || !is_directory) {
                error = EINVAL;
                break;
            }
            found_root = true;
            continue;
        }
        if (path_bytes == NULL || path_length > PATH_MAX ||
                memchr(path_bytes, '\0', (size_t) path_length) != NULL ||
                ((const unsigned char *) path_bytes)[0] != '/') {
            error = path_length > PATH_MAX ? ENAMETOOLONG : EINVAL;
            break;
        }

        char relative_path[PATH_MAX];
        memcpy(relative_path, (const unsigned char *) path_bytes + 1,
                (size_t) path_length - 1);
        relative_path[path_length - 1] = '\0';
        if (!valid_relative_path(relative_path)) {
            error = EINVAL;
            break;
        }
        unsigned depth = 1;
        for (const char *cursor = relative_path; *cursor != '\0'; cursor++) {
            if (*cursor == '/')
                depth++;
        }
        if (depth > COPY_TREE_DEPTH_LIMIT) {
            error = ELOOP;
            break;
        }
        if (is_directory)
            error = materialize_relative_directory(
                    data_directory, relative_path);
    }
    if (error == 0 && result != SQLITE_DONE)
        error = sqlite_error(database);
    if (error == 0 && !found_root)
        error = EINVAL;
    if (sqlite3_finalize(statement) != SQLITE_OK && error == 0)
        error = sqlite_error(database);
    return error;
}

static int validate_and_update_database(
        int staging_directory, int data_directory,
        struct hardlink_manifest *hardlinks) {
    // SQLite 没有公开底层 fd；私有 staging 内用 held fd、名字身份与 HAS_MOVED 交叉复核。
    int database_file = openat(staging_directory, "meta.db",
            O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (database_file < 0)
        return errno_or_io();

    struct stat metadata = {0};
    int error = 0;
    if (fstat(database_file, &metadata) < 0)
        error = errno_or_io();
    else if (!S_ISREG(metadata.st_mode) || metadata.st_uid != geteuid())
        error = EINVAL;
    char database_path[PATH_MAX];
    if (error == 0)
        error = database_path_for_file(database_file, database_path);
    if (error == 0)
        error = verify_database_name_identity(
                staging_directory, database_path, &metadata);

    sqlite3 *database = NULL;
    int open_flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX;
    // iOS 11 的系统 SQLite 早于 SQLITE_OPEN_NOFOLLOW，运行时只传已支持的 flag。
#ifdef SQLITE_OPEN_NOFOLLOW
    if (sqlite3_libversion_number() >= 3031000)
        open_flags |= SQLITE_OPEN_NOFOLLOW;
#endif
    int result = error == 0 ? sqlite3_open_v2(database_path, &database,
            open_flags, NULL) : SQLITE_OK;
    if (error == 0 && result != SQLITE_OK)
        error = database == NULL ? EINVAL : sqlite_error(database);
    if (error == 0) {
        sqlite3_extended_result_codes(database, 1);
        sqlite3_busy_timeout(database, 1000);
        error = verify_sqlite_database_identity(database,
                staging_directory, database_path, &metadata);
    }

    if (error == 0)
        error = sqlite_scalar_text(database,
                "pragma journal_mode=delete", "delete");
    if (error == 0)
        error = sqlite_scalar_text(database, "pragma quick_check", "ok");
    sqlite3_int64 value;
    if (error == 0)
        error = sqlite_scalar_int64(database, "pragma user_version", &value);
    if (error == 0 && value != 3)
        error = EINVAL;
    if (error == 0)
        error = sqlite_scalar_int64(database,
                "select count(*) from paths where length(path) = 0", &value);
    if (error == 0 && value != 1)
        error = EINVAL;
    if (error == 0)
        error = sqlite_scalar_int64(database,
                "select count(*) from meta", &value);
    if (error == 0 && value != 1)
        error = EINVAL;
    if (error == 0)
        error = sqlite_scalar_int64(database,
                "select count(*) from sqlite_master where type = 'trigger'",
                &value);
    if (error == 0 && value != 0)
        error = EINVAL;
    if (error == 0)
        error = sqlite_scalar_int64(database,
                "select db_inode from meta", &value);
    if (error == 0 && value != 0)
        error = EINVAL;
    if (error == 0)
        error = sqlite_scalar_int64(database,
                "select count(*) from paths left join stats using (inode) "
                "where stats.inode is null", &value);
    if (error == 0 && value != 0)
        error = EINVAL;
    if (error == 0)
        error = materialize_database_directories(database, data_directory);
    if (error == 0)
        error = validate_database_hardlinks(database, hardlinks);
    if (error == 0)
        error = verify_sqlite_database_identity(database,
                staging_directory, database_path, &metadata);
    if (error == 0)
        error = rebuild_hardlinks(data_directory, hardlinks);

    uintmax_t inode = (uintmax_t) metadata.st_ino;
    if (error == 0 && inode > INT64_MAX)
        error = EOVERFLOW;
    if (error == 0)
        error = verify_sqlite_database_identity(database,
                staging_directory, database_path, &metadata);
    if (error == 0)
        error = update_database_inode(database, (sqlite3_int64) inode);
    if (error == 0)
        error = sqlite_scalar_int64(database,
                "select db_inode from meta", &value);
    if (error == 0 && value != (sqlite3_int64) inode)
        error = EINVAL;
    if (error == 0)
        error = sqlite_scalar_int64(database,
                "select count(*) from paths left join stats using (inode) "
                "where stats.inode is null", &value);
    if (error == 0 && value != 0)
        error = EINVAL;
    if (error == 0)
        error = validate_database_hardlinks(database, hardlinks);
    if (error == 0)
        error = sqlite_scalar_text(database, "pragma quick_check", "ok");
    if (error == 0)
        error = verify_sqlite_database_identity(database,
                staging_directory, database_path, &metadata);
    if (database != NULL && sqlite3_close_v2(database) != SQLITE_OK &&
            error == 0)
        error = EINVAL;
    if (error == 0)
        error = verify_database_name_identity(
                staging_directory, database_path, &metadata);

    static const char *artifacts[] = {
        "meta.db-wal", "meta.db-shm", "meta.db-journal",
    };
    for (size_t i = 0; error == 0 &&
            i < sizeof(artifacts) / sizeof(artifacts[0]); i++) {
        struct stat artifact;
        if (fstatat(staging_directory, artifacts[i], &artifact,
                AT_SYMLINK_NOFOLLOW) == 0)
            error = EINVAL;
        else if (errno != ENOENT)
            error = errno_or_io();
    }
    if (error == 0 && fsync(database_file) < 0)
        error = errno_or_io();
    if (close(database_file) < 0 && error == 0)
        error = errno_or_io();
    return error;
}

static bool valid_receipt(const char *bytes, size_t length) {
    static const char digest_prefix[] = "seed_archive_sha256=";
    size_t format_length = sizeof(receipt_format) - 1;
    size_t prefix_length = sizeof(digest_prefix) - 1;
    size_t expected_length = format_length + prefix_length + 64 + 1;
    return length == expected_length &&
            memcmp(bytes, receipt_format, format_length) == 0 &&
            memcmp(bytes + format_length,
                    digest_prefix, prefix_length) == 0 &&
            valid_sha256(bytes + format_length + prefix_length, 64) &&
            bytes[expected_length - 1] == '\n';
}

static int write_receipt_at(
        int directory, const struct seed_manifest *manifest) {
    static const char digest_prefix[] = "seed_archive_sha256=";
    char receipt[(sizeof(receipt_format) - 1) +
            (sizeof(digest_prefix) - 1) + 64 + 1];
    size_t offset = 0;
    memcpy(receipt + offset, receipt_format, sizeof(receipt_format) - 1);
    offset += sizeof(receipt_format) - 1;
    memcpy(receipt + offset, digest_prefix, sizeof(digest_prefix) - 1);
    offset += sizeof(digest_prefix) - 1;
    memcpy(receipt + offset, manifest->archive_sha256, 64);
    offset += 64;
    receipt[offset++] = '\n';
    return create_regular_at(directory, install_receipt_name,
            receipt, offset, true);
}

static int validate_opened_root(int root) {
    char *receipt = NULL;
    size_t receipt_length = 0;
    int error = read_regular_at(root, install_receipt_name,
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
            error = errno_or_io();
        } else if ((metadata.st_mode & S_IFMT) != required[i].type ||
                metadata.st_uid != geteuid()) {
            error = EEXIST;
        }
    }
    return error;
}

static int open_existing_root(
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
        return errno_or_io();
    }
    *present = true;
    if (!S_ISDIR(root_metadata.st_mode) ||
            root_metadata.st_uid != geteuid())
        return EEXIST;

    int root = openat(parent, root_name,
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (root < 0)
        return errno_or_io();
    struct stat opened_metadata;
    int error = 0;
    if (fstat(root, &opened_metadata) < 0)
        error = errno_or_io();
    else if (opened_metadata.st_dev != root_metadata.st_dev ||
            opened_metadata.st_ino != root_metadata.st_ino)
        error = EAGAIN;
    if (error == 0)
        error = validate_opened_root(root);
    if (error != 0 || root_out == NULL) {
        if (close(root) < 0 && error == 0)
            error = errno_or_io();
    } else {
        *root_out = root;
    }
    return error;
}

static int inspect_existing_root(
        int parent, const char *root_name, bool *present) {
    return open_existing_root(parent, root_name, present, NULL);
}

static int write_copy_operation_marker(
        int root,
        const char *source_name,
        const char *destination_name,
        const char *operation_token) {
    char marker_name[NAME_MAX + 1];
    char record[COPY_OPERATION_RECORD_LIMIT + 1];
    int error = format_copy_operation_marker_name(
            marker_name, operation_token);
    if (error == 0)
        error = format_copy_operation_record(
            record, source_name, destination_name, operation_token);
    if (error != 0)
        return error;
    return create_regular_at(root, marker_name,
            record, strlen(record), true);
}

static int inspect_copy_operation_marker(
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
    int error = format_copy_operation_marker_name(
            marker_name, operation_token);
    if (error == 0)
        error = format_copy_operation_record(
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
        return errno_or_io();
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
        return errno == ELOOP ? 0 : errno_or_io();
    struct stat metadata;
    if (fstat(file, &metadata) < 0)
        error = errno_or_io();
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
            error = errno_or_io();
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
            error = errno_or_io();
        else if (count != 0)
            error = EAGAIN;
    }

    struct stat verified;
    if (error == 0 &&
            fstatat(root, marker_name,
                    &verified, AT_SYMLINK_NOFOLLOW) < 0)
        error = errno_or_io();
    if (error == 0 && (!S_ISREG(verified.st_mode) ||
            verified.st_uid != geteuid() ||
            verified.st_nlink != 1 ||
            verified.st_size != metadata.st_size ||
            verified.st_dev != metadata.st_dev ||
            verified.st_ino != metadata.st_ino))
        error = EAGAIN;
    if (close(file) < 0 && error == 0)
        error = errno_or_io();
    if (error != 0)
        return error;
    *matches = memcmp(bytes, expected, expected_length) == 0;
    return 0;
}

static int verify_named_root_identity(
        int parent, const char *root_name, int root) {
    struct stat opened;
    struct stat named;
    if (fstat(root, &opened) < 0)
        return errno_or_io();
    if (fstatat(parent, root_name,
            &named, AT_SYMLINK_NOFOLLOW) < 0)
        return errno_or_io();
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
            owner_format, owner->staging_name,
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
    if (take_line(&cursor, end, &line, &line_length) != 0 ||
            !line_equals(line, line_length, owner_format))
        return false;

    static const char staging_prefix[] = "staging=";
    if (take_line(&cursor, end, &line, &line_length) != 0 ||
            line_length <= sizeof(staging_prefix) - 1 ||
            memcmp(line, staging_prefix, sizeof(staging_prefix) - 1) != 0)
        return false;
    const char *staging = line + sizeof(staging_prefix) - 1;
    size_t staging_length = line_length - (sizeof(staging_prefix) - 1);
    char expected_prefix[NAME_MAX + 1];
    int prefix_error = format_private_name(
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
    if (take_line(&cursor, end, &line, &line_length) != 0 ||
            line_length <= sizeof(device_prefix) - 1 ||
            memcmp(line, device_prefix, sizeof(device_prefix) - 1) != 0 ||
            !parse_hex_uintmax(line + sizeof(device_prefix) - 1,
                    line_length - (sizeof(device_prefix) - 1),
                    &owner->staging_device))
        return false;
    static const char inode_prefix[] = "inode=";
    if (take_line(&cursor, end, &line, &line_length) != 0 ||
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
        return errno_or_io();
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
        return errno_or_io();
    if ((size_t) count > OWNER_RECORD_LIMIT)
        return 0;
    bytes[count] = '\0';
    if (!parse_owner_record(bytes, (size_t) count, root_name, owner))
        return 0;

    struct stat verified;
    if (fstatat(parent, owner_name, &verified,
            AT_SYMLINK_NOFOLLOW) < 0)
        return errno_or_io();
    if (!S_ISLNK(verified.st_mode) ||
            verified.st_dev != metadata.st_dev ||
            verified.st_ino != metadata.st_ino)
        return EAGAIN;
    owner->marker_device = (uintmax_t) verified.st_dev;
    owner->marker_inode = (uintmax_t) verified.st_ino;
    *state = OWNER_VALID;
    return 0;
}

static int entry_metadata(
        int parent, const char *name,
        bool *exists, struct stat *metadata) {
    *exists = false;
    if (fstatat(parent, name, metadata, AT_SYMLINK_NOFOLLOW) < 0) {
        if (errno == ENOENT)
            return 0;
        return errno_or_io();
    }
    *exists = true;
    return 0;
}

static int verify_staging_identity(
        int parent, const struct staging_owner *owner, bool *exists) {
    struct stat metadata;
    int error = entry_metadata(
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
        return errno_or_io();
    struct stat opened;
    if (fstat(directory, &opened) < 0)
        error = errno_or_io();
    else if (opened.st_dev != metadata.st_dev ||
            opened.st_ino != metadata.st_ino)
        error = EAGAIN;
    if (close(directory) < 0 && error == 0)
        error = errno_or_io();
    return error;
}

static int remove_unpublished_staging(
        int parent, const struct staging_owner *owner) {
    bool exists;
    int error = verify_staging_identity(parent, owner, &exists);
    if (error != 0)
        return error;
    if (exists)
        error = remove_entry_at(parent, owner->staging_name, 0);
    if (error == 0)
        error = sync_directory(parent);
    return error;
}

static int unlink_owner_marker(
        int parent, const char *owner_name,
        const struct staging_owner *owner, int sync_phase) {
    struct stat marker;
    if (fstatat(parent, owner_name, &marker,
            AT_SYMLINK_NOFOLLOW) < 0)
        return errno_or_io();
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
        return errno_or_io();
    if (sync_phase == ISH_APPLE_ROOTFS_SEED_TEST_NONE)
        return sync_directory(parent);
    return sync_directory_phase(parent, sync_phase);
}

static int remove_owned_staging(
        int parent, const char *owner_name,
        const struct staging_owner *owner) {
    bool exists;
    int error = verify_staging_identity(parent, owner, &exists);
    if (error != 0)
        return error;
    if (exists) {
        error = remove_entry_at(parent, owner->staging_name, 0);
        if (error != 0)
            return error;
    }
    // 先持久化 staging 消失，再删除恢复凭据；两个阶段不能合并。
    if (error == 0)
        error = sync_directory_phase(parent,
                ISH_APPLE_ROOTFS_SEED_TEST_CLEANUP_STAGING_SYNC);
    if (error == 0)
        error = unlink_owner_marker(parent, owner_name, owner,
                ISH_APPLE_ROOTFS_SEED_TEST_CLEANUP_OWNER_SYNC);
    return error;
}

static int recover_staging(
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
            remove_owned_staging(parent, owner_name, &owner) : 0;
}

static int cleanup_staging_if_owned(
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
            remove_owned_staging(parent, owner_name, &owner) : 0;
}

static int create_staging(
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
            return errno_or_io();
    }
    if (error != 0)
        return error;
    int staging = -1;
    staging = openat(parent, owner.staging_name,
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (staging < 0)
        error = errno_or_io();
    if (error == 0 && fchmod(staging, 0700) < 0)
        error = errno_or_io();
    struct stat metadata;
    if (error == 0 && fstat(staging, &metadata) < 0)
        error = errno_or_io();
    if (error == 0 && (!S_ISDIR(metadata.st_mode) ||
            metadata.st_uid != geteuid()))
        error = EEXIST;
    if (error == 0) {
        owner.staging_device = (uintmax_t) metadata.st_dev;
        owner.staging_inode = (uintmax_t) metadata.st_ino;
    }
    if (error == 0)
        error = sync_directory(parent);

    char owner_record[OWNER_RECORD_LIMIT + 1];
    if (error == 0)
        error = format_owner_record(owner_record, &owner);
    if (error == 0 && symlinkat(owner_record, parent, owner_name) < 0)
        error = errno_or_io();
    bool owner_created = error == 0;
    if (error == 0)
        error = sync_directory(parent);
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
            (void) recover_staging(parent, owner_name, root_name);
        else
            (void) remove_unpublished_staging(parent, &owner);
        return original_error;
    }
    *staging_out = staging;
    *owner_out = owner;
    return 0;
}

static int rollback_unsynchronized_publish(
        int parent, const char *root_name,
        const struct staging_owner *owner) {
    struct stat metadata;
    if (fstatat(parent, root_name, &metadata,
            AT_SYMLINK_NOFOLLOW) < 0)
        return errno_or_io();
    if (!S_ISDIR(metadata.st_mode) ||
            metadata.st_uid != geteuid() ||
            (uintmax_t) metadata.st_dev != owner->staging_device ||
            (uintmax_t) metadata.st_ino != owner->staging_inode)
        return EAGAIN;
    if (renameatx_np(parent, root_name,
            parent, owner->staging_name, RENAME_EXCL) < 0)
        return errno_or_io();
    return sync_directory(parent);
}

static int build_staging_root(int seed, int staging) {
    struct seed_manifest source_manifest = {0};
    int error = validate_seed_top(seed, &source_manifest);
    static const char *regular_resources[] = {
        "meta.db", rootfs_manifest_name, hardlink_manifest_name,
    };
    for (size_t i = 0; error == 0 &&
            i < sizeof(regular_resources) / sizeof(regular_resources[0]); i++)
        error = copy_regular_at(seed, staging, regular_resources[i]);

    if (error == 0 && mkdirat(staging, "data", 0700) < 0)
        error = errno_or_io();
    int source_data = -1;
    int staging_data = -1;
    if (error == 0) {
        source_data = openat(seed, "data",
                O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (source_data < 0)
            error = errno_or_io();
    }
    if (error == 0) {
        staging_data = openat(staging, "data",
                O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (staging_data < 0)
            error = errno_or_io();
    }
    if (error == 0)
        error = copy_directory_contents(source_data, staging_data, 0);
    if (source_data >= 0 && close(source_data) < 0 && error == 0)
        error = errno_or_io();

    struct seed_manifest copied_manifest = {0};
    if (error == 0)
        error = validate_seed_top(staging, &copied_manifest);
    if (error == 0 && strcmp(source_manifest.archive_sha256,
            copied_manifest.archive_sha256) != 0)
        error = EINVAL;
    if (error == 0)
        error = validate_busybox_elf(staging_data);

    char *hardlink_bytes = NULL;
    size_t hardlink_length = 0;
    struct hardlink_manifest hardlinks = {0};
    if (error == 0)
        error = read_regular_at(staging, hardlink_manifest_name,
                HARDLINK_MANIFEST_LIMIT,
                &hardlink_bytes, &hardlink_length);
    if (error == 0) {
        error = parse_hardlink_manifest(
                hardlink_bytes, hardlink_length, &hardlinks);
        hardlink_bytes = NULL;
    }
    if (error == 0)
        error = validate_and_update_database(
                staging, staging_data, &hardlinks);
    hardlink_manifest_destroy(&hardlinks);
    free(hardlink_bytes);

    if (staging_data >= 0 && close(staging_data) < 0 && error == 0)
        error = errno_or_io();
    if (error == 0)
        error = write_receipt_at(staging, &copied_manifest);
    if (error == 0)
        error = sync_directory(staging);
    return error;
}

static int unlink_optional_regular_at(int directory, const char *name) {
    struct stat metadata;
    if (fstatat(directory, name, &metadata,
            AT_SYMLINK_NOFOLLOW) < 0)
        return errno == ENOENT ? 0 : errno_or_io();
    if (!S_ISREG(metadata.st_mode))
        return EINVAL;
    return unlinkat(directory, name, 0) < 0 ? errno_or_io() : 0;
}

static int prepare_copied_database(int staging) {
    // SHM 只保存进程间协调状态；复制后必须由新数据库重新建立。
    int error = unlink_optional_regular_at(staging, "meta.db-shm");
    int database_file = -1;
    if (error == 0) {
        database_file = openat(staging, "meta.db",
                O_RDWR | O_CLOEXEC | O_NOFOLLOW);
        if (database_file < 0)
            error = errno_or_io();
    }

    struct stat metadata = {0};
    if (error == 0 && fstat(database_file, &metadata) < 0)
        error = errno_or_io();
    if (error == 0 && (!S_ISREG(metadata.st_mode) ||
            metadata.st_uid != geteuid() || metadata.st_nlink != 1))
        error = EINVAL;
    char database_path[PATH_MAX];
    if (error == 0)
        error = database_path_for_file(database_file, database_path);
    if (error == 0)
        error = verify_database_name_identity(
                staging, database_path, &metadata);

    sqlite3 *database = NULL;
    int open_flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX;
#ifdef SQLITE_OPEN_NOFOLLOW
    if (sqlite3_libversion_number() >= 3031000)
        open_flags |= SQLITE_OPEN_NOFOLLOW;
#endif
    int result = error == 0 ? sqlite3_open_v2(
            database_path, &database, open_flags, NULL) : SQLITE_OK;
    if (error == 0 && result != SQLITE_OK)
        error = database == NULL ? EINVAL : sqlite_error(database);
    if (error == 0) {
        sqlite3_extended_result_codes(database, 1);
        sqlite3_busy_timeout(database, 1000);
        error = verify_sqlite_database_identity(
                database, staging, database_path, &metadata);
    }
    if (error == 0)
        error = sqlite_scalar_text(database, "pragma quick_check", "ok");
    if (error == 0 && (uintmax_t) metadata.st_ino > INT64_MAX)
        error = EOVERFLOW;
    if (error == 0)
        error = update_database_inode(
                database, (sqlite3_int64) metadata.st_ino);
    int log_frames = 0;
    int checkpointed_frames = 0;
    if (error == 0 && sqlite3_wal_checkpoint_v2(
            database, NULL, SQLITE_CHECKPOINT_TRUNCATE,
            &log_frames, &checkpointed_frames) != SQLITE_OK)
        error = sqlite_error(database);
    if (error == 0 && log_frames >= 0 &&
            checkpointed_frames != log_frames)
        error = EBUSY;
    if (error == 0)
        error = sqlite_scalar_text(
                database, "pragma journal_mode=delete", "delete");
    if (error == 0)
        error = sqlite_scalar_text(database, "pragma quick_check", "ok");
    sqlite3_int64 database_inode;
    if (error == 0)
        error = sqlite_scalar_int64(
                database, "select db_inode from meta", &database_inode);
    if (error == 0 &&
            database_inode != (sqlite3_int64) metadata.st_ino)
        error = EINVAL;
    if (database != NULL &&
            sqlite3_close_v2(database) != SQLITE_OK && error == 0)
        error = EINVAL;
    if (error == 0)
        error = verify_database_name_identity(
                staging, database_path, &metadata);
    if (database_file >= 0 && error == 0 && fsync(database_file) < 0)
        error = errno_or_io();
    if (database_file >= 0 && close(database_file) < 0 && error == 0)
        error = errno_or_io();
    static const char *artifacts[] = {
        "meta.db-wal", "meta.db-shm", "meta.db-journal",
    };
    for (size_t i = 0; error == 0 &&
            i < sizeof(artifacts) / sizeof(artifacts[0]); i++)
        error = unlink_optional_regular_at(staging, artifacts[i]);
    if (error == 0)
        error = sync_directory(staging);
    return error;
}

static int build_copied_root(
        int source,
        int staging,
        const char *source_name,
        const char *destination_name,
        const char *operation_token) {
    struct root_copy_context context = {
        .destination_root = staging,
    };
    int error = copy_managed_root_contents(
            source, staging, "", 0, &context);
    root_copy_context_destroy(&context);
    if (error == 0)
        error = validate_opened_root(staging);
    if (error == 0)
        error = prepare_copied_database(staging);
    if (error == 0)
        error = validate_opened_root(staging);
    if (error == 0 && operation_token != NULL)
        error = write_copy_operation_marker(
                staging, source_name,
                destination_name, operation_token);
    if (error == 0)
        error = sync_directory(staging);
    return error;
}

static int validate_imported_root_top(int root) {
    unsigned found = 0;
    int iterator_file = dup(root);
    if (iterator_file < 0)
        return errno_or_io();
    DIR *iterator = fdopendir(iterator_file);
    if (iterator == NULL) {
        int error = errno_or_io();
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
            error = errno_or_io();
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
        error = errno_or_io();
    if (closedir(iterator) < 0 && error == 0)
        error = errno_or_io();
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
        return errno_or_io();
    int iterator_file = openat(directory, ".",
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (iterator_file < 0)
        return errno_or_io();
    DIR *iterator = fdopendir(iterator_file);
    if (iterator == NULL) {
        int error = errno_or_io();
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
        error = report_copy_stage(
                progress, 0, "正在检查文件系统内容");
        if (error != 0)
            break;
        struct stat metadata;
        if (fstatat(directory, entry->d_name, &metadata,
                AT_SYMLINK_NOFOLLOW) < 0) {
            error = errno_or_io();
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
                error = errno_or_io();
            } else {
                error = count_imported_regular_bytes(
                        child, depth + 1, total, progress);
                if (close(child) < 0 && error == 0)
                    error = errno_or_io();
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
        error = errno_or_io();
    if (closedir(iterator) < 0 && error == 0)
        error = errno_or_io();
    struct timespec directory_times[2];
    stat_times(&directory_metadata, directory_times);
    if (futimens(directory, directory_times) < 0 && error == 0)
        error = errno_or_io();
    return error;
}

static int build_imported_root(
        int seed, int imported, int staging,
        struct progress progress) {
    struct seed_manifest manifest = {0};
    int error = validate_seed_top(seed, &manifest);
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
        error = report_root_copy_progress(
                &context, 0, "正在复制文件系统内容");
    if (error == 0)
        error = copy_managed_root_contents(
                imported, staging, "", 0, &context);
    root_copy_context_destroy(&context);
    if (error == 0)
        error = report_copy_stage(
                progress, 0.86, "正在准备文件系统数据库");
    if (error == 0)
        error = prepare_copied_database(staging);
    if (error == 0)
        error = report_copy_stage(
                progress, 0.92, "正在写入文件系统收据");
    if (error == 0)
        error = write_receipt_at(staging, &manifest);
    if (error == 0)
        error = report_copy_stage(
                progress, 0.96, "正在验证文件系统");
    if (error == 0)
        error = validate_opened_root(staging);
    if (error == 0)
        error = report_copy_stage(
                progress, 0.99, "正在同步文件系统");
    if (error == 0)
        error = sync_directory(staging);
    if (error == 0)
        error = report_copy_stage(
                progress, 1, "文件系统发布准备完成");
    return error;
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
            return errno_or_io();
        lock = openat(parent, lock_name,
                O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (lock >= 0) {
            int error = fsync(lock) < 0 ? errno_or_io() :
                    sync_directory(parent);
            if (error != 0) {
                close(lock);
                return error;
            }
            break;
        }
        if (errno != EEXIST && errno != ENOENT)
            return errno_or_io();
    }
    if (lock < 0)
        return EAGAIN;
    struct stat metadata;
    int error = 0;
    if (fstat(lock, &metadata) < 0)
        error = errno_or_io();
    else if (!S_ISREG(metadata.st_mode) ||
            metadata.st_uid != geteuid() || metadata.st_nlink != 1)
        error = EEXIST;
    while (error == 0 && flock(lock, operation) < 0) {
        if (errno != EINTR) {
            error = errno == EWOULDBLOCK ? EBUSY : errno_or_io();
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
        error = errno_or_io();
        break;
    }
    if (close(lock) < 0 && error == 0)
        error = errno_or_io();
    return error;
}

int ish_apple_rootfs_lock_managed_root(
        const char *persistent_parent,
        const char *root_name,
        bool exclusive,
        bool require_valid_root,
        int *lock_out) {
    if (persistent_parent == NULL || persistent_parent[0] == '\0' ||
            !valid_root_name(root_name) || lock_out == NULL)
        return EINVAL;
    *lock_out = -1;

    char lock_name[NAME_MAX + 1];
    int error = format_private_name(
            lock_name, root_name, ".lifecycle.lock");
    if (error != 0)
        return error;
    int parent = open(persistent_parent,
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (parent < 0)
        return errno_or_io();

    int lock = -1;
    int operation = exclusive ? LOCK_EX | LOCK_NB : LOCK_SH;
    error = lock_file(parent, lock_name, operation, &lock);
    bool present = false;
    if (error == 0 && require_valid_root)
        error = open_existing_root(parent, root_name, &present, NULL);
    if (error == 0 && !require_valid_root) {
        struct stat metadata;
        error = entry_metadata(parent, root_name, &present, &metadata);
    }
    if (error == 0 && !present)
        error = ENOENT;
    if (close(parent) < 0 && error == 0)
        error = errno_or_io();
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
        return errno_or_io();
    int lock = -1;
    int error = lock_file(parent, lock_name, LOCK_EX, &lock);
    if (close(parent) < 0 && error == 0)
        error = errno_or_io();
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
    int error = inspect_existing_root(parent, root_name, &root_present);
    if (error != 0)
        return error;
    if (root_present) {
        error = cleanup_staging_if_owned(parent, owner_name, root_name);
        if (error != 0)
            return error;
        *result = ISH_APPLE_ROOTFS_SEED_ALREADY_PRESENT;
        return 0;
    }

    error = recover_staging(parent, owner_name, root_name);
    if (error != 0)
        return error;
    int seed = open(seed_root,
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (seed < 0)
        return errno_or_io();

    int staging = -1;
    struct staging_owner owner = {0};
    error = create_staging(parent, root_name,
            owner_name, &staging, &owner);
    if (error == 0)
        error = build_staging_root(seed, staging);
    if (close(seed) < 0 && error == 0)
        error = errno_or_io();
    if (staging >= 0 && close(staging) < 0 && error == 0)
        error = errno_or_io();
    if (error != 0) {
        if (owner.marker_inode != 0)
            (void) remove_owned_staging(parent, owner_name, &owner);
        return error;
    }

    if (renameatx_np(parent, owner.staging_name,
            parent, root_name, RENAME_EXCL) < 0) {
        int rename_error = errno_or_io();
        int cleanup_error = remove_owned_staging(
                parent, owner_name, &owner);
        if (cleanup_error != 0)
            return cleanup_error;
        if (rename_error != EEXIST)
            return rename_error;
        error = inspect_existing_root(parent, root_name, &root_present);
        if (error != 0)
            return error;
        if (!root_present)
            return EAGAIN;
        *result = ISH_APPLE_ROOTFS_SEED_ALREADY_PRESENT;
        return 0;
    }

    error = sync_directory_phase(parent,
            ISH_APPLE_ROOTFS_SEED_TEST_PUBLISH_ROOT_SYNC);
    if (error != 0) {
        int rollback_error = rollback_unsynchronized_publish(
                parent, root_name, &owner);
        return rollback_error == 0 ? error : rollback_error;
    }
    /*
     * final 的父目录项已经持久化；owner 清理失败只会留下可识别的恢复凭据，
     * 不能把已提交安装重新报告成失败。
     */
    *result = ISH_APPLE_ROOTFS_SEED_INSTALLED;
    (void) unlink_owner_marker(parent, owner_name, &owner,
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
    int error = open_existing_root(
            parent, source_name, &source_present, &source);
    if (error == EEXIST)
        return EINVAL;
    if (error != 0)
        return error;
    if (!source_present)
        return ENOENT;

    bool destination_present;
    int destination = -1;
    error = open_existing_root(
            parent, destination_name,
            &destination_present, &destination);
    bool already_completed = false;
    if (error == 0 && destination_present) {
        if (operation_token == NULL) {
            error = EEXIST;
        } else {
            bool marker_present;
            bool marker_matches;
            error = inspect_copy_operation_marker(
                    destination, source_name, destination_name,
                    operation_token, &marker_present, &marker_matches);
            if (error == 0 && (!marker_present || !marker_matches))
                error = EEXIST;
            if (error == 0)
                error = verify_named_root_identity(
                        parent, destination_name, destination);
            if (error == 0)
                error = sync_directory(parent);
            if (error == 0) {
                already_completed = true;
                (void) cleanup_staging_if_owned(
                        parent, owner_name, destination_name);
            }
        }
    }
    if (destination >= 0 && close(destination) < 0 && error == 0)
        error = errno_or_io();
    if (already_completed) {
        (void) close(source);
        return 0;
    }

    int seed = -1;
    if (error == 0) {
        seed = open(seed_root,
                O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (seed < 0)
            error = errno_or_io();
    }
    struct seed_manifest manifest;
    if (error == 0)
        error = validate_seed_top(seed, &manifest);
    if (seed >= 0 && close(seed) < 0 && error == 0)
        error = errno_or_io();
    if (error == 0)
        error = recover_staging(
                parent, owner_name, destination_name);

    int staging = -1;
    struct staging_owner owner = {0};
    if (error == 0)
        error = create_staging(parent, destination_name,
                owner_name, &staging, &owner);
    if (error == 0)
        error = build_copied_root(
                source, staging, source_name,
                destination_name, operation_token);
    if (close(source) < 0 && error == 0)
        error = errno_or_io();
    if (staging >= 0 && close(staging) < 0 && error == 0)
        error = errno_or_io();
    if (error != 0) {
        if (owner.marker_inode != 0) {
            int cleanup_error = remove_owned_staging(
                    parent, owner_name, &owner);
            if (cleanup_error != 0)
                return cleanup_error;
        }
        return error;
    }

    // 与 seed 安装共用排他发布，目录目标无论何种类型都不会被覆盖。
    if (renameatx_np(parent, owner.staging_name,
            parent, destination_name, RENAME_EXCL) < 0) {
        int rename_error = errno_or_io();
        int cleanup_error = remove_owned_staging(
                parent, owner_name, &owner);
        return cleanup_error != 0 ? cleanup_error : rename_error;
    }
    error = sync_directory_phase(parent,
            ISH_APPLE_ROOTFS_SEED_TEST_PUBLISH_ROOT_SYNC);
    if (error != 0) {
        int rollback_error = rollback_unsynchronized_publish(
                parent, destination_name, &owner);
        return rollback_error == 0 ? error : rollback_error;
    }
    // final 已持久化；后续 catalog 操作可以安全收敛残留 owner。
    (void) unlink_owner_marker(parent, owner_name, &owner,
            ISH_APPLE_ROOTFS_SEED_TEST_PUBLISH_OWNER_SYNC);
    return 0;
}

int ish_apple_rootfs_seed_install(
        const char *seed_root,
        const char *persistent_parent,
        const char *root_name,
        enum ish_apple_rootfs_seed_result *result) {
    if (seed_root == NULL || persistent_parent == NULL ||
            result == NULL || !valid_root_name(root_name))
        return EINVAL;

    char lock_name[NAME_MAX + 1];
    char owner_name[NAME_MAX + 1];
    int error = format_private_name(
            lock_name, root_name, ".install.lock");
    if (error == 0)
        error = format_private_name(
                owner_name, root_name, ".installing.owner");
    if (error != 0)
        return error;

    int parent = open(persistent_parent,
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (parent < 0)
        return errno_or_io();
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
        error = errno_or_io();
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
            !valid_root_name(destination_name))
        return EINVAL;

    char lock_name[NAME_MAX + 1];
    char owner_name[NAME_MAX + 1];
    int error = format_private_name(
            lock_name, destination_name, ".install.lock");
    if (error == 0)
        error = format_private_name(
                owner_name, destination_name, ".installing.owner");
    if (error != 0)
        return error;

    int parent = open(persistent_parent,
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (parent < 0)
        return errno_or_io();
    int lock = -1;
    error = lock_file(parent, lock_name, LOCK_EX, &lock);

    bool destination_present = false;
    if (error == 0)
        error = inspect_existing_root(
                parent, destination_name, &destination_present);
    if (error == 0 && destination_present)
        error = EEXIST;
    if (error == 0)
        error = recover_staging(
                parent, owner_name, destination_name);

    int seed = -1;
    int imported = -1;
    if (error == 0) {
        seed = open(seed_root,
                O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (seed < 0)
            error = errno_or_io();
    }
    if (error == 0) {
        imported = open(imported_root,
                O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (imported < 0)
            error = errno_or_io();
    }

    int staging = -1;
    struct staging_owner owner = {0};
    if (error == 0)
        error = create_staging(parent, destination_name,
                owner_name, &staging, &owner);
    if (error == 0)
        error = build_imported_root(
                seed, imported, staging, progress);
    if (seed >= 0 && close(seed) < 0 && error == 0)
        error = errno_or_io();
    if (imported >= 0 && close(imported) < 0 && error == 0)
        error = errno_or_io();
    if (staging >= 0 && close(staging) < 0 && error == 0)
        error = errno_or_io();
    if (error != 0 && owner.marker_inode != 0) {
        int cleanup_error = remove_owned_staging(
                parent, owner_name, &owner);
        if (cleanup_error != 0)
            error = cleanup_error;
    }

    if (error == 0 && renameatx_np(parent, owner.staging_name,
            parent, destination_name, RENAME_EXCL) < 0) {
        error = errno_or_io();
        int cleanup_error = remove_owned_staging(
                parent, owner_name, &owner);
        if (cleanup_error != 0)
            error = cleanup_error;
    }
    if (error == 0) {
        error = sync_directory_phase(parent,
                ISH_APPLE_ROOTFS_SEED_TEST_PUBLISH_ROOT_SYNC);
        if (error != 0) {
            int rollback_error = rollback_unsynchronized_publish(
                    parent, destination_name, &owner);
            if (rollback_error != 0)
                error = rollback_error;
        }
    }
    if (error == 0)
        (void) unlink_owner_marker(parent, owner_name, &owner,
                ISH_APPLE_ROOTFS_SEED_TEST_PUBLISH_OWNER_SYNC);

    bool completed = error == 0;
    if (lock >= 0) {
        while (flock(lock, LOCK_UN) < 0 && errno == EINTR) {}
        close(lock);
    }
    if (close(parent) < 0 && error == 0)
        error = errno_or_io();
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
            !valid_root_name(source_name) ||
            !valid_root_name(destination_name) ||
            (operation_token != NULL &&
             !ish_apple_rootfs_copy_operation_token_is_valid(
                     operation_token)) ||
            strcmp(source_name, destination_name) == 0)
        return EINVAL;

    char lock_name[NAME_MAX + 1];
    char owner_name[NAME_MAX + 1];
    int error = format_private_name(
            lock_name, destination_name, ".install.lock");
    if (error == 0)
        error = format_private_name(
                owner_name, destination_name, ".installing.owner");
    if (error != 0)
        return error;

    int parent = open(persistent_parent,
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (parent < 0)
        return errno_or_io();
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
        error = errno_or_io();
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
            !valid_root_name(source_name) ||
            !valid_root_name(destination_name) ||
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
            !valid_root_name(source_name) ||
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
        return errno_or_io();
    int iterator_file = dup(parent);
    if (iterator_file < 0) {
        int error = errno_or_io();
        close(parent);
        return error;
    }
    DIR *iterator = fdopendir(iterator_file);
    if (iterator == NULL) {
        int error = errno_or_io();
        close(iterator_file);
        close(parent);
        return error;
    }

    int error = 0;
    errno = 0;
    struct dirent *entry;
    while ((entry = readdir(iterator)) != NULL) {
        if (!valid_root_name(entry->d_name)) {
            errno = 0;
            continue;
        }

        bool root_present;
        int root = -1;
        error = open_existing_root(
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
        error = inspect_copy_operation_marker(
                root, source_name, entry->d_name,
                operation_token, &marker_present, &marker_matches);
        if (error == 0)
            error = verify_named_root_identity(
                    parent, entry->d_name, root);
        if (close(root) < 0 && error == 0)
            error = errno_or_io();
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
        error = errno_or_io();
    if (closedir(iterator) < 0 && error == 0)
        error = errno_or_io();
    if (close(parent) < 0 && error == 0)
        error = errno_or_io();
    return error;
}
