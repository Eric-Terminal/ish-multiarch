#include "platform/apple-command-session.h"
#include "platform/apple-watch-runtime.h"

#include <limits.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "kernel/errno.h"
#include "kernel/fs.h"
#include "kernel/signal.h"
#include "kernel/task.h"

#define IMAGE_SIZE 1024
#define IMAGE_BASE UINT64_C(0x400000)
#define IMAGE_ENTRY_OFFSET UINT64_C(0x200)
#define IMAGE_STDOUT_OFFSET UINT64_C(0x300)
#define IMAGE_STDERR_OFFSET UINT64_C(0x340)
#define IMAGE_MASK_OFFSET UINT64_C(0x380)
#define IMAGE_SIGACTION_OFFSET UINT64_C(0x3a0)
#define IMAGE_READY_OFFSET UINT64_C(0x3c0)
#define TEST_TIMEOUT_SECONDS 5
#define CONCURRENT_COMMAND_COUNT 4

static const char probe_stdout[] = "真实 AArch64 stdout\n";
static const char probe_stderr[] = "真实 AArch64 stderr\n";
static const char *const concurrent_guest_paths[
        CONCURRENT_COMMAND_COUNT] = {
    "/bin/probe-a",
    "/bin/probe-b",
    "/bin/probe-c",
    "/bin/probe-d",
};
static int failures;

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "Apple 命令桥真实 guest 测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        failures++; \
    } \
} while (0)

enum observed_event {
    EVENT_STDOUT_DATA = 1,
    EVENT_STDOUT_END = 2,
    EVENT_STDERR_DATA = 3,
    EVENT_STDERR_END = 4,
    EVENT_EXIT = 5,
};

struct fixture {
    char root[PATH_MAX];
    char data[PATH_MAX];
    char documents[PATH_MAX];
    bool runtime_started;
    bool runtime_stopped;
};

struct observation {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    uint64_t expected_request_id;
    uint64_t callback_request_id;
    unsigned char stdout_bytes[256];
    unsigned char stderr_bytes[256];
    size_t stdout_length;
    size_t stderr_length;
    int32_t stdout_error;
    int32_t stderr_error;
    enum observed_event events[16];
    size_t event_count;
    bool exited;
    struct ish_apple_command_session *callback_session;
    struct ish_apple_command_result_v1 result;
};

static void put_u16(unsigned char *bytes, uint16_t value) {
    bytes[0] = (unsigned char) value;
    bytes[1] = (unsigned char) (value >> 8);
}

static void put_u32(unsigned char *bytes, uint32_t value) {
    for (unsigned index = 0; index < 4; index++)
        bytes[index] = (unsigned char) (value >> (index * 8));
}

static void put_u64(unsigned char *bytes, uint64_t value) {
    for (unsigned index = 0; index < 8; index++)
        bytes[index] = (unsigned char) (value >> (index * 8));
}

static void put_program_header(
        unsigned char *bytes,
        uint32_t type,
        uint32_t flags,
        uint64_t offset,
        uint64_t address,
        uint64_t file_size,
        uint64_t memory_size,
        uint64_t alignment) {
    put_u32(bytes, type);
    put_u32(bytes + 4, flags);
    put_u64(bytes + 8, offset);
    put_u64(bytes + 16, address);
    put_u64(bytes + 32, file_size);
    put_u64(bytes + 40, memory_size);
    put_u64(bytes + 48, alignment);
}

static uint32_t movz_x(unsigned reg, uint16_t immediate) {
    return UINT32_C(0xd2800000) |
            (uint32_t) immediate << 5 |
            (uint32_t) reg;
}

static uint32_t movk_x_lsl16(unsigned reg, uint16_t immediate) {
    return UINT32_C(0xf2a00000) |
            (uint32_t) immediate << 5 |
            (uint32_t) reg;
}

static void make_image_header(unsigned char image[IMAGE_SIZE]) {
    memset(image, 0, IMAGE_SIZE);
    image[0] = 0x7f;
    image[1] = 'E';
    image[2] = 'L';
    image[3] = 'F';
    image[4] = 2;
    image[5] = 1;
    image[6] = 1;
    image[7] = 3;
    put_u16(image + 16, 2);
    put_u16(image + 18, 183);
    put_u32(image + 20, 1);
    put_u64(image + 24, IMAGE_BASE + IMAGE_ENTRY_OFFSET);
    put_u64(image + 32, 64);
    put_u16(image + 52, 64);
    put_u16(image + 54, 56);
    put_u16(image + 56, 2);

    unsigned char *headers = image + 64;
    put_program_header(
            headers, 6, 4, 64, IMAGE_BASE + 64, 112, 112, 8);
    put_program_header(
            headers + 56, 1, 5, 0, IMAGE_BASE,
            IMAGE_SIZE, IMAGE_SIZE, UINT64_C(0x1000));
}

static void emit_address(
        unsigned char **program, unsigned reg, uint64_t address) {
    put_u32(*program, movz_x(reg, (uint16_t) address));
    put_u32(
            *program + 4,
            movk_x_lsl16(reg, (uint16_t) (address >> 16)));
    *program += 8;
}

static void emit_syscall(
        unsigned char **program, uint16_t number) {
    put_u32(*program, movz_x(8, number));
    put_u32(*program + 4, UINT32_C(0xd4000001));
    *program += 8;
}

static void emit_write(
        unsigned char **program,
        unsigned fd,
        uint64_t address,
        uint16_t length) {
    put_u32(*program, movz_x(0, (uint16_t) fd));
    *program += 4;
    emit_address(program, 1, address);
    put_u32(*program, movz_x(2, length));
    *program += 4;
    emit_syscall(program, 64);
}

static void emit_exit(
        unsigned char **program, uint16_t status) {
    put_u32(*program, movz_x(0, status));
    *program += 4;
    emit_syscall(program, 93);
}

static void emit_sigsuspend_loop(
        unsigned char **program) {
    emit_address(program, 0, IMAGE_BASE + IMAGE_MASK_OFFSET);
    put_u32(*program, movz_x(1, sizeof(sigset_t_)));
    *program += 4;
    emit_syscall(program, 133);
    // 返回 syscall 起点，未屏蔽的 SIGKILL 会从内核路径直接结束进程。
    put_u32(*program, UINT32_C(0x17fffffb));
    *program += 4;
}

static void make_blocking_image(unsigned char image[IMAGE_SIZE]) {
    make_image_header(image);
    unsigned char *program = image + IMAGE_ENTRY_OFFSET;
    /*
     * 测试 PID1 没有 libc 的 wait 循环；显式忽略 SIGCHLD，让内核自动
     * 回收命令子进程，避免 fixture 自身制造僵尸。
     */
    put_u32(program, movz_x(0, SIGCHLD_));
    program += 4;
    emit_address(&program, 1, IMAGE_BASE + IMAGE_SIGACTION_OFFSET);
    put_u32(program, movz_x(2, 0));
    put_u32(program + 4, movz_x(3, sizeof(sigset_t_)));
    program += 8;
    emit_syscall(&program, 134);
    emit_write(&program, 1, IMAGE_BASE + IMAGE_READY_OFFSET, 1);
    emit_sigsuspend_loop(&program);
    put_u64(image + IMAGE_SIGACTION_OFFSET, SIG_IGN_);
    image[IMAGE_READY_OFFSET] = 'R';
}

static void make_probe_image(unsigned char image[IMAGE_SIZE]) {
    make_image_header(image);
    unsigned char *program = image + IMAGE_ENTRY_OFFSET;
    emit_write(
            &program, 1, IMAGE_BASE + IMAGE_STDOUT_OFFSET,
            (uint16_t) (sizeof(probe_stdout) - 1));
    emit_write(
            &program, 2, IMAGE_BASE + IMAGE_STDERR_OFFSET,
            (uint16_t) (sizeof(probe_stderr) - 1));
    emit_exit(&program, 37);
    memcpy(
            image + IMAGE_STDOUT_OFFSET,
            probe_stdout,
            sizeof(probe_stdout) - 1);
    memcpy(
            image + IMAGE_STDERR_OFFSET,
            probe_stderr,
            sizeof(probe_stderr) - 1);
}

static void make_concurrent_probe_image(
        unsigned char image[IMAGE_SIZE], unsigned index) {
    make_image_header(image);
    unsigned char *program = image + IMAGE_ENTRY_OFFSET;
    emit_write(
            &program, 1, IMAGE_BASE + IMAGE_STDOUT_OFFSET, 1);
    emit_write(
            &program, 2, IMAGE_BASE + IMAGE_STDERR_OFFSET, 1);
    emit_exit(&program, (uint16_t) (40 + index));
    image[IMAGE_STDOUT_OFFSET] = (unsigned char) ('A' + index);
    image[IMAGE_STDERR_OFFSET] = (unsigned char) ('a' + index);
}

static void make_escaping_image(unsigned char image[IMAGE_SIZE]) {
    make_image_header(image);
    unsigned char *program = image + IMAGE_ENTRY_OFFSET;

    /*
     * clone(SIGCHLD, 0, 0, 0, 0)。父进程等 stdin EOF；子进程完成 setsid
     * 后先写 S，宿主收到 S 才关闭 stdin，因此 leader 退出时子进程已经
     * 确实脱离原进程组并仍持有 stdout。
     */
    put_u32(program, movz_x(0, 17));
    put_u32(program + 4, movz_x(1, 0));
    put_u32(program + 8, movz_x(2, 0));
    put_u32(program + 12, movz_x(3, 0));
    put_u32(program + 16, movz_x(4, 0));
    program += 20;
    emit_syscall(&program, 220);
    // child 标签距当前 cbz 九条指令。
    put_u32(program, UINT32_C(0xb4000120));
    program += 4;
    put_u32(program, movz_x(0, 0));
    // add x1, sp, #0
    put_u32(program + 4, UINT32_C(0x910003e1));
    put_u32(program + 8, movz_x(2, 1));
    program += 12;
    emit_syscall(&program, 63);
    emit_exit(&program, 0);
    emit_syscall(&program, 157);
    emit_write(&program, 1, IMAGE_BASE + IMAGE_STDOUT_OFFSET, 1);
    emit_sigsuspend_loop(&program);
    image[IMAGE_STDOUT_OFFSET] = 'S';
}

static bool write_host_file(
        const char *path,
        const void *bytes,
        size_t length,
        mode_t mode) {
    FILE *file = fopen(path, "wb");
    if (file == NULL)
        return false;
    bool written = fwrite(bytes, 1, length, file) == length;
    if (fclose(file) != 0)
        written = false;
    return written && chmod(path, mode) == 0;
}

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
    bool inserted = error == SQLITE_DONE;
    sqlite3_finalize(statement);
    if (!inserted)
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
                statement, 1, path, (int) strlen(path), SQLITE_STATIC);
    if (error == SQLITE_OK)
        error = sqlite3_bind_int64(statement, 2, inode);
    if (error == SQLITE_OK)
        error = sqlite3_step(statement);
    inserted = error == SQLITE_DONE;
    sqlite3_finalize(statement);
    return inserted;
}

static bool create_fakefs_database(const struct fixture *fixture) {
    char database_path[PATH_MAX];
    if (snprintf(
            database_path,
            sizeof(database_path),
            "%s/meta.db",
            fixture->root) >= (int) sizeof(database_path))
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
        {"/dev", 0040755},
        {"/dev/pts", 0040755},
        {"/proc", 0040755},
        {"/bin", 0040755},
        {"/bin/sh", 0100755},
        {"/bin/probe", 0100755},
        {"/bin/probe-a", 0100755},
        {"/bin/probe-b", 0100755},
        {"/bin/probe-c", 0100755},
        {"/bin/probe-d", 0100755},
        {"/bin/escape", 0100755},
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

    struct stat status;
    if (created && stat(database_path, &status) < 0)
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
            statement, 1, (sqlite3_int64) status.st_ino) != SQLITE_OK)
        created = false;
    if (created && sqlite3_step(statement) != SQLITE_DONE)
        created = false;
    sqlite3_finalize(statement);
    if (sqlite3_close(database) != SQLITE_OK)
        created = false;
    return created;
}

static bool create_fixture(struct fixture *fixture) {
    strcpy(fixture->root, "/tmp/ish-command-guest-XXXXXX");
    if (mkdtemp(fixture->root) == NULL ||
            snprintf(
                    fixture->data,
                    sizeof(fixture->data),
                    "%s/data",
                    fixture->root) >= (int) sizeof(fixture->data) ||
            snprintf(
                    fixture->documents,
                    sizeof(fixture->documents),
                    "%s/documents",
                    fixture->root) >=
                    (int) sizeof(fixture->documents) ||
            mkdir(fixture->data, 0700) < 0 ||
            mkdir(fixture->documents, 0700) < 0)
        return false;

    char path[PATH_MAX];
    static const char *const directories[] = {
        "dev", "dev/pts", "proc", "bin", "etc", "etc/apk",
    };
    for (size_t index = 0;
            index < sizeof(directories) / sizeof(directories[0]);
            index++) {
        if (snprintf(
                path,
                sizeof(path),
                "%s/%s",
                fixture->data,
                directories[index]) >= (int) sizeof(path) ||
                mkdir(path, 0700) < 0)
            return false;
    }

    unsigned char image[IMAGE_SIZE];
    make_blocking_image(image);
    if (snprintf(
            path,
            sizeof(path),
            "%s/bin/sh",
            fixture->data) >= (int) sizeof(path) ||
            !write_host_file(path, image, sizeof(image), 0700))
        return false;
    make_probe_image(image);
    if (snprintf(
            path,
            sizeof(path),
            "%s/bin/probe",
            fixture->data) >= (int) sizeof(path) ||
            !write_host_file(path, image, sizeof(image), 0700))
        return false;
    for (unsigned index = 0;
            index < CONCURRENT_COMMAND_COUNT; index++) {
        make_concurrent_probe_image(image, index);
        if (snprintf(
                path,
                sizeof(path),
                "%s%s",
                fixture->data,
                concurrent_guest_paths[index]) >= (int) sizeof(path) ||
                !write_host_file(path, image, sizeof(image), 0700))
            return false;
    }
    make_escaping_image(image);
    if (snprintf(
            path,
            sizeof(path),
            "%s/bin/escape",
            fixture->data) >= (int) sizeof(path) ||
            !write_host_file(path, image, sizeof(image), 0700))
        return false;

    static const char repositories[] = "https://runtime.example\n";
    if (snprintf(
            path,
            sizeof(path),
            "%s/etc/apk/repositories",
            fixture->data) >= (int) sizeof(path) ||
            !write_host_file(
                    path, repositories,
                    sizeof(repositories) - 1, 0600))
        return false;
    return create_fakefs_database(fixture);
}

static void remove_if_present(const char *path) {
    if (unlink(path) < 0)
        (void) path;
}

static void destroy_fixture(const struct fixture *fixture) {
    if (fixture->runtime_started && !fixture->runtime_stopped)
        return;
    if (fixture->runtime_started) {
        lock(&mounts_lock);
        (void) do_umount("/dev/pts");
        (void) do_umount("/proc");
        (void) do_umount("/mnt/shared");
        (void) do_umount("");
        unlock(&mounts_lock);
    }
    if (fixture->data[0] == '\0')
        return;

    char path[PATH_MAX];
    static const char *const device_entries[] = {
        "tty1", "tty2", "tty3", "tty4", "tty5", "tty6", "tty7",
        "tty", "console", "ptmx", "null", "zero", "full",
        "random", "urandom",
    };
    for (size_t index = 0;
            index < sizeof(device_entries) / sizeof(device_entries[0]);
            index++) {
        if (snprintf(
                path,
                sizeof(path),
                "%s/dev/%s",
                fixture->data,
                device_entries[index]) < (int) sizeof(path))
            remove_if_present(path);
    }
    static const char *const files[] = {
        "bin/sh", "bin/probe",
        "bin/probe-a", "bin/probe-b",
        "bin/probe-c", "bin/probe-d",
        "bin/escape",
        "etc/apk/repositories", "etc/resolv.conf",
    };
    for (size_t index = 0;
            index < sizeof(files) / sizeof(files[0]);
            index++) {
        if (snprintf(
                path,
                sizeof(path),
                "%s/%s",
                fixture->data,
                files[index]) < (int) sizeof(path))
            remove_if_present(path);
    }
    static const char *const directories[] = {
        "dev/pts", "dev", "proc", "bin", "etc/apk", "etc",
        "mnt/shared", "mnt",
    };
    for (size_t index = 0;
            index < sizeof(directories) / sizeof(directories[0]);
            index++) {
        if (snprintf(
                path,
                sizeof(path),
                "%s/%s",
                fixture->data,
                directories[index]) < (int) sizeof(path))
            (void) rmdir(path);
    }
    (void) rmdir(fixture->data);
    (void) rmdir(fixture->documents);
    static const char *const databases[] = {
        "meta.db", "meta.db-wal", "meta.db-shm", "meta.db-journal",
    };
    for (size_t index = 0;
            index < sizeof(databases) / sizeof(databases[0]);
            index++) {
        if (snprintf(
                path,
                sizeof(path),
                "%s/%s",
                fixture->root,
                databases[index]) < (int) sizeof(path))
            remove_if_present(path);
    }
    (void) rmdir(fixture->root);
}

static bool no_command_children(void) {
    bool absent = true;
    lock(&pids_lock);
    for (dword_t pid = 2; pid <= MAX_PID; pid++) {
        if (pid_get_task_zombie(pid) != NULL) {
            absent = false;
            break;
        }
    }
    unlock(&pids_lock);
    return absent;
}

static bool has_setsid_child(void) {
    bool found = false;
    lock(&pids_lock);
    for (dword_t pid = 2; pid <= MAX_PID; pid++) {
        struct task *task = pid_get_task(pid);
        if (task != NULL &&
                task->group->leader == task &&
                task->group->sid == (pid_t_) pid &&
                task->group->pgid == (pid_t_) pid) {
            found = true;
            break;
        }
    }
    unlock(&pids_lock);
    return found;
}

static bool wait_for_no_command_children(void) {
    for (unsigned attempt = 0; attempt < 5000; attempt++) {
        if (no_command_children())
            return true;
        usleep(1000);
    }
    return false;
}

static bool wait_for_runtime_ready(void) {
    for (unsigned attempt = 0; attempt < 5000; attempt++) {
        unsigned char byte;
        if (ish_watch_runtime_read_output(
                    &byte, sizeof(byte), NULL) == sizeof(byte) &&
                byte == 'R')
            return true;
        usleep(1000);
    }
    return false;
}

static void kill_command_children(void) {
    lock(&pids_lock);
    for (dword_t pid = 2; pid <= MAX_PID; pid++) {
        struct task *task = pid_get_task(pid);
        if (task != NULL)
            send_signal_locked(task, SIGKILL_, SIGINFO_NIL);
    }
    unlock(&pids_lock);
}

static bool stop_runtime(void) {
    lock(&pids_lock);
    struct task *init = pid_get_task(1);
    if (init != NULL)
        send_signal_locked(init, SIGKILL_, SIGINFO_NIL);
    unlock(&pids_lock);
    if (init == NULL)
        return false;
    for (unsigned attempt = 0; attempt < 5000; attempt++) {
        if (ish_watch_runtime_current_phase() ==
                ISH_WATCH_RUNTIME_STOPPED)
            return true;
        usleep(1000);
    }
    return false;
}

static void observation_init(
        struct observation *observation,
        uint64_t request_id) {
    memset(observation, 0, sizeof(*observation));
    pthread_mutex_init(&observation->lock, NULL);
    pthread_cond_init(&observation->changed, NULL);
    observation->expected_request_id = request_id;
}

static void observation_destroy(struct observation *observation) {
    pthread_cond_destroy(&observation->changed);
    pthread_mutex_destroy(&observation->lock);
}

static void observation_add_event(
        struct observation *observation,
        enum observed_event event) {
    if (observation->event_count <
            sizeof(observation->events) /
                    sizeof(observation->events[0]))
        observation->events[observation->event_count++] = event;
}

static void command_stream(
        void *opaque,
        struct ish_apple_command_session *session,
        uint64_t request_id,
        uint32_t stream,
        const void *bytes,
        uint32_t length,
        int32_t terminal_error) {
    struct observation *observation = opaque;
    pthread_mutex_lock(&observation->lock);
    observation->callback_session = session;
    observation->callback_request_id = request_id;
    unsigned char *destination;
    size_t *destination_length;
    int32_t *stream_error;
    enum observed_event data_event;
    enum observed_event end_event;
    if (stream == ISH_APPLE_COMMAND_STREAM_STDOUT) {
        destination = observation->stdout_bytes;
        destination_length = &observation->stdout_length;
        stream_error = &observation->stdout_error;
        data_event = EVENT_STDOUT_DATA;
        end_event = EVENT_STDOUT_END;
    } else {
        destination = observation->stderr_bytes;
        destination_length = &observation->stderr_length;
        stream_error = &observation->stderr_error;
        data_event = EVENT_STDERR_DATA;
        end_event = EVENT_STDERR_END;
    }
    if (length != 0) {
        size_t available = 256 - *destination_length;
        size_t copied = length < available ? length : available;
        memcpy(destination + *destination_length, bytes, copied);
        *destination_length += copied;
        observation_add_event(observation, data_event);
    } else {
        *stream_error = terminal_error;
        observation_add_event(observation, end_event);
    }
    pthread_cond_broadcast(&observation->changed);
    pthread_mutex_unlock(&observation->lock);
}

static void command_exited(
        void *opaque,
        struct ish_apple_command_session *session,
        const struct ish_apple_command_result_v1 *result) {
    struct observation *observation = opaque;
    pthread_mutex_lock(&observation->lock);
    observation->callback_session = session;
    observation->callback_request_id = result->request_id;
    observation->result = *result;
    observation_add_event(observation, EVENT_EXIT);
    observation->exited = true;
    pthread_cond_broadcast(&observation->changed);
    pthread_mutex_unlock(&observation->lock);
}

static bool observation_wait_for_exit(
        struct observation *observation) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += TEST_TIMEOUT_SECONDS;
    pthread_mutex_lock(&observation->lock);
    int error = 0;
    while (!observation->exited && error == 0)
        error = pthread_cond_timedwait(
                &observation->changed,
                &observation->lock,
                &deadline);
    bool exited = observation->exited;
    pthread_mutex_unlock(&observation->lock);
    return exited;
}

static bool observation_wait_for_stdout(
        struct observation *observation, size_t length) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += TEST_TIMEOUT_SECONDS;
    pthread_mutex_lock(&observation->lock);
    int error = 0;
    while (observation->stdout_length < length &&
            !observation->exited && error == 0)
        error = pthread_cond_timedwait(
                &observation->changed,
                &observation->lock,
                &deadline);
    bool available = observation->stdout_length >= length;
    pthread_mutex_unlock(&observation->lock);
    return available;
}

static int event_index(
        const struct observation *observation,
        enum observed_event event) {
    for (size_t index = 0;
            index < observation->event_count;
            index++) {
        if (observation->events[index] == event)
            return (int) index;
    }
    return -1;
}

static int32_t start_command(
        const char *executable,
        uint64_t request_id,
        struct observation *observation,
        struct ish_apple_command_session **session) {
    const char *arguments[] = {executable};
    const struct ish_apple_command_spec_v1 spec = {
        .version = ISH_APPLE_ABI_VERSION,
        .structure_size = sizeof(spec),
        .timeout_milliseconds = 5000,
        .request_id = request_id,
        .executable = executable,
        .arguments = arguments,
        .argument_count = 1,
    };
    const struct ish_apple_command_callbacks_v1 callbacks = {
        .version = ISH_APPLE_ABI_VERSION,
        .structure_size = sizeof(callbacks),
        .context = observation,
        .stream = command_stream,
        .completed = command_exited,
    };
    return ish_apple_command_session_start(
            &spec, &callbacks, session);
}

struct concurrent_start_gate {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    unsigned ready;
    bool go;
};

struct concurrent_start_request {
    struct concurrent_start_gate *gate;
    const char *executable;
    uint64_t request_id;
    struct observation *observation;
    struct ish_apple_command_session *session;
    int32_t error;
};

static void *start_command_concurrently(void *opaque) {
    struct concurrent_start_request *request = opaque;
    pthread_mutex_lock(&request->gate->lock);
    request->gate->ready++;
    pthread_cond_broadcast(&request->gate->changed);
    while (!request->gate->go)
        pthread_cond_wait(
                &request->gate->changed,
                &request->gate->lock);
    pthread_mutex_unlock(&request->gate->lock);
    request->error = start_command(
            request->executable,
            request->request_id,
            request->observation,
            &request->session);
    return NULL;
}

static void test_concurrent_start(void) {
    struct concurrent_start_gate gate = {
        .lock = PTHREAD_MUTEX_INITIALIZER,
        .changed = PTHREAD_COND_INITIALIZER,
    };
    struct observation observations[CONCURRENT_COMMAND_COUNT];
    struct concurrent_start_request requests[
            CONCURRENT_COMMAND_COUNT];
    pthread_t threads[CONCURRENT_COMMAND_COUNT];
    bool thread_created[CONCURRENT_COMMAND_COUNT] = {};
    unsigned created = 0;
    for (unsigned index = 0;
            index < CONCURRENT_COMMAND_COUNT; index++) {
        uint64_t request_id = UINT64_C(1100) + index;
        observation_init(&observations[index], request_id);
        requests[index] = (struct concurrent_start_request) {
            .gate = &gate,
            .executable = concurrent_guest_paths[index],
            .request_id = request_id,
            .observation = &observations[index],
            .error = _EIO,
        };
        int error = pthread_create(
                &threads[index],
                NULL,
                start_command_concurrently,
                &requests[index]);
        CHECK(error == 0, "建立生产后端并发 start 线程");
        if (error == 0) {
            thread_created[index] = true;
            created++;
        }
    }

    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += TEST_TIMEOUT_SECONDS;
    pthread_mutex_lock(&gate.lock);
    int gate_error = 0;
    while (gate.ready < created && gate_error == 0)
        gate_error = pthread_cond_timedwait(
                &gate.changed, &gate.lock, &deadline);
    CHECK(gate.ready == created,
            "四个 start 线程在同一门闩前就绪");
    gate.go = true;
    pthread_cond_broadcast(&gate.changed);
    pthread_mutex_unlock(&gate.lock);

    for (unsigned index = 0;
            index < CONCURRENT_COMMAND_COUNT; index++) {
        if (thread_created[index])
            pthread_join(threads[index], NULL);
        CHECK(requests[index].error == 0 &&
                requests[index].session != NULL,
                "并发 start 均发布独立命令 session");
    }

    bool all_exited = true;
    for (unsigned index = 0;
            index < CONCURRENT_COMMAND_COUNT; index++) {
        if (requests[index].session == NULL)
            continue;
        bool exited = observation_wait_for_exit(
                &observations[index]);
        CHECK(exited, "并发真实 guest 命令均在期限内完成");
        all_exited &= exited;
    }
    if (!all_exited)
        kill_command_children();

    for (unsigned index = 0;
            index < CONCURRENT_COMMAND_COUNT; index++) {
        struct ish_apple_command_session *session =
                requests[index].session;
        if (session != NULL && !observations[index].exited)
            (void) observation_wait_for_exit(&observations[index]);
        if (session != NULL && observations[index].exited) {
            struct ish_apple_command_result_v1 waited = {};
            CHECK(ish_apple_command_session_wait(
                    session, &waited) == 0 &&
                    waited.request_id == requests[index].request_id &&
                    waited.reason ==
                            ISH_APPLE_COMMAND_COMPLETION_EXITED &&
                    waited.exit_code == (int32_t) (40 + index) &&
                    observations[index].stdout_length == 1 &&
                    observations[index].stdout_bytes[0] ==
                            (unsigned char) ('A' + index) &&
                    observations[index].stderr_length == 1 &&
                    observations[index].stderr_bytes[0] ==
                            (unsigned char) ('a' + index),
                    "并发命令输出、request ID 与退出码互不串线");
        }
        if (session != NULL)
            ish_apple_command_session_release(session);
        observation_destroy(&observations[index]);
    }
    CHECK(wait_for_no_command_children(),
            "并发 start 完成后 PID1 之外没有 guest 残留");
    pthread_cond_destroy(&gate.changed);
    pthread_mutex_destroy(&gate.lock);
}

static void test_probe(void) {
    struct observation observation;
    observation_init(&observation, 1001);
    struct ish_apple_command_session *session = NULL;
    int32_t start_error = start_command(
            "/bin/probe", 1001, &observation, &session);
    CHECK(start_error == 0 && session != NULL,
            "通过生产 command session 启动真实 AArch64 ELF");
    if (session == NULL) {
        observation_destroy(&observation);
        return;
    }
    CHECK(observation_wait_for_exit(&observation),
            "真实 probe 在超时前完成退出回调");
    struct ish_apple_command_result_v1 waited = {};
    CHECK(ish_apple_command_session_wait(session, &waited) == 0,
            "同步等待返回结构化结果");

    int stdout_data = event_index(
            &observation, EVENT_STDOUT_DATA);
    int stdout_end = event_index(
            &observation, EVENT_STDOUT_END);
    int stderr_data = event_index(
            &observation, EVENT_STDERR_DATA);
    int stderr_end = event_index(
            &observation, EVENT_STDERR_END);
    int exited = event_index(&observation, EVENT_EXIT);
    CHECK(observation.callback_session == session &&
            observation.callback_request_id == 1001 &&
            observation.result.request_id == 1001,
            "所有回调关联同一 session 与 request ID");
    CHECK(observation.stdout_length == sizeof(probe_stdout) - 1 &&
            memcmp(
                    observation.stdout_bytes,
                    probe_stdout,
                    sizeof(probe_stdout) - 1) == 0 &&
            observation.stdout_error == 0,
            "真实 guest stdout 原样且只以 EOF 结束");
    CHECK(observation.stderr_length == sizeof(probe_stderr) - 1 &&
            memcmp(
                    observation.stderr_bytes,
                    probe_stderr,
                    sizeof(probe_stderr) - 1) == 0 &&
            observation.stderr_error == 0,
            "真实 guest stderr 原样且只以 EOF 结束");
    CHECK(stdout_data >= 0 && stdout_end > stdout_data &&
            stderr_data >= 0 && stderr_end > stderr_data &&
            exited > stdout_end && exited > stderr_end,
            "每路数据先于 EOF，退出回调晚于两路 EOF");
    CHECK(observation.result.reason ==
                    ISH_APPLE_COMMAND_COMPLETION_EXITED &&
            observation.result.exit_code == 37 &&
            observation.result.termination_signal == 0 &&
            observation.result.error == 0 &&
            waited.exit_code == observation.result.exit_code &&
            waited.reason == observation.result.reason &&
            waited.request_id == observation.result.request_id,
            "exit 37 返回一致的结构化完成状态");
    CHECK(observation.result.stdout_bytes ==
                    sizeof(probe_stdout) - 1 &&
            observation.result.stderr_bytes ==
                    sizeof(probe_stderr) - 1,
            "结构化结果统计两路已交付字节数");
    ish_apple_command_session_release(session);
    CHECK(wait_for_no_command_children(),
            "正常命令完成后 PID1 之外没有 guest 残留");
    observation_destroy(&observation);
}

static void test_cancel(void) {
    struct observation observation;
    observation_init(&observation, 1002);
    struct ish_apple_command_session *session = NULL;
    int32_t start_error = start_command(
            "/bin/sh", 1002, &observation, &session);
    CHECK(start_error == 0 && session != NULL,
            "启动阻塞的真实 AArch64 命令");
    if (session == NULL) {
        observation_destroy(&observation);
        return;
    }
    CHECK(ish_apple_command_session_cancel(session) == 0,
            "cancel 请求由生产后端接受");
    CHECK(observation_wait_for_exit(&observation),
            "cancel 后输出闭合并收到退出回调");
    struct ish_apple_command_result_v1 waited = {};
    CHECK(ish_apple_command_session_wait(session, &waited) == 0 &&
            waited.request_id == 1002 &&
            waited.reason == ISH_APPLE_COMMAND_COMPLETION_CANCELLED &&
            waited.termination_signal == SIGKILL_,
            "cancel 结果标识 SIGKILL 与原 request ID");
    ish_apple_command_session_release(session);
    CHECK(wait_for_no_command_children(),
            "cancel 完成后 PID1 之外没有 guest 残留");
    observation_destroy(&observation);
}

static void test_escaping_child(void) {
    struct observation observation;
    observation_init(&observation, 1003);
    struct ish_apple_command_session *session = NULL;
    int32_t start_error = start_command(
            "/bin/escape", 1003, &observation, &session);
    CHECK(start_error == 0 && session != NULL,
            "启动 fork 后 setsid 的真实 guest 作业");
    if (session == NULL) {
        observation_destroy(&observation);
        return;
    }
    bool child_ready = observation_wait_for_stdout(
            &observation, 1);
    CHECK(child_ready && observation.stdout_bytes[0] == 'S' &&
            has_setsid_child(),
            "setsid 子进程在 leader 退出前确认新会话身份与 stdout");
    CHECK(ish_apple_command_session_close_stdin(session) == 0,
            "关闭 stdin 允许已同步的 leader 退出");
    bool exited = observation_wait_for_exit(&observation);
    CHECK(exited,
            "作业 leader 退出时清理脱离进程组但继承管道的子进程");
    if (!exited) {
        /*
         * 此分支只为让失败测试可回收；正常实现应由不可继承伪造的
         * host job 身份完成清理。
         */
        kill_command_children();
        (void) observation_wait_for_exit(&observation);
    }
    if (observation.exited) {
        struct ish_apple_command_result_v1 waited = {};
        CHECK(ish_apple_command_session_wait(session, &waited) == 0 &&
                waited.request_id == 1003 &&
                waited.reason ==
                        ISH_APPLE_COMMAND_COMPLETION_EXITED &&
                waited.exit_code == 0,
                "逃逸作业保留 leader 的退出结果");
    }
    ish_apple_command_session_release(session);
    CHECK(wait_for_no_command_children(),
            "逃逸作业完成后 PID1 之外没有 guest 残留");
    observation_destroy(&observation);
}

int main(void) {
    struct fixture fixture;
    memset(&fixture, 0, sizeof(fixture));
    if (!create_fixture(&fixture)) {
        fprintf(stderr, "Apple 命令桥真实 guest 测试失败：创建 fixture\n");
        destroy_fixture(&fixture);
        return 1;
    }

    char socket_prefix[96];
    CHECK(snprintf(
            socket_prefix,
            sizeof(socket_prefix),
            "/tmp/ish-command-guest-%ld-",
            (long) getpid()) < (int) sizeof(socket_prefix),
            "生成唯一 socket 前缀");
    int start_error = ish_watch_runtime_start(
            fixture.data,
            fixture.documents,
            socket_prefix,
            "Command-Test",
            "ignored");
    fixture.runtime_started = start_error == 0;
    CHECK(start_error == 0 &&
            ish_watch_runtime_current_phase() ==
                    ISH_WATCH_RUNTIME_RUNNING,
            "启动真实 iSH runtime 与 PID1");

    if (fixture.runtime_started) {
        bool runtime_ready = wait_for_runtime_ready();
        CHECK(runtime_ready,
                "等待 PID1 安装 SIGCHLD 自动回收策略");
        if (runtime_ready) {
            test_probe();
            test_concurrent_start();
            test_cancel();
            test_escaping_child();
        }
        fixture.runtime_stopped = stop_runtime();
        CHECK(fixture.runtime_stopped, "测试结束时停止 PID1");
    }
    destroy_fixture(&fixture);

    if (failures != 0) {
        fprintf(
                stderr,
                "Apple 命令桥真实 guest 测试共 %d 项失败\n",
                failures);
        return 1;
    }
    puts("Apple 命令桥真实生产后端回归通过");
    return 0;
}
