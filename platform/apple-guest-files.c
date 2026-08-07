#include "sdk/iSHApple/Headers/iSHAppleGuestFile.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "fs/fd.h"
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "kernel/init.h"
#include "kernel/task.h"
#include "platform/apple-diagnostics-private.h"
#include "platform/apple-guest-file-mutations.h"
#include "platform/apple-watch-runtime-private.h"
#include "sdk/iSHApple/Headers/iSHAppleDiagnostics.h"

#define GUEST_FILE_COPY_CHUNK (64 * 1024)

typedef int (*guest_file_operation)(
        struct task *task,
        const struct ish_apple_guest_file_request_v1 *request,
        void *context);

static bool reserved_zero(const uint64_t reserved[2]) {
    return reserved[0] == 0 && reserved[1] == 0;
}

static int validate_guest_path(const char *path) {
    if (path == NULL || path[0] != '/')
        return _EINVAL;
    size_t length = strnlen(
            path, (size_t) ISH_APPLE_GUEST_FILE_PATH_BYTES_MAX + 1);
    if (length == 0)
        return _EINVAL;
    return length > ISH_APPLE_GUEST_FILE_PATH_BYTES_MAX ?
            _ENAMETOOLONG : 0;
}

static int validate_request(
        const struct ish_apple_guest_file_request_v1 *request) {
    if (request == NULL)
        return _EINVAL;
    if (request->version != ISH_APPLE_ABI_VERSION)
        return _ENOTSUP;
    if (request->structure_size < sizeof(*request) ||
            request->reserved_0 != 0 ||
            !reserved_zero(request->reserved) ||
            request->request_id == 0 ||
            (request->flags &
                    ~ISH_APPLE_GUEST_FILE_REQUEST_NOFOLLOW) != 0)
        return _EINVAL;
    return validate_guest_path(request->path);
}

static void record_unsupported_if_needed(
        const struct ish_apple_guest_file_request_v1 *request,
        int error) {
    if (error == _ENOSYS || error == _ENOTSUP) {
        ish_apple_diagnostics_record_filesystem(
                ISH_APPLE_DIAGNOSTIC_SCOPE_GUEST_FILE,
                request->request_id,
                ISH_APPLE_DIAGNOSTIC_FILESYSTEM_UNSUPPORTED,
                error);
    }
}

static int run_guest_file_operation(
        const struct ish_apple_guest_file_request_v1 *request,
        guest_file_operation operation,
        void *context) {
    int error = validate_request(request);
    if (error < 0)
        return error;
    error = ish_watch_runtime_operation_availability();
    if (error < 0)
        return error;

    /*
     * prepared task 事务会把 current 绑定到当前宿主线程，确保仍读取 current
     * 的 provider 也使用同一份 guest 凭据与根目录。
     */
    lock(&ish_watch_prepared_task_lock);
    error = ish_watch_runtime_operation_availability();
    if (error >= 0)
        error = begin_new_init_child();
    if (error >= 0) {
        error = operation(current, request, context);
        cancel_prepared_process();
    }
    unlock(&ish_watch_prepared_task_lock);

    record_unsupported_if_needed(request, error);
    return error;
}

static void fill_info(
        const struct ish_apple_guest_file_request_v1 *request,
        const struct statbuf *stat,
        struct ish_apple_guest_file_info_v1 *info) {
    *info = (struct ish_apple_guest_file_info_v1) {
        .version = ISH_APPLE_ABI_VERSION,
        .structure_size = sizeof(*info),
        .request_id = request->request_id,
        .device = stat->dev,
        .inode = stat->inode,
        .size = stat->size,
        .blocks = stat->blocks,
        .mode = stat->mode,
        .link_count = stat->nlink,
        .user_id = stat->uid,
        .group_id = stat->gid,
        .block_size = stat->blksize,
        .access_time_seconds = stat->atime,
        .modification_time_seconds = stat->mtime,
        .status_change_time_seconds = stat->ctime,
        .access_time_nanoseconds = stat->atime_nsec,
        .modification_time_nanoseconds = stat->mtime_nsec,
        .status_change_time_nanoseconds = stat->ctime_nsec,
    };
}

struct stat_context {
    struct ish_apple_guest_file_info_v1 *info;
};

static int stat_operation(
        struct task *task,
        const struct ish_apple_guest_file_request_v1 *request,
        void *opaque) {
    struct stat_context *context = opaque;
    struct statbuf stat;
    int flags = (request->flags &
            ISH_APPLE_GUEST_FILE_REQUEST_NOFOLLOW) != 0 ?
            AT_SYMLINK_NOFOLLOW_ : 0;
    int error = file_statat_task(
            task, AT_FDCWD_, request->path, flags, &stat);
    if (error == 0)
        fill_info(request, &stat, context->info);
    return error;
}

int32_t ish_apple_guest_file_stat(
        const struct ish_apple_guest_file_request_v1 *request,
        struct ish_apple_guest_file_info_v1 *info_out) {
    if (info_out == NULL)
        return _EINVAL;
    memset(info_out, 0, sizeof(*info_out));
    struct stat_context context = {.info = info_out};
    return run_guest_file_operation(
            request, stat_operation, &context);
}

struct list_context {
    struct task *task;
    const struct ish_apple_guest_file_request_v1 *request;
    fd_t directory;
    struct ish_apple_guest_file_directory_entry_v1 *entries;
    uint32_t capacity;
    uint32_t count;
    uint64_t next_cursor;
    int error;
    bool full;
};

static sqword_t emit_directory_entry(
        void *opaque,
        const struct dir_entry *entry,
        off_t_ next_position) {
    struct list_context *context = opaque;
    if (strcmp(entry->name, ".") == 0 ||
            strcmp(entry->name, "..") == 0) {
        if (next_position < 0) {
            context->error = _EOVERFLOW;
            return context->error;
        }
        context->next_cursor = (uint64_t) next_position;
        return 1;
    }
    if (context->count == context->capacity) {
        context->full = true;
        return _ENOSPC;
    }

    struct statbuf stat;
    int error = file_statat_task(
            context->task,
            context->directory,
            entry->name,
            AT_SYMLINK_NOFOLLOW_,
            &stat);
    if (error == _ENOENT) {
        if (next_position < 0) {
            context->error = _EOVERFLOW;
            return context->error;
        }
        context->next_cursor = (uint64_t) next_position;
        return 1;
    }
    if (error < 0 || next_position < 0) {
        context->error = error < 0 ? error : _EOVERFLOW;
        return context->error;
    }

    struct ish_apple_guest_file_directory_entry_v1 *destination =
            &context->entries[context->count];
    memset(destination, 0, sizeof(*destination));
    fill_info(context->request, &stat, &destination->info);
    size_t name_length = strlen(entry->name);
    destination->name_bytes = (uint32_t) name_length;
    memcpy(destination->name, entry->name, name_length + 1);
    context->count++;
    context->next_cursor = (uint64_t) next_position;
    return 1;
}

struct list_arguments {
    uint64_t cursor;
    struct ish_apple_guest_file_directory_entry_v1 *entries;
    uint32_t capacity;
    uint32_t *count;
    uint64_t *next_cursor;
    int32_t *eof;
};

static int list_operation(
        struct task *task,
        const struct ish_apple_guest_file_request_v1 *request,
        void *opaque) {
    struct list_arguments *arguments = opaque;
    int flags = O_RDONLY_ | O_DIRECTORY_;
    if ((request->flags &
            ISH_APPLE_GUEST_FILE_REQUEST_NOFOLLOW) != 0)
        flags |= O_NOFOLLOW_;
    fd_t directory = file_openat_task(
            task, AT_FDCWD_, request->path, flags, 0);
    if (directory < 0)
        return directory;

    int error = 0;
    if (arguments->cursor > INT64_MAX) {
        error = _EOVERFLOW;
    } else if (arguments->cursor != 0) {
        sqword_t positioned = file_lseek_task(
                task,
                directory,
                (sqword_t) arguments->cursor,
                LSEEK_SET);
        if (positioned < 0)
            error = (int) positioned;
    }

    struct list_context context = {
        .task = task,
        .request = request,
        .directory = directory,
        .entries = arguments->entries,
        .capacity = arguments->capacity,
        .next_cursor = arguments->cursor,
    };
    if (error == 0) {
        sqword_t result = file_getdents_task(
                task, directory, emit_directory_entry, &context);
        if (context.error < 0)
            error = context.error;
        else if (result < 0)
            error = (int) result;
    }
    int close_error = f_close_task(task, directory);
    if (error == 0 && close_error < 0)
        error = close_error;
    if (error == 0) {
        *arguments->count = context.count;
        *arguments->next_cursor = context.next_cursor;
        *arguments->eof = context.full ? 0 : 1;
    }
    return error;
}

int32_t ish_apple_guest_file_list(
        const struct ish_apple_guest_file_request_v1 *request,
        uint64_t cursor,
        struct ish_apple_guest_file_directory_entry_v1 *entries,
        uint32_t capacity,
        uint32_t *count_out,
        uint64_t *next_cursor_out,
        int32_t *eof_out) {
    if (entries == NULL || capacity == 0 || count_out == NULL ||
            next_cursor_out == NULL || eof_out == NULL)
        return _EINVAL;
    *count_out = 0;
    *next_cursor_out = cursor;
    *eof_out = 0;
    struct list_arguments arguments = {
        .cursor = cursor,
        .entries = entries,
        .capacity = capacity,
        .count = count_out,
        .next_cursor = next_cursor_out,
        .eof = eof_out,
    };
    return run_guest_file_operation(
            request, list_operation, &arguments);
}

struct read_arguments {
    uint64_t offset;
    void *bytes;
    uint32_t capacity;
    uint32_t *count;
    uint64_t *total_size;
    int32_t *eof;
};

static int read_operation(
        struct task *task,
        const struct ish_apple_guest_file_request_v1 *request,
        void *opaque) {
    struct read_arguments *arguments = opaque;
    if (arguments->offset > INT64_MAX)
        return _EOVERFLOW;
    int flags = O_RDONLY_;
    if ((request->flags &
            ISH_APPLE_GUEST_FILE_REQUEST_NOFOLLOW) != 0)
        flags |= O_NOFOLLOW_;
    fd_t number = file_openat_task(
            task, AT_FDCWD_, request->path, flags, 0);
    if (number < 0)
        return number;

    struct statbuf stat;
    int error = file_fstat_task(task, number, &stat);
    struct fd *file = error == 0 ?
            f_get_task_retain(task, number) : NULL;
    if (error == 0 && file == NULL)
        error = _EBADF;
    ssize_t count = error == 0 ? file_pread_fd(
            file,
            arguments->bytes,
            arguments->capacity,
            (off_t_) arguments->offset) : error;
    if (file != NULL)
        fd_close(file);
    int close_error = f_close_task(task, number);
    if (count >= 0 && close_error < 0)
        count = close_error;
    if (count < 0)
        return (int) count;

    *arguments->count = (uint32_t) count;
    *arguments->total_size = stat.size;
    uint64_t end = arguments->offset + (uint64_t) count;
    bool known_size_reached = stat.size != 0 && end >= stat.size;
    *arguments->eof = arguments->capacity == 0 ?
            (arguments->offset >= stat.size ? 1 : 0) :
            (count == 0 || (uint32_t) count < arguments->capacity ||
                    known_size_reached ? 1 : 0);
    return 0;
}

int32_t ish_apple_guest_file_read(
        const struct ish_apple_guest_file_request_v1 *request,
        uint64_t offset,
        void *bytes,
        uint32_t capacity,
        uint32_t *count_out,
        uint64_t *total_size_out,
        int32_t *eof_out) {
    if ((bytes == NULL && capacity != 0) || count_out == NULL ||
            total_size_out == NULL || eof_out == NULL)
        return _EINVAL;
    *count_out = 0;
    *total_size_out = 0;
    *eof_out = 0;
    struct read_arguments arguments = {
        .offset = offset,
        .bytes = bytes,
        .capacity = capacity,
        .count = count_out,
        .total_size = total_size_out,
        .eof = eof_out,
    };
    return run_guest_file_operation(
            request, read_operation, &arguments);
}

struct replacement_file {
    struct task *task;
    fd_t parent;
    fd_t temporary;
    char target_name[NAME_MAX + 1];
    char temporary_name[NAME_MAX + 1];
    struct statbuf original;
    bool target_exists;
    bool temporary_exists;
};

static int split_parent_and_name(
        const char *path,
        char parent[MAX_PATH],
        char name[NAME_MAX + 1]) {
    size_t length = strlen(path);
    if (length == 1 || path[length - 1] == '/')
        return _EISDIR;
    const char *separator = strrchr(path, '/');
    size_t name_length = strlen(separator + 1);
    if (name_length == 0)
        return _EISDIR;
    if (name_length > NAME_MAX)
        return _ENAMETOOLONG;
    size_t parent_length = separator == path ? 1 :
            (size_t) (separator - path);
    memcpy(parent, path, parent_length);
    parent[parent_length] = '\0';
    memcpy(name, separator + 1, name_length + 1);
    return 0;
}

static int open_replacement(
        struct task *task,
        const struct ish_apple_guest_file_request_v1 *request,
        uint32_t new_mode,
        bool require_existing,
        struct replacement_file *replacement) {
    char parent_path[MAX_PATH];
    int error = split_parent_and_name(
            request->path,
            parent_path,
            replacement->target_name);
    if (error < 0)
        return error;

    replacement->task = task;
    replacement->parent = file_openat_task(
            task,
            AT_FDCWD_,
            parent_path,
            O_RDONLY_ | O_DIRECTORY_,
            0);
    if (replacement->parent < 0)
        return replacement->parent;

    error = file_statat_task(
            task,
            replacement->parent,
            replacement->target_name,
            AT_SYMLINK_NOFOLLOW_,
            &replacement->original);
    if (error == 0) {
        if (!S_ISREG(replacement->original.mode))
            error = S_ISDIR(replacement->original.mode) ?
                    _EISDIR : _EINVAL;
        else
            replacement->target_exists = true;
    } else if (error == _ENOENT && !require_existing) {
        error = 0;
    }
    if (error < 0)
        return error;

    for (unsigned attempt = 0; attempt < 128; attempt++) {
        uint64_t random[2];
        arc4random_buf(random, sizeof(random));
        int length = snprintf(
                replacement->temporary_name,
                sizeof(replacement->temporary_name),
                ".ish-file-%016llx-%016llx%016llx",
                (unsigned long long) request->request_id,
                (unsigned long long) random[0],
                (unsigned long long) random[1]);
        if (length < 0 || length >=
                (int) sizeof(replacement->temporary_name))
            return _ENAMETOOLONG;
        replacement->temporary = file_openat_task(
                task,
                replacement->parent,
                replacement->temporary_name,
                O_WRONLY_ | O_CREAT_ | O_EXCL_ | O_NOFOLLOW_,
                (mode_t_) new_mode);
        if (replacement->temporary >= 0) {
            replacement->temporary_exists = true;
            return 0;
        }
        error = replacement->temporary;
        if (error != _EEXIST)
            return error;
    }
    return _EEXIST;
}

static void cleanup_replacement(struct replacement_file *replacement) {
    if (replacement->temporary >= 0) {
        (void) f_close_task(
                replacement->task, replacement->temporary);
        replacement->temporary = -1;
    }
    if (replacement->temporary_exists) {
        (void) file_unlinkat_task(
                replacement->task,
                replacement->parent,
                replacement->temporary_name,
                false);
        replacement->temporary_exists = false;
    }
    if (replacement->parent >= 0) {
        (void) f_close_task(replacement->task, replacement->parent);
        replacement->parent = -1;
    }
}

static int write_all(
        struct task *task,
        fd_t file,
        const void *bytes,
        size_t length) {
    const unsigned char *cursor = bytes;
    size_t remaining = length;
    while (remaining != 0) {
        ssize_t written = file_write_task(
                task, file, cursor, remaining);
        if (written < 0)
            return (int) written;
        if (written == 0)
            return _EIO;
        cursor += (size_t) written;
        remaining -= (size_t) written;
    }
    return 0;
}

static int apply_replacement_metadata(
        struct replacement_file *replacement,
        uint32_t new_mode) {
    int error = 0;
    if (replacement->target_exists) {
        error = file_fchown_task(
                replacement->task,
                replacement->temporary,
                replacement->original.uid,
                replacement->original.gid);
        if (error == 0)
            error = file_fchmod_task(
                    replacement->task,
                    replacement->temporary,
                    replacement->original.mode & 07777);
    } else {
        error = file_fchmod_task(
                replacement->task,
                replacement->temporary,
                (mode_t_) new_mode);
    }
    return error;
}

static bool stat_identity_equal(
        const struct statbuf *left,
        const struct statbuf *right) {
    return left->dev == right->dev &&
            left->inode == right->inode &&
            left->size == right->size &&
            left->mtime == right->mtime &&
            left->mtime_nsec == right->mtime_nsec &&
            left->ctime == right->ctime &&
            left->ctime_nsec == right->ctime_nsec;
}

static int commit_replacement(
        struct replacement_file *replacement,
        uint32_t new_mode,
        bool verify_original) {
    int error = apply_replacement_metadata(replacement, new_mode);
    if (error == 0)
        error = file_sync_task(
                replacement->task,
                replacement->temporary,
                false);
    int close_error = f_close_task(
            replacement->task, replacement->temporary);
    replacement->temporary = -1;
    if (error == 0 && close_error < 0)
        error = close_error;

    if (error == 0 && verify_original) {
        struct statbuf current_stat;
        error = file_statat_task(
                replacement->task,
                replacement->parent,
                replacement->target_name,
                AT_SYMLINK_NOFOLLOW_,
                &current_stat);
        if (error == 0 && !stat_identity_equal(
                &replacement->original, &current_stat))
            error = _ESTALE;
    }
    if (error == 0)
        error = file_renameat_task(
                replacement->task,
                replacement->parent,
                replacement->temporary_name,
                replacement->parent,
                replacement->target_name);
    if (error < 0)
        return error;

    replacement->temporary_exists = false;
    struct fd *parent = f_get_task_retain(
            replacement->task, replacement->parent);
    if (parent != NULL) {
        /* rename 已发布，目录同步只增强掉电持久性，不反转成功结果。 */
        (void) file_sync_fd(parent, false);
        fd_close(parent);
    }
    close_error = f_close_task(
            replacement->task, replacement->parent);
    replacement->parent = -1;
    (void) close_error;
    return 0;
}

struct write_arguments {
    const void *bytes;
    uint32_t length;
    uint32_t mode;
};

static int write_operation(
        struct task *task,
        const struct ish_apple_guest_file_request_v1 *request,
        void *opaque) {
    struct write_arguments *arguments = opaque;
    struct replacement_file replacement = {
        .parent = -1,
        .temporary = -1,
    };
    int error = open_replacement(
            task,
            request,
            arguments->mode,
            false,
            &replacement);
    if (error == 0)
        error = write_all(
                task,
                replacement.temporary,
                arguments->bytes,
                arguments->length);
    if (error == 0)
        error = commit_replacement(
                &replacement, arguments->mode, false);
    if (error < 0)
        cleanup_replacement(&replacement);
    return error;
}

int32_t ish_apple_guest_file_write(
        const struct ish_apple_guest_file_request_v1 *request,
        const void *bytes,
        uint32_t length,
        uint32_t mode) {
    if ((bytes == NULL && length != 0) || (mode & ~07777U) != 0)
        return _EINVAL;
    struct write_arguments arguments = {
        .bytes = bytes,
        .length = length,
        .mode = mode,
    };
    return run_guest_file_operation(
            request, write_operation, &arguments);
}

static int copy_exact(
        struct task *task,
        fd_t source,
        fd_t destination,
        uint64_t length,
        unsigned char *buffer) {
    while (length != 0) {
        size_t requested = length < GUEST_FILE_COPY_CHUNK ?
                (size_t) length : GUEST_FILE_COPY_CHUNK;
        ssize_t count = file_read_task(
                task, source, buffer, requested);
        if (count < 0)
            return (int) count;
        if (count == 0)
            return _EIO;
        int error = write_all(
                task, destination, buffer, (size_t) count);
        if (error < 0)
            return error;
        length -= (uint64_t) count;
    }
    return 0;
}

static int copy_to_end(
        struct task *task,
        fd_t source,
        fd_t destination,
        unsigned char *buffer) {
    for (;;) {
        ssize_t count = file_read_task(
                task, source, buffer, GUEST_FILE_COPY_CHUNK);
        if (count < 0)
            return (int) count;
        if (count == 0)
            return 0;
        int error = write_all(
                task, destination, buffer, (size_t) count);
        if (error < 0)
            return error;
    }
}

struct edit_arguments {
    uint64_t offset;
    uint64_t removed_length;
    const void *replacement;
    uint32_t replacement_length;
};

static int edit_operation(
        struct task *task,
        const struct ish_apple_guest_file_request_v1 *request,
        void *opaque) {
    struct edit_arguments *arguments = opaque;
    struct replacement_file replacement = {
        .parent = -1,
        .temporary = -1,
    };
    int error = open_replacement(
            task, request, 0600, true, &replacement);
    if (error < 0) {
        cleanup_replacement(&replacement);
        return error;
    }
    if (arguments->offset > replacement.original.size ||
            arguments->removed_length >
                    replacement.original.size - arguments->offset) {
        cleanup_replacement(&replacement);
        return _EINVAL;
    }

    fd_t source = file_openat_task(
            task,
            replacement.parent,
            replacement.target_name,
            O_RDONLY_ | O_NOFOLLOW_,
            0);
    if (source < 0) {
        cleanup_replacement(&replacement);
        return source;
    }
    unsigned char *buffer = malloc(GUEST_FILE_COPY_CHUNK);
    if (buffer == NULL)
        error = _ENOMEM;
    if (error == 0)
        error = copy_exact(
                task,
                source,
                replacement.temporary,
                arguments->offset,
                buffer);
    if (error == 0)
        error = write_all(
                task,
                replacement.temporary,
                arguments->replacement,
                arguments->replacement_length);
    if (error == 0) {
        uint64_t suffix = arguments->offset +
                arguments->removed_length;
        if (suffix > INT64_MAX)
            error = _EOVERFLOW;
        else {
            sqword_t positioned = file_lseek_task(
                    task, source, (sqword_t) suffix, LSEEK_SET);
            if (positioned < 0)
                error = (int) positioned;
        }
    }
    if (error == 0)
        error = copy_to_end(
                task,
                source,
                replacement.temporary,
                buffer);
    free(buffer);
    int close_error = f_close_task(task, source);
    if (error == 0 && close_error < 0)
        error = close_error;
    if (error == 0)
        error = commit_replacement(&replacement, 0600, true);
    if (error < 0)
        cleanup_replacement(&replacement);
    return error;
}

int32_t ish_apple_guest_file_edit(
        const struct ish_apple_guest_file_request_v1 *request,
        uint64_t offset,
        uint64_t removed_length,
        const void *replacement,
        uint32_t replacement_length) {
    if (replacement == NULL && replacement_length != 0)
        return _EINVAL;
    struct edit_arguments arguments = {
        .offset = offset,
        .removed_length = removed_length,
        .replacement = replacement,
        .replacement_length = replacement_length,
    };
    return run_guest_file_operation(
            request, edit_operation, &arguments);
}

struct remove_arguments {
    uint32_t flags;
};

static int remove_operation(
        struct task *task,
        const struct ish_apple_guest_file_request_v1 *request,
        void *opaque) {
    struct remove_arguments *arguments = opaque;
    return ish_apple_guest_file_remove_task(
            task,
            request->path,
            (arguments->flags &
                    ISH_APPLE_GUEST_FILE_REMOVE_RECURSIVE) != 0);
}

int32_t ish_apple_guest_file_remove(
        const struct ish_apple_guest_file_request_v1 *request,
        uint32_t flags) {
    if ((flags & ~ISH_APPLE_GUEST_FILE_REMOVE_RECURSIVE) != 0)
        return _EINVAL;
    struct remove_arguments arguments = {.flags = flags};
    return run_guest_file_operation(
            request, remove_operation, &arguments);
}

struct rename_arguments {
    const char *destination;
};

static int rename_operation(
        struct task *task,
        const struct ish_apple_guest_file_request_v1 *request,
        void *opaque) {
    struct rename_arguments *arguments = opaque;
    return ish_apple_guest_file_rename_task(
            task, request->path, arguments->destination);
}

int32_t ish_apple_guest_file_rename(
        const struct ish_apple_guest_file_request_v1 *request,
        const char *destination) {
    int error = validate_guest_path(destination);
    if (error < 0)
        return error;
    struct rename_arguments arguments = {
        .destination = destination,
    };
    return run_guest_file_operation(
            request, rename_operation, &arguments);
}

struct mkdir_arguments {
    uint32_t mode;
    uint32_t flags;
};

static int mkdir_operation(
        struct task *task,
        const struct ish_apple_guest_file_request_v1 *request,
        void *opaque) {
    struct mkdir_arguments *arguments = opaque;
    return ish_apple_guest_file_mkdir_task(
            task,
            request->path,
            arguments->mode,
            (arguments->flags &
                    ISH_APPLE_GUEST_FILE_MKDIR_PARENTS) != 0);
}

int32_t ish_apple_guest_file_mkdir(
        const struct ish_apple_guest_file_request_v1 *request,
        uint32_t mode,
        uint32_t flags) {
    if ((mode & ~07777U) != 0 ||
            (flags & ~ISH_APPLE_GUEST_FILE_MKDIR_PARENTS) != 0)
        return _EINVAL;
    struct mkdir_arguments arguments = {
        .mode = mode,
        .flags = flags,
    };
    return run_guest_file_operation(
            request, mkdir_operation, &arguments);
}
