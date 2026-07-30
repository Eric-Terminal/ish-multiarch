#include "platform/apple-watch-root-archive.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "platform/apple-rootfs-storage-private.h"
#include "tools/fakefs.h"

static int errno_or_io(void) {
    return errno == 0 ? EIO : errno;
}

static void clear_error(
        struct ish_apple_watch_root_archive_error *error_out) {
    if (error_out != NULL)
        *error_out = (struct ish_apple_watch_root_archive_error) {0};
}

static void record_fakefs_error(
        struct ish_apple_watch_root_archive_error *error_out,
        struct fakefsify_error *error) {
    if (error_out != NULL) {
        error_out->kind = error->type + 1;
        error_out->code = error->code;
        error_out->line = error->line;
        if (error->message != NULL)
            snprintf(error_out->message, sizeof(error_out->message),
                    "%s", error->message);
    }
    free(error->message);
    error->message = NULL;
}

static bool safe_leaf_name(const char *name) {
    if (name == NULL || name[0] == '\0' ||
            strlen(name) > NAME_MAX ||
            strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return false;
    return strchr(name, '/') == NULL;
}

static bool has_suffix(const char *name, const char *suffix) {
    size_t name_length = strlen(name);
    size_t suffix_length = strlen(suffix);
    return name_length > suffix_length &&
            strcasecmp(name + name_length - suffix_length, suffix) == 0;
}

bool ish_apple_watch_root_archive_is_supported_name(const char *name) {
    return safe_leaf_name(name) &&
            (has_suffix(name, ".tar") ||
             has_suffix(name, ".tar.gz") ||
             has_suffix(name, ".tgz"));
}

static int open_directory(const char *path) {
    int directory = open(path,
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    return directory < 0 ? -errno_or_io() : directory;
}

static bool hexadecimal_token(const char *value, size_t length) {
    for (size_t index = 0; index < length; index++) {
        if (!((value[index] >= '0' && value[index] <= '9') ||
                (value[index] >= 'a' && value[index] <= 'f')))
            return false;
    }
    return true;
}

static bool is_watch_archive_partial(const char *name) {
    static const char prefix[] = ".watch-root-";
    static const char suffix[] = ".partial";
    if (strncmp(name, prefix, sizeof(prefix) - 1) != 0)
        return false;
    const char *kind = name + sizeof(prefix) - 1;
    const char *token = NULL;
    if (strncmp(kind, "archive-", 8) == 0)
        token = kind + 8;
    else if (strncmp(kind, "fakefs-", 7) == 0)
        token = kind + 7;
    else if (strncmp(kind, "export-", 7) == 0)
        token = kind + 7;
    if (token == NULL)
        return false;
    size_t suffix_length = sizeof(suffix) - 1;
    size_t token_length = strlen(token);
    return token_length == 32 + suffix_length &&
            strcmp(token + 32, suffix) == 0 &&
            hexadecimal_token(token, 32);
}

int ish_apple_watch_root_archive_cleanup(
        const char *persistent_parent) {
    if (persistent_parent == NULL || persistent_parent[0] == '\0')
        return EINVAL;
    int parent = open_directory(persistent_parent);
    if (parent < 0)
        return -parent;
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
    bool removed = false;
    errno = 0;
    struct dirent *entry;
    while ((entry = readdir(iterator)) != NULL) {
        if (!is_watch_archive_partial(entry->d_name))
            continue;
        error = ish_apple_rootfs_remove_entry_at(
                parent, entry->d_name);
        if (error != 0)
            break;
        removed = true;
        errno = 0;
    }
    if (entry == NULL && errno != 0 && error == 0)
        error = errno_or_io();
    if (closedir(iterator) < 0 && error == 0)
        error = errno_or_io();
    if (removed && error == 0)
        error = ish_apple_rootfs_sync_directory(parent);
    if (close(parent) < 0 && error == 0)
        error = errno_or_io();
    return error;
}

static void generate_token(char output[33]) {
    unsigned char random[16];
    static const char hexadecimal[] = "0123456789abcdef";
    arc4random_buf(random, sizeof(random));
    for (size_t index = 0; index < sizeof(random); index++) {
        output[index * 2] = hexadecimal[random[index] >> 4u];
        output[index * 2 + 1] = hexadecimal[random[index] & 0x0fu];
    }
    output[32] = '\0';
}

static int format_private_name(
        char output[NAME_MAX + 1],
        const char *kind,
        const char token[33]) {
    int length = snprintf(output, NAME_MAX + 1,
            ".watch-root-%s-%s.partial", kind, token);
    return length < 0 || length > NAME_MAX ? ENAMETOOLONG : 0;
}

static int format_path(
        char output[PATH_MAX],
        const char *directory,
        const char *name) {
    int length = snprintf(output, PATH_MAX, "%s/%s", directory, name);
    return length < 0 || length >= PATH_MAX ? ENAMETOOLONG : 0;
}

static int report_progress(
        struct progress progress,
        double fraction,
        const char *message) {
    bool cancelled = false;
    if (progress.callback != NULL)
        progress.callback(
                progress.cookie, fraction, message, &cancelled);
    return cancelled ? ECANCELED : 0;
}

static int copy_file_contents(
        int source,
        int destination,
        off_t total_size,
        struct progress progress) {
    char bytes[64 * 1024];
    off_t copied = 0;
    for (;;) {
        ssize_t count = read(source, bytes, sizeof(bytes));
        if (count < 0) {
            if (errno == EINTR)
                continue;
            return errno_or_io();
        }
        if (count == 0)
            return report_progress(
                    progress, 0.10, "已创建私有归档快照");
        size_t offset = 0;
        while (offset < (size_t) count) {
            ssize_t written = write(
                    destination, bytes + offset,
                    (size_t) count - offset);
            if (written < 0) {
                if (errno == EINTR)
                    continue;
                return errno_or_io();
            }
            if (written == 0)
                return EIO;
            offset += (size_t) written;
        }
        copied += count;
        double fraction = total_size > 0 ?
                0.10 * (double) copied / (double) total_size : 0.05;
        int error = report_progress(
                progress, fraction, "正在创建私有归档快照");
        if (error != 0)
            return error;
    }
}

static int snapshot_shared_archive(
        int shared,
        int parent,
        const char *archive_name,
        const char *snapshot_name,
        struct progress progress) {
    struct stat named;
    if (fstatat(shared, archive_name, &named,
            AT_SYMLINK_NOFOLLOW) < 0)
        return errno_or_io();
    if (!S_ISREG(named.st_mode))
        return EINVAL;

    int source = openat(shared, archive_name,
            O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (source < 0)
        return errno_or_io();
    struct stat opened;
    int error = 0;
    if (fstat(source, &opened) < 0)
        error = errno_or_io();
    else if (!S_ISREG(opened.st_mode) ||
            opened.st_dev != named.st_dev ||
            opened.st_ino != named.st_ino)
        error = EAGAIN;

    int snapshot = -1;
    if (error == 0) {
        snapshot = openat(parent, snapshot_name,
                O_WRONLY | O_CREAT | O_EXCL |
                O_CLOEXEC | O_NOFOLLOW, 0600);
        if (snapshot < 0)
            error = errno_or_io();
    }
    if (error == 0)
        error = copy_file_contents(
                source, snapshot, opened.st_size, progress);
    if (error == 0 && fsync(snapshot) < 0)
        error = errno_or_io();
    struct stat final_opened;
    struct stat final_named;
    if (error == 0 && fstat(source, &final_opened) < 0)
        error = errno_or_io();
    if (error == 0 && fstatat(shared, archive_name, &final_named,
            AT_SYMLINK_NOFOLLOW) < 0)
        error = errno_or_io();
    if (error == 0 && (
            !S_ISREG(final_opened.st_mode) ||
            !S_ISREG(final_named.st_mode) ||
            final_opened.st_dev != opened.st_dev ||
            final_opened.st_ino != opened.st_ino ||
            final_opened.st_size != opened.st_size ||
            final_opened.st_mtimespec.tv_sec !=
                    opened.st_mtimespec.tv_sec ||
            final_opened.st_mtimespec.tv_nsec !=
                    opened.st_mtimespec.tv_nsec ||
            final_opened.st_ctimespec.tv_sec !=
                    opened.st_ctimespec.tv_sec ||
            final_opened.st_ctimespec.tv_nsec !=
                    opened.st_ctimespec.tv_nsec ||
            final_named.st_dev != final_opened.st_dev ||
            final_named.st_ino != final_opened.st_ino ||
            final_named.st_size != final_opened.st_size ||
            final_named.st_mtimespec.tv_sec !=
                    final_opened.st_mtimespec.tv_sec ||
            final_named.st_mtimespec.tv_nsec !=
                    final_opened.st_mtimespec.tv_nsec ||
            final_named.st_ctimespec.tv_sec !=
                    final_opened.st_ctimespec.tv_sec ||
            final_named.st_ctimespec.tv_nsec !=
                    final_opened.st_ctimespec.tv_nsec))
        error = EAGAIN;
    if (snapshot >= 0 && close(snapshot) < 0 && error == 0)
        error = errno_or_io();
    if (close(source) < 0 && error == 0)
        error = errno_or_io();
    if (error != 0)
        (void) unlinkat(parent, snapshot_name, 0);
    return error;
}

struct scaled_progress {
    struct progress outer;
    double start;
    double span;
};

static void report_scaled_progress(
        void *cookie,
        double fraction,
        const char *message,
        bool *cancel_out) {
    struct scaled_progress *scaled = cookie;
    if (scaled->outer.callback == NULL)
        return;
    double clamped = fraction < 0 ? 0 :
            (fraction > 1 ? 1 : fraction);
    scaled->outer.callback(
            scaled->outer.cookie,
            scaled->start + clamped * scaled->span,
            message,
            cancel_out);
}

int ish_apple_watch_root_archive_import(
        const char *seed_root,
        const char *persistent_parent,
        const char *shared_directory,
        const char *archive_name,
        char root_name[ISH_APPLE_ROOT_NAME_CAPACITY],
        struct progress progress,
        struct ish_apple_watch_root_archive_error *error_out) {
    clear_error(error_out);
    if (seed_root == NULL || seed_root[0] == '\0' ||
            persistent_parent == NULL || persistent_parent[0] == '\0' ||
            shared_directory == NULL || shared_directory[0] == '\0' ||
            !ish_apple_watch_root_archive_is_supported_name(archive_name) ||
            root_name == NULL)
        return EINVAL;
    root_name[0] = '\0';

    int parent = open_directory(persistent_parent);
    if (parent < 0)
        return -parent;
    int shared = open_directory(shared_directory);
    int error = shared < 0 ? -shared : 0;

    char token[33];
    char snapshot_name[NAME_MAX + 1] = {0};
    char imported_name[NAME_MAX + 1] = {0};
    char snapshot_path[PATH_MAX];
    char imported_path[PATH_MAX];
    generate_token(token);
    if (error == 0)
        error = format_private_name(
                snapshot_name, "archive", token);
    if (error == 0)
        error = format_private_name(
                imported_name, "fakefs", token);
    if (error == 0)
        error = format_path(
                snapshot_path, persistent_parent, snapshot_name);
    if (error == 0)
        error = format_path(
                imported_path, persistent_parent, imported_name);
    if (error == 0)
        error = snapshot_shared_archive(
                shared, parent, archive_name, snapshot_name, progress);

    struct fakefsify_error fakefs_error = {0};
    struct scaled_progress scaled = {
        .outer = progress,
        .start = 0.10,
        .span = 0.85,
    };
    struct progress fakefs_progress = {
        .cookie = &scaled,
        .callback = report_scaled_progress,
    };
    if (error == 0 && !fakefs_import(
            snapshot_path, imported_path,
            &fakefs_error, fakefs_progress)) {
        bool cancelled = fakefs_error.type == ERR_CANCELLED;
        record_fakefs_error(error_out, &fakefs_error);
        error = cancelled ? ECANCELED : EINVAL;
    }
    if (error == 0)
        error = report_progress(
                progress, 0.96, "正在发布新文件系统");
    struct scaled_progress publish_scaled = {
        .outer = progress,
        .start = 0.96,
        .span = 0.04,
    };
    struct progress publish_progress = {
        .cookie = &publish_scaled,
        .callback = report_scaled_progress,
    };
    if (error == 0)
        error = ish_apple_root_catalog_import_fakefs(
                seed_root, persistent_parent,
                imported_path, root_name, publish_progress);
    if (error == 0)
        (void) report_progress(progress, 1, "恢复完成");

    if (imported_name[0] != '\0')
        (void) ish_apple_rootfs_remove_entry_at(
                parent, imported_name);
    if (snapshot_name[0] != '\0')
        (void) unlinkat(parent, snapshot_name, 0);
    (void) ish_apple_rootfs_sync_directory(parent);
    if (shared >= 0)
        close(shared);
    close(parent);
    return error;
}

static int verify_export_partial(
        int parent,
        const char *partial_name,
        struct stat *metadata_out) {
    int file = openat(parent, partial_name,
            O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (file < 0)
        return errno_or_io();
    struct stat metadata;
    int error = fstat(file, &metadata) < 0 ? errno_or_io() : 0;
    if (error == 0 && (!S_ISREG(metadata.st_mode) ||
            metadata.st_uid != geteuid() || metadata.st_nlink != 1))
        error = EINVAL;
    if (error == 0 && fsync(file) < 0)
        error = errno_or_io();
    if (close(file) < 0 && error == 0)
        error = errno_or_io();
    if (error == 0)
        *metadata_out = metadata;
    return error;
}

int ish_apple_watch_root_archive_export(
        const char *persistent_parent,
        const char *root_name,
        const char *active_name,
        const char *shared_directory,
        const char *output_name,
        struct progress progress,
        struct ish_apple_watch_root_archive_error *error_out) {
    clear_error(error_out);
    if (persistent_parent == NULL || persistent_parent[0] == '\0' ||
            !ish_apple_root_catalog_is_managed_name(root_name) ||
            shared_directory == NULL || shared_directory[0] == '\0' ||
            !safe_leaf_name(output_name) ||
            !has_suffix(output_name, ".tar.gz"))
        return EINVAL;
    if (active_name != NULL && active_name[0] != '\0' &&
            strcmp(root_name, active_name) == 0)
        return EBUSY;

    int source_lock = -1;
    int error = ish_apple_rootfs_lock_managed_root(
            persistent_parent, root_name, true, true, &source_lock);
    if (error != 0)
        return error;
    int parent = open_directory(persistent_parent);
    if (parent < 0)
        error = -parent;
    int shared = error == 0 ? open_directory(shared_directory) : -1;
    if (error == 0 && shared < 0)
        error = -shared;

    char token[33];
    char partial_name[NAME_MAX + 1] = {0};
    char source_path[PATH_MAX];
    char partial_path[PATH_MAX];
    generate_token(token);
    if (error == 0)
        error = format_private_name(partial_name, "export", token);
    if (error == 0)
        error = format_path(source_path, persistent_parent, root_name);
    if (error == 0)
        error = format_path(
                partial_path, persistent_parent, partial_name);

    struct fakefsify_error fakefs_error = {0};
    struct scaled_progress scaled = {
        .outer = progress,
        .start = 0,
        .span = 0.95,
    };
    struct progress fakefs_progress = {
        .cookie = &scaled,
        .callback = report_scaled_progress,
    };
    if (error == 0 && !fakefs_export(
            source_path, partial_path,
            &fakefs_error, fakefs_progress)) {
        bool cancelled = fakefs_error.type == ERR_CANCELLED;
        record_fakefs_error(error_out, &fakefs_error);
        error = cancelled ? ECANCELED : EINVAL;
    }
    struct stat published = {0};
    if (error == 0)
        error = verify_export_partial(
                parent, partial_name, &published);
    if (error == 0)
        error = report_progress(
                progress, 0.97, "正在发布共享归档");
    bool renamed = false;
    if (error == 0) {
        if (renameatx_np(parent, partial_name,
                shared, output_name, RENAME_EXCL) < 0) {
            error = errno_or_io();
        } else {
            renamed = true;
        }
    }
    if (error == 0)
        error = ish_apple_rootfs_sync_directory(shared);
    if (error == 0)
        error = ish_apple_rootfs_sync_directory(parent);
    bool completed = error == 0;
    if (completed)
        (void) report_progress(progress, 1, "导出完成");
    if (error != 0 && renamed) {
        int rollback_error = 0;
        struct stat named;
        if (fstatat(shared, output_name, &named,
                AT_SYMLINK_NOFOLLOW) < 0)
            rollback_error = errno_or_io();
        else if (named.st_dev != published.st_dev ||
                named.st_ino != published.st_ino)
            rollback_error = ESTALE;
        else if (renameatx_np(shared, output_name,
                parent, partial_name, RENAME_EXCL) < 0)
            rollback_error = errno_or_io();
        else
            renamed = false;
        if (rollback_error == 0)
            rollback_error =
                    ish_apple_rootfs_sync_directory(shared);
        if (rollback_error == 0)
            rollback_error =
                    ish_apple_rootfs_sync_directory(parent);
        if (rollback_error != 0)
            error = rollback_error;
    }

    if (partial_name[0] != '\0' && parent >= 0 && !renamed) {
        if (unlinkat(parent, partial_name, 0) == 0) {
            int cleanup_error =
                    ish_apple_rootfs_sync_directory(parent);
            if (error == 0)
                error = cleanup_error;
        } else if (errno != ENOENT && error == 0) {
            error = errno_or_io();
        }
    }
    if (shared >= 0)
        close(shared);
    if (parent >= 0)
        close(parent);
    int unlock_error =
            ish_apple_rootfs_unlock_managed_root(source_lock);
    return completed ? 0 :
            (error == 0 ? unlock_error : error);
}
