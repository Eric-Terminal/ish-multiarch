#include "platform/apple-watch-runtime.h"

#include <limits.h>
#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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
        char root[PATH_MAX], char data[PATH_MAX]) {
    strcpy(root, "/tmp/ish-watch-boot-failure-XXXXXX");
    if (mkdtemp(root) == NULL ||
            snprintf(data, PATH_MAX, "%s/data", root) >= PATH_MAX ||
            mkdir(data, 0700) < 0)
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

static void remove_boot_failure_root(
        const char *root, const char *data) {
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
    static const char *const directories[] = {
        "dev/pts", "dev", "proc", "bin",
    };
    for (size_t index = 0;
            index < sizeof(directories) / sizeof(directories[0]);
            index++) {
        if (snprintf(path, sizeof(path), "%s/%s",
                    data, directories[index]) < (int) sizeof(path))
            (void) rmdir(path);
    }
    (void) rmdir(data);
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

    CHECK(ish_watch_runtime_start(
            NULL, "/tmp/ishsock", "Watch",
            "exec /sbin/init") == _EINVAL,
            "拒绝缺失 root data 路径");
    check_idle("缺失 root data 路径不消耗一次性启动机会");
    CHECK(ish_watch_runtime_start(
            "", "/tmp/ishsock", "Watch",
            "exec /sbin/init") == _EINVAL,
            "拒绝空 root data 路径");
    check_idle("空 root data 路径不消耗一次性启动机会");
    CHECK(ish_watch_runtime_start(
            "/tmp", NULL, "Watch",
            "exec /sbin/init") == _EINVAL,
            "拒绝缺失 socket 前缀");
    check_idle("缺失 socket 前缀不消耗一次性启动机会");
    CHECK(ish_watch_runtime_start(
            "/tmp", "", "Watch",
            "exec /sbin/init") == _EINVAL,
            "拒绝空 socket 前缀");
    check_idle("空 socket 前缀不消耗一次性启动机会");
    CHECK(ish_watch_runtime_start(
            "/tmp", "/tmp/ishsock", NULL,
            "exec /sbin/init") == _EINVAL,
            "拒绝缺失 hostname");
    check_idle("缺失 hostname 不消耗一次性启动机会");
    CHECK(ish_watch_runtime_start(
            "/tmp", "/tmp/ishsock", "",
            "exec /sbin/init") == _EINVAL,
            "拒绝空 hostname");
    check_idle("空 hostname 不消耗一次性启动机会");
    CHECK(ish_watch_runtime_start(
            "/tmp", "/tmp/ishsock", "Watch", NULL) == _EINVAL,
            "拒绝缺失启动命令");
    check_idle("缺失启动命令不消耗一次性启动机会");
    CHECK(ish_watch_runtime_start(
            "/tmp", "/tmp/ishsock", "Watch", "") == _EINVAL,
            "拒绝空启动命令");
    check_idle("空启动命令不消耗一次性启动机会");

    char long_socket_prefix[256];
    memset(long_socket_prefix, 's', sizeof(long_socket_prefix) - 1);
    long_socket_prefix[sizeof(long_socket_prefix) - 1] = '\0';
    CHECK(ish_watch_runtime_start(
            "/tmp", long_socket_prefix, "Watch",
            "exec /sbin/init") ==
            _ENAMETOOLONG,
            "拒绝无法放入 sockaddr_un 的 socket 前缀");
    check_idle("过长 socket 前缀不消耗一次性启动机会");

    char long_command[4097];
    memset(long_command, 'x', sizeof(long_command) - 1);
    long_command[sizeof(long_command) - 1] = '\0';
    CHECK(ish_watch_runtime_start(
            "/tmp", "/tmp/ishsock", "Watch", long_command) == _E2BIG,
            "拒绝过长启动命令");
    check_idle("过长启动命令不消耗一次性启动机会");

    test_output_overflow();

    char root[PATH_MAX];
    char data[PATH_MAX];
    bool created_root = create_boot_failure_root(root, data);
    CHECK(created_root,
            "创建启动 exec 失败的最小 fakefs");
    if (!created_root)
        return 1;
    int start_error = ish_watch_runtime_start(
            data, "/tmp/ishsock", "Watch",
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
    CHECK(ish_watch_runtime_start(
            "/tmp", "/tmp/ishsock", "Watch",
            "exec /sbin/init") == _EALREADY,
            "失败后拒绝第二次启动全局 guest");
    remove_boot_failure_root(root, data);

    if (failures == 0)
        puts("Watch runtime 公共边界回归通过");
    return failures == 0 ? 0 : 1;
}
