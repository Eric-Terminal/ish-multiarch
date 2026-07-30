#include "platform/apple-watch-runtime.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "fs/real.h"
#include "kernel/errno.h"
#include "kernel/task.h"

#define TEST_OUTPUT_CAPACITY (64 * 1024)

static int failures;

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "失败：%s\n", message); \
        failures++; \
    } \
} while (0)

enum directory_hook_mode {
    DIRECTORY_HOOK_OBSERVE,
    DIRECTORY_HOOK_REPLACE_AFTER_LSTAT,
    DIRECTORY_HOOK_REPLACE_AFTER_VALIDATION,
};

static struct {
    enum directory_hook_mode mode;
    char displaced_path[PATH_MAX];
    bool mutation_succeeded;
    unsigned lstat_count;
    unsigned validation_count;
    unsigned release_count;
    bool released_fd_was_closed;
} directory_hook_state;

static bool write_host_marker(const char *directory, const char *name) {
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/%s", directory, name) >=
            (int) sizeof(path))
        return false;
    FILE *file = fopen(path, "wb");
    if (file == NULL)
        return false;
    static const char contents[] = "marker\n";
    bool wrote = fwrite(
            contents, 1, sizeof(contents) - 1, file) ==
            sizeof(contents) - 1;
    return fclose(file) == 0 && wrote;
}

static bool replace_test_directory(const char *path) {
    if (rename(path, directory_hook_state.displaced_path) < 0)
        return false;
    if (mkdir(path, 0700) < 0) {
        (void) rename(directory_hook_state.displaced_path, path);
        return false;
    }
    return write_host_marker(path, "replacement-only");
}

static void directory_test_hook(
        int32_t stage, const char *path, int directory_fd) {
    if (stage == ISH_WATCH_RUNTIME_TEST_DIRECTORY_AFTER_LSTAT) {
        directory_hook_state.lstat_count++;
        if (directory_hook_state.mode ==
                DIRECTORY_HOOK_REPLACE_AFTER_LSTAT)
            directory_hook_state.mutation_succeeded =
                    replace_test_directory(path);
        return;
    }
    if (stage == ISH_WATCH_RUNTIME_TEST_DIRECTORY_AFTER_VALIDATION) {
        directory_hook_state.validation_count++;
        if (directory_hook_state.mode ==
                DIRECTORY_HOOK_REPLACE_AFTER_VALIDATION)
            directory_hook_state.mutation_succeeded =
                    replace_test_directory(path);
        return;
    }
    if (stage == ISH_WATCH_RUNTIME_TEST_DIRECTORY_AFTER_RELEASE) {
        directory_hook_state.release_count++;
        errno = 0;
        directory_hook_state.released_fd_was_closed =
                fcntl(directory_fd, F_GETFD) < 0 && errno == EBADF;
    }
}

static void reset_directory_hook(
        enum directory_hook_mode mode, const char *displaced_path) {
    memset(&directory_hook_state, 0, sizeof(directory_hook_state));
    directory_hook_state.mode = mode;
    if (displaced_path != NULL)
        CHECK(snprintf(
                directory_hook_state.displaced_path,
                sizeof(directory_hook_state.displaced_path),
                "%s",
                displaced_path) <
                (int) sizeof(directory_hook_state.displaced_path),
                "记录目录替换 fixture 路径");
    ish_watch_runtime_test_set_directory_hook(directory_test_hook);
}

static void disable_directory_hook(void) {
    ish_watch_runtime_test_set_directory_hook(NULL);
}

static int count_open_file_descriptors(void) {
    int count = 0;
    int limit = getdtablesize();
    for (int descriptor = 0; descriptor < limit; descriptor++) {
        errno = 0;
        if (fcntl(descriptor, F_GETFD) >= 0 || errno != EBADF)
            count++;
    }
    return count;
}

static bool insert_fakefs_entry(
        sqlite3 *database, sqlite3_int64 inode,
        const char *path, uint32_t mode) {
    const uint32_t stat[4] = {mode, 0, 0, 0};
    sqlite3_stmt *statement = NULL;
    int error = sqlite3_prepare_v2(database,
            "insert into stats (inode, stat) values (?, ?)",
            -1, &statement, NULL);
    if (error != SQLITE_OK)
        return false;

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
    error = sqlite3_prepare_v2(database,
            "insert into paths (path, inode) values (?, ?)",
            -1, &statement, NULL);
    if (error == SQLITE_OK)
        error = sqlite3_bind_blob(
                statement, 1, path, (int) strlen(path), SQLITE_STATIC);
    if (error == SQLITE_OK)
        error = sqlite3_bind_int64(statement, 2, inode);
    if (error == SQLITE_OK)
        error = sqlite3_step(statement);
    bool inserted_path = error == SQLITE_DONE;
    sqlite3_finalize(statement);
    return inserted_path;
}

static bool create_boot_failure_root(
        char root[PATH_MAX],
        char data[PATH_MAX],
        char documents[PATH_MAX]) {
    strcpy(root, "/tmp/ish-watch-boot-failure-XXXXXX");
    if (mkdtemp(root) == NULL ||
            snprintf(data, PATH_MAX, "%s/data", root) >= PATH_MAX ||
            snprintf(
                    documents,
                    PATH_MAX,
                    "%s/documents",
                    root) >= PATH_MAX ||
            mkdir(data, 0700) < 0 ||
            mkdir(documents, 0700) < 0)
        return false;

    char path[PATH_MAX];
    static const char *const directories[] = {
        "dev", "dev/pts", "proc", "bin",
    };
    for (size_t index = 0;
            index < sizeof(directories) / sizeof(directories[0]);
            index++) {
        if (snprintf(path, sizeof(path), "%s/%s",
                    data, directories[index]) >= (int) sizeof(path) ||
                mkdir(path, 0700) < 0)
            return false;
    }
    if (snprintf(path, sizeof(path), "%s/bin/sh", data) >=
            (int) sizeof(path))
        return false;
    FILE *shell = fopen(path, "wb");
    if (shell == NULL)
        return false;
    bool wrote_shell = fwrite(
            "不是 ELF\n", 1, sizeof("不是 ELF\n") - 1, shell) ==
            sizeof("不是 ELF\n") - 1;
    if (fclose(shell) != 0 || !wrote_shell)
        return false;

    if (snprintf(path, sizeof(path), "%s/meta.db", root) >=
            (int) sizeof(path))
        return false;
    sqlite3 *database = NULL;
    if (sqlite3_open_v2(path, &database,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) != SQLITE_OK) {
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
        {"/dev", 0040755},
        {"/dev/pts", 0040755},
        {"/proc", 0040755},
        {"/bin", 0040755},
        {"/bin/sh", 0100755},
    };
    for (size_t index = 0;
            created && index < sizeof(entries) / sizeof(entries[0]);
            index++)
        created = insert_fakefs_entry(
                database, (sqlite3_int64) index + 1,
                entries[index].path, entries[index].mode);
    if (sqlite3_close(database) != SQLITE_OK)
        created = false;
    return created;
}

enum shared_mountpoint_fixture {
    SHARED_MOUNTPOINT_MNT_FILE,
    SHARED_MOUNTPOINT_SHARED_SYMLINK,
};

static bool configure_shared_mountpoint_fixture(
        const char *root,
        const char *data,
        enum shared_mountpoint_fixture fixture) {
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/mnt", data) >=
            (int) sizeof(path))
        return false;

    if (fixture == SHARED_MOUNTPOINT_MNT_FILE) {
        FILE *file = fopen(path, "wb");
        if (file == NULL || fclose(file) != 0)
            return false;
    } else {
        if (mkdir(path, 0700) < 0)
            return false;
        if (snprintf(path, sizeof(path), "%s/mnt/shared", data) >=
                (int) sizeof(path) ||
                symlink(".", path) < 0)
            return false;
    }

    char database_path[PATH_MAX];
    if (snprintf(
            database_path,
            sizeof(database_path),
            "%s/meta.db",
            root) >= (int) sizeof(database_path))
        return false;
    sqlite3 *database = NULL;
    if (sqlite3_open_v2(
            database_path,
            &database,
            SQLITE_OPEN_READWRITE,
            NULL) != SQLITE_OK) {
        sqlite3_close(database);
        return false;
    }

    bool configured;
    if (fixture == SHARED_MOUNTPOINT_MNT_FILE) {
        configured = insert_fakefs_entry(
                database, 100, "/mnt", 0100644);
    } else {
        configured =
                insert_fakefs_entry(
                        database, 100, "/mnt", 0040755) &&
                insert_fakefs_entry(
                        database, 101, "/mnt/shared", 0120777);
    }
    if (sqlite3_close(database) != SQLITE_OK)
        configured = false;
    return configured;
}

static bool shared_mountpoint_start_fails_without_pid_one(
        const char *data,
        const char *documents,
        int expected_error,
        int alternate_error) {
    pid_t child = fork();
    if (child < 0)
        return false;
    if (child == 0) {
        reset_directory_hook(DIRECTORY_HOOK_OBSERVE, NULL);
        int error = ish_watch_runtime_start(
                data,
                documents,
                "/tmp/ish-watch-mountpoint-test-",
                "Watch",
                "exec /sbin/init");
        lock(&pids_lock);
        bool pid_one_unpublished = pid_get_task_zombie(1) == NULL;
        unlock(&pids_lock);
        bool passed =
                (error == expected_error ||
                        error == alternate_error) &&
                ish_watch_runtime_current_phase() ==
                        ISH_WATCH_RUNTIME_FAILED &&
                ish_watch_runtime_last_error() == error &&
                directory_hook_state.validation_count == 1 &&
                directory_hook_state.release_count == 1 &&
                directory_hook_state.released_fd_was_closed &&
                current == NULL &&
                pid_one_unpublished;
        disable_directory_hook();
        _exit(passed ? 0 : 1);
    }

    int status;
    return waitpid(child, &status, 0) == child &&
            WIFEXITED(status) &&
            WEXITSTATUS(status) == 0;
}

static void remove_boot_failure_root(
        const char *root,
        const char *data,
        const char *documents) {
    char path[PATH_MAX];
    static const char *const device_entries[] = {
        "tty1", "tty2", "tty3", "tty4", "tty5", "tty6", "tty7",
        "tty", "console", "ptmx", "null", "zero", "full",
        "random", "urandom",
    };
    for (size_t index = 0;
            index < sizeof(device_entries) / sizeof(device_entries[0]);
            index++) {
        if (snprintf(path, sizeof(path), "%s/dev/%s",
                    data, device_entries[index]) < (int) sizeof(path))
            (void) unlink(path);
    }
    if (snprintf(path, sizeof(path), "%s/bin/sh", data) <
            (int) sizeof(path))
        (void) unlink(path);
    if (snprintf(path, sizeof(path), "%s/mnt/shared", data) <
            (int) sizeof(path))
        (void) unlink(path);
    if (snprintf(path, sizeof(path), "%s/mnt", data) <
            (int) sizeof(path))
        (void) unlink(path);
    static const char *const directories[] = {
        "dev/pts", "dev", "proc", "bin", "mnt/shared", "mnt",
    };
    for (size_t index = 0;
            index < sizeof(directories) / sizeof(directories[0]);
            index++) {
        if (snprintf(path, sizeof(path), "%s/%s",
                    data, directories[index]) < (int) sizeof(path))
            (void) rmdir(path);
    }
    (void) rmdir(data);
    (void) rmdir(documents);
    static const char *const database_suffixes[] = {
        "meta.db", "meta.db-wal", "meta.db-shm", "meta.db-journal",
    };
    for (size_t index = 0;
            index < sizeof(database_suffixes) /
                    sizeof(database_suffixes[0]);
            index++) {
        if (snprintf(path, sizeof(path), "%s/%s",
                    root, database_suffixes[index]) < (int) sizeof(path))
            (void) unlink(path);
    }
    (void) rmdir(root);
}

static bool test_shared_mountpoint_failure(
        enum shared_mountpoint_fixture fixture,
        int expected_error,
        int alternate_error) {
    char root[PATH_MAX];
    char data[PATH_MAX];
    char documents[PATH_MAX];
    bool created = create_boot_failure_root(
            root, data, documents);
    if (!created)
        return false;

    bool passed =
            configure_shared_mountpoint_fixture(
                    root, data, fixture) &&
            shared_mountpoint_start_fails_without_pid_one(
                    data,
                    documents,
                    expected_error,
                    alternate_error);
    remove_boot_failure_root(root, data, documents);
    return passed;
}

static void remove_directory_marker(
        const char *directory, const char *name) {
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/%s", directory, name) <
            (int) sizeof(path))
        (void) unlink(path);
}

static bool test_directory_replacement_during_validation(void) {
    char root[] = "/tmp/ish-watch-directory-validation-XXXXXX";
    if (mkdtemp(root) == NULL)
        return false;

    char documents[PATH_MAX];
    char displaced[PATH_MAX];
    bool paths_fit =
            snprintf(
                    documents,
                    sizeof(documents),
                    "%s/shared",
                    root) < (int) sizeof(documents) &&
            snprintf(
                    displaced,
                    sizeof(displaced),
                    "%s/shared-original",
                    root) < (int) sizeof(displaced);
    if (!paths_fit || mkdir(documents, 0700) < 0) {
        (void) rmdir(root);
        return false;
    }

    pid_t child = fork();
    if (child == 0) {
        reset_directory_hook(
                DIRECTORY_HOOK_REPLACE_AFTER_LSTAT, displaced);
        int descriptors_before = count_open_file_descriptors();
        int error = ish_watch_runtime_start(
                "/tmp",
                documents,
                "/tmp/ish-watch-validation-test-",
                "Watch",
                "exec /sbin/init");
        int descriptors_after = count_open_file_descriptors();
        bool passed =
                error == _ESTALE &&
                ish_watch_runtime_current_phase() ==
                        ISH_WATCH_RUNTIME_IDLE &&
                ish_watch_runtime_last_error() == 0 &&
                directory_hook_state.mutation_succeeded &&
                directory_hook_state.lstat_count == 1 &&
                directory_hook_state.validation_count == 0 &&
                directory_hook_state.release_count == 1 &&
                directory_hook_state.released_fd_was_closed &&
                descriptors_after == descriptors_before;
        disable_directory_hook();
        _exit(passed ? 0 : 1);
    }

    int status = 0;
    bool passed =
            child > 0 &&
            waitpid(child, &status, 0) == child &&
            WIFEXITED(status) &&
            WEXITSTATUS(status) == 0;
    remove_directory_marker(documents, "replacement-only");
    (void) rmdir(documents);
    (void) rmdir(displaced);
    (void) rmdir(root);
    return passed;
}

static bool test_validated_replacement_mounts_open_directory(void) {
    char root[PATH_MAX];
    char data[PATH_MAX];
    char documents[PATH_MAX];
    if (!create_boot_failure_root(root, data, documents))
        return false;

    char displaced[PATH_MAX];
    bool fixture_ready =
            snprintf(
                    displaced,
                    sizeof(displaced),
                    "%s-original",
                    documents) < (int) sizeof(displaced) &&
            write_host_marker(documents, "original-only");
    if (!fixture_ready) {
        remove_directory_marker(documents, "original-only");
        remove_boot_failure_root(root, data, documents);
        return false;
    }

    pid_t child = fork();
    if (child == 0) {
        char expected_source[PATH_MAX];
        bool source_resolved =
                realpath(documents, expected_source) != NULL;
        reset_directory_hook(
                DIRECTORY_HOOK_REPLACE_AFTER_VALIDATION, displaced);
        int error = ish_watch_runtime_start(
                data,
                documents,
                "/tmp/ish-watch-stable-directory-test-",
                "Watch",
                "exec /sbin/init");

        bool mounted_original = false;
        if (error == _ENOEXEC) {
            char mount_point[] = "/mnt/shared";
            struct mount *mount = mount_find(mount_point);
            struct stat mounted_status;
            struct stat original_status;
            struct stat replacement_status;
            int original_marker = openat(
                    mount->root_fd, "original-only", O_RDONLY);
            int replacement_marker = openat(
                    mount->root_fd, "replacement-only", O_RDONLY);
            mounted_original =
                    source_resolved &&
                    strcmp(mount->point, "/mnt/shared") == 0 &&
                    mount->fs == &realfs &&
                    strcmp(mount->source, expected_source) == 0 &&
                    fstat(mount->root_fd, &mounted_status) == 0 &&
                    lstat(displaced, &original_status) == 0 &&
                    lstat(documents, &replacement_status) == 0 &&
                    mounted_status.st_dev == original_status.st_dev &&
                    mounted_status.st_ino == original_status.st_ino &&
                    (mounted_status.st_dev != replacement_status.st_dev ||
                            mounted_status.st_ino !=
                                    replacement_status.st_ino) &&
                    original_marker >= 0 &&
                    replacement_marker < 0;
            if (original_marker >= 0)
                (void) close(original_marker);
            if (replacement_marker >= 0)
                (void) close(replacement_marker);
            mount_release(mount);
        }

        lock(&pids_lock);
        bool pid_one_unpublished = pid_get_task_zombie(1) == NULL;
        unlock(&pids_lock);
        bool passed =
                error == _ENOEXEC &&
                ish_watch_runtime_current_phase() ==
                        ISH_WATCH_RUNTIME_FAILED &&
                ish_watch_runtime_last_error() == error &&
                directory_hook_state.mutation_succeeded &&
                directory_hook_state.validation_count == 1 &&
                directory_hook_state.release_count == 1 &&
                directory_hook_state.released_fd_was_closed &&
                mounted_original &&
                current == NULL &&
                pid_one_unpublished;
        disable_directory_hook();
        _exit(passed ? 0 : 1);
    }

    int status = 0;
    bool passed =
            child > 0 &&
            waitpid(child, &status, 0) == child &&
            WIFEXITED(status) &&
            WEXITSTATUS(status) == 0;
    remove_directory_marker(documents, "replacement-only");
    (void) rmdir(documents);
    remove_directory_marker(displaced, "original-only");
    (void) rmdir(displaced);
    remove_boot_failure_root(root, data, documents);
    return passed;
}

static void check_idle(const char *message) {
    CHECK(ish_watch_runtime_current_phase() == ISH_WATCH_RUNTIME_IDLE,
            message);
    CHECK(ish_watch_runtime_last_error() == 0,
            "参数校验失败不应记录 runtime 错误");
}

static void test_output_overflow(void) {
    const size_t extra = 7;
    const size_t source_length = TEST_OUTPUT_CAPACITY + extra;
    unsigned char *source = malloc(source_length);
    unsigned char *output = malloc(TEST_OUTPUT_CAPACITY);
    CHECK(source != NULL && output != NULL,
            "应能分配终端环形缓冲测试数据");
    if (source == NULL || output == NULL) {
        free(source);
        free(output);
        return;
    }

    for (size_t i = 0; i < source_length; i++)
        source[i] = (unsigned char) (i & 0xff);
    ish_watch_runtime_test_append_output(source, source_length);

    uint64_t dropped = 0;
    size_t length = ish_watch_runtime_read_output(
            output, TEST_OUTPUT_CAPACITY, &dropped);
    CHECK(length == TEST_OUTPUT_CAPACITY,
            "终端环形缓冲应保留最新的 64 KiB 输出");
    CHECK(dropped == extra,
            "终端环形缓冲应报告精确的丢弃字节数");
    CHECK(memcmp(output, source + extra, TEST_OUTPUT_CAPACITY) == 0,
            "终端环形缓冲溢出后应保持最新字节顺序");

    ish_watch_runtime_test_append_output(
            source, TEST_OUTPUT_CAPACITY - 4);
    ish_watch_runtime_test_append_output(
            source + TEST_OUTPUT_CAPACITY - 4, 8);
    dropped = 0;
    length = ish_watch_runtime_read_output(
            output, TEST_OUTPUT_CAPACITY, &dropped);
    CHECK(length == TEST_OUTPUT_CAPACITY,
            "多次小块写入溢出后应保留完整容量");
    CHECK(dropped == 4,
            "多次小块写入应报告被覆盖的四个字节");
    CHECK(memcmp(output, source + 4, TEST_OUTPUT_CAPACITY) == 0,
            "跨环形尾部的读取应保持字节顺序");

    dropped = UINT64_MAX;
    CHECK(ish_watch_runtime_read_output(
            output, TEST_OUTPUT_CAPACITY, &dropped) == 0,
            "读取后终端环形缓冲应为空");
    CHECK(dropped == 0,
            "读取后应清零终端丢弃计数");
    free(source);
    free(output);
}

static void test_session_boundaries(void) {
    struct ish_watch_session_status status;
    unsigned char byte = 0;
    uint64_t dropped = 0;
    ish_watch_session_id session_id = UINT64_MAX;

    CHECK(ish_watch_session_create(
            NULL, 40, 18, &session_id) == _EINVAL,
            "拒绝缺失 session 命令");
    CHECK(ish_watch_session_create(
            "", 40, 18, &session_id) == _EINVAL,
            "拒绝空 session 命令");
    CHECK(ish_watch_session_create(
            "exec /bin/login -f root", 0, 18, &session_id) == _EINVAL,
            "拒绝 session 零列窗口尺寸");
    CHECK(ish_watch_session_create(
            "exec /bin/login -f root", 40, 0, &session_id) == _EINVAL,
            "拒绝 session 零行窗口尺寸");
    CHECK(ish_watch_session_create(
            "exec /bin/login -f root", 40, 18, NULL) == _EINVAL,
            "拒绝缺失 session id 输出地址");

    char long_command[4097];
    memset(long_command, 'x', sizeof(long_command) - 1);
    long_command[sizeof(long_command) - 1] = '\0';
    CHECK(ish_watch_session_create(
            long_command, 40, 18, &session_id) == _E2BIG,
            "拒绝过长 session 命令");

    session_id = UINT64_MAX;
    CHECK(ish_watch_session_create(
            "exec /bin/login -f root", 40, 18, &session_id) == _EAGAIN,
            "runtime 启动前不能派生 session");
    CHECK(session_id == 0,
            "未创建 session 时应清空输出 id");
    check_idle("session 创建边界不消耗 runtime 启动机会");

    CHECK(ish_watch_session_status(0, &status) == _ESTALE,
            "零 session id 应视为失效句柄");
    CHECK(ish_watch_session_status(1, NULL) == _EINVAL,
            "拒绝缺失 session 状态输出地址");
    CHECK(ish_watch_session_read_output(
            1, NULL, 1, &dropped) == _EINVAL,
            "拒绝缺失 session 输出缓冲");
    CHECK(ish_watch_session_read_output(
            1, &byte, (size_t) SSIZE_MAX + 1, &dropped) == _EMSGSIZE,
            "拒绝 ssize_t 无法表示的 session 读取容量");
    CHECK(ish_watch_session_read_output(
            1, &byte, sizeof(byte), &dropped) == _ESTALE,
            "拒绝读取失效 session");
    CHECK(ish_watch_session_send_input(1, NULL, 1) == _EINVAL,
            "拒绝缺失 session 输入缓冲");
    CHECK(ish_watch_session_send_input(
            1, &byte, (size_t) SSIZE_MAX + 1) == _EMSGSIZE,
            "拒绝 ssize_t 无法表示的 session 输入长度");
    CHECK(ish_watch_session_send_input(1, &byte, 1) == _ESTALE,
            "拒绝向失效 session 输入");
    CHECK(ish_watch_session_set_window_size(1, 0, 18) == _EINVAL,
            "拒绝 session 零列窗口更新");
    CHECK(ish_watch_session_set_window_size(1, 40, 0) == _EINVAL,
            "拒绝 session 零行窗口更新");
    CHECK(ish_watch_session_set_window_size(1, 40, 18) == _ESTALE,
            "拒绝更新失效 session 的窗口");
    CHECK(ish_watch_session_close(1) == _ESTALE,
            "拒绝关闭失效 session");
}

static void test_session_lifecycle(void) {
    const size_t extra = 9;
    const size_t source_length = TEST_OUTPUT_CAPACITY + extra;
    unsigned char *source = malloc(source_length);
    unsigned char *output = malloc(TEST_OUTPUT_CAPACITY);
    CHECK(source != NULL && output != NULL,
            "应能分配 session 环形缓冲测试数据");
    if (source == NULL || output == NULL) {
        free(source);
        free(output);
        return;
    }
    for (size_t index = 0; index < source_length; index++)
        source[index] = (unsigned char) (index & 0xff);

    ish_watch_session_id first;
    ish_watch_session_id second;
    CHECK(ish_watch_runtime_test_add_session(
            ISH_WATCH_SESSION_RUNNING, &first) == 0,
            "测试外观应能建立首个 session");
    CHECK(ish_watch_runtime_test_add_session(
            ISH_WATCH_SESSION_RUNNING, &second) == 0,
            "测试外观应能建立第二个 session");
    CHECK(second > first,
            "session id 应单调递增");

    ish_watch_runtime_test_append_session_output(
            first, source, source_length);
    static const char second_output[] = "second";
    ish_watch_runtime_test_append_session_output(
            second, second_output, sizeof(second_output) - 1);

    uint64_t dropped = 0;
    ssize_t length = ish_watch_session_read_output(
            first, output, TEST_OUTPUT_CAPACITY, &dropped);
    CHECK(length == TEST_OUTPUT_CAPACITY,
            "session 环形缓冲应保留最新 64 KiB 输出");
    CHECK(dropped == extra,
            "session 环形缓冲应报告精确丢弃字节数");
    CHECK(memcmp(output, source + extra, TEST_OUTPUT_CAPACITY) == 0,
            "session 环形缓冲溢出后应保持最新字节顺序");

    memset(output, 0, sizeof(second_output));
    dropped = UINT64_MAX;
    length = ish_watch_session_read_output(
            second, output, sizeof(second_output) - 1, &dropped);
    CHECK(length == (ssize_t) (sizeof(second_output) - 1) &&
            memcmp(output, second_output, sizeof(second_output) - 1) == 0,
            "不同 session 的输出缓冲应彼此独立");
    CHECK(dropped == 0,
            "未溢出的 session 不应继承其他 session 的丢弃计数");

    struct ish_watch_session_status status;
    CHECK(ish_watch_session_status(first, &status) == 0 &&
            status.phase == ISH_WATCH_SESSION_RUNNING &&
            status.wait_status == 0,
            "运行中 session 应返回稳定状态");
    CHECK(ish_watch_session_send_input(first, "x", 1) == _ESHUTDOWN,
            "runtime 未运行时 session transport 应报告关闭");
    CHECK(ish_watch_session_set_window_size(first, 40, 18) == _ESHUTDOWN,
            "runtime 未运行时 session 尺寸更新应报告关闭");

    ish_watch_runtime_test_mark_session_exited(first, 37 << 8);
    CHECK(ish_watch_session_status(first, &status) == 0 &&
            status.phase == ISH_WATCH_SESSION_EXITED &&
            status.wait_status == (37 << 8),
            "退出后应保留原始 Linux wait status");
    CHECK(ish_watch_session_send_input(first, "x", 1) == _ESHUTDOWN,
            "退出后应拒绝 session 输入");
    CHECK(ish_watch_session_close(first) == 0,
            "退出的 session 应能关闭并释放槽位");
    CHECK(ish_watch_session_status(first, &status) == _ESTALE,
            "关闭后旧 session id 应立即失效");
    CHECK(ish_watch_session_close(first) == _ESTALE,
            "重复关闭旧 session id 应报告失效");

    ish_watch_runtime_test_mark_session_exited(second, 0);
    CHECK(ish_watch_session_close(second) == 0,
            "第二个 session 应能独立释放");

    ish_watch_session_id replacement;
    CHECK(ish_watch_runtime_test_add_session(
            ISH_WATCH_SESSION_STARTING, &replacement) == 0,
            "释放槽位后应能创建替代 session");
    CHECK(replacement > second,
            "复用静态槽位时不得复用旧 session id");
    CHECK(ish_watch_session_send_input(replacement, "x", 1) == _EAGAIN,
            "启动中的 session transport 应报告稍后重试");
    ish_watch_runtime_test_mark_session_exited(replacement, 0);
    CHECK(ish_watch_session_close(replacement) == 0,
            "启动态测试 session 应能结束并释放");

    ish_watch_session_id closed_while_running;
    CHECK(ish_watch_runtime_test_add_session(
            ISH_WATCH_SESSION_RUNNING, &closed_while_running) == 0,
            "应能建立 close-before-exit 回归 session");
    CHECK(ish_watch_session_close(closed_while_running) == 0,
            "运行中的 session 应先使客户端句柄失效");
    CHECK(ish_watch_session_status(
            closed_while_running, &status) == _ESTALE,
            "close 返回后运行中句柄必须立即失效");
    ish_watch_runtime_test_mark_session_exited(closed_while_running, 0);

    ish_watch_session_id after_close_before_exit;
    CHECK(ish_watch_runtime_test_add_session(
            ISH_WATCH_SESSION_RUNNING, &after_close_before_exit) == 0,
            "close-before-exit 完成后必须释放静态槽位");
    ish_watch_runtime_test_mark_session_exited(after_close_before_exit, 0);
    CHECK(ish_watch_session_close(after_close_before_exit) == 0,
            "close-before-exit 替代 session 应能正常回收");
    CHECK(ish_watch_runtime_test_recycled_transport() == 0,
            "复用 tty 编号与静态槽位后必须拒绝旧 transport 代际");

    free(source);
    free(output);
}

static void test_session_limit(void) {
    ish_watch_session_id session_ids[ISH_WATCH_SESSION_LIMIT];
    for (size_t index = 0; index < ISH_WATCH_SESSION_LIMIT; index++) {
        CHECK(ish_watch_runtime_test_add_session(
                ISH_WATCH_SESSION_RUNNING,
                &session_ids[index]) == 0,
                "四个静态 session 槽位均应可用");
    }

    ish_watch_session_id overflow;
    CHECK(ish_watch_runtime_test_add_session(
            ISH_WATCH_SESSION_RUNNING, &overflow) == _EMFILE,
            "第五个并发 session 应报告槽位已满");

    for (size_t index = 0; index < ISH_WATCH_SESSION_LIMIT; index++) {
        ish_watch_runtime_test_mark_session_exited(
                session_ids[index], 0);
        CHECK(ish_watch_session_close(session_ids[index]) == 0,
                "容量测试结束后应释放每个 session 槽位");
    }
}

int main(void) {
    unsigned char output;
    uint64_t dropped = UINT64_MAX;
    CHECK(ish_watch_runtime_current_phase() == ISH_WATCH_RUNTIME_IDLE,
            "新进程中的 Watch runtime 应为空闲态");
    CHECK(ish_watch_runtime_last_error() == 0,
            "空闲态不应携带旧错误");
    CHECK(ish_watch_runtime_read_output(
            &output, sizeof(output), &dropped) == 0,
            "启动前没有终端输出");
    CHECK(dropped == 0,
            "启动前没有被丢弃的终端输出");
    CHECK(ish_watch_runtime_send_input("x", 1) == _EAGAIN,
            "启动前拒绝终端输入");
    CHECK(ish_watch_runtime_send_input(NULL, 1) == _EINVAL,
            "拒绝无字节地址的非空输入");
    CHECK(ish_watch_runtime_send_input(NULL, 0) == 0,
            "允许无字节地址的空输入");
    CHECK(ish_watch_runtime_send_input(
            "x", (size_t) SSIZE_MAX + 1) == _EMSGSIZE,
            "拒绝无法由 ssize_t 表示的输入长度");
    CHECK(ish_watch_runtime_set_window_size(40, 18) == _EAGAIN,
            "启动前拒绝窗口尺寸更新");
    CHECK(ish_watch_runtime_set_window_size(0, 18) == _EINVAL,
            "拒绝零列窗口尺寸");
    CHECK(ish_watch_runtime_set_window_size(40, 0) == _EINVAL,
            "拒绝零行窗口尺寸");

    test_session_boundaries();
    test_session_lifecycle();
    test_session_limit();
    CHECK(ish_watch_runtime_test_exit_ownership() == 0,
            "普通前台程序退出不得结束所属 shell 会话");
    CHECK(test_directory_replacement_during_validation(),
            "目录在 lstat 与 open 间被替换时返回 ESTALE、关闭 fd 且保持 IDLE");

    CHECK(ish_watch_runtime_start(
            NULL, "/tmp", "/tmp/ishsock", "Watch",
            "exec /sbin/init") == _EINVAL,
            "拒绝缺失 root data 路径");
    check_idle("缺失 root data 路径不消耗一次性启动机会");
    CHECK(ish_watch_runtime_start(
            "", "/tmp", "/tmp/ishsock", "Watch",
            "exec /sbin/init") == _EINVAL,
            "拒绝空 root data 路径");
    check_idle("空 root data 路径不消耗一次性启动机会");
    CHECK(ish_watch_runtime_start(
            "/tmp", NULL, "/tmp/ishsock", "Watch",
            "exec /sbin/init") == _EINVAL,
            "拒绝缺失 Documents/Shared 路径");
    check_idle("缺失 Documents/Shared 路径不消耗一次性启动机会");
    CHECK(ish_watch_runtime_start(
            "/tmp", "", "/tmp/ishsock", "Watch",
            "exec /sbin/init") == _EINVAL,
            "拒绝空 Documents/Shared 路径");
    check_idle("空 Documents/Shared 路径不消耗一次性启动机会");

    char missing_documents[PATH_MAX];
    CHECK(snprintf(
            missing_documents,
            sizeof(missing_documents),
            "/tmp/ish-watch-missing-documents-%ld",
            (long) getpid()) < (int) sizeof(missing_documents),
            "生成不存在的 Documents/Shared 路径");
    (void) unlink(missing_documents);
    (void) rmdir(missing_documents);
    CHECK(ish_watch_runtime_start(
            "/tmp", missing_documents, "/tmp/ishsock", "Watch",
            "exec /sbin/init") == _ENOENT,
            "拒绝不存在的 Documents/Shared 路径");
    check_idle("不存在的 Documents/Shared 路径不消耗一次性启动机会");

    char regular_documents[] = "/tmp/ish-watch-documents-file-XXXXXX";
    int regular_documents_fd = mkstemp(regular_documents);
    CHECK(regular_documents_fd >= 0,
            "创建非目录 Documents/Shared fixture");
    if (regular_documents_fd >= 0) {
        CHECK(ish_watch_runtime_start(
                "/tmp", regular_documents, "/tmp/ishsock", "Watch",
                "exec /sbin/init") == _ENOTDIR,
                "拒绝非目录 Documents/Shared 路径");
        check_idle("非目录 Documents/Shared 路径不消耗一次性启动机会");
        (void) close(regular_documents_fd);
        (void) unlink(regular_documents);
    }

    char link_fixture[] = "/tmp/ish-watch-documents-link-XXXXXX";
    char *link_directory = mkdtemp(link_fixture);
    CHECK(link_directory != NULL,
            "创建符号链接 Documents/Shared fixture");
    if (link_directory != NULL) {
        char link_path[PATH_MAX];
        CHECK(snprintf(
                link_path,
                sizeof(link_path),
                "%s/shared",
                link_directory) < (int) sizeof(link_path),
                "生成符号链接 Documents/Shared 路径");
        if (symlink("/tmp", link_path) == 0) {
            CHECK(ish_watch_runtime_start(
                    "/tmp", link_path, "/tmp/ishsock", "Watch",
                    "exec /sbin/init") == _ELOOP,
                    "拒绝符号链接 Documents/Shared 路径");
            check_idle(
                    "符号链接 Documents/Shared 路径不消耗一次性启动机会");
            (void) unlink(link_path);
        } else {
            CHECK(false, "创建符号链接 Documents/Shared 路径");
        }
        (void) rmdir(link_directory);
    }
    CHECK(ish_watch_runtime_start(
            "/tmp", "/tmp", NULL, "Watch",
            "exec /sbin/init") == _EINVAL,
            "拒绝缺失 socket 前缀");
    check_idle("缺失 socket 前缀不消耗一次性启动机会");
    CHECK(ish_watch_runtime_start(
            "/tmp", "/tmp", "", "Watch",
            "exec /sbin/init") == _EINVAL,
            "拒绝空 socket 前缀");
    check_idle("空 socket 前缀不消耗一次性启动机会");
    CHECK(ish_watch_runtime_start(
            "/tmp", "/tmp", "/tmp/ishsock", NULL,
            "exec /sbin/init") == _EINVAL,
            "拒绝缺失 hostname");
    check_idle("缺失 hostname 不消耗一次性启动机会");
    CHECK(ish_watch_runtime_start(
            "/tmp", "/tmp", "/tmp/ishsock", "",
            "exec /sbin/init") == _EINVAL,
            "拒绝空 hostname");
    check_idle("空 hostname 不消耗一次性启动机会");
    CHECK(ish_watch_runtime_start(
            "/tmp", "/tmp", "/tmp/ishsock", "Watch", NULL) == _EINVAL,
            "拒绝缺失启动命令");
    check_idle("缺失启动命令不消耗一次性启动机会");
    CHECK(ish_watch_runtime_start(
            "/tmp", "/tmp", "/tmp/ishsock", "Watch", "") == _EINVAL,
            "拒绝空启动命令");
    check_idle("空启动命令不消耗一次性启动机会");

    char long_socket_prefix[256];
    memset(long_socket_prefix, 's', sizeof(long_socket_prefix) - 1);
    long_socket_prefix[sizeof(long_socket_prefix) - 1] = '\0';
    CHECK(ish_watch_runtime_start(
            "/tmp", "/tmp", long_socket_prefix, "Watch",
            "exec /sbin/init") ==
            _ENAMETOOLONG,
            "拒绝无法放入 sockaddr_un 的 socket 前缀");
    check_idle("过长 socket 前缀不消耗一次性启动机会");

    char long_command[4097];
    memset(long_command, 'x', sizeof(long_command) - 1);
    long_command[sizeof(long_command) - 1] = '\0';
    CHECK(ish_watch_runtime_start(
            "/tmp", "/tmp", "/tmp/ishsock", "Watch",
            long_command) == _E2BIG,
            "拒绝过长启动命令");
    check_idle("过长启动命令不消耗一次性启动机会");

    test_output_overflow();

    CHECK(test_shared_mountpoint_failure(
            SHARED_MOUNTPOINT_MNT_FILE,
            _ENOTDIR,
            _ENOTDIR),
            "guest /mnt 非目录时拒绝挂载且不发布 PID 1");
    CHECK(test_shared_mountpoint_failure(
            SHARED_MOUNTPOINT_SHARED_SYMLINK,
            _ELOOP,
            _ENOTDIR),
            "guest /mnt/shared 为符号链接时拒绝挂载且不发布 PID 1");
    CHECK(test_validated_replacement_mounts_open_directory(),
            "验证后替换宿主路径仍以稳定 fd 挂载原目录并释放验证 fd");
    check_idle("子进程挂载点失败不消耗父进程启动机会");

    char root[PATH_MAX];
    char data[PATH_MAX];
    char documents[PATH_MAX];
    bool created_root = create_boot_failure_root(
            root, data, documents);
    CHECK(created_root,
            "创建启动 exec 失败的最小 fakefs");
    if (!created_root)
        return 1;
    int start_error = ish_watch_runtime_start(
            data, documents, "/tmp/ishsock", "Watch",
            "exec /sbin/init");
    CHECK(start_error == _ENOEXEC,
            "无效 /bin/sh 应在 boot exec 阶段返回 ENOEXEC");
    CHECK(ish_watch_runtime_current_phase() == ISH_WATCH_RUNTIME_FAILED,
            "boot exec 失败后 runtime 应进入失败态");
    CHECK(ish_watch_runtime_last_error() == start_error,
            "失败态应保留公共 API 返回的同一错误");
    lock(&pids_lock);
    bool pid_one_unpublished = pid_get_task_zombie(1) == NULL;
    unlock(&pids_lock);
    CHECK(current == NULL && pid_one_unpublished,
            "boot exec 失败后不得保留 current 或已发布 PID 1");
    reset_directory_hook(DIRECTORY_HOOK_OBSERVE, NULL);
    int descriptors_before_repeat = count_open_file_descriptors();
    int repeated_start_error = ish_watch_runtime_start(
            "/tmp", documents, "/tmp/ishsock", "Watch",
            "exec /sbin/init");
    int descriptors_after_repeat = count_open_file_descriptors();
    disable_directory_hook();
    CHECK(repeated_start_error == _EALREADY,
            "失败后拒绝第二次启动全局 guest");
    CHECK(directory_hook_state.validation_count == 1 &&
            directory_hook_state.release_count == 1 &&
            directory_hook_state.released_fd_was_closed &&
            descriptors_after_repeat == descriptors_before_repeat,
            "CAS 拒绝重复启动时只关闭一次已验证目录 fd");
    remove_boot_failure_root(root, data, documents);

    if (failures == 0)
        puts("Watch runtime 公共边界回归通过");
    return failures == 0 ? 0 : 1;
}
