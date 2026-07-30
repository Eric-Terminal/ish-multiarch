#include "platform/apple-watch-guest-files-private.h"
#include "platform/apple-watch-runtime.h"

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fs/fd.h"
#include "fs/path.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "kernel/init.h"
#include "kernel/task.h"
#include "misc.h"

static int failures;
static int parent_mutation_error;
static int parent_mutation_mode;
static char replacement_temporary_name[MAX_PATH];

enum parent_mutation_mode {
    PARENT_MUTATION_NONE,
    PARENT_MUTATION_PUBLISH,
    PARENT_MUTATION_FAILED_WRITE,
};

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "Watch guest 文件测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        failures++; \
    } \
} while (0)

struct fixture {
    struct task task;
    char root_directory[PATH_MAX];
    char data_directory[PATH_MAX];
    bool mounted;
};

static bool insert_fakefs_entry(
        sqlite3 *database,
        sqlite3_int64 inode,
        const char *path,
        uint32_t mode) {
    const uint32_t stat[4] = {mode, 0, 0, 0};
    sqlite3_stmt *statement = NULL;
    int error = sqlite3_prepare_v2(
            database,
            "insert into stats (inode, stat) values (?, ?)",
            -1,
            &statement,
            NULL);
    if (error == SQLITE_OK)
        error = sqlite3_bind_int64(statement, 1, inode);
    if (error == SQLITE_OK)
        error = sqlite3_bind_blob(
                statement, 2, stat, sizeof(stat), SQLITE_STATIC);
    if (error == SQLITE_OK)
        error = sqlite3_step(statement);
    bool inserted_stat = error == SQLITE_DONE;
    sqlite3_finalize(statement);
    if (!inserted_stat)
        return false;

    statement = NULL;
    error = sqlite3_prepare_v2(
            database,
            "insert into paths (path, inode) values (?, ?)",
            -1,
            &statement,
            NULL);
    if (error == SQLITE_OK)
        error = sqlite3_bind_blob(
                statement,
                1,
                path,
                (int) strlen(path),
                SQLITE_STATIC);
    if (error == SQLITE_OK)
        error = sqlite3_bind_int64(statement, 2, inode);
    if (error == SQLITE_OK)
        error = sqlite3_step(statement);
    bool inserted_path = error == SQLITE_DONE;
    sqlite3_finalize(statement);
    return inserted_path;
}

static bool write_host_file(
        const char *path, const void *bytes, size_t length) {
    int file = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (file < 0)
        return false;
    const unsigned char *next = bytes;
    size_t remaining = length;
    while (remaining != 0) {
        ssize_t written = write(file, next, remaining);
        if (written <= 0) {
            close(file);
            return false;
        }
        next += (size_t) written;
        remaining -= (size_t) written;
    }
    return close(file) == 0;
}

static int write_guest_file(
        const char *path,
        const void *bytes,
        size_t length,
        int open_flags) {
    struct fd *file = generic_open(
            path, O_WRONLY_ | open_flags, 0644);
    if (IS_ERR(file))
        return (int) PTR_ERR(file);

    const unsigned char *next = bytes;
    size_t remaining = length;
    int error = 0;
    while (remaining != 0) {
        ssize_t written = file_write_fd(file, next, remaining);
        if (written <= 0) {
            error = written < 0 ? (int) written : _EIO;
            break;
        }
        next += (size_t) written;
        remaining -= (size_t) written;
    }
    int close_error = fd_close(file);
    if (error >= 0 && close_error < 0)
        error = close_error;
    return error;
}

static bool guest_file_matches(
        const char *path,
        const void *expected,
        size_t expected_length) {
    struct fd *file = generic_open(path, O_RDONLY_, 0);
    if (IS_ERR(file))
        return false;

    unsigned char buffer[256];
    ssize_t length = file_read_fd(file, buffer, sizeof(buffer));
    int close_error = fd_close(file);
    return close_error == 0 &&
            length == (ssize_t) expected_length &&
            memcmp(buffer, expected, expected_length) == 0;
}

static void mutate_repositories_parent(
        int32_t file_id,
        int event,
        const char *temporary_name) {
    if (file_id != ISH_WATCH_GUEST_FILE_REPOSITORIES ||
            event != ISH_WATCH_GUEST_FILE_TEST_TEMPORARY_OPENED ||
            parent_mutation_mode == PARENT_MUTATION_NONE)
        return;

    int mode = parent_mutation_mode;
    parent_mutation_mode = PARENT_MUTATION_NONE;
    parent_mutation_error = generic_renameat(
            AT_PWD, "/etc/apk",
            AT_PWD, "/etc/apk-original");
    if (parent_mutation_error >= 0)
        parent_mutation_error = generic_mkdirat(
                AT_PWD, "/etc/apk", 0755);

    static const char replacement[] =
            "https://replacement-parent.example\n";
    if (parent_mutation_error >= 0)
        parent_mutation_error = write_guest_file(
                "/etc/apk/repositories",
                replacement,
                sizeof(replacement) - 1,
                O_CREAT_ | O_EXCL_);

    replacement_temporary_name[0] = '\0';
    if (parent_mutation_error >= 0 &&
            mode == PARENT_MUTATION_FAILED_WRITE) {
        int length = snprintf(
                replacement_temporary_name,
                sizeof(replacement_temporary_name),
                "/etc/apk/%s",
                temporary_name);
        if (length < 0 ||
                length >= (int) sizeof(replacement_temporary_name))
            parent_mutation_error = _ENAMETOOLONG;
        else {
            static const char collision[] = "replacement-temp";
            parent_mutation_error = write_guest_file(
                    replacement_temporary_name,
                    collision,
                    sizeof(collision) - 1,
                    O_CREAT_ | O_EXCL_);
        }
    }
}

static int restore_repositories_parent(void) {
    if (replacement_temporary_name[0] != '\0')
        (void) generic_unlinkat(
                AT_PWD, replacement_temporary_name);
    int error = generic_unlinkat(
            AT_PWD, "/etc/apk/repositories");
    if (error >= 0)
        error = generic_rmdirat(AT_PWD, "/etc/apk");
    if (error >= 0)
        error = generic_renameat(
                AT_PWD, "/etc/apk-original",
                AT_PWD, "/etc/apk");
    replacement_temporary_name[0] = '\0';
    return error;
}

static bool create_fixture_storage(struct fixture *fixture) {
    strcpy(
            fixture->root_directory,
            "/tmp/ish-watch-guest-files-XXXXXX");
    if (mkdtemp(fixture->root_directory) == NULL)
        return false;
    if (snprintf(
            fixture->data_directory,
            sizeof(fixture->data_directory),
            "%s/data",
            fixture->root_directory) >=
            (int) sizeof(fixture->data_directory) ||
            mkdir(fixture->data_directory, 0700) < 0)
        return false;

    char path[PATH_MAX];
    static const char *const directories[] = {
        "etc", "etc/apk",
    };
    for (size_t index = 0;
            index < sizeof(directories) / sizeof(directories[0]);
            index++) {
        if (snprintf(
                path,
                sizeof(path),
                "%s/%s",
                fixture->data_directory,
                directories[index]) >= (int) sizeof(path) ||
                mkdir(path, 0700) < 0)
            return false;
    }

    static const char initial_repositories[] =
            "https://initial.example/alpine\n";
    if (snprintf(
            path,
            sizeof(path),
            "%s/etc/apk/repositories",
            fixture->data_directory) >= (int) sizeof(path) ||
            !write_host_file(
                    path,
                    initial_repositories,
                    sizeof(initial_repositories) - 1))
        return false;

    char database_path[PATH_MAX];
    if (snprintf(
            database_path,
            sizeof(database_path),
            "%s/meta.db",
            fixture->root_directory) >= (int) sizeof(database_path))
        return false;

    sqlite3 *database = NULL;
    if (sqlite3_open_v2(
            database_path,
            &database,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
            NULL) != SQLITE_OK) {
        sqlite3_close(database);
        return false;
    }
    static const char schema[] =
            "create table meta (id integer unique default 0, db_inode integer);"
            "insert into meta (db_inode) values (0);"
            "create table stats (inode integer primary key, stat blob);"
            "create table paths (path blob primary key, "
                    "inode integer references stats(inode));"
            "create index inode_to_path on paths (inode, path);"
            "pragma user_version=3;";
    bool created = sqlite3_exec(
            database, schema, NULL, NULL, NULL) == SQLITE_OK;
    static const struct {
        const char *path;
        uint32_t mode;
    } entries[] = {
        {"", 0040755},
        {"/etc", 0040755},
        {"/etc/apk", 0040755},
        {"/etc/apk/repositories", 0100644},
    };
    for (size_t index = 0;
            created && index < sizeof(entries) / sizeof(entries[0]);
            index++)
        created = insert_fakefs_entry(
                database,
                (sqlite3_int64) index + 1,
                entries[index].path,
                entries[index].mode);

    struct stat database_stat;
    if (created && stat(database_path, &database_stat) < 0)
        created = false;
    sqlite3_stmt *statement = NULL;
    if (created && sqlite3_prepare_v2(
            database,
            "update meta set db_inode = ?",
            -1,
            &statement,
            NULL) != SQLITE_OK)
        created = false;
    if (created && sqlite3_bind_int64(
            statement, 1, (sqlite3_int64) database_stat.st_ino) !=
            SQLITE_OK)
        created = false;
    if (created && sqlite3_step(statement) != SQLITE_DONE)
        created = false;
    sqlite3_finalize(statement);
    if (sqlite3_close(database) != SQLITE_OK)
        created = false;
    return created;
}

static bool fixture_init(struct fixture *fixture) {
    memset(fixture, 0, sizeof(*fixture));
    if (!create_fixture_storage(fixture))
        return false;

    current = &fixture->task;
    fixture->task.euid = 0;
    fixture->task.egid = 0;
    fixture->task.fs = fs_info_new();
    if (fixture->task.fs == NULL)
        return false;
    if (mount_root(&fakefs, fixture->data_directory) < 0)
        return false;
    fixture->mounted = true;
    return true;
}

static void remove_if_present(const char *path) {
    if (unlink(path) < 0)
        (void) path;
}

static void fixture_destroy(struct fixture *fixture) {
    current = &fixture->task;
    if (fixture->task.fs != NULL)
        fs_info_release(fixture->task.fs);
    if (fixture->mounted) {
        lock(&mounts_lock);
        (void) do_umount("");
        unlock(&mounts_lock);
    }

    char path[PATH_MAX];
    static const char *const files[] = {
        "data/etc/apk/repositories",
        "data/etc/apk-original/repositories",
        "data/ish/apk-version",
    };
    for (size_t index = 0;
            index < sizeof(files) / sizeof(files[0]);
            index++) {
        if (snprintf(
                path,
                sizeof(path),
                "%s/%s",
                fixture->root_directory,
                files[index]) < (int) sizeof(path))
            remove_if_present(path);
    }
    static const char *const directories[] = {
        "data/ish", "data/etc/apk", "data/etc/apk-original",
        "data/etc", "data",
    };
    for (size_t index = 0;
            index < sizeof(directories) / sizeof(directories[0]);
            index++) {
        if (snprintf(
                path,
                sizeof(path),
                "%s/%s",
                fixture->root_directory,
                directories[index]) < (int) sizeof(path))
            (void) rmdir(path);
    }
    static const char *const database_suffixes[] = {
        "meta.db", "meta.db-wal", "meta.db-shm", "meta.db-journal",
    };
    for (size_t index = 0;
            index < sizeof(database_suffixes) /
                    sizeof(database_suffixes[0]);
            index++) {
        if (snprintf(
                path,
                sizeof(path),
                "%s/%s",
                fixture->root_directory,
                database_suffixes[index]) < (int) sizeof(path))
            remove_if_present(path);
    }
    (void) rmdir(fixture->root_directory);
    current = NULL;
}

static bool directory_has_temporary_file(const char *path) {
    DIR *directory = opendir(path);
    if (directory == NULL)
        return true;
    bool found = false;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (strncmp(entry->d_name, ".ish-", 5) == 0) {
            found = true;
            break;
        }
    }
    closedir(directory);
    return found;
}

static void check_contents(
        int32_t file_id,
        const void *expected,
        size_t expected_length,
        const char *message) {
    unsigned char buffer[256] = {0};
    ssize_t length = ish_watch_guest_file_test_read_current(
            file_id, buffer, sizeof(buffer));
    CHECK(length == (ssize_t) expected_length &&
            memcmp(buffer, expected, expected_length) == 0,
            message);
}

static int write_oversized_repositories(void) {
    struct fd *file = generic_open(
            "/etc/apk/repositories",
            O_WRONLY_ | O_TRUNC_,
            0);
    if (IS_ERR(file))
        return (int) PTR_ERR(file);

    unsigned char block[4096];
    memset(block, 'x', sizeof(block));
    size_t remaining = ISH_WATCH_REPOSITORIES_LIMIT + 1;
    int error = 0;
    while (remaining != 0) {
        size_t chunk = remaining < sizeof(block) ?
                remaining : sizeof(block);
        ssize_t written = file_write_fd(file, block, chunk);
        if (written <= 0) {
            error = written < 0 ? (int) written : _EIO;
            break;
        }
        remaining -= (size_t) written;
    }
    if (error >= 0)
        error = file_sync_fd(file, false);
    int close_error = fd_close(file);
    if (error >= 0 && close_error < 0)
        error = close_error;
    return error;
}

static void test_public_boundaries(void) {
    static const char byte = 'x';
    CHECK(ish_watch_guest_file_read(0, NULL, 0) == _EINVAL,
            "拒绝未知读文件 ID");
    CHECK(ish_watch_guest_file_read(
            ISH_WATCH_GUEST_FILE_REPOSITORIES, NULL, 1) == _EINVAL,
            "拒绝非零容量的空读缓冲");
    CHECK(ish_watch_guest_file_replace(
            0, NULL, 0, 1) == _EINVAL,
            "拒绝未知替换文件 ID");
    CHECK(ish_watch_guest_file_replace(
            ISH_WATCH_GUEST_FILE_REPOSITORIES,
            &byte,
            1,
            2) == _EINVAL,
            "拒绝非法删除标志");
    CHECK(ish_watch_guest_file_replace(
            ISH_WATCH_GUEST_FILE_REPOSITORIES,
            &byte,
            1,
            1) == _EINVAL,
            "删除不接受内容");
    CHECK(ish_watch_guest_file_replace(
            ISH_WATCH_GUEST_FILE_REPOSITORIES,
            NULL,
            1,
            0) == _EINVAL,
            "非空写入必须提供缓冲");
    CHECK(ish_watch_guest_file_replace(
            ISH_WATCH_GUEST_FILE_REPOSITORIES,
            &byte,
            ISH_WATCH_REPOSITORIES_LIMIT + 1,
            0) == _E2BIG,
            "边界先拒绝超长仓库内容");
    CHECK(ish_watch_guest_file_read(
            ISH_WATCH_GUEST_FILE_REPOSITORIES, NULL, 0) == _EAGAIN,
            "runtime 未运行时读取返回 EAGAIN");
    CHECK(ish_watch_guest_file_replace(
            ISH_WATCH_GUEST_FILE_REPOSITORIES,
            NULL,
            0,
            0) == _EAGAIN,
            "runtime 未运行时替换返回 EAGAIN");
}

static void test_real_fakefs(struct fixture *fixture) {
    static const char initial[] =
            "https://initial.example/alpine\n";
    check_contents(
            ISH_WATCH_GUEST_FILE_REPOSITORIES,
            initial,
            sizeof(initial) - 1,
            "通过 fakefs 读取初始仓库");

    unsigned char unchanged[sizeof(initial)];
    memset(unchanged, 0xa5, sizeof(unchanged));
    CHECK(ish_watch_guest_file_test_read_current(
            ISH_WATCH_GUEST_FILE_REPOSITORIES,
            unchanged,
            sizeof(initial) - 2) == _E2BIG,
            "容量不足返回 E2BIG");
    CHECK(unchanged[0] == 0xa5,
            "容量不足不写入调用方缓冲");
    CHECK(ish_watch_guest_file_test_read_current(
            ISH_WATCH_GUEST_FILE_REPOSITORIES,
            NULL,
            0) == _E2BIG,
            "零容量读取非空文件返回 E2BIG");
    CHECK(ish_watch_guest_file_test_read_current(
            ISH_WATCH_GUEST_FILE_APK_VERSION,
            NULL,
            0) == _ENOENT,
            "缺少版本标记精确返回 ENOENT");

    static const char partial_write[] =
            "https://partial.example/v3.24/main\n";
    ish_watch_guest_file_test_set_write_behavior(2, 0, 0);
    CHECK(ish_watch_guest_file_test_replace_current(
            ISH_WATCH_GUEST_FILE_REPOSITORIES,
            partial_write,
            sizeof(partial_write) - 1,
            0) == 0,
            "短写循环发布完整仓库内容");
    ish_watch_guest_file_test_set_write_behavior(0, 0, 0);
    check_contents(
            ISH_WATCH_GUEST_FILE_REPOSITORIES,
            partial_write,
            sizeof(partial_write) - 1,
            "短写结果没有截断");

    static const char rejected[] =
            "https://must-not-publish.example\n";
    ish_watch_guest_file_test_set_write_behavior(
            2, 3, _ENOSPC);
    CHECK(ish_watch_guest_file_test_replace_current(
            ISH_WATCH_GUEST_FILE_REPOSITORIES,
            rejected,
            sizeof(rejected) - 1,
            0) == _ENOSPC,
            "中途写失败保留原 errno");
    ish_watch_guest_file_test_set_write_behavior(0, 0, 0);
    check_contents(
            ISH_WATCH_GUEST_FILE_REPOSITORIES,
            partial_write,
            sizeof(partial_write) - 1,
            "中途写失败不替换目标");

    char apk_directory[PATH_MAX];
    CHECK(snprintf(
            apk_directory,
            sizeof(apk_directory),
            "%s/etc/apk",
            fixture->data_directory) < (int) sizeof(apk_directory) &&
            !directory_has_temporary_file(apk_directory),
            "中途写失败清理仓库临时文件");

    static const char marker[] = "32400\n";
    ish_watch_guest_file_test_set_write_behavior(
            1, 1, _ENOSPC);
    CHECK(ish_watch_guest_file_test_replace_current(
            ISH_WATCH_GUEST_FILE_APK_VERSION,
            marker,
            sizeof(marker) - 1,
            0) == _ENOSPC,
            "版本标记中途失败");
    ish_watch_guest_file_test_set_write_behavior(0, 0, 0);
    CHECK(ish_watch_guest_file_test_read_current(
            ISH_WATCH_GUEST_FILE_APK_VERSION,
            NULL,
            0) == _ENOENT,
            "失败的版本标记没有发布");
    struct statbuf ish_directory = {};
    CHECK(generic_statat(
            AT_PWD, "/ish", &ish_directory, true) == 0 &&
            S_ISDIR(ish_directory.mode),
            "首次版本标记失败保留同一身份的空 /ish");
    char ish_host_directory[PATH_MAX];
    CHECK(snprintf(
            ish_host_directory,
            sizeof(ish_host_directory),
            "%s/ish",
            fixture->data_directory) < (int) sizeof(ish_host_directory) &&
            access(ish_host_directory, F_OK) == 0 &&
            !directory_has_temporary_file(ish_host_directory),
            "版本标记失败清理自己的临时文件");

    CHECK(ish_watch_guest_file_test_replace_current(
            ISH_WATCH_GUEST_FILE_APK_VERSION,
            marker,
            sizeof(marker) - 1,
            0) == 0,
            "创建版本标记");
    memset(&ish_directory, 0, sizeof(ish_directory));
    CHECK(generic_statat(
            AT_PWD, "/ish", &ish_directory, true) == 0 &&
            S_ISDIR(ish_directory.mode),
            "版本标记写入通过 fakefs 创建 /ish 元数据");
    check_contents(
            ISH_WATCH_GUEST_FILE_APK_VERSION,
            marker,
            sizeof(marker) - 1,
            "读取版本标记");
    static const char rejected_marker[] = "99999\n";
    ish_watch_guest_file_test_set_write_behavior(
            1, 1, _ENOSPC);
    CHECK(ish_watch_guest_file_test_replace_current(
            ISH_WATCH_GUEST_FILE_APK_VERSION,
            rejected_marker,
            sizeof(rejected_marker) - 1,
            0) == _ENOSPC,
            "已有版本标记的替换中途失败");
    ish_watch_guest_file_test_set_write_behavior(0, 0, 0);
    check_contents(
            ISH_WATCH_GUEST_FILE_APK_VERSION,
            marker,
            sizeof(marker) - 1,
            "已有版本标记写失败后保持原内容");
    CHECK(!directory_has_temporary_file(ish_host_directory),
            "已有版本标记写失败清理临时文件");
    CHECK(ish_watch_guest_file_test_replace_current(
            ISH_WATCH_GUEST_FILE_APK_VERSION,
            NULL,
            0,
            1) == 0,
            "删除版本标记");
    CHECK(ish_watch_guest_file_test_read_current(
            ISH_WATCH_GUEST_FILE_APK_VERSION,
            NULL,
            0) == _ENOENT,
            "删除后版本标记不存在");
    CHECK(ish_watch_guest_file_test_replace_current(
            ISH_WATCH_GUEST_FILE_APK_VERSION,
            NULL,
            0,
            1) == _ENOENT,
            "重复删除精确返回 ENOENT");

    static const char fixed_parent_publish[] =
            "https://fixed-parent-publish.example\n";
    static const char replacement_parent[] =
            "https://replacement-parent.example\n";
    parent_mutation_error = 0;
    parent_mutation_mode = PARENT_MUTATION_PUBLISH;
    ish_watch_guest_file_test_set_event_hook(
            mutate_repositories_parent);
    CHECK(ish_watch_guest_file_test_replace_current(
            ISH_WATCH_GUEST_FILE_REPOSITORIES,
            fixed_parent_publish,
            sizeof(fixed_parent_publish) - 1,
            0) == 0 &&
            parent_mutation_error == 0,
            "父目录被改名替换后仍在原目录身份发布");
    ish_watch_guest_file_test_set_event_hook(NULL);
    CHECK(guest_file_matches(
            "/etc/apk/repositories",
            replacement_parent,
            sizeof(replacement_parent) - 1),
            "并发替换目录中的同名目标保持不变");
    CHECK(guest_file_matches(
            "/etc/apk-original/repositories",
            fixed_parent_publish,
            sizeof(fixed_parent_publish) - 1),
            "发布结果跟随已改名的原目录身份");
    char original_apk_directory[PATH_MAX];
    CHECK(snprintf(
            original_apk_directory,
            sizeof(original_apk_directory),
            "%s/etc/apk-original",
            fixture->data_directory) <
            (int) sizeof(original_apk_directory) &&
            !directory_has_temporary_file(original_apk_directory),
            "固定目录发布后不留下临时文件");
    CHECK(restore_repositories_parent() == 0,
            "恢复发布测试的仓库父目录");
    check_contents(
            ISH_WATCH_GUEST_FILE_REPOSITORIES,
            fixed_parent_publish,
            sizeof(fixed_parent_publish) - 1,
            "恢复后保留原目录身份中的发布结果");

    static const char failed_parent_write[] =
            "https://fixed-parent-failure.example\n";
    parent_mutation_error = 0;
    parent_mutation_mode = PARENT_MUTATION_FAILED_WRITE;
    ish_watch_guest_file_test_set_event_hook(
            mutate_repositories_parent);
    ish_watch_guest_file_test_set_write_behavior(
            2, 3, _ENOSPC);
    CHECK(ish_watch_guest_file_test_replace_current(
            ISH_WATCH_GUEST_FILE_REPOSITORIES,
            failed_parent_write,
            sizeof(failed_parent_write) - 1,
            0) == _ENOSPC &&
            parent_mutation_error == 0,
            "父目录被替换后的失败写保持原错误");
    ish_watch_guest_file_test_set_write_behavior(0, 0, 0);
    ish_watch_guest_file_test_set_event_hook(NULL);
    CHECK(guest_file_matches(
            "/etc/apk/repositories",
            replacement_parent,
            sizeof(replacement_parent) - 1),
            "失败清理不修改并发替换目录的目标");
    static const char replacement_collision[] = "replacement-temp";
    CHECK(replacement_temporary_name[0] != '\0' &&
            guest_file_matches(
                    replacement_temporary_name,
                    replacement_collision,
                    sizeof(replacement_collision) - 1),
            "失败清理不误删替换目录中的同名临时文件");
    CHECK(!directory_has_temporary_file(original_apk_directory),
            "失败清理只删除原目录身份中的自有临时文件");
    CHECK(guest_file_matches(
            "/etc/apk-original/repositories",
            fixed_parent_publish,
            sizeof(fixed_parent_publish) - 1),
            "失败写不替换原目录身份中的目标");
    CHECK(restore_repositories_parent() == 0,
            "恢复失败写测试的仓库父目录");

    static const char stale_path[] =
            "/etc/apk/.ish-repositories.tmp.crash-residue";
    static const char reserved_directory[] =
            "/etc/apk/.ish-repositories.tmp.keep-directory";
    static const char stale[] = "stale";
    CHECK(write_guest_file(
            stale_path,
            stale,
            sizeof(stale) - 1,
            O_CREAT_ | O_EXCL_) == 0 &&
            generic_mkdirat(
                    AT_PWD, reserved_directory, 0755) == 0,
            "构造桥接临时文件崩溃残留与保守跳过目录");
    CHECK(ish_watch_guest_file_test_replace_current(
            ISH_WATCH_GUEST_FILE_REPOSITORIES,
            fixed_parent_publish,
            sizeof(fixed_parent_publish) - 1,
            0) == 0,
            "存在临时文件残留时仍能发布");
    struct statbuf stale_stat = {};
    struct statbuf directory_stat = {};
    CHECK(generic_statat(
            AT_PWD, stale_path, &stale_stat, false) == _ENOENT,
            "下一次操作有界清理同前缀常规文件残留");
    CHECK(generic_statat(
            AT_PWD,
            reserved_directory,
            &directory_stat,
            false) == 0 &&
            S_ISDIR(directory_stat.mode),
            "前缀清理不删除非临时文件类型");
    CHECK(generic_rmdirat(
            AT_PWD, reserved_directory) == 0,
            "清理测试保留的同前缀目录");

    CHECK(write_oversized_repositories() == 0,
            "构造超限 fakefs 文件");
    unsigned char byte;
    CHECK(ish_watch_guest_file_test_read_current(
            ISH_WATCH_GUEST_FILE_REPOSITORIES,
            &byte,
            sizeof(byte)) == _EFBIG,
            "读取超限 guest 文件返回 EFBIG");
}

int main(void) {
    test_public_boundaries();

    struct fixture fixture;
    if (!fixture_init(&fixture)) {
        fprintf(stderr, "Watch guest 文件测试失败：创建 fakefs fixture\n");
        fixture_destroy(&fixture);
        return 1;
    }
    test_real_fakefs(&fixture);
    fixture_destroy(&fixture);

    if (failures != 0) {
        fprintf(stderr, "Watch guest 文件测试共 %d 项失败\n", failures);
        return 1;
    }
    puts("Watch guest 文件测试通过");
    return 0;
}
