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

// 负责 fakefs SQLite 身份校验、硬链接重建与复制后数据库整理。

int ish_apple_rootfs_validate_busybox_elf(int data_directory) {
    int busybox = -1;
    int error = ish_apple_rootfs_open_regular_relative(
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
            error = ish_apple_rootfs_errno_or_io();
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
        error = ish_apple_rootfs_errno_or_io();
    return error;
}

static int sqlite_scalar_int64(
        sqlite3 *database, const char *sql, sqlite3_int64 *value) {
    sqlite3_stmt *statement = NULL;
    int result = sqlite3_prepare_v2(database, sql, -1, &statement, NULL);
    if (result != SQLITE_OK)
        return ish_apple_rootfs_sqlite_error(database);
    int error = 0;
    result = sqlite3_step(statement);
    if (result != SQLITE_ROW) {
        error = ish_apple_rootfs_sqlite_error(database);
    } else {
        *value = sqlite3_column_int64(statement, 0);
        result = sqlite3_step(statement);
        if (result != SQLITE_DONE)
            error = result == SQLITE_ROW ? EINVAL : ish_apple_rootfs_sqlite_error(database);
    }
    if (sqlite3_finalize(statement) != SQLITE_OK && error == 0)
        error = ish_apple_rootfs_sqlite_error(database);
    return error;
}

static int sqlite_scalar_text(
        sqlite3 *database, const char *sql, const char *expected) {
    sqlite3_stmt *statement = NULL;
    int result = sqlite3_prepare_v2(database, sql, -1, &statement, NULL);
    if (result != SQLITE_OK)
        return ish_apple_rootfs_sqlite_error(database);
    int error = 0;
    result = sqlite3_step(statement);
    if (result != SQLITE_ROW) {
        error = ish_apple_rootfs_sqlite_error(database);
    } else {
        const unsigned char *value = sqlite3_column_text(statement, 0);
        if (value == NULL || strcmp((const char *) value, expected) != 0) {
            error = EINVAL;
        } else {
            result = sqlite3_step(statement);
            if (result != SQLITE_DONE)
                error = result == SQLITE_ROW ? EINVAL : ish_apple_rootfs_sqlite_error(database);
        }
    }
    if (sqlite3_finalize(statement) != SQLITE_OK && error == 0)
        error = ish_apple_rootfs_sqlite_error(database);
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
        return ish_apple_rootfs_sqlite_error(database);
    result = sqlite3_step(statement);
    if (result != SQLITE_ROW)
        return result == SQLITE_DONE ? EINVAL : ish_apple_rootfs_sqlite_error(database);
    *inode = sqlite3_column_int64(statement, 0);
    result = sqlite3_step(statement);
    if (result != SQLITE_DONE)
        return result == SQLITE_ROW ? EINVAL : ish_apple_rootfs_sqlite_error(database);
    return 0;
}

static int database_path_count(
        sqlite3 *database, sqlite3_stmt *statement,
        sqlite3_int64 inode, sqlite3_int64 *count) {
    sqlite3_reset(statement);
    sqlite3_clear_bindings(statement);
    int result = sqlite3_bind_int64(statement, 1, inode);
    if (result != SQLITE_OK)
        return ish_apple_rootfs_sqlite_error(database);
    result = sqlite3_step(statement);
    if (result != SQLITE_ROW)
        return ish_apple_rootfs_sqlite_error(database);
    *count = sqlite3_column_int64(statement, 0);
    result = sqlite3_step(statement);
    if (result != SQLITE_DONE)
        return result == SQLITE_ROW ? EINVAL : ish_apple_rootfs_sqlite_error(database);
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
        return ish_apple_rootfs_sqlite_error(database);
    result = sqlite3_prepare_v2(database,
            "select count(*) from paths where inode = ?", -1,
            &count_statement, NULL);
    int error = result == SQLITE_OK ? 0 : ish_apple_rootfs_sqlite_error(database);

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
        error = ish_apple_rootfs_sqlite_error(database);
    if (count_statement != NULL &&
            sqlite3_finalize(count_statement) != SQLITE_OK && error == 0)
        error = ish_apple_rootfs_sqlite_error(database);
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
            return ish_apple_rootfs_errno_or_io();
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
    int error = ish_apple_rootfs_open_regular_relative(
            directory, left_path, O_RDONLY, &left);
    if (error != 0)
        return error;
    error = ish_apple_rootfs_open_regular_relative(directory, right_path, O_RDONLY, &right);
    if (error != 0) {
        close(left);
        return error;
    }
    struct stat left_metadata;
    struct stat right_metadata;
    if (fstat(left, &left_metadata) < 0 || fstat(right, &right_metadata) < 0)
        error = ish_apple_rootfs_errno_or_io();
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
        error = ish_apple_rootfs_errno_or_io();
    if (close(right) < 0 && error == 0)
        error = ish_apple_rootfs_errno_or_io();
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
        int error = ish_apple_rootfs_open_relative_parent(
                data_directory, canonical, &canonical_parent);
        if (error != 0)
            return error;
        struct stat canonical_metadata;
        if (fstatat(canonical_parent.directory, canonical_parent.leaf,
                &canonical_metadata, AT_SYMLINK_NOFOLLOW) < 0)
            error = ish_apple_rootfs_errno_or_io();
        else if (!S_ISREG(canonical_metadata.st_mode))
            error = EINVAL;

        for (size_t member = group_start + 1;
                error == 0 && member < index; member++) {
            const char *member_path = manifest->entries[member].member;
            struct relative_parent member_parent = {.directory = -1};
            error = ish_apple_rootfs_open_relative_parent(
                    data_directory, member_path, &member_parent);
            struct stat member_metadata;
            if (error == 0 && fstatat(member_parent.directory,
                    member_parent.leaf, &member_metadata,
                    AT_SYMLINK_NOFOLLOW) < 0)
                error = ish_apple_rootfs_errno_or_io();
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
                error = ish_apple_rootfs_errno_or_io();
            if (error == 0 && !already_linked && linkat(
                    canonical_parent.directory, canonical_parent.leaf,
                    member_parent.directory, member_parent.leaf, 0) < 0)
                error = ish_apple_rootfs_errno_or_io();
            if (error == 0 && !already_linked)
                error = ish_apple_rootfs_sync_directory_internal(member_parent.directory);
            if (member_parent.directory >= 0)
                error = ish_apple_rootfs_close_relative_parent(&member_parent, error);
        }

        if (error == 0 && fstatat(canonical_parent.directory,
                canonical_parent.leaf, &canonical_metadata,
                AT_SYMLINK_NOFOLLOW) < 0)
            error = ish_apple_rootfs_errno_or_io();
        if (error == 0 && (uintmax_t) canonical_metadata.st_nlink !=
                (uintmax_t) group_size)
            error = EINVAL;
        for (size_t member = group_start;
                error == 0 && member < index; member++) {
            struct relative_parent member_parent = {.directory = -1};
            error = ish_apple_rootfs_open_relative_parent(data_directory,
                    manifest->entries[member].member, &member_parent);
            struct stat member_metadata;
            if (error == 0 && fstatat(member_parent.directory,
                    member_parent.leaf, &member_metadata,
                    AT_SYMLINK_NOFOLLOW) < 0)
                error = ish_apple_rootfs_errno_or_io();
            if (error == 0 && (!S_ISREG(member_metadata.st_mode) ||
                    member_metadata.st_dev != canonical_metadata.st_dev ||
                    member_metadata.st_ino != canonical_metadata.st_ino ||
                    member_metadata.st_nlink != canonical_metadata.st_nlink))
                error = EINVAL;
            if (member_parent.directory >= 0)
                error = ish_apple_rootfs_close_relative_parent(&member_parent, error);
        }
        if (canonical_parent.directory >= 0)
            error = ish_apple_rootfs_close_relative_parent(&canonical_parent, error);
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
        return ish_apple_rootfs_sqlite_error(database);
    sqlite3_stmt *statement = NULL;
    result = sqlite3_prepare_v2(database,
            "update meta set db_inode = ?", -1, &statement, NULL);
    int error = result == SQLITE_OK ? 0 : ish_apple_rootfs_sqlite_error(database);
    if (error == 0 && sqlite3_bind_int64(
            statement, 1, database_inode) != SQLITE_OK)
        error = ish_apple_rootfs_sqlite_error(database);
    if (error == 0 && sqlite3_step(statement) != SQLITE_DONE)
        error = ish_apple_rootfs_sqlite_error(database);
    if (error == 0 && sqlite3_changes(database) != 1)
        error = EINVAL;
    if (statement != NULL && sqlite3_finalize(statement) != SQLITE_OK &&
            error == 0)
        error = ish_apple_rootfs_sqlite_error(database);

    const char *transaction = error == 0 ? "commit" : "rollback";
    message = NULL;
    result = sqlite3_exec(database, transaction, NULL, NULL, &message);
    sqlite3_free(message);
    if (result != SQLITE_OK && error == 0)
        error = ish_apple_rootfs_sqlite_error(database);
    return error;
}

static int database_path_for_file(int database_file, char path[PATH_MAX]) {
    if (fcntl(database_file, F_GETPATH, path) < 0)
        return ish_apple_rootfs_errno_or_io();
    size_t length = strnlen(path, PATH_MAX);
    return length == 0 || length == PATH_MAX ? ENAMETOOLONG : 0;
}

static int verify_database_name_identity(
        int staging_directory, const char *database_path,
        const struct stat *expected) {
    struct stat directory_entry;
    if (fstatat(staging_directory, "meta.db", &directory_entry,
            AT_SYMLINK_NOFOLLOW) < 0)
        return ish_apple_rootfs_errno_or_io();
    struct stat path_entry;
    if (lstat(database_path, &path_entry) < 0)
        return ish_apple_rootfs_errno_or_io();
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
        error = result == SQLITE_OK ? EAGAIN : ish_apple_rootfs_sqlite_error(database);
    return error;
}

static int materialize_relative_directory(
        int data_directory, const char *path) {
    struct relative_parent parent = {.directory = -1};
    int error = ish_apple_rootfs_open_relative_parent(data_directory, path, &parent);
    int directory = -1;
    bool created = false;
    if (error == 0) {
        directory = openat(parent.directory, parent.leaf,
                O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (directory < 0 && errno == ENOENT) {
            if (mkdirat(parent.directory, parent.leaf, 0700) < 0) {
                error = ish_apple_rootfs_errno_or_io();
            } else {
                created = true;
                directory = openat(parent.directory, parent.leaf,
                        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
                if (directory < 0)
                    error = ish_apple_rootfs_errno_or_io();
            }
        } else if (directory < 0) {
            error = ish_apple_rootfs_errno_or_io();
        }
    }

    struct stat metadata;
    if (error == 0 && fstat(directory, &metadata) < 0)
        error = ish_apple_rootfs_errno_or_io();
    if (error == 0 && (!S_ISDIR(metadata.st_mode) ||
            metadata.st_uid != geteuid()))
        error = EINVAL;
    if (directory >= 0 && close(directory) < 0 && error == 0)
        error = ish_apple_rootfs_errno_or_io();
    if (created && error == 0)
        error = ish_apple_rootfs_sync_directory_internal(parent.directory);
    if (parent.directory >= 0)
        error = ish_apple_rootfs_close_relative_parent(&parent, error);
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
        return ish_apple_rootfs_sqlite_error(database);

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
        if (!ish_apple_rootfs_relative_path_is_valid(relative_path)) {
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
        error = ish_apple_rootfs_sqlite_error(database);
    if (error == 0 && !found_root)
        error = EINVAL;
    if (sqlite3_finalize(statement) != SQLITE_OK && error == 0)
        error = ish_apple_rootfs_sqlite_error(database);
    return error;
}

int ish_apple_rootfs_validate_and_update_database(
        int staging_directory, int data_directory,
        struct hardlink_manifest *hardlinks) {
    // SQLite 没有公开底层 fd；私有 staging 内用 held fd、名字身份与 HAS_MOVED 交叉复核。
    int database_file = openat(staging_directory, "meta.db",
            O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (database_file < 0)
        return ish_apple_rootfs_errno_or_io();

    struct stat metadata = {0};
    int error = 0;
    if (fstat(database_file, &metadata) < 0)
        error = ish_apple_rootfs_errno_or_io();
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
        error = database == NULL ? EINVAL : ish_apple_rootfs_sqlite_error(database);
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
            error = ish_apple_rootfs_errno_or_io();
    }
    if (error == 0 && fsync(database_file) < 0)
        error = ish_apple_rootfs_errno_or_io();
    if (close(database_file) < 0 && error == 0)
        error = ish_apple_rootfs_errno_or_io();
    return error;
}

static int unlink_optional_regular_at(int directory, const char *name) {
    struct stat metadata;
    if (fstatat(directory, name, &metadata,
            AT_SYMLINK_NOFOLLOW) < 0)
        return errno == ENOENT ? 0 : ish_apple_rootfs_errno_or_io();
    if (!S_ISREG(metadata.st_mode))
        return EINVAL;
    return unlinkat(directory, name, 0) < 0 ? ish_apple_rootfs_errno_or_io() : 0;
}

int ish_apple_rootfs_prepare_copied_database(int staging) {
    // SHM 只保存进程间协调状态；复制后必须由新数据库重新建立。
    int error = unlink_optional_regular_at(staging, "meta.db-shm");
    int database_file = -1;
    if (error == 0) {
        database_file = openat(staging, "meta.db",
                O_RDWR | O_CLOEXEC | O_NOFOLLOW);
        if (database_file < 0)
            error = ish_apple_rootfs_errno_or_io();
    }

    struct stat metadata = {0};
    if (error == 0 && fstat(database_file, &metadata) < 0)
        error = ish_apple_rootfs_errno_or_io();
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
        error = database == NULL ? EINVAL : ish_apple_rootfs_sqlite_error(database);
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
        error = ish_apple_rootfs_sqlite_error(database);
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
        error = ish_apple_rootfs_errno_or_io();
    if (database_file >= 0 && close(database_file) < 0 && error == 0)
        error = ish_apple_rootfs_errno_or_io();
    static const char *artifacts[] = {
        "meta.db-wal", "meta.db-shm", "meta.db-journal",
    };
    for (size_t i = 0; error == 0 &&
            i < sizeof(artifacts) / sizeof(artifacts[0]); i++)
        error = unlink_optional_regular_at(staging, artifacts[i]);
    if (error == 0)
        error = ish_apple_rootfs_sync_directory_internal(staging);
    return error;
}
