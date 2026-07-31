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

// 负责宿主文件树复制、稀疏文件、硬链接与复制进度。

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
            return ish_apple_rootfs_errno_or_io();
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
        return ish_apple_rootfs_errno_or_io();
    *result_out = result;
    return 0;
}

int ish_apple_rootfs_report_root_copy_progress(
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

int ish_apple_rootfs_report_copy_stage(
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
            return ish_apple_rootfs_errno_or_io();
        }
        if (count == 0)
            return EIO;
        int error = write_all(destination, buffer, (size_t) count);
        if (error != 0)
            return error;
        error = ish_apple_rootfs_report_root_copy_progress(
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
        return ish_apple_rootfs_errno_or_io();
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
            error = ish_apple_rootfs_errno_or_io();
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
            error = ish_apple_rootfs_report_root_copy_progress(
                    context, (uintmax_t) count, path);
    }
    if (error == 0 && ftruncate(destination, logical_size) < 0)
        error = ish_apple_rootfs_errno_or_io();
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
            ish_apple_rootfs_errno_or_io() : 0;
}

int ish_apple_rootfs_create_regular_at(
        int directory, const char *name,
        const void *bytes, size_t length, bool synchronize) {
    int file = openat(directory, name,
            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (file < 0)
        return ish_apple_rootfs_errno_or_io();
    int error = write_all(file, bytes, length);
    if (error == 0 && synchronize && fsync(file) < 0)
        error = ish_apple_rootfs_errno_or_io();
    if (close(file) < 0 && error == 0)
        error = ish_apple_rootfs_errno_or_io();
    if (error != 0)
        unlinkat(directory, name, 0);
    return error;
}

int ish_apple_rootfs_read_regular_at(
        int directory, const char *name, size_t limit,
        char **bytes_out, size_t *length_out) {
    int file = openat(directory, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (file < 0)
        return ish_apple_rootfs_errno_or_io();

    struct stat metadata = {0};
    int error = 0;
    if (fstat(file, &metadata) < 0)
        error = ish_apple_rootfs_errno_or_io();
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
            error = EFBIG;
    }
    if (close(file) < 0 && error == 0)
        error = ish_apple_rootfs_errno_or_io();
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
        return ish_apple_rootfs_errno_or_io();
    if (!S_ISDIR(metadata.st_mode)) {
        if (unlinkat(parent, name, 0) < 0)
            return ish_apple_rootfs_errno_or_io();
        return 0;
    }

    int directory = openat(parent, name,
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (directory < 0)
        return ish_apple_rootfs_errno_or_io();
    int iterator_fd = dup(directory);
    if (iterator_fd < 0) {
        int error = ish_apple_rootfs_errno_or_io();
        close(directory);
        return error;
    }
    DIR *iterator = fdopendir(iterator_fd);
    if (iterator == NULL) {
        int error = ish_apple_rootfs_errno_or_io();
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
        error = ish_apple_rootfs_errno_or_io();
    if (closedir(iterator) < 0 && error == 0)
        error = ish_apple_rootfs_errno_or_io();
    if (close(directory) < 0 && error == 0)
        error = ish_apple_rootfs_errno_or_io();
    if (error == 0 && unlinkat(parent, name, AT_REMOVEDIR) < 0)
        error = ish_apple_rootfs_errno_or_io();
    return error;
}

int ish_apple_rootfs_remove_entry_at(int parent, const char *name) {
    return remove_entry_at(parent, name, 0);
}

int ish_apple_rootfs_copy_regular_at(
        int source_directory, int destination_directory,
        const char *name) {
    int source = openat(source_directory, name,
            O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (source < 0)
        return ish_apple_rootfs_errno_or_io();
    struct stat metadata = {0};
    int error = 0;
    if (fstat(source, &metadata) < 0)
        error = ish_apple_rootfs_errno_or_io();
    else if (!S_ISREG(metadata.st_mode))
        error = EINVAL;

    int destination = -1;
    if (error == 0) {
        destination = openat(destination_directory, name,
                O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                0600);
        if (destination < 0)
            error = ish_apple_rootfs_errno_or_io();
    }
    unsigned char buffer[COPY_BUFFER_SIZE];
    while (error == 0) {
        ssize_t count = read(source, buffer, sizeof(buffer));
        if (count < 0) {
            if (errno == EINTR)
                continue;
            error = ish_apple_rootfs_errno_or_io();
        } else if (count == 0) {
            break;
        } else {
            error = write_all(destination, buffer, (size_t) count);
        }
    }
    if (destination >= 0 && error == 0 && fsync(destination) < 0)
        error = ish_apple_rootfs_errno_or_io();
    if (destination >= 0 && close(destination) < 0 && error == 0)
        error = ish_apple_rootfs_errno_or_io();
    if (close(source) < 0 && error == 0)
        error = ish_apple_rootfs_errno_or_io();
    if (error != 0 && destination >= 0)
        unlinkat(destination_directory, name, 0);
    return error;
}

int ish_apple_rootfs_copy_directory_contents(
        int source, int destination, unsigned depth) {
    if (depth > COPY_TREE_DEPTH_LIMIT)
        return ELOOP;
    int iterator_fd = dup(source);
    if (iterator_fd < 0)
        return ish_apple_rootfs_errno_or_io();
    DIR *iterator = fdopendir(iterator_fd);
    if (iterator == NULL) {
        int error = ish_apple_rootfs_errno_or_io();
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
            error = ish_apple_rootfs_errno_or_io();
            break;
        }
        if (S_ISREG(metadata.st_mode)) {
            error = ish_apple_rootfs_copy_regular_at(source, destination, entry->d_name);
        } else if (S_ISDIR(metadata.st_mode)) {
            // 在创建下一层前拒绝深树，确保失败 staging 总能被删除器收回。
            if (depth >= COPY_TREE_DEPTH_LIMIT) {
                error = ELOOP;
                break;
            }
            if (mkdirat(destination, entry->d_name, 0700) < 0) {
                error = ish_apple_rootfs_errno_or_io();
            } else {
                int source_child = openat(source, entry->d_name,
                        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
                int destination_child = -1;
                if (source_child < 0) {
                    error = ish_apple_rootfs_errno_or_io();
                } else {
                    destination_child = openat(destination, entry->d_name,
                            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
                    if (destination_child < 0)
                        error = ish_apple_rootfs_errno_or_io();
                    else
                        error = ish_apple_rootfs_copy_directory_contents(source_child,
                                destination_child, depth + 1);
                }
                if (source_child >= 0 && close(source_child) < 0 && error == 0)
                    error = ish_apple_rootfs_errno_or_io();
                if (destination_child >= 0 &&
                        close(destination_child) < 0 && error == 0)
                    error = ish_apple_rootfs_errno_or_io();
            }
        } else {
            error = EINVAL;
        }
        if (error != 0)
            break;
        errno = 0;
    }
    if (entry == NULL && errno != 0 && error == 0)
        error = ish_apple_rootfs_errno_or_io();
    if (closedir(iterator) < 0 && error == 0)
        error = ish_apple_rootfs_errno_or_io();
    if (error == 0)
        error = ish_apple_rootfs_sync_directory_internal(destination);
    return error;
}
void ish_apple_rootfs_copy_context_destroy(struct root_copy_context *context) {
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

void ish_apple_rootfs_stat_times(
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
    ish_apple_rootfs_stat_times(metadata, times);
    return utimensat(
            directory, name, times, AT_SYMLINK_NOFOLLOW) < 0 ?
            ish_apple_rootfs_errno_or_io() : 0;
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
        return ish_apple_rootfs_errno_or_io();
    struct stat metadata;
    int error = 0;
    if (fstat(source, &metadata) < 0)
        error = ish_apple_rootfs_errno_or_io();
    else if (!S_ISREG(metadata.st_mode))
        error = EINVAL;

    const char *existing_path = error == 0 && metadata.st_nlink > 1 ?
            copied_hardlink_path(context, &metadata) : NULL;
    if (existing_path != NULL) {
        if (linkat(context->destination_root, existing_path,
                destination_directory, name, 0) < 0)
            error = ish_apple_rootfs_errno_or_io();
        if (error == 0)
            error = ish_apple_rootfs_report_root_copy_progress(
                    context, (uintmax_t) metadata.st_size,
                    destination_path);
        if (close(source) < 0 && error == 0)
            error = ish_apple_rootfs_errno_or_io();
        return error;
    }

    int destination = -1;
    if (error == 0) {
        destination = openat(destination_directory, name,
                O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                0600);
        if (destination < 0)
            error = ish_apple_rootfs_errno_or_io();
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
        error = ish_apple_rootfs_report_root_copy_progress(
                context,
                (uintmax_t) metadata.st_size - copied_for_file,
                destination_path);
    if (destination >= 0 && error == 0 &&
            fchmod(destination, metadata.st_mode & 0777) < 0)
        error = ish_apple_rootfs_errno_or_io();
    if (destination >= 0 && error == 0) {
        struct timespec times[2];
        ish_apple_rootfs_stat_times(&metadata, times);
        if (futimens(destination, times) < 0)
            error = ish_apple_rootfs_errno_or_io();
    }
    if (destination >= 0 && error == 0 && fsync(destination) < 0)
        error = ish_apple_rootfs_errno_or_io();
    if (destination >= 0 && close(destination) < 0 && error == 0)
        error = ish_apple_rootfs_errno_or_io();
    if (close(source) < 0 && error == 0)
        error = ish_apple_rootfs_errno_or_io();
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
            error = ish_apple_rootfs_errno_or_io();
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
        return ish_apple_rootfs_errno_or_io();
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

int ish_apple_rootfs_copy_managed_root_contents(
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
        // 复制操作凭据属于宿主事务元数据，不能继承到下一代副本。
        if (depth == 0 && strncmp(
                entry->d_name, ish_apple_rootfs_copy_operation_marker_prefix,
                sizeof(ish_apple_rootfs_copy_operation_marker_prefix) - 1) == 0)
            continue;
        char child_path[PATH_MAX];
        error = format_copy_path(
                child_path, destination_path, entry->d_name);
        if (error != 0)
            break;
        error = ish_apple_rootfs_report_root_copy_progress(
                context, 0, child_path);
        if (error != 0)
            break;

        struct stat metadata;
        if (fstatat(source, entry->d_name, &metadata,
                AT_SYMLINK_NOFOLLOW) < 0) {
            error = ish_apple_rootfs_errno_or_io();
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
                error = ish_apple_rootfs_errno_or_io();
            } else {
                int source_child = openat(source, entry->d_name,
                        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
                int destination_child = -1;
                struct stat opened_metadata;
                if (source_child < 0) {
                    error = ish_apple_rootfs_errno_or_io();
                } else if (fstat(source_child, &opened_metadata) < 0) {
                    error = ish_apple_rootfs_errno_or_io();
                } else if (opened_metadata.st_dev != metadata.st_dev ||
                        opened_metadata.st_ino != metadata.st_ino) {
                    error = EAGAIN;
                } else {
                    destination_child = openat(destination, entry->d_name,
                            O_RDONLY | O_DIRECTORY |
                            O_CLOEXEC | O_NOFOLLOW);
                    if (destination_child < 0)
                        error = ish_apple_rootfs_errno_or_io();
                    else
                        error = ish_apple_rootfs_copy_managed_root_contents(
                                source_child, destination_child,
                                child_path, depth + 1, context);
                }
                if (destination_child >= 0 && error == 0 &&
                        fchmod(destination_child,
                                metadata.st_mode & 0777) < 0)
                    error = ish_apple_rootfs_errno_or_io();
                if (destination_child >= 0 && error == 0) {
                    struct timespec times[2];
                    ish_apple_rootfs_stat_times(&metadata, times);
                    if (futimens(destination_child, times) < 0)
                        error = ish_apple_rootfs_errno_or_io();
                }
                if (destination_child >= 0 && error == 0)
                    error = ish_apple_rootfs_sync_directory_internal(destination_child);
                if (source_child >= 0 &&
                        close(source_child) < 0 && error == 0)
                    error = ish_apple_rootfs_errno_or_io();
                if (destination_child >= 0 &&
                        close(destination_child) < 0 && error == 0)
                    error = ish_apple_rootfs_errno_or_io();
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
        error = ish_apple_rootfs_errno_or_io();
    if (closedir(iterator) < 0 && error == 0)
        error = ish_apple_rootfs_errno_or_io();
    if (error == 0)
        error = ish_apple_rootfs_sync_directory_internal(destination);
    return error;
}
