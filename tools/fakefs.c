#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#if __APPLE__
#include <sys/mount.h>
#endif
#include <archive.h>
#include <archive_entry.h>

#define ISH_INTERNAL
#include "fs/fake-db.h"
#include "fs/sqlutil.h"
#include "tools/fakefs.h"
#include "util/fchdir.h"
#include "misc.h"

#ifndef MAX_PATH
#define MAX_PATH 4096
#endif

#ifdef ISH_FAKEFS_TESTING
unsigned ish_fakefs_test_single_link_entries;
unsigned ish_fakefs_test_multi_link_entries;
#endif

// I have a weird way of error handling
#define FILL_ERR(_type, _code, _message) do { \
    const char *error_message = (_message); \
    err_out->line = __LINE__; \
    err_out->type = _type; \
    err_out->code = _code; \
    err_out->message = strdup( \
            error_message != NULL ? error_message : ""); \
    goto cleanup; \
} while (0)
#define ARCHIVE_ERR(archive) FILL_ERR(ERR_ARCHIVE, archive_errno(archive), archive_error_string(archive))
#define POSIX_ERR() FILL_ERR(ERR_POSIX, errno, strerror(errno))
#undef HANDLE_ERR // for sqlite
#define HANDLE_ERR(db) FILL_ERR(ERR_SQLITE, sqlite3_extended_errcode(db), sqlite3_errmsg(db))
#define CANCEL() FILL_ERR(ERR_CANCELLED, 0, "");

static bool progress_update(struct progress *p, double progress, const char *message) {
    bool cancelled = false;
    if (p && p->callback)
        p->callback(p->cookie, progress, message, &cancelled);
    return !cancelled;
}

static double import_progress(
        struct archive *archive, off_t archive_bytes) {
    if (archive_bytes == 0)
        return 0;
    double fraction =
            (double) archive_filter_bytes(archive, -1) / archive_bytes;
    return fraction < 0 ? 0 : (fraction > 1 ? 1 : fraction);
}

static bool preserve_sparse_hole(
        int descriptor, off_t offset, off_t length) {
    if (length == 0)
        return true;
#if __APPLE__
    struct statfs filesystem;
    if (fstatfs(descriptor, &filesystem) < 0)
        return false;
    off_t block_size = (off_t) filesystem.f_bsize;
    if (block_size <= 0) {
        errno = EIO;
        return false;
    }
    off_t prefix = offset % block_size;
    if (prefix != 0) {
        prefix = block_size - prefix;
        if (prefix >= length)
            return true;
        offset += prefix;
        length -= prefix;
    }
    length -= length % block_size;
    if (length == 0)
        return true;
    fpunchhole_t hole = {
        .fp_offset = offset,
        .fp_length = length,
    };
    int status;
    do {
        status = fcntl(descriptor, F_PUNCHHOLE, &hole);
    } while (status < 0 && errno == EINTR);
    return status == 0;
#else
    (void) descriptor;
    (void) offset;
    return true;
#endif
}

// This isn't linked with ish which is why there's so much copy/pasted code

// I hate this code
static bool path_normalize(
        const char *path, char *out, size_t capacity) {
#define ends_path(c) (c == '\0' || c == '/')
    if (capacity == 0)
        return false;
    char *end = out + capacity - 1;
    // normalized format:
    // ( '/' path-component ) *
    while (path[0] != '\0') {
        while (path[0] == '/')
            path++;
        if (path[0] == '\0')
            break; // if the path ends with a slash
        // path points to the start of a path component
        if (path[0] == '.' && path[1] == '.' && ends_path(path[2]))
            return false; // no dotdot allowed!
        if (path[0] == '.' && ends_path(path[1])) {
            path++;
        } else {
            if (out == end)
                return false;
            *out++ = '/';
            while (path[0] != '/' && path[0] != '\0') {
                if (out == end)
                    return false;
                *out++ = *path++;
            }
        }
    }
    *out = '\0';
    return true;
}

static const char *schema = Q(
    create table meta (id integer unique default 0, db_inode integer);
    insert into meta (db_inode) values (0);
    create table stats (inode integer primary key, stat blob);
    create table paths (path blob primary key, inode integer references stats(inode));
    create index inode_to_path on paths (inode, path);
    // no index is needed on stats, because the rows are ordered by the primary key
    pragma user_version=3;
);

struct imported_directory_time {
    char *path;
    struct timespec times[2];
    unsigned depth;
};

static int compare_imported_directory_depth(
        const void *left_pointer,
        const void *right_pointer) {
    const struct imported_directory_time *left = left_pointer;
    const struct imported_directory_time *right = right_pointer;
    return left->depth > right->depth ? -1 :
            (left->depth < right->depth ? 1 : 0);
}

static void free_imported_directory_times(
        struct imported_directory_time *directories,
        size_t count) {
    for (size_t index = 0; index < count; index++)
        free(directories[index].path);
    free(directories);
}

bool fakefs_import(const char *archive_path, const char *fs,
        struct fakefsify_error *err_out, struct progress p) {
    bool success = false;
    bool transaction_started = false;
    int err = 0;
    int root_fd = -1;
    int entry_fd = -1;
    sqlite3 *db = NULL;
    sqlite3_stmt *insert_stat = NULL;
    sqlite3_stmt *insert_path = NULL;
    sqlite3_stmt *insert_hardlink = NULL;
    sqlite3_stmt *select_path = NULL;
    sqlite3_stmt *insert_implicit = NULL;
    sqlite3_stmt *select_implicit = NULL;
    sqlite3_stmt *delete_implicit = NULL;
    sqlite3_stmt *update_implicit_stat = NULL;
    struct archive *archive = NULL;
    char *entry_path_copy = NULL;
    struct imported_directory_time *directory_times = NULL;
    size_t directory_time_count = 0;
    size_t directory_time_capacity = 0;

    err = mkdir(fs, 0777);
    if (err < 0)
        POSIX_ERR();

    char path_tmp[PATH_MAX];
    snprintf(path_tmp, sizeof(path_tmp), "%s/data", fs);
    err = mkdir(path_tmp, 0777);
    if (err < 0)
        POSIX_ERR();
    root_fd = open(path_tmp, O_RDONLY | O_CLOEXEC);
    if (root_fd < 0)
        POSIX_ERR();

    snprintf(path_tmp, sizeof(path_tmp), "%s/meta.db", fs);
    err = sqlite3_open_v2(path_tmp, &db,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
    CHECK_ERR();
    EXEC("pragma journal_mode=wal")
    EXEC("begin");
    transaction_started = true;
    EXEC(schema);
    EXEC("create temp table implicit_paths "
            "(path blob primary key)");

    archive = archive_read_new();
    if (archive == NULL)
        FILL_ERR(ERR_ARCHIVE, ENOMEM, "cannot allocate archive reader");
    archive_read_support_filter_gzip(archive);
    archive_read_support_format_tar(archive);
    if (archive_read_open_filename(
            archive, archive_path, 65536) != ARCHIVE_OK)
        ARCHIVE_ERR(archive);

    struct stat real_stat;
    if (stat(archive_path, &real_stat) < 0)
        POSIX_ERR();
    off_t archive_bytes = real_stat.st_size;

    insert_stat = PREPARE("insert into stats (stat) values (?)");
    insert_path = PREPARE(
            "insert into paths values (?, ?)");
    insert_hardlink = PREPARE(
            "insert into paths values (?, "
            "(select inode from paths where path = ? limit 1))");
    select_path = PREPARE(
            "select 1 from paths where path = ? limit 1");
    insert_implicit = PREPARE(
            "insert into implicit_paths values (?)");
    select_implicit = PREPARE(
            "select 1 from implicit_paths where path = ? limit 1");
    delete_implicit = PREPARE(
            "delete from implicit_paths where path = ?");
    update_implicit_stat = PREPARE(
            "update stats set stat = ? where inode = "
            "(select inode from paths where path = ?)");

    bool archive_has_root = false;
    struct archive_entry *entry;
    while ((err = archive_read_next_header(
            archive, &entry)) == ARCHIVE_OK) {
        char entry_path[MAX_PATH];
        const char *archive_pathname = archive_entry_pathname(entry);
        if (archive_pathname == NULL)
            FILL_ERR(ERR_ARCHIVE, EINVAL,
                    "archive entry has no path");
        if (!path_normalize(
                archive_pathname, entry_path, sizeof(entry_path)))
            FILL_ERR(ERR_ARCHIVE, EINVAL,
                    "archive entry path is unsafe or too long");
        if (!progress_update(
                &p, import_progress(archive, archive_bytes), entry_path))
            CANCEL();
        sqlite3_bind_blob64(select_path, 1,
                entry_path, strlen(entry_path), SQLITE_TRANSIENT);
        bool duplicate_path = STEP(select_path);
        RESET(select_path);
        bool replaces_implicit_directory = false;
        if (duplicate_path) {
            sqlite3_bind_blob64(select_implicit, 1,
                    entry_path, strlen(entry_path), SQLITE_TRANSIENT);
            replaces_implicit_directory = STEP(select_implicit);
            RESET(select_implicit);
        }
        if (duplicate_path &&
                (!replaces_implicit_directory ||
                 archive_entry_filetype(entry) != AE_IFDIR))
            FILL_ERR(ERR_ARCHIVE, EEXIST,
                    "archive contains a duplicate normalized path");
        if (entry_path[0] == '\0')
            archive_has_root = true;

        entry_path_copy = strdup(entry_path);
        if (entry_path_copy == NULL)
            FILL_ERR(ERR_POSIX, ENOMEM, strerror(ENOMEM));
        char *slash = entry_path_copy;
        while ((slash = strchr(
                *slash ? slash + 1 : slash, '/')) != NULL) {
            *slash = '\0';
            sqlite3_bind_blob64(select_path, 1,
                    entry_path_copy, strlen(entry_path_copy),
                    SQLITE_TRANSIENT);
            bool parent_in_database = STEP(select_path);
            RESET(select_path);
            if (parent_in_database) {
                struct stat parent_metadata;
                if (fstatat(root_fd, fix_path(entry_path_copy),
                        &parent_metadata,
                        AT_SYMLINK_NOFOLLOW) < 0)
                    POSIX_ERR();
                if (!S_ISDIR(parent_metadata.st_mode))
                    FILL_ERR(ERR_ARCHIVE, EINVAL,
                            "archive parent path is not a directory");
            } else {
                if (mkdirat(root_fd,
                        fix_path(entry_path_copy), 0777) < 0)
                    POSIX_ERR();
                struct ish_stat parent_stat = {
                    .mode = S_IFDIR | 0755,
                };
                sqlite3_bind_blob64(insert_stat, 1,
                        &parent_stat, sizeof(parent_stat),
                        SQLITE_TRANSIENT);
                STEP_RESET(insert_stat);
                sqlite3_int64 parent_inode =
                        sqlite3_last_insert_rowid(db);
                sqlite3_bind_blob64(insert_path, 1,
                        entry_path_copy, strlen(entry_path_copy),
                        SQLITE_TRANSIENT);
                sqlite3_bind_int64(
                        insert_path, 2, parent_inode);
                STEP_RESET(insert_path);
                sqlite3_bind_blob64(insert_implicit, 1,
                        entry_path_copy, strlen(entry_path_copy),
                        SQLITE_TRANSIENT);
                STEP_RESET(insert_implicit);
            }
            *slash = '/';
        }
        free(entry_path_copy);
        entry_path_copy = NULL;

        const char *hardlink = archive_entry_hardlink(entry);
        if (hardlink != NULL) {
            char hardlink_path[MAX_PATH];
            if (!path_normalize(
                    hardlink, hardlink_path, sizeof(hardlink_path)))
                FILL_ERR(ERR_ARCHIVE, EINVAL,
                        "archive hardlink path is unsafe or too long");
            sqlite3_bind_blob64(select_path, 1,
                    hardlink_path, strlen(hardlink_path),
                    SQLITE_TRANSIENT);
            bool hardlink_target_exists = STEP(select_path);
            RESET(select_path);
            if (!hardlink_target_exists)
                FILL_ERR(ERR_ARCHIVE, EINVAL,
                        "archive hardlink target does not exist");
            if (linkat(root_fd, fix_path(hardlink_path),
                    root_fd, fix_path(entry_path), 0) < 0)
                POSIX_ERR();
            sqlite3_bind_blob64(insert_hardlink, 1,
                    entry_path, strlen(entry_path), SQLITE_TRANSIENT);
            sqlite3_bind_blob64(insert_hardlink, 2,
                    hardlink_path, strlen(hardlink_path), SQLITE_TRANSIENT);
            STEP_RESET(insert_hardlink);
            continue;
        }

        switch (archive_entry_filetype(entry)) {
            case AE_IFREG:
            case AE_IFLNK:
            case AE_IFBLK:
            case AE_IFCHR:
            case AE_IFSOCK:
                entry_fd = openat(root_fd, fix_path(entry_path),
                        O_WRONLY | O_CREAT | O_EXCL |
                        O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0666);
                if (entry_fd < 0)
                    POSIX_ERR();
                struct stat regular_metadata;
                if (fstat(entry_fd, &regular_metadata) < 0)
                    POSIX_ERR();
                if (!S_ISREG(regular_metadata.st_mode))
                    FILL_ERR(ERR_ARCHIVE, EINVAL,
                            "archive leaf is not a regular placeholder");
                break;
            case AE_IFDIR:
                if (replaces_implicit_directory ||
                        entry_path[0] == '\0') {
                    struct stat directory_metadata;
                    if (fstatat(root_fd, fix_path(entry_path),
                            &directory_metadata,
                            AT_SYMLINK_NOFOLLOW) < 0)
                        POSIX_ERR();
                    if (!S_ISDIR(directory_metadata.st_mode))
                        FILL_ERR(ERR_ARCHIVE, EINVAL,
                                "archive leaf is not a directory");
                } else if (mkdirat(
                        root_fd, fix_path(entry_path), 0777) < 0)
                    POSIX_ERR();
                break;
            case AE_IFIFO: {
                lock_fchdir(root_fd);
                err = mkfifo(fix_path(entry_path), 0666);
                int saved_errno = errno;
                unlock_fchdir();
                errno = saved_errno;
                if (err < 0)
                    POSIX_ERR();
                break;
            }
            default:
                FILL_ERR(ERR_ARCHIVE, EINVAL,
                        "unsupported archive entry type");
                break;
        }

        switch (archive_entry_filetype(entry)) {
            case AE_IFREG: {
                if (!archive_entry_size_is_set(entry))
                    FILL_ERR(ERR_ARCHIVE, EINVAL,
                            "regular entry has no size");
                la_int64_t declared_size =
                        archive_entry_size(entry);
                if (declared_size < 0)
                    FILL_ERR(ERR_ARCHIVE, EINVAL,
                            "regular entry size is negative");
                off_t logical_size = (off_t) declared_size;
                if (logical_size < 0 ||
                        (la_int64_t) logical_size != declared_size)
                    FILL_ERR(ERR_ARCHIVE, EOVERFLOW,
                            "regular entry size is not representable");
                off_t output_offset = 0;

                for (;;) {
                    const void *block = NULL;
                    size_t block_size = 0;
                    la_int64_t raw_offset = -1;
                    int data_status = archive_read_data_block(
                            archive, &block, &block_size, &raw_offset);
                    if (data_status == ARCHIVE_EOF)
                        break;
                    if (data_status != ARCHIVE_OK)
                        ARCHIVE_ERR(archive);
                    if (block == NULL || block_size == 0 ||
                            raw_offset < 0)
                        FILL_ERR(ERR_ARCHIVE, EINVAL,
                                "archive returned an invalid data block");
                    if (raw_offset > declared_size ||
                            (uint64_t) block_size >
                            (uint64_t) (declared_size - raw_offset))
                        FILL_ERR(ERR_ARCHIVE, EINVAL,
                                "archive data block exceeds entry size");

                    off_t block_offset = (off_t) raw_offset;
                    if (block_offset < output_offset)
                        FILL_ERR(ERR_ARCHIVE, EINVAL,
                                "archive data blocks overlap or "
                                "are out of order");
                    off_t hole_offset = output_offset;
                    off_t hole_length =
                            block_offset - output_offset;
                    if (block_offset != output_offset) {
                        off_t positioned;
                        do {
                            positioned = lseek(
                                    entry_fd, block_offset, SEEK_SET);
                        } while (positioned < 0 && errno == EINTR);
                        if (positioned < 0)
                            POSIX_ERR();
                        if (positioned != block_offset) {
                            errno = EIO;
                            POSIX_ERR();
                        }
                        output_offset = block_offset;
                    }

                    const unsigned char *cursor = block;
                    size_t remaining = block_size;
                    while (remaining != 0) {
                        size_t chunk = remaining > 64 * 1024 ?
                                64 * 1024 : remaining;
                        size_t done = 0;
                        while (done < chunk) {
                            ssize_t written = write(
                                    entry_fd, cursor + done,
                                    chunk - done);
                            if (written < 0) {
                                if (errno == EINTR)
                                    continue;
                                POSIX_ERR();
                            }
                            if (written == 0) {
                                errno = EIO;
                                POSIX_ERR();
                            }
                            done += (size_t) written;
                            output_offset += (off_t) written;
                        }
                        cursor += chunk;
                        remaining -= chunk;
                        if (!progress_update(
                                &p,
                                import_progress(archive, archive_bytes),
                                entry_path))
                            CANCEL();
                    }
                    if (!preserve_sparse_hole(
                            entry_fd, hole_offset, hole_length))
                        POSIX_ERR();
                }

                off_t tail_hole_offset = output_offset;
                do {
                    err = ftruncate(entry_fd, logical_size);
                } while (err < 0 && errno == EINTR);
                if (err < 0)
                    POSIX_ERR();
                if (!preserve_sparse_hole(
                        entry_fd, tail_hole_offset,
                        logical_size - tail_hole_offset))
                    POSIX_ERR();
                break;
            }
            case AE_IFLNK: {
                const char *target = archive_entry_symlink(entry);
                if (target == NULL)
                    FILL_ERR(ERR_ARCHIVE, EINVAL,
                            "symlink entry has no target");
                size_t length = strlen(target);
                if (length == 0 || length >= MAX_PATH)
                    FILL_ERR(ERR_ARCHIVE, ENAMETOOLONG,
                            "symlink target length is invalid");
                size_t offset = 0;
                while (offset < length) {
                    ssize_t written = write(
                            entry_fd, target + offset, length - offset);
                    if (written < 0) {
                        if (errno == EINTR)
                            continue;
                        POSIX_ERR();
                    }
                    if (written == 0) {
                        errno = EIO;
                        POSIX_ERR();
                    }
                    offset += (size_t) written;
                }
                break;
            }
            case AE_IFDIR:
            case AE_IFBLK:
            case AE_IFCHR:
            case AE_IFSOCK:
            case AE_IFIFO:
                break;
            default:
                FILL_ERR(ERR_ARCHIVE, EINVAL,
                        "unsupported archive entry type");
        }
        if (entry_fd >= 0) {
            if (close(entry_fd) < 0) {
                entry_fd = -1;
                POSIX_ERR();
            }
            entry_fd = -1;
        }

        struct timespec times[2] = {
            {
                .tv_sec = archive_entry_atime(entry),
                .tv_nsec = archive_entry_atime_nsec(entry),
            },
            {
                .tv_sec = archive_entry_mtime(entry),
                .tv_nsec = archive_entry_mtime_nsec(entry),
            },
        };
        if (!archive_entry_atime_is_set(entry))
            times[0].tv_nsec = UTIME_OMIT;
        if (!archive_entry_mtime_is_set(entry))
            times[1].tv_nsec = UTIME_OMIT;
        if (archive_entry_filetype(entry) == AE_IFDIR) {
            if (directory_time_count == directory_time_capacity) {
                size_t next_capacity =
                        directory_time_capacity == 0 ?
                        16 : directory_time_capacity * 2;
                if (next_capacity < directory_time_capacity ||
                        next_capacity > SIZE_MAX /
                        sizeof(*directory_times))
                    FILL_ERR(ERR_POSIX, ENOMEM, strerror(ENOMEM));
                void *grown = realloc(
                        directory_times,
                        next_capacity * sizeof(*directory_times));
                if (grown == NULL)
                    FILL_ERR(ERR_POSIX, ENOMEM, strerror(ENOMEM));
                directory_times = grown;
                directory_time_capacity = next_capacity;
            }
            char *saved_path = strdup(entry_path);
            if (saved_path == NULL)
                FILL_ERR(ERR_POSIX, ENOMEM, strerror(ENOMEM));
            unsigned depth = 0;
            for (const char *cursor = entry_path;
                    *cursor != '\0'; cursor++) {
                if (*cursor == '/')
                    depth++;
            }
            directory_times[directory_time_count] =
                    (struct imported_directory_time) {
                        .path = saved_path,
                        .times = {times[0], times[1]},
                        .depth = depth,
                    };
            directory_time_count++;
        } else if (utimensat(
                root_fd, fix_path(entry_path), times, 0) < 0) {
            POSIX_ERR();
        }

        struct ish_stat stat = {
            .mode = (uint32_t) archive_entry_mode(entry),
            .uid = (uint32_t) archive_entry_uid(entry),
            .gid = (uint32_t) archive_entry_gid(entry),
            .rdev = (uint32_t) archive_entry_rdev(entry),
        };
        if (replaces_implicit_directory) {
            sqlite3_bind_blob64(update_implicit_stat, 1,
                    &stat, sizeof(stat), SQLITE_TRANSIENT);
            sqlite3_bind_blob64(update_implicit_stat, 2,
                    entry_path, strlen(entry_path), SQLITE_TRANSIENT);
            STEP_RESET(update_implicit_stat);
            sqlite3_bind_blob64(delete_implicit, 1,
                    entry_path, strlen(entry_path), SQLITE_TRANSIENT);
            STEP_RESET(delete_implicit);
        } else {
            sqlite3_bind_blob64(insert_stat, 1,
                    &stat, sizeof(stat), SQLITE_TRANSIENT);
            STEP_RESET(insert_stat);
            sqlite3_bind_blob64(insert_path, 1,
                    entry_path, strlen(entry_path), SQLITE_TRANSIENT);
            sqlite3_bind_int64(
                    insert_path, 2, sqlite3_last_insert_rowid(db));
            STEP_RESET(insert_path);
        }
    }
    if (err != ARCHIVE_EOF)
        ARCHIVE_ERR(archive);
    qsort(directory_times, directory_time_count,
            sizeof(*directory_times),
            compare_imported_directory_depth);
    for (size_t index = 0;
            index < directory_time_count; index++) {
        struct imported_directory_time *directory =
                &directory_times[index];
        if (utimensat(root_fd, fix_path(directory->path),
                directory->times, 0) < 0)
            POSIX_ERR();
    }

    if (!archive_has_root) {
        struct ish_stat stat = {.mode = S_IFDIR | 0755};
        sqlite3_bind_blob64(insert_stat, 1,
                &stat, sizeof(stat), SQLITE_TRANSIENT);
        STEP_RESET(insert_stat);
        sqlite3_bind_blob64(
                insert_path, 1, "", 0, SQLITE_TRANSIENT);
        sqlite3_bind_int64(
                insert_path, 2, sqlite3_last_insert_rowid(db));
        STEP_RESET(insert_path);
    }

    err = sqlite3_finalize(insert_stat);
    insert_stat = NULL;
    CHECK_ERR();
    err = sqlite3_finalize(insert_path);
    insert_path = NULL;
    CHECK_ERR();
    err = sqlite3_finalize(insert_hardlink);
    insert_hardlink = NULL;
    CHECK_ERR();
    err = sqlite3_finalize(select_path);
    select_path = NULL;
    CHECK_ERR();
    err = sqlite3_finalize(insert_implicit);
    insert_implicit = NULL;
    CHECK_ERR();
    err = sqlite3_finalize(select_implicit);
    select_implicit = NULL;
    CHECK_ERR();
    err = sqlite3_finalize(delete_implicit);
    delete_implicit = NULL;
    CHECK_ERR();
    err = sqlite3_finalize(update_implicit_stat);
    update_implicit_stat = NULL;
    CHECK_ERR();
    EXEC("commit");
    transaction_started = false;
    success = true;

cleanup:
    free(entry_path_copy);
    free_imported_directory_times(
            directory_times, directory_time_count);
    if (entry_fd >= 0)
        close(entry_fd);
    if (insert_stat != NULL)
        sqlite3_finalize(insert_stat);
    if (insert_path != NULL)
        sqlite3_finalize(insert_path);
    if (insert_hardlink != NULL)
        sqlite3_finalize(insert_hardlink);
    if (select_path != NULL)
        sqlite3_finalize(select_path);
    if (insert_implicit != NULL)
        sqlite3_finalize(insert_implicit);
    if (select_implicit != NULL)
        sqlite3_finalize(select_implicit);
    if (delete_implicit != NULL)
        sqlite3_finalize(delete_implicit);
    if (update_implicit_stat != NULL)
        sqlite3_finalize(update_implicit_stat);
    if (transaction_started && db != NULL)
        sqlite3_exec(db, "rollback", NULL, NULL, NULL);
    if (db != NULL)
        sqlite3_close(db);
    if (root_fd >= 0)
        close(root_fd);
    if (archive != NULL)
        archive_read_free(archive);
    return success;
}

bool fakefs_export(const char *fs, const char *archive_path,
        struct fakefsify_error *err_out, struct progress p) {
    bool success = false;
    bool transaction_started = false;
    int err = 0;
    int root_fd = -1;
    int entry_fd = -1;
    sqlite3 *db = NULL;
    sqlite3_stmt *count_stmt = NULL;
    sqlite3_stmt *query = NULL;
    struct archive *archive = NULL;
    struct archive_entry_linkresolver *linkresolver = NULL;
    struct archive_entry *entry = NULL;
    struct archive_entry *sparse = NULL;
    char *path = NULL;

#ifdef ISH_FAKEFS_TESTING
    ish_fakefs_test_single_link_entries = 0;
    ish_fakefs_test_multi_link_entries = 0;
#endif

    archive = archive_write_new();
    if (archive == NULL)
        FILL_ERR(ERR_ARCHIVE, ENOMEM, "cannot allocate archive writer");
    if (archive_write_add_filter_gzip(archive) != ARCHIVE_OK)
        ARCHIVE_ERR(archive);
    if (archive_write_set_format_pax(archive) != ARCHIVE_OK)
        ARCHIVE_ERR(archive);
    if (archive_write_open_filename(
            archive, archive_path) != ARCHIVE_OK)
        ARCHIVE_ERR(archive);

    char path_tmp[PATH_MAX];
    snprintf(path_tmp, sizeof(path_tmp), "%s/data", fs);
    root_fd = open(path_tmp, O_RDONLY | O_CLOEXEC);
    if (root_fd < 0)
        POSIX_ERR();

    snprintf(path_tmp, sizeof(path_tmp), "%s/meta.db", fs);
    err = sqlite3_open_v2(
            path_tmp, &db, SQLITE_OPEN_READONLY, NULL);
    CHECK_ERR();
    EXEC("begin");
    transaction_started = true;

    count_stmt = PREPARE("select count(*) from paths");
    STEP(count_stmt);
    int64_t paths_total = sqlite3_column_int64(count_stmt, 0);
    err = sqlite3_finalize(count_stmt);
    count_stmt = NULL;
    CHECK_ERR();
    int64_t paths_done = 0;

    linkresolver = archive_entry_linkresolver_new();
    if (linkresolver == NULL)
        FILL_ERR(ERR_ARCHIVE, ENOMEM,
                "cannot allocate archive link resolver");
    archive_entry_linkresolver_set_strategy(
            linkresolver, ARCHIVE_FORMAT_TAR_PAX_INTERCHANGE);

    query = PREPARE(
            "select current.path, current.inode, stats.stat, "
            "(select count(*) from paths as links "
            "where links.inode = current.inode) "
            "from paths as current left join stats using (inode)");
    while (STEP(query)) {
        entry = archive_entry_new();
        if (entry == NULL)
            FILL_ERR(ERR_ARCHIVE, ENOMEM,
                    "cannot allocate archive entry");

        const void *path_in_db = sqlite3_column_blob(query, 0);
        int path_bytes = sqlite3_column_bytes(query, 0);
        if (path_bytes < 0 || path_bytes > MAX_PATH - 2 ||
                (path_bytes > 0 && path_in_db == NULL))
            FILL_ERR(ERR_SQLITE, SQLITE_CORRUPT,
                    "invalid fakefs path");
        size_t path_len = (size_t) path_bytes;
        path = malloc(path_len + 2);
        if (path == NULL)
            FILL_ERR(ERR_POSIX, ENOMEM, strerror(ENOMEM));
        path[0] = '.';
        memcpy(path + 1, path_in_db, path_len);
        path[path_len + 1] = '\0';
        archive_entry_set_pathname(entry, path);

        double fraction = paths_total > 0 ?
                (double) paths_done / paths_total : 0;
        if (!progress_update(&p, fraction, path))
            CANCEL();

        if (sqlite3_column_type(query, 1) == SQLITE_NULL)
            FILL_ERR(ERR_SQLITE, SQLITE_CORRUPT,
                    "fakefs path has no inode");
        const void *stat_blob = sqlite3_column_blob(query, 2);
        int stat_size = sqlite3_column_bytes(query, 2);
        if (stat_blob == NULL ||
                stat_size != (int) sizeof(struct ish_stat))
            FILL_ERR(ERR_SQLITE, SQLITE_CORRUPT,
                    "invalid fakefs stat");
        struct ish_stat guest_stat =
                *(const struct ish_stat *) stat_blob;
        archive_entry_set_ino64(
                entry, sqlite3_column_int64(query, 1));
        archive_entry_set_dev(entry, 0);
        sqlite3_int64 link_count =
                sqlite3_column_int64(query, 3);
        if (link_count < 1 || link_count > UINT_MAX)
            FILL_ERR(ERR_SQLITE, SQLITE_CORRUPT,
                    "invalid fakefs link count");
        archive_entry_set_nlink(
                entry, (unsigned int) link_count);
#ifdef ISH_FAKEFS_TESTING
        if (link_count == 1)
            ish_fakefs_test_single_link_entries++;
        else
            ish_fakefs_test_multi_link_entries++;
#endif
        archive_entry_set_mode(entry, guest_stat.mode);
        archive_entry_set_uid(entry, guest_stat.uid);
        archive_entry_set_gid(entry, guest_stat.gid);
        archive_entry_set_rdev(entry, guest_stat.rdev);

        struct stat real_stat;
        if (fstatat(root_fd, path, &real_stat,
                AT_SYMLINK_NOFOLLOW) < 0)
            POSIX_ERR();
        mode_t guest_type = guest_stat.mode & S_IFMT;
        bool regular_placeholder =
                guest_type == S_IFREG ||
                guest_type == S_IFLNK ||
                guest_type == S_IFBLK ||
                guest_type == S_IFCHR ||
                guest_type == S_IFSOCK;
        bool host_type_matches =
                (regular_placeholder &&
                        S_ISREG(real_stat.st_mode)) ||
                (guest_type == S_IFDIR &&
                        S_ISDIR(real_stat.st_mode)) ||
                (guest_type == S_IFIFO &&
                        S_ISFIFO(real_stat.st_mode));
        if (!host_type_matches)
            FILL_ERR(ERR_ARCHIVE, EINVAL,
                    "fakefs guest and host types do not match");
        archive_entry_set_size(entry, real_stat.st_size);
#if __APPLE__
#define TIMESPEC(x) st_##x##timespec
#elif __linux__
#define TIMESPEC(x) st_##x##tim
#endif
        archive_entry_set_atime(entry, real_stat.st_atime,
                real_stat.TIMESPEC(a).tv_nsec);
        archive_entry_set_mtime(entry, real_stat.st_mtime,
                real_stat.TIMESPEC(m).tv_nsec);
        archive_entry_set_ctime(entry, real_stat.st_ctime,
                real_stat.TIMESPEC(c).tv_nsec);

        if (regular_placeholder) {
            entry_fd = openat(root_fd, path,
                    O_RDONLY | O_CLOEXEC |
                    O_NOFOLLOW | O_NONBLOCK);
            if (entry_fd < 0)
                POSIX_ERR();
            struct stat opened_metadata;
            if (fstat(entry_fd, &opened_metadata) < 0)
                POSIX_ERR();
            if (!S_ISREG(opened_metadata.st_mode) ||
                    opened_metadata.st_dev != real_stat.st_dev ||
                    opened_metadata.st_ino != real_stat.st_ino)
                FILL_ERR(ERR_ARCHIVE, EAGAIN,
                        "fakefs host placeholder changed");
        }
        if (S_ISLNK(guest_stat.mode)) {
            char target[MAX_PATH];
            ssize_t length;
            do {
                length = read(entry_fd, target, sizeof(target));
            } while (length < 0 && errno == EINTR);
            if (length < 0)
                POSIX_ERR();
            if (length == 0 || length >= MAX_PATH)
                FILL_ERR(
                        ERR_ARCHIVE,
                        length == 0 ? EINVAL : ENAMETOOLONG,
                        "symlink target length is invalid");
            target[length] = '\0';
            archive_entry_set_symlink(entry, target);
        }

        archive_entry_linkify(linkresolver, &entry, &sparse);
        bool write_contents = entry != NULL &&
                S_ISREG(guest_stat.mode) &&
                real_stat.st_size != 0 &&
                archive_entry_hardlink(entry) == NULL;
        if (entry != NULL &&
                archive_write_header(archive, entry) != ARCHIVE_OK)
            ARCHIVE_ERR(archive);
        if (sparse != NULL &&
                archive_write_header(archive, sparse) != ARCHIVE_OK)
            ARCHIVE_ERR(archive);

        if (write_contents) {
            char buffer[64 * 1024];
            ssize_t length;
            off_t entry_bytes_done = 0;
            while ((length = read(
                    entry_fd, buffer, sizeof(buffer))) > 0) {
                ssize_t written =
                        archive_write_data(archive, buffer, length);
                if (written < 0)
                    ARCHIVE_ERR(archive);
                if (written != length)
                    FILL_ERR(ERR_ARCHIVE, EIO,
                            "short archive write");
                entry_bytes_done += length;
                double entry_fraction = real_stat.st_size > 0 ?
                        (double) entry_bytes_done /
                        (double) real_stat.st_size : 1;
                if (entry_fraction > 1)
                    entry_fraction = 1;
                double byte_fraction = paths_total > 0 ?
                        ((double) paths_done + entry_fraction) /
                        (double) paths_total : 1;
                if (!progress_update(&p, byte_fraction, path))
                    CANCEL();
            }
            if (length < 0)
                POSIX_ERR();
        }

        if (entry_fd >= 0) {
            if (close(entry_fd) < 0) {
                entry_fd = -1;
                POSIX_ERR();
            }
            entry_fd = -1;
        }
        paths_done++;
        free(path);
        path = NULL;
        archive_entry_free(entry);
        entry = NULL;
        archive_entry_free(sparse);
        sparse = NULL;
    }

    err = sqlite3_finalize(query);
    query = NULL;
    CHECK_ERR();
    if (paths_done != paths_total)
        FILL_ERR(ERR_SQLITE, SQLITE_CORRUPT,
                "fakefs path count changed during export");
    if (archive_write_close(archive) != ARCHIVE_OK)
        ARCHIVE_ERR(archive);
    success = true;

cleanup:
    if (entry_fd >= 0)
        close(entry_fd);
    free(path);
    archive_entry_free(entry);
    archive_entry_free(sparse);
    if (query != NULL)
        sqlite3_finalize(query);
    if (count_stmt != NULL)
        sqlite3_finalize(count_stmt);
    if (linkresolver != NULL)
        archive_entry_linkresolver_free(linkresolver);
    if (transaction_started && db != NULL)
        sqlite3_exec(db, "rollback", NULL, NULL, NULL);
    if (db != NULL)
        sqlite3_close(db);
    if (root_fd >= 0)
        close(root_fd);
    if (archive != NULL)
        archive_write_free(archive);
    return success;
}
