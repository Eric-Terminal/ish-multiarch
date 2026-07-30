#include <archive.h>
#include <archive_entry.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <sqlite3.h>

#include "tools/fakefs.h"

#define GUEST_PATH_LIMIT 4096
#define TIMED_DIRECTORY_MTIME 946684800
#define TIMED_FILE_MTIME 946684801
#define LARGE_REGULAR_SIZE (512 * 1024)
#define SPARSE_LOGICAL_SIZE (16 * 1024 * 1024)
#define SPARSE_FIRST_CHUNK_OFFSET (64 * 1024)
#define SPARSE_SECOND_CHUNK_OFFSET (8 * 1024 * 1024)
#define SPARSE_FIRST_OFFSET (SPARSE_FIRST_CHUNK_OFFSET + 3)
#define SPARSE_SECOND_OFFSET (SPARSE_SECOND_CHUNK_OFFSET + 7)
#define SPARSE_EXTENT_SIZE (4 * 1024 - 6)

extern unsigned ish_fakefs_test_single_link_entries;
extern unsigned ish_fakefs_test_multi_link_entries;

static void require(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "失败：%s（errno=%d）\n", message, errno);
        exit(1);
    }
}

static void format_child(
        char output[PATH_MAX], const char *parent, const char *name) {
    int length = snprintf(output, PATH_MAX, "%s/%s", parent, name);
    require(length > 0 && length < PATH_MAX, "拼接测试路径");
}

static void execute_database_sql(
        const char *database_path, const char *sql) {
    sqlite3 *database = NULL;
    require(sqlite3_open_v2(
                    database_path, &database,
                    SQLITE_OPEN_READWRITE, NULL) == SQLITE_OK,
            "打开损坏 DB 回归数据库");
    char *message = NULL;
    int status = sqlite3_exec(database, sql, NULL, NULL, &message);
    if (status != SQLITE_OK)
        fprintf(stderr, "SQLite 回归准备错误：%s\n",
                message != NULL ? message : "");
    sqlite3_free(message);
    require(status == SQLITE_OK, "修改损坏 DB 回归数据库");
    require(sqlite3_close(database) == SQLITE_OK,
            "关闭损坏 DB 回归数据库");
}

static int open_file_count(void) {
    int count = 0;
    int limit = getdtablesize();
    for (int descriptor = 0; descriptor < limit; descriptor++) {
        if (fcntl(descriptor, F_GETFD) >= 0 || errno != EBADF)
            count++;
    }
    return count;
}

static bool directory_is_case_insensitive(const char *directory) {
    char uppercase[PATH_MAX];
    char lowercase[PATH_MAX];
    format_child(uppercase, directory, "CaseSensitivityProbe");
    format_child(lowercase, directory, "casesensitivityprobe");
    require(mkdir(uppercase, 0700) == 0,
            "创建大小写敏感性探针");
    bool insensitive = false;
    if (mkdir(lowercase, 0700) < 0) {
        require(errno == EEXIST,
                "探测文件系统大小写敏感性");
        insensitive = true;
    } else {
        require(rmdir(lowercase) == 0,
                "清理小写敏感性探针");
    }
    require(rmdir(uppercase) == 0,
            "清理大写敏感性探针");
    return insensitive;
}

static void cancel_progress(
        void *cookie,
        double fraction,
        const char *message,
        bool *cancel_out) {
    (void) cookie;
    (void) fraction;
    (void) message;
    *cancel_out = true;
}

struct body_cancel_observer {
    const char *partial_path;
    unsigned path_callbacks;
    off_t partial_size;
    bool cancelled_inside_body;
};

static void cancel_large_body_progress(
        void *cookie,
        double fraction,
        const char *message,
        bool *cancel_out) {
    (void) fraction;
    struct body_cancel_observer *observer = cookie;
    if (message == NULL ||
            strcmp(message, "/var/large.bin") != 0)
        return;
    observer->path_callbacks++;
    struct stat metadata;
    if (stat(observer->partial_path, &metadata) == 0 &&
            metadata.st_size > 0 &&
            metadata.st_size < LARGE_REGULAR_SIZE) {
        observer->partial_size = metadata.st_size;
        observer->cancelled_inside_body = true;
        *cancel_out = true;
    }
}

static void add_regular(
        struct archive *archive,
        const char *path,
        const char *contents) {
    struct archive_entry *entry = archive_entry_new();
    require(entry != NULL, "创建归档条目");
    archive_entry_set_pathname(entry, path);
    archive_entry_set_filetype(entry, AE_IFREG);
    archive_entry_set_perm(entry, 0644);
    archive_entry_set_size(entry, (la_int64_t) strlen(contents));
    require(archive_write_header(archive, entry) == ARCHIVE_OK,
            "写入归档条目头");
    require(archive_write_data(archive, contents, strlen(contents)) ==
            (la_ssize_t) strlen(contents), "写入归档条目正文");
    archive_entry_free(entry);
}

static void add_large_regular(struct archive *archive) {
    struct archive_entry *entry = archive_entry_new();
    require(entry != NULL, "创建大文件归档条目");
    archive_entry_set_pathname(entry, "var/large.bin");
    archive_entry_set_filetype(entry, AE_IFREG);
    archive_entry_set_perm(entry, 0600);
    archive_entry_set_size(entry, LARGE_REGULAR_SIZE);
    require(archive_write_header(archive, entry) == ARCHIVE_OK,
            "写入大文件归档条目头");
    unsigned char bytes[64 * 1024];
    memset(bytes, 0x5a, sizeof(bytes));
    for (size_t written = 0; written < LARGE_REGULAR_SIZE;
            written += sizeof(bytes))
        require(archive_write_data(
                archive, bytes, sizeof(bytes)) ==
                (la_ssize_t) sizeof(bytes), "写入大文件归档正文");
    archive_entry_free(entry);
}

static void add_sparse_regular(struct archive *archive) {
    struct archive_entry *entry = archive_entry_new();
    require(entry != NULL, "创建稀疏文件归档条目");
    archive_entry_set_pathname(entry, "var/sparse.bin");
    archive_entry_set_filetype(entry, AE_IFREG);
    archive_entry_set_perm(entry, 0600);
    archive_entry_set_size(entry, SPARSE_LOGICAL_SIZE);
    archive_entry_sparse_add_entry(
            entry, SPARSE_FIRST_OFFSET, SPARSE_EXTENT_SIZE);
    archive_entry_sparse_add_entry(
            entry, SPARSE_SECOND_OFFSET, SPARSE_EXTENT_SIZE);
    require(archive_write_header(archive, entry) == ARCHIVE_OK,
            "写入稀疏文件归档条目头");

    unsigned char bytes[64 * 1024];
    for (size_t offset = 0; offset < SPARSE_LOGICAL_SIZE;
            offset += sizeof(bytes)) {
        memset(bytes, 0, sizeof(bytes));
        if (offset == SPARSE_FIRST_CHUNK_OFFSET)
            memset(bytes + 3, 0x41, SPARSE_EXTENT_SIZE);
        if (offset == SPARSE_SECOND_CHUNK_OFFSET)
            memset(bytes + 7, 0x42, SPARSE_EXTENT_SIZE);
        require(archive_write_data(
                archive, bytes, sizeof(bytes)) ==
                (la_ssize_t) sizeof(bytes), "写入稀疏文件逻辑正文");
    }
    archive_entry_free(entry);
}

static void add_timed_entries(struct archive *archive) {
    struct archive_entry *directory = archive_entry_new();
    require(directory != NULL, "创建带时间目录归档条目");
    archive_entry_set_pathname(directory, "timed");
    archive_entry_set_filetype(directory, AE_IFDIR);
    archive_entry_set_perm(directory, 0750);
    archive_entry_set_size(directory, 0);
    archive_entry_set_mtime(directory, TIMED_DIRECTORY_MTIME, 123456789);
    archive_entry_set_atime(directory, TIMED_DIRECTORY_MTIME - 10, 0);
    require(archive_write_header(archive, directory) == ARCHIVE_OK,
            "写入带时间目录归档条目");
    archive_entry_free(directory);

    struct archive_entry *file = archive_entry_new();
    require(file != NULL, "创建带时间文件归档条目");
    archive_entry_set_pathname(file, "timed/child");
    archive_entry_set_filetype(file, AE_IFREG);
    archive_entry_set_perm(file, 0640);
    archive_entry_set_size(file, 6);
    archive_entry_set_mtime(file, TIMED_FILE_MTIME, 987654321);
    archive_entry_set_atime(file, TIMED_FILE_MTIME - 10, 0);
    require(archive_write_header(archive, file) == ARCHIVE_OK &&
            archive_write_data(archive, "timed\n", 6) == 6,
            "写入带时间文件归档条目");
    archive_entry_free(file);
}

static void add_fifo(struct archive *archive) {
    struct archive_entry *entry = archive_entry_new();
    require(entry != NULL, "创建 FIFO 归档条目");
    archive_entry_set_pathname(entry, "run/guest.pipe");
    archive_entry_set_filetype(entry, AE_IFIFO);
    archive_entry_set_perm(entry, 0620);
    archive_entry_set_size(entry, 0);
    require(archive_write_header(archive, entry) == ARCHIVE_OK,
            "写入 FIFO 归档条目");
    archive_entry_free(entry);
    entry = archive_entry_new();
    require(entry != NULL, "创建 FIFO 硬链接归档条目");
    archive_entry_set_pathname(entry, "run/guest-alias.pipe");
    archive_entry_set_filetype(entry, AE_IFIFO);
    archive_entry_set_perm(entry, 0620);
    archive_entry_set_size(entry, 0);
    archive_entry_set_hardlink(entry, "run/guest.pipe");
    require(archive_write_header(archive, entry) == ARCHIVE_OK,
            "写入 FIFO 硬链接归档条目");
    archive_entry_free(entry);
}

static void add_hardlink_pair(struct archive *archive) {
    add_regular(archive, "hard/source.txt", "linked\n");
    struct archive_entry *entry = archive_entry_new();
    require(entry != NULL, "创建硬链接归档条目");
    archive_entry_set_pathname(entry, "hard/alias.txt");
    archive_entry_set_filetype(entry, AE_IFREG);
    archive_entry_set_perm(entry, 0644);
    archive_entry_set_size(entry, 0);
    archive_entry_set_hardlink(entry, "hard/source.txt");
    require(archive_write_header(archive, entry) == ARCHIVE_OK,
            "写入硬链接归档条目");
    archive_entry_free(entry);
}

static void add_symlink(
        struct archive *archive,
        const char *path,
        size_t target_length) {
    char *target = malloc(target_length + 1);
    require(target != NULL, "分配符号链接目标");
    memset(target, 's', target_length);
    target[target_length] = '\0';
    struct archive_entry *entry = archive_entry_new();
    require(entry != NULL, "创建符号链接归档条目");
    archive_entry_set_pathname(entry, path);
    archive_entry_set_filetype(entry, AE_IFLNK);
    archive_entry_set_perm(entry, 0777);
    archive_entry_set_size(entry, 0);
    archive_entry_set_symlink(entry, target);
    require(archive_write_header(archive, entry) == ARCHIVE_OK,
            "写入符号链接归档条目");
    archive_entry_free(entry);
    free(target);
}

static void create_input_archive(const char *path) {
    struct archive *archive = archive_write_new();
    require(archive != NULL, "创建 tar.gz 写入器");
    require(archive_write_add_filter_gzip(archive) == ARCHIVE_OK,
            "启用 gzip");
    require(archive_write_set_format_pax(archive) == ARCHIVE_OK,
            "启用 pax tar");
    require(archive_write_open_filename(archive, path) == ARCHIVE_OK,
            "打开输入 tar.gz");
    add_regular(archive, "etc/message.txt", "watch-root-roundtrip\n");
    add_timed_entries(archive);
    add_large_regular(archive);
    add_sparse_regular(archive);
    add_fifo(archive);
    add_hardlink_pair(archive);
    add_symlink(
            archive, "boundary-link", GUEST_PATH_LIMIT - 1);
    for (unsigned index = 0; index < 1024; index++) {
        char name[64];
        snprintf(name, sizeof(name), "many/item-%04u", index);
        add_regular(archive, name, "");
    }
    require(archive_write_close(archive) == ARCHIVE_OK,
            "关闭输入 tar.gz");
    require(archive_write_free(archive) == ARCHIVE_OK,
            "释放 tar.gz 写入器");
}

static void create_overlong_symlink_archive(const char *path) {
    struct archive *archive = archive_write_new();
    require(archive != NULL, "创建超长链接归档写入器");
    require(archive_write_add_filter_gzip(archive) == ARCHIVE_OK &&
            archive_write_set_format_pax(archive) == ARCHIVE_OK &&
            archive_write_open_filename(archive, path) == ARCHIVE_OK,
            "打开超长链接 tar.gz");
    add_symlink(archive, "overlong-link", GUEST_PATH_LIMIT);
    require(archive_write_close(archive) == ARCHIVE_OK &&
            archive_write_free(archive) == ARCHIVE_OK,
            "关闭超长链接 tar.gz");
}

static void add_empty_typed_entry(
        struct archive *archive,
        const char *path,
        mode_t filetype) {
    struct archive_entry *entry = archive_entry_new();
    require(entry != NULL, "创建重复路径归档条目");
    archive_entry_set_pathname(entry, path);
    archive_entry_set_filetype(entry, filetype);
    archive_entry_set_perm(entry, 0600);
    archive_entry_set_size(entry, 0);
    require(archive_write_header(archive, entry) == ARCHIVE_OK,
            "写入重复路径归档条目");
    archive_entry_free(entry);
}

static void create_collision_archive(
        const char *path,
        const char *first_path,
        mode_t first_type,
        const char *second_path,
        mode_t second_type) {
    struct archive *archive = archive_write_new();
    require(archive != NULL, "创建重复路径归档写入器");
    require(archive_write_add_filter_gzip(archive) == ARCHIVE_OK &&
            archive_write_set_format_pax(archive) == ARCHIVE_OK &&
            archive_write_open_filename(archive, path) == ARCHIVE_OK,
            "打开重复路径 tar.gz");
    add_empty_typed_entry(archive, first_path, first_type);
    add_empty_typed_entry(archive, second_path, second_type);
    require(archive_write_close(archive) == ARCHIVE_OK &&
            archive_write_free(archive) == ARCHIVE_OK,
            "关闭重复路径 tar.gz");
}

static void create_hardlink_collision_archive(const char *path) {
    struct archive *archive = archive_write_new();
    require(archive != NULL, "创建硬链接重复路径归档写入器");
    require(archive_write_add_filter_gzip(archive) == ARCHIVE_OK &&
            archive_write_set_format_pax(archive) == ARCHIVE_OK &&
            archive_write_open_filename(archive, path) == ARCHIVE_OK,
            "打开硬链接重复路径 tar.gz");
    add_regular(archive, "source", "first\n");
    struct archive_entry *entry = archive_entry_new();
    require(entry != NULL, "创建硬链接重复路径条目");
    archive_entry_set_pathname(entry, "alias");
    archive_entry_set_filetype(entry, AE_IFREG);
    archive_entry_set_perm(entry, 0600);
    archive_entry_set_size(entry, 0);
    archive_entry_set_hardlink(entry, "source");
    require(archive_write_header(archive, entry) == ARCHIVE_OK,
            "写入硬链接重复路径条目");
    archive_entry_free(entry);
    add_regular(archive, "source", "second\n");
    require(archive_write_close(archive) == ARCHIVE_OK &&
            archive_write_free(archive) == ARCHIVE_OK,
            "关闭硬链接重复路径 tar.gz");
}

static void create_single_path_archive(
        const char *path, const char *entry_path) {
    struct archive *archive = archive_write_new();
    require(archive != NULL, "创建不安全路径归档写入器");
    require(archive_write_add_filter_gzip(archive) == ARCHIVE_OK &&
            archive_write_set_format_pax(archive) == ARCHIVE_OK &&
            archive_write_open_filename(archive, path) == ARCHIVE_OK,
            "打开不安全路径 tar.gz");
    add_regular(archive, entry_path, "");
    require(archive_write_close(archive) == ARCHIVE_OK &&
            archive_write_free(archive) == ARCHIVE_OK,
            "关闭不安全路径 tar.gz");
}

static void create_unsafe_hardlink_archive(const char *path) {
    struct archive *archive = archive_write_new();
    require(archive != NULL, "创建不安全硬链接归档写入器");
    require(archive_write_add_filter_gzip(archive) == ARCHIVE_OK &&
            archive_write_set_format_pax(archive) == ARCHIVE_OK &&
            archive_write_open_filename(archive, path) == ARCHIVE_OK,
            "打开不安全硬链接 tar.gz");
    add_regular(archive, "source", "");
    struct archive_entry *entry = archive_entry_new();
    require(entry != NULL, "创建不安全硬链接归档条目");
    archive_entry_set_pathname(entry, "alias");
    archive_entry_set_filetype(entry, AE_IFREG);
    archive_entry_set_perm(entry, 0600);
    archive_entry_set_size(entry, 0);
    archive_entry_set_hardlink(entry, "../source");
    require(archive_write_header(archive, entry) == ARCHIVE_OK,
            "写入不安全硬链接归档条目");
    archive_entry_free(entry);
    require(archive_write_close(archive) == ARCHIVE_OK &&
            archive_write_free(archive) == ARCHIVE_OK,
            "关闭不安全硬链接 tar.gz");
}

struct progress_observer {
    double last_fraction;
    unsigned calls;
    unsigned increases;
};

static void observe_progress(
        void *cookie,
        double fraction,
        const char *message,
        bool *cancel_out) {
    (void) message;
    (void) cancel_out;
    struct progress_observer *observer = cookie;
    require(fraction >= observer->last_fraction,
            "真实 fakefs 进度必须单调");
    if (fraction > observer->last_fraction)
        observer->increases++;
    observer->last_fraction = fraction;
    observer->calls++;
}

static void verify_sparse_region(
        int descriptor,
        off_t offset,
        size_t length,
        unsigned char expected,
        const char *message) {
    unsigned char bytes[SPARSE_EXTENT_SIZE];
    require(length <= sizeof(bytes), "稀疏文件检查范围必须有界");
    ssize_t count;
    do {
        count = pread(descriptor, bytes, length, offset);
    } while (count < 0 && errno == EINTR);
    require(count == (ssize_t) length, message);
    for (size_t index = 0; index < length; index++)
        require(bytes[index] == expected, message);
}

static void verify_imported_sparse(const char *fakefs) {
    char path[PATH_MAX];
    format_child(path, fakefs, "data/var/sparse.bin");
    struct stat metadata;
    require(stat(path, &metadata) == 0 &&
            S_ISREG(metadata.st_mode) &&
            metadata.st_size == SPARSE_LOGICAL_SIZE,
            "稀疏文件必须保留逻辑长度");
    require(metadata.st_blocks >= 0 &&
            metadata.st_blocks <
            SPARSE_LOGICAL_SIZE / (8 * 512),
            "稀疏文件空洞不能物化为真实磁盘块");

    int descriptor = open(path, O_RDONLY | O_CLOEXEC);
    require(descriptor >= 0, "打开导入后的稀疏文件");
    verify_sparse_region(
            descriptor, 0, 1, 0,
            "稀疏文件首洞必须读为零");
    verify_sparse_region(
            descriptor, SPARSE_FIRST_OFFSET,
            SPARSE_EXTENT_SIZE, 0x41,
            "稀疏文件首个数据 extent 必须完整");
    verify_sparse_region(
            descriptor,
            SPARSE_FIRST_OFFSET + SPARSE_EXTENT_SIZE,
            1, 0, "稀疏文件中间洞必须读为零");
    verify_sparse_region(
            descriptor, SPARSE_SECOND_OFFSET - 1,
            1, 0, "稀疏文件第二个 extent 前必须保留空洞");
    verify_sparse_region(
            descriptor, SPARSE_SECOND_OFFSET,
            SPARSE_EXTENT_SIZE, 0x42,
            "稀疏文件第二个数据 extent 必须完整");
    verify_sparse_region(
            descriptor, SPARSE_LOGICAL_SIZE - 1,
            1, 0, "稀疏文件尾洞必须读为零");
    unsigned char byte;
    ssize_t eof_count;
    do {
        eof_count = pread(
                descriptor, &byte, 1, SPARSE_LOGICAL_SIZE);
    } while (eof_count < 0 && errno == EINTR);
    require(eof_count == 0, "稀疏文件逻辑 EOF 必须精确");
    require(close(descriptor) == 0, "关闭导入后的稀疏文件");
}

static void verify_output_archive(const char *path) {
    struct archive *archive = archive_read_new();
    require(archive != NULL, "创建 tar.gz 读取器");
    require(archive_read_support_filter_gzip(archive) == ARCHIVE_OK,
            "启用 gzip 读取");
    require(archive_read_support_format_tar(archive) == ARCHIVE_OK,
            "启用 tar 读取");
    require(archive_read_open_filename(archive, path, 64 * 1024) ==
            ARCHIVE_OK, "打开输出 tar.gz");

    bool found = false;
    unsigned fifo_entries = 0;
    unsigned fifo_hardlinks = 0;
    bool found_boundary_symlink = false;
    bool found_timed_directory = false;
    bool found_timed_file = false;
    bool found_implicit_directory = false;
    unsigned hardlink_entries = 0;
    unsigned hardlink_bodies = 0;
    unsigned single_link_entries = 0;
    struct archive_entry *entry;
    int status;
    while ((status = archive_read_next_header(archive, &entry)) ==
            ARCHIVE_OK) {
        const char *entry_path = archive_entry_pathname(entry);
        if (strcmp(entry_path, "./run/guest.pipe") == 0 ||
                strcmp(entry_path, "./run/guest-alias.pipe") == 0) {
            const char *hardlink = archive_entry_hardlink(entry);
            require(archive_entry_perm(entry) == 0620 &&
                    ((hardlink == NULL &&
                            archive_entry_filetype(entry) == AE_IFIFO) ||
                     (hardlink != NULL &&
                            strcmp(hardlink, "./run/guest.pipe") == 0)),
                    "FIFO roundtrip 必须保留类型、权限与硬链接");
            fifo_entries++;
            if (hardlink != NULL)
                fifo_hardlinks++;
            continue;
        }
        if (strcmp(entry_path, "./boundary-link") == 0) {
            const char *target = archive_entry_symlink(entry);
            require(target != NULL &&
                    strlen(target) == GUEST_PATH_LIMIT - 1 &&
                    target[0] == 's' &&
                    target[GUEST_PATH_LIMIT - 2] == 's',
                    "MAX_PATH-1 符号链接目标必须无损 roundtrip");
            found_boundary_symlink = true;
            continue;
        }
        if (strcmp(entry_path, "./timed") == 0 ||
                strcmp(entry_path, "./timed/") == 0) {
            require(
                    archive_entry_filetype(entry) == AE_IFDIR &&
                    archive_entry_mtime(entry) ==
                            TIMED_DIRECTORY_MTIME &&
                    archive_entry_mtime_nsec(entry) == 123456789,
                    "目录 mtime 必须在子项创建后恢复");
            found_timed_directory = true;
            continue;
        }
        if (strcmp(entry_path, "./timed/child") == 0) {
            require(
                    archive_entry_filetype(entry) == AE_IFREG &&
                    archive_entry_mtime(entry) == TIMED_FILE_MTIME &&
                    archive_entry_mtime_nsec(entry) == 987654321,
                    "普通文件 mtime 必须无损 roundtrip");
            found_timed_file = true;
            continue;
        }
        if (strcmp(entry_path, "./etc") == 0 ||
                strcmp(entry_path, "./etc/") == 0) {
            require(archive_entry_filetype(entry) == AE_IFDIR,
                    "省略的父目录必须补入 guest DB 并导出为目录");
            found_implicit_directory = true;
            continue;
        }
        if (strncmp(entry_path, "./many/item-", 12) == 0) {
            single_link_entries++;
            continue;
        }
        if (strcmp(entry_path, "./hard/source.txt") == 0 ||
                strcmp(entry_path, "./hard/alias.txt") == 0) {
            hardlink_entries++;
            if (archive_entry_hardlink(entry) == NULL) {
                char linked[16] = {0};
                la_ssize_t linked_count = archive_read_data(
                        archive, linked, sizeof(linked) - 1);
                require(linked_count == 7 &&
                        strcmp(linked, "linked\n") == 0,
                        "硬链接实体正文必须保持一致");
                hardlink_bodies++;
            }
            continue;
        }
        if (strcmp(archive_entry_pathname(entry),
                "./etc/message.txt") != 0) {
            archive_read_data_skip(archive);
            continue;
        }
        char contents[64] = {0};
        la_ssize_t count = archive_read_data(
                archive, contents, sizeof(contents) - 1);
        require(count == (la_ssize_t) strlen("watch-root-roundtrip\n"),
                "读取 roundtrip 正文");
        require(strcmp(contents, "watch-root-roundtrip\n") == 0,
                "roundtrip 正文必须保持一致");
        found = true;
    }
    bool complete = status == ARCHIVE_EOF && found &&
            fifo_entries == 2 && fifo_hardlinks == 1 &&
            found_boundary_symlink &&
            found_timed_directory && found_timed_file &&
            found_implicit_directory &&
            hardlink_entries == 2 && hardlink_bodies == 1 &&
            single_link_entries == 1024;
    if (!complete) {
        fprintf(stderr,
                "roundtrip 统计：status=%d file=%d fifo=%u/%u "
                "symlink=%d timed=%d/%d implicit=%d "
                "hardlink=%u/%u single=%u\n",
                status, found, fifo_entries, fifo_hardlinks,
                found_boundary_symlink,
                found_timed_directory, found_timed_file,
                found_implicit_directory,
                hardlink_entries, hardlink_bodies,
                single_link_entries);
    }
    require(complete,
            "输出 tar.gz 必须保留普通文件、FIFO、硬链接与单链接闭包");
    require(archive_read_free(archive) == ARCHIVE_OK,
            "释放 tar.gz 读取器");
}

static int remove_tree_at(int parent, const char *name) {
    struct stat metadata;
    if (fstatat(parent, name, &metadata, AT_SYMLINK_NOFOLLOW) < 0)
        return errno == ENOENT ? 0 : errno;
    if (!S_ISDIR(metadata.st_mode))
        return unlinkat(parent, name, 0) < 0 ? errno : 0;
    int child = openat(parent, name,
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (child < 0)
        return errno;
    DIR *iterator = fdopendir(dup(child));
    if (iterator == NULL) {
        close(child);
        return errno;
    }
    int error = 0;
    struct dirent *entry;
    while (error == 0 && (entry = readdir(iterator)) != NULL) {
        if (strcmp(entry->d_name, ".") != 0 &&
                strcmp(entry->d_name, "..") != 0)
            error = remove_tree_at(child, entry->d_name);
    }
    closedir(iterator);
    close(child);
    if (error == 0 && unlinkat(parent, name, AT_REMOVEDIR) < 0)
        error = errno;
    return error;
}

int main(void) {
    char temporary[] = "/tmp/ish-watch-root-roundtrip.XXXXXX";
    char *base = mkdtemp(temporary);
    require(base != NULL, "创建 roundtrip 临时目录");

    char input[PATH_MAX];
    char fakefs[PATH_MAX];
    char output[PATH_MAX];
    char invalid_input[PATH_MAX];
    char invalid_fakefs[PATH_MAX];
    format_child(input, base, "input.tar.gz");
    format_child(fakefs, base, "fakefs");
    format_child(output, base, "output.tar.gz");
    format_child(invalid_input, base, "invalid-symlink.tar.gz");
    format_child(invalid_fakefs, base, "invalid-symlink-fakefs");
    create_input_archive(input);
    create_overlong_symlink_archive(invalid_input);
    static const struct {
        const char *file_name;
        const char *first_path;
        mode_t first_type;
        const char *second_path;
        mode_t second_type;
    } collision_specs[] = {
        {
            "fifo-then-regular.tar.gz",
            "collision", AE_IFIFO,
            "collision", AE_IFREG,
        },
        {
            "regular-then-fifo.tar.gz",
            "collision", AE_IFREG,
            "collision", AE_IFIFO,
        },
        {
            "normalized-duplicate.tar.gz",
            "parent/item", AE_IFREG,
            "parent/./item", AE_IFREG,
        },
        {
            "parent-type-collision.tar.gz",
            "parent", AE_IFREG,
            "parent/item", AE_IFREG,
        },
    };
    for (size_t index = 0;
            index < sizeof(collision_specs) /
                    sizeof(collision_specs[0]);
            index++) {
        char collision_archive[PATH_MAX];
        format_child(collision_archive, base,
                collision_specs[index].file_name);
        create_collision_archive(
                collision_archive,
                collision_specs[index].first_path,
                collision_specs[index].first_type,
                collision_specs[index].second_path,
                collision_specs[index].second_type);
    }
    char casefold_collision[PATH_MAX];
    format_child(casefold_collision, base,
            "casefold-parent-collision.tar.gz");
    create_collision_archive(
            casefold_collision,
            "A/first", AE_IFREG,
            "a/second", AE_IFREG);
    char hardlink_collision[PATH_MAX];
    format_child(hardlink_collision, base,
            "hardlink-duplicate.tar.gz");
    create_hardlink_collision_archive(hardlink_collision);
    char traversal_archive[PATH_MAX];
    char unsafe_hardlink_archive[PATH_MAX];
    format_child(traversal_archive, base,
            "traversal.tar.gz");
    format_child(unsafe_hardlink_archive, base,
            "unsafe-hardlink.tar.gz");
    create_single_path_archive(traversal_archive, "../escape");
    create_unsafe_hardlink_archive(unsafe_hardlink_archive);

    struct fakefsify_error error = {0};
    require(!fakefs_import(
                invalid_input, invalid_fakefs,
                &error, (struct progress) {0}) &&
            error.type == ERR_ARCHIVE &&
            error.code == ENAMETOOLONG,
            "导入必须快速拒绝 MAX_PATH 符号链接目标");
    free(error.message);
    error = (struct fakefsify_error) {0};
    int invalid_parent = open(
            base, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    require(invalid_parent >= 0 &&
            remove_tree_at(
                    invalid_parent, "invalid-symlink-fakefs") == 0,
            "清理超长链接导入 fakefs");
    close(invalid_parent);

    int baseline_descriptors = open_file_count();
    for (size_t index = 0;
            index < sizeof(collision_specs) /
                    sizeof(collision_specs[0]) + 1;
            index++) {
        const char *archive_name = index <
                sizeof(collision_specs) /
                        sizeof(collision_specs[0]) ?
                collision_specs[index].file_name :
                "hardlink-duplicate.tar.gz";
        char collision_archive[PATH_MAX];
        char collision_fakefs[PATH_MAX];
        char fakefs_name[64];
        format_child(collision_archive, base, archive_name);
        snprintf(fakefs_name, sizeof(fakefs_name),
                "collision-fakefs-%zu", index);
        format_child(collision_fakefs, base, fakefs_name);
        alarm(3);
        bool collision_imported = fakefs_import(
                collision_archive, collision_fakefs,
                &error, (struct progress) {0});
        alarm(0);
        require(!collision_imported &&
                error.type != ERR_CANCELLED,
                "重复或类型冲突路径必须快速拒绝");
        free(error.message);
        error = (struct fakefsify_error) {0};
        int collision_parent = open(
                base, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        require(collision_parent >= 0 &&
                remove_tree_at(
                        collision_parent, fakefs_name) == 0,
                "清理重复路径导入 fakefs");
        close(collision_parent);
    }
    require(open_file_count() == baseline_descriptors,
            "重复路径快速失败不能泄漏文件描述符");
    if (directory_is_case_insensitive(base)) {
        char casefold_fakefs[PATH_MAX];
        format_child(casefold_fakefs, base,
                "casefold-collision-fakefs");
        alarm(3);
        bool casefold_imported = fakefs_import(
                casefold_collision, casefold_fakefs,
                &error, (struct progress) {0});
        alarm(0);
        require(!casefold_imported &&
                error.type == ERR_POSIX &&
                error.code == EEXIST,
                "大小写折叠宿主不能让两个 guest 父目录共享对象");
        free(error.message);
        error = (struct fakefsify_error) {0};
        int casefold_parent = open(
                base, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        require(casefold_parent >= 0 &&
                remove_tree_at(casefold_parent,
                        "casefold-collision-fakefs") == 0,
                "清理大小写碰撞导入 fakefs");
        close(casefold_parent);
    }
    static const char *unsafe_names[] = {
        "traversal.tar.gz",
        "unsafe-hardlink.tar.gz",
    };
    for (size_t index = 0;
            index < sizeof(unsafe_names) /
                    sizeof(unsafe_names[0]);
            index++) {
        char unsafe_archive[PATH_MAX];
        char unsafe_fakefs[PATH_MAX];
        char fakefs_name[64];
        format_child(unsafe_archive, base, unsafe_names[index]);
        snprintf(fakefs_name, sizeof(fakefs_name),
                "unsafe-fakefs-%zu", index);
        format_child(unsafe_fakefs, base, fakefs_name);
        require(!fakefs_import(
                    unsafe_archive, unsafe_fakefs,
                    &error, (struct progress) {0}) &&
                error.type == ERR_ARCHIVE &&
                error.code == EINVAL,
                "不安全条目必须让整个导入失败");
        free(error.message);
        error = (struct fakefsify_error) {0};
        int unsafe_parent = open(
                base, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        require(unsafe_parent >= 0 &&
                remove_tree_at(
                        unsafe_parent, fakefs_name) == 0,
                "清理不安全条目导入 fakefs");
        close(unsafe_parent);
    }

    char body_cancel_fakefs[PATH_MAX];
    char body_cancel_path[PATH_MAX];
    format_child(
            body_cancel_fakefs, base, "body-cancel-fakefs");
    format_child(
            body_cancel_path, body_cancel_fakefs,
            "data/var/large.bin");
    struct body_cancel_observer body_cancel = {
        .partial_path = body_cancel_path,
    };
    bool body_imported = fakefs_import(
            input, body_cancel_fakefs, &error,
            (struct progress) {
                .cookie = &body_cancel,
                .callback = cancel_large_body_progress,
            });
    require(!body_imported &&
            error.type == ERR_CANCELLED &&
            body_cancel.path_callbacks >= 2 &&
            body_cancel.cancelled_inside_body &&
            body_cancel.partial_size > 0 &&
            body_cancel.partial_size < LARGE_REGULAR_SIZE,
            "大文件导入必须能在条目正文内部取消");
    free(error.message);
    error = (struct fakefsify_error) {0};
    int body_cancel_parent = open(
            base, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    require(body_cancel_parent >= 0 &&
            remove_tree_at(
                    body_cancel_parent,
                    "body-cancel-fakefs") == 0,
            "清理正文中途取消的 fakefs");
    close(body_cancel_parent);
    require(open_file_count() == baseline_descriptors,
            "正文中途取消不能泄漏文件描述符");

    for (unsigned attempt = 0; attempt < 16; attempt++) {
        char cancelled_fakefs[PATH_MAX];
        char name[32];
        snprintf(name, sizeof(name), "cancel-import-%u", attempt);
        format_child(cancelled_fakefs, base, name);
        bool imported = fakefs_import(
                input, cancelled_fakefs, &error,
                (struct progress) {
                    .callback = cancel_progress,
                });
        require(!imported && error.type == ERR_CANCELLED,
                "取消真实导入必须返回 ERR_CANCELLED");
        free(error.message);
        error = (struct fakefsify_error) {0};
        int parent = open(base, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        require(parent >= 0, "打开取消导入临时目录");
        require(remove_tree_at(parent, name) == 0,
                "清理取消导入 fakefs");
        close(parent);
    }
    require(open_file_count() == baseline_descriptors,
            "反复取消真实导入不能泄漏文件描述符");

    if (!fakefs_import(
            input, fakefs, &error, (struct progress) {0})) {
        fprintf(stderr, "fakefs 导入错误：line=%d type=%d code=%d %s\n",
                error.line, error.type, error.code,
                error.message != NULL ? error.message : "");
        require(false, "真实 libarchive 应导入 tar.gz");
    }
    verify_imported_sparse(fakefs);
    baseline_descriptors = open_file_count();
    for (unsigned attempt = 0; attempt < 16; attempt++) {
        char cancelled_output[PATH_MAX];
        char name[32];
        snprintf(name, sizeof(name), "cancel-export-%u.tar.gz", attempt);
        format_child(cancelled_output, base, name);
        bool exported = fakefs_export(
                fakefs, cancelled_output, &error,
                (struct progress) {
                    .callback = cancel_progress,
                });
        require(!exported && error.type == ERR_CANCELLED,
                "取消真实导出必须返回 ERR_CANCELLED");
        free(error.message);
        error = (struct fakefsify_error) {0};
        require(unlink(cancelled_output) == 0,
                "清理取消导出 partial");
    }
    require(open_file_count() == baseline_descriptors,
            "反复取消真实导出不能泄漏文件描述符");

    char boundary_link[PATH_MAX];
    format_child(boundary_link, fakefs, "data/boundary-link");
    int boundary_file = open(
            boundary_link, O_WRONLY | O_APPEND | O_CLOEXEC);
    require(boundary_file >= 0 &&
            write(boundary_file, "x", 1) == 1 &&
            close(boundary_file) == 0,
            "构造 MAX_PATH 宿主链接占位文件");
    char overlong_output[PATH_MAX];
    format_child(overlong_output, base, "overlong-output.tar.gz");
    require(!fakefs_export(
                fakefs, overlong_output,
                &error, (struct progress) {0}) &&
            error.type == ERR_ARCHIVE &&
            error.code == ENAMETOOLONG,
            "导出必须快速拒绝 MAX_PATH 宿主链接占位文件");
    free(error.message);
    error = (struct fakefsify_error) {0};
    require(unlink(overlong_output) == 0,
            "清理超长链接导出 partial");
    boundary_file = open(
            boundary_link, O_WRONLY | O_CLOEXEC);
    require(boundary_file >= 0 &&
            ftruncate(
                    boundary_file, GUEST_PATH_LIMIT - 1) == 0 &&
            close(boundary_file) == 0,
            "恢复 MAX_PATH-1 链接占位文件");

    char corrupted_regular[PATH_MAX];
    format_child(corrupted_regular, fakefs, "data/etc/message.txt");
    require(unlink(corrupted_regular) == 0 &&
            mkfifo(corrupted_regular, 0600) == 0,
            "构造 guest regular 与宿主 FIFO 类型错配");
    char corrupted_output[PATH_MAX];
    format_child(corrupted_output, base, "corrupted-output.tar.gz");
    alarm(3);
    bool corrupted_export = fakefs_export(
            fakefs, corrupted_output,
            &error, (struct progress) {0});
    alarm(0);
    require(!corrupted_export &&
            error.type == ERR_ARCHIVE &&
            error.code == EINVAL,
            "损坏 fakefs 导出必须快速拒绝而不能阻塞 FIFO");
    free(error.message);
    error = (struct fakefsify_error) {0};
    require(unlink(corrupted_output) == 0 &&
            unlink(corrupted_regular) == 0,
            "清理损坏 fakefs 导出 partial 与 FIFO");
    int restored_regular = open(corrupted_regular,
            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    static const char restored_contents[] =
            "watch-root-roundtrip\n";
    require(restored_regular >= 0 &&
            write(restored_regular, restored_contents,
                    sizeof(restored_contents) - 1) ==
                    (ssize_t) sizeof(restored_contents) - 1 &&
            close(restored_regular) == 0,
            "恢复 guest regular 宿主占位文件");

    require(unlink(corrupted_regular) == 0,
            "删除 DB 仍引用的宿主普通文件");
    char missing_output[PATH_MAX];
    format_child(missing_output, base, "missing-output.tar.gz");
    require(!fakefs_export(
                fakefs, missing_output,
                &error, (struct progress) {0}) &&
            error.type == ERR_POSIX &&
            error.code == ENOENT,
            "DB 路径缺少宿主对象必须导出失败");
    free(error.message);
    error = (struct fakefsify_error) {0};
    require(unlink(missing_output) == 0,
            "清理宿主缺项导出 partial");
    restored_regular = open(corrupted_regular,
            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    require(restored_regular >= 0 &&
            write(restored_regular, restored_contents,
                    sizeof(restored_contents) - 1) ==
                    (ssize_t) sizeof(restored_contents) - 1 &&
            close(restored_regular) == 0,
            "恢复 DB 引用的宿主普通文件");

    char database_path[PATH_MAX];
    format_child(database_path, fakefs, "meta.db");
    static const struct {
        const char *name;
        const char *insert_sql;
        const char *delete_sql;
    } database_corruptions[] = {
        {
            "orphan-db",
            "insert into paths(path, inode) "
            "values('/orphan-db', 9223372036854775806)",
            "delete from paths where path = '/orphan-db'",
        },
        {
            "null-inode",
            "insert into paths(path, inode) "
            "values('/null-inode', null)",
            "delete from paths where path = '/null-inode'",
        },
    };
    for (size_t index = 0;
            index < sizeof(database_corruptions) /
                    sizeof(database_corruptions[0]);
            index++) {
        char data_directory[PATH_MAX];
        char host_path[PATH_MAX];
        format_child(data_directory, fakefs, "data");
        format_child(host_path, data_directory,
                database_corruptions[index].name);
        int host_file = open(host_path,
                O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        require(host_file >= 0 && close(host_file) == 0,
                "创建损坏 DB 路径的宿主占位文件");
        execute_database_sql(
                database_path, database_corruptions[index].insert_sql);
        char corrupt_database_output[PATH_MAX];
        char output_name[64];
        snprintf(output_name, sizeof(output_name),
                "%s-output.tar.gz", database_corruptions[index].name);
        format_child(corrupt_database_output, base, output_name);
        require(!fakefs_export(
                        fakefs, corrupt_database_output,
                        &error, (struct progress) {0}) &&
                error.type == ERR_SQLITE &&
                error.code == SQLITE_CORRUPT,
                "缺失 stats 或 NULL inode 的 DB 路径必须导出失败");
        free(error.message);
        error = (struct fakefsify_error) {0};
        require(unlink(corrupt_database_output) == 0,
                "清理损坏 DB 导出 partial");
        execute_database_sql(
                database_path, database_corruptions[index].delete_sql);
        require(unlink(host_path) == 0,
                "清理损坏 DB 路径的宿主占位文件");
    }

    struct progress_observer observer = {0};
    if (!fakefs_export(
            fakefs, output, &error,
            (struct progress) {
                .cookie = &observer,
                .callback = observe_progress,
            })) {
        fprintf(stderr, "fakefs 导出错误：line=%d type=%d code=%d %s\n",
                error.line, error.type, error.code,
                error.message != NULL ? error.message : "");
        require(false, "真实 libarchive 应导出 tar.gz");
    }
    require(observer.calls > 4 && observer.increases > 2,
            "真实大文件导出必须分块推进进度");
    require(ish_fakefs_test_single_link_entries >= 1024 &&
            ish_fakefs_test_multi_link_entries >= 4,
            "resolver 必须收到数据库推导的单链接与多链接 nlink");
    verify_output_archive(output);

    int parent = open(base, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    require(parent >= 0, "打开 roundtrip 临时目录");
    require(remove_tree_at(parent, "input.tar.gz") == 0,
            "清理输入归档");
    require(remove_tree_at(parent, "invalid-symlink.tar.gz") == 0,
            "清理超长链接输入归档");
    for (size_t index = 0;
            index < sizeof(collision_specs) /
                    sizeof(collision_specs[0]);
            index++)
        require(remove_tree_at(
                    parent, collision_specs[index].file_name) == 0,
                "清理重复路径输入归档");
    require(remove_tree_at(parent, "hardlink-duplicate.tar.gz") == 0,
            "清理硬链接重复路径输入归档");
    require(remove_tree_at(
                    parent, "casefold-parent-collision.tar.gz") == 0,
            "清理大小写碰撞输入归档");
    require(remove_tree_at(parent, "traversal.tar.gz") == 0 &&
            remove_tree_at(parent, "unsafe-hardlink.tar.gz") == 0,
            "清理不安全路径输入归档");
    require(remove_tree_at(parent, "fakefs") == 0,
            "清理 fakefs");
    require(remove_tree_at(parent, "output.tar.gz") == 0,
            "清理输出归档");
    close(parent);
    require(rmdir(base) == 0, "清理 roundtrip 临时目录");
    puts("Watch 宿主 Root 真实 libarchive/fakefs roundtrip 通过");
    return 0;
}
