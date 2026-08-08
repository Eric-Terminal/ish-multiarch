#include "platform/apple-watch-runtime.h"

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
#include "fs/real.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "kernel/init.h"
#include "kernel/signal.h"
#include "kernel/task.h"
#include "platform/apple-guest-file-mutations.h"
#include "sdk/iSHApple/Headers/iSHAppleGuestFile.h"
#include "sdk/iSHApple/Headers/iSHAppleRuntime.h"

#define IMAGE_SIZE 1024
#define IMAGE_BASE UINT64_C(0x400000)
#define IMAGE_ENTRY_OFFSET UINT64_C(0x200)
#define IMAGE_MASK_OFFSET UINT64_C(0x300)

static int failures;

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "Watch guest 运行态测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        failures++; \
    } \
} while (0)

struct fixture {
    char root[PATH_MAX];
    char data[PATH_MAX];
    char documents[PATH_MAX];
    bool runtime_started;
    bool runtime_stopped;
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

static void make_blocking_shell(unsigned char image[IMAGE_SIZE]) {
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
            headers,
            6,
            4,
            64,
            IMAGE_BASE + 64,
            112,
            112,
            8);
    put_program_header(
            headers + 56,
            1,
            5,
            0,
            IMAGE_BASE,
            IMAGE_SIZE,
            IMAGE_SIZE,
            UINT64_C(0x1000));

    uint64_t mask_address = IMAGE_BASE + IMAGE_MASK_OFFSET;
    unsigned char *program = image + IMAGE_ENTRY_OFFSET;
    put_u32(program, movz_x(0, (uint16_t) mask_address));
    put_u32(
            program + 4,
            movk_x_lsl16(0, (uint16_t) (mask_address >> 16)));
    put_u32(program + 8, movz_x(1, sizeof(sigset_t_)));
    put_u32(program + 12, movz_x(8, 133));
    put_u32(program + 16, UINT32_C(0xd4000001));
    put_u32(program + 20, UINT32_C(0x17fffffb));
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

static bool create_fixture(struct fixture *fixture) {
    strcpy(fixture->root, "/tmp/ish-watch-guest-runtime-XXXXXX");
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

    unsigned char shell[IMAGE_SIZE];
    make_blocking_shell(shell);
    if (snprintf(
            path,
            sizeof(path),
            "%s/bin/sh",
            fixture->data) >= (int) sizeof(path) ||
            !write_host_file(path, shell, sizeof(shell), 0700))
        return false;

    static const char repositories[] =
            "https://runtime-initial.example\n";
    if (snprintf(
            path,
            sizeof(path),
            "%s/etc/apk/repositories",
            fixture->data) >= (int) sizeof(path) ||
            !write_host_file(
                    path,
                    repositories,
                    sizeof(repositories) - 1,
                    0600))
        return false;

    static const char shared_from_host[] = "host-to-guest\n";
    if (snprintf(
            path,
            sizeof(path),
            "%s/from-host.txt",
            fixture->documents) >= (int) sizeof(path) ||
            !write_host_file(
                    path,
                    shared_from_host,
                    sizeof(shared_from_host) - 1,
                    0600))
        return false;

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
            statement,
            1,
            (sqlite3_int64) database_stat.st_ino) != SQLITE_OK)
        created = false;
    if (created && sqlite3_step(statement) != SQLITE_DONE)
        created = false;
    sqlite3_finalize(statement);
    if (sqlite3_close(database) != SQLITE_OK)
        created = false;
    return created;
}

static bool no_published_children(void) {
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

static bool stop_runtime(void) {
    lock(&pids_lock);
    struct task *init = pid_get_task(1);
    if (init != NULL)
        send_signal_locked(init, SIGKILL_, SIGINFO_NIL);
    unlock(&pids_lock);
    if (init == NULL)
        return false;

    for (unsigned attempt = 0; attempt < 5000; attempt++) {
        /*
         * PID 1 没有父进程可回收，停机后会保留无资源的进程表记录；
         * STOPPED 在 halt_system 已卸载文件系统之后发布。
         */
        if (ish_watch_runtime_current_phase() ==
                ISH_WATCH_RUNTIME_STOPPED)
            return true;
        usleep(1000);
    }
    return false;
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
        "bin/sh", "etc/apk/repositories", "etc/resolv.conf",
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
    if (fixture->documents[0] != '\0') {
        static const char *const shared_files[] = {
            "from-host.txt", "from-guest.txt",
        };
        for (size_t index = 0;
                index < sizeof(shared_files) / sizeof(shared_files[0]);
                index++) {
            if (snprintf(
                    path,
                    sizeof(path),
                    "%s/%s",
                    fixture->documents,
                    shared_files[index]) < (int) sizeof(path))
                remove_if_present(path);
        }
        (void) rmdir(fixture->documents);
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
                fixture->root,
                database_suffixes[index]) < (int) sizeof(path))
            remove_if_present(path);
    }
    (void) rmdir(fixture->root);
}

static bool shared_mount_is_unique(const char *documents_directory) {
    char canonical_documents[PATH_MAX];
    if (realpath(documents_directory, canonical_documents) == NULL)
        return false;

    char mount_point[] = "/mnt/shared";
    struct mount *mount = mount_find(mount_point);
    bool matches =
            strcmp(mount->point, "/mnt/shared") == 0 &&
            mount->fs == &realfs &&
            strcmp(mount->source, canonical_documents) == 0;
    mount_release(mount);
    return matches;
}

static ssize_t read_guest_path(
        const char *path, void *buffer, size_t capacity) {
    int error = begin_new_init_child();
    if (error < 0)
        return error;

    struct fd *file = generic_open(path, O_RDONLY_, 0);
    ssize_t result;
    if (IS_ERR(file)) {
        result = PTR_ERR(file);
    } else {
        result = file_read_fd(file, buffer, capacity);
        int close_error = fd_close(file);
        if (result >= 0 && close_error < 0)
            result = close_error;
    }
    cancel_prepared_process();
    return result;
}

static int write_guest_path(
        const char *path, const void *bytes, size_t length) {
    int error = begin_new_init_child();
    if (error < 0)
        return error;

    struct fd *file = generic_open(
            path, O_WRONLY_ | O_CREAT_ | O_TRUNC_, 0644);
    if (IS_ERR(file)) {
        error = (int) PTR_ERR(file);
    } else {
        ssize_t written = file_write_fd(file, bytes, length);
        error = written == (ssize_t) length ?
                0 : written < 0 ? (int) written : _EIO;
        int close_error = fd_close(file);
        if (error >= 0 && close_error < 0)
            error = close_error;
    }
    cancel_prepared_process();
    return error;
}

static ssize_t read_host_path(
        const char *path, void *buffer, size_t capacity) {
    FILE *file = fopen(path, "rb");
    if (file == NULL)
        return -1;
    size_t length = fread(buffer, 1, capacity, file);
    bool complete = !ferror(file);
    if (fclose(file) != 0)
        complete = false;
    return complete ? (ssize_t) length : -1;
}

static bool host_directory_exists(const char *path) {
    struct stat status;
    return lstat(path, &status) == 0 && S_ISDIR(status.st_mode);
}

static struct ish_apple_guest_file_request_v1 guest_file_request(
        const char *path,
        uint64_t request_id) {
    return (struct ish_apple_guest_file_request_v1) {
        .version = ISH_APPLE_ABI_VERSION,
        .structure_size = sizeof(
                struct ish_apple_guest_file_request_v1),
        .request_id = request_id,
        .path = path,
    };
}

int main(void) {
    struct fixture fixture;
    memset(&fixture, 0, sizeof(fixture));
    if (!create_fixture(&fixture)) {
        fprintf(stderr, "Watch guest 运行态测试失败：创建 fixture\n");
        destroy_fixture(&fixture);
        return 1;
    }

    char socket_prefix[96];
    CHECK(snprintf(
            socket_prefix,
            sizeof(socket_prefix),
            "/tmp/ish-watch-guest-%ld-",
            (long) getpid()) < (int) sizeof(socket_prefix),
            "生成唯一 socket 前缀");
    int start_error = ish_watch_runtime_start(
            fixture.data,
            fixture.documents,
            socket_prefix,
            "Watch-Test",
            "ignored");
    fixture.runtime_started = start_error == 0;
    CHECK(start_error == 0 &&
            ish_watch_runtime_current_phase() ==
                    ISH_WATCH_RUNTIME_RUNNING,
            "启动真实 RUNNING runtime");

    struct ish_apple_runtime_capabilities_v1 capabilities;
    CHECK(ish_apple_runtime_copy_capabilities(&capabilities) == 0 &&
            capabilities.version == ISH_APPLE_ABI_VERSION &&
            capabilities.structure_size == sizeof(capabilities) &&
            capabilities.public_abi_version == ISH_APPLE_ABI_VERSION &&
            capabilities.guest_architecture ==
                    ISH_APPLE_RUNTIME_ARCHITECTURE_AARCH64 &&
            (capabilities.backend == ISH_APPLE_RUNTIME_BACKEND_C ||
                    capabilities.backend ==
                            ISH_APPLE_RUNTIME_BACKEND_THREADED) &&
            capabilities.feature_flags ==
                    (ISH_APPLE_RUNTIME_CAPABILITY_PTY |
                    ISH_APPLE_RUNTIME_CAPABILITY_LIVE_MOUNTS |
                    ISH_APPLE_RUNTIME_CAPABILITY_DIAGNOSTICS |
                    ISH_APPLE_RUNTIME_CAPABILITY_GUEST_FILES),
            "RUNNING 后公开完整且不可猜测的能力快照");

    unsigned char buffer[256] = {0};
    char path[PATH_MAX];
    bool mountpoints_created =
            snprintf(
                    path,
                    sizeof(path),
                    "%s/mnt",
                    fixture.data) < (int) sizeof(path) &&
            host_directory_exists(path) &&
            snprintf(
                    path,
                    sizeof(path),
                    "%s/mnt/shared",
                    fixture.data) < (int) sizeof(path) &&
            host_directory_exists(path);
    CHECK(mountpoints_created,
            "启动时创建并验证 guest /mnt 与 /mnt/shared 目录");
    CHECK(shared_mount_is_unique(fixture.documents),
            "Documents/Shared 只以 realfs 挂载一次到 /mnt/shared");

    static const char shared_from_host[] = "host-to-guest\n";
    ssize_t length = read_guest_path(
            "/mnt/shared/from-host.txt", buffer, sizeof(buffer));
    CHECK(length == (ssize_t) (sizeof(shared_from_host) - 1) &&
            memcmp(
                    buffer,
                    shared_from_host,
                    sizeof(shared_from_host) - 1) == 0 &&
            current == NULL &&
            no_published_children(),
            "宿主 Documents/Shared 文件可由 guest 立即读取");

    static const char shared_from_guest[] = "guest-to-host\n";
    CHECK(write_guest_path(
            "/mnt/shared/from-guest.txt",
            shared_from_guest,
            sizeof(shared_from_guest) - 1) == 0 &&
            current == NULL &&
            no_published_children(),
            "guest 可通过 /mnt/shared 写入宿主目录");
    memset(buffer, 0, sizeof(buffer));
    CHECK(snprintf(
            path,
            sizeof(path),
            "%s/from-guest.txt",
            fixture.documents) < (int) sizeof(path),
            "生成 guest 写入文件的宿主路径");
    length = read_host_path(path, buffer, sizeof(buffer));
    CHECK(length == (ssize_t) (sizeof(shared_from_guest) - 1) &&
            memcmp(
                    buffer,
                    shared_from_guest,
                    sizeof(shared_from_guest) - 1) == 0,
            "guest 写入内容可由宿主原路径立即读取");

    char expected_documents[PATH_MAX + 2];
    int expected_documents_length = snprintf(
            expected_documents,
            sizeof(expected_documents),
            "%s\n",
            fixture.documents);
    memset(buffer, 0, sizeof(buffer));
    length = read_guest_path(
            "/proc/ish/documents", buffer, sizeof(buffer));
    CHECK(expected_documents_length > 0 &&
            expected_documents_length < (int) sizeof(expected_documents) &&
            length == expected_documents_length &&
            memcmp(
                    buffer,
                    expected_documents,
                    (size_t) expected_documents_length) == 0,
            "/proc/ish/documents 返回 runtime 持有的同一宿主路径");

    memset(buffer, 0, sizeof(buffer));
    static const char initial[] =
            "https://runtime-initial.example\n";
    length = ish_watch_guest_file_read(
            ISH_WATCH_GUEST_FILE_REPOSITORIES,
            buffer,
            sizeof(buffer));
    CHECK(length == (ssize_t) (sizeof(initial) - 1) &&
            memcmp(buffer, initial, sizeof(initial) - 1) == 0 &&
            current == NULL &&
            no_published_children(),
            "公共读取执行 begin→op→cancel 且不发布临时子进程");

    static const char first[] =
            "https://runtime-first.example\n";
    CHECK(ish_watch_guest_file_replace(
            ISH_WATCH_GUEST_FILE_REPOSITORIES,
            first,
            sizeof(first) - 1,
            0) == 0 &&
            current == NULL &&
            no_published_children(),
            "公共替换执行 begin→op→cancel");

    static const char second[] =
            "https://runtime-second.example\n";
    CHECK(ish_watch_guest_file_replace(
            ISH_WATCH_GUEST_FILE_REPOSITORIES,
            second,
            sizeof(second) - 1,
            0) == 0,
            "连续公共替换证明 prepared task 锁已释放");
    memset(buffer, 0, sizeof(buffer));
    length = ish_watch_guest_file_read(
            ISH_WATCH_GUEST_FILE_REPOSITORIES,
            buffer,
            sizeof(buffer));
    CHECK(length == (ssize_t) (sizeof(second) - 1) &&
            memcmp(buffer, second, sizeof(second) - 1) == 0 &&
            current == NULL &&
            no_published_children(),
            "连续公共读取返回第二次发布结果且再次释放锁");

    struct ish_apple_guest_file_request_v1 request =
            guest_file_request("/etc/agent/nested", 1001);
    CHECK(ish_apple_guest_file_mkdir(
            &request,
            0750,
            ISH_APPLE_GUEST_FILE_MKDIR_PARENTS) == 0,
            "公共 guest 文件接口递归创建目录");

    static const unsigned char original[] = "abcdef";
    request = guest_file_request(
            "/etc/agent/nested/example.txt", 1002);
    CHECK(ish_apple_guest_file_write(
            &request,
            original,
            sizeof(original) - 1,
            0640) == 0,
            "完整内容通过同目录临时文件原子发布");

    struct ish_apple_guest_file_info_v1 info;
    request = guest_file_request(
            "/etc/agent/nested/example.txt", 1003);
    CHECK(ish_apple_guest_file_stat(&request, &info) == 0 &&
            info.request_id == 1003 &&
            info.size == sizeof(original) - 1 &&
            (info.mode & 0777) == 0640,
            "stat 返回固定宽度元数据并保留新文件权限");

    uint32_t read_count = 0;
    uint64_t total_size = 0;
    int32_t eof = 0;
    memset(buffer, 0, sizeof(buffer));
    request = guest_file_request(
            "/etc/agent/nested/example.txt", 1004);
    CHECK(ish_apple_guest_file_read(
            &request,
            2,
            buffer,
            3,
            &read_count,
            &total_size,
            &eof) == 0 &&
            read_count == 3 &&
            total_size == sizeof(original) - 1 &&
            eof == 0 &&
            memcmp(buffer, "cde", 3) == 0,
            "read 使用 64 位 offset 分页且报告总长度");

    static const unsigned char replacement[] = "XY";
    request = guest_file_request(
            "/etc/agent/nested/example.txt", 1005);
    CHECK(ish_apple_guest_file_edit(
            &request,
            2,
            2,
            replacement,
            sizeof(replacement) - 1) == 0,
            "edit 流式复制未修改区间并原子替换");
    memset(buffer, 0, sizeof(buffer));
    request = guest_file_request(
            "/etc/agent/nested/example.txt", 1006);
    CHECK(ish_apple_guest_file_read(
            &request,
            0,
            buffer,
            sizeof(buffer),
            &read_count,
            &total_size,
            &eof) == 0 &&
            read_count == 6 && eof == 1 &&
            memcmp(buffer, "abXYef", 6) == 0,
            "edit 结果完整且读取到末尾");

    request = guest_file_request(
            "/etc/agent/nested/example.txt", 1007);
    CHECK(ish_apple_guest_file_copy(
            &request,
            "/etc/agent/nested/copied.txt") == 0,
            "copy 通过固定缓冲区原子复制普通文件");
    memset(buffer, 0, sizeof(buffer));
    request = guest_file_request(
            "/etc/agent/nested/copied.txt", 1008);
    CHECK(ish_apple_guest_file_read(
            &request,
            0,
            buffer,
            sizeof(buffer),
            &read_count,
            &total_size,
            &eof) == 0 &&
            read_count == 6 && eof == 1 &&
            memcmp(buffer, "abXYef", 6) == 0,
            "copy 结果与源文件一致");
    request = guest_file_request(
            "/etc/agent/nested/copied.txt", 1009);
    CHECK(ish_apple_guest_file_stat(&request, &info) == 0 &&
            (info.mode & 0777) == 0640 &&
            info.user_id == 0 && info.group_id == 0,
            "copy 新目标保留源文件权限与属主");
    static const unsigned char old_copy[] = "old";
    request = guest_file_request(
            "/etc/agent/nested/copied.txt", 1089);
    CHECK(ish_apple_guest_file_write(
            &request,
            old_copy,
            sizeof(old_copy) - 1,
            0640) == 0,
            "为 copy 中途失败准备不同内容的既有目标");
    ish_apple_guest_file_test_fail_copy_after(1, _EIO);
    request = guest_file_request(
            "/etc/agent/nested/example.txt", 1090);
    CHECK(ish_apple_guest_file_copy(
            &request,
            "/etc/agent/nested/copied.txt") < 0,
            "copy 写入部分临时文件后失败时不发布临时目标");
    memset(buffer, 0, sizeof(buffer));
    request = guest_file_request(
            "/etc/agent/nested/copied.txt", 1091);
    CHECK(ish_apple_guest_file_read(
            &request,
            0,
            buffer,
            sizeof(buffer),
            &read_count,
            &total_size,
            &eof) == 0 &&
            read_count == sizeof(old_copy) - 1 && eof == 1 &&
            memcmp(buffer, old_copy, sizeof(old_copy) - 1) == 0,
            "copy 中途失败后保留原目标内容");

    struct ish_apple_guest_file_directory_entry_v1 entries[2];
    uint64_t next_cursor = 0;
    uint32_t entry_count = 0;
    request = guest_file_request("/etc/agent/nested", 1010);
    CHECK(ish_apple_guest_file_list(
            &request,
            0,
            entries,
            2,
            &entry_count,
            &next_cursor,
            &eof) == 0 &&
            entry_count == 2 && eof == 1,
            "目录分页跳过点目录且 copy 失败未留下临时节点");

    request = guest_file_request(
            "/etc/agent/nested/example.txt", 1011);
    CHECK(ish_apple_guest_file_rename(
            &request,
            "/etc/agent/nested/renamed.txt") == 0,
            "rename 保持 guest 命名空间语义");
    request = guest_file_request("/etc/agent", 1012);
    CHECK(ish_apple_guest_file_remove(
            &request,
            ISH_APPLE_GUEST_FILE_REMOVE_RECURSIVE) == 0,
            "递归删除只遍历 guest 目录且删除完整子树");
    request = guest_file_request("/etc/agent", 1013);
    CHECK(ish_apple_guest_file_stat(&request, &info) == _ENOENT &&
            current == NULL && no_published_children(),
            "所有公共文件操作结束后释放 prepared task");

    request = guest_file_request("/", 1014);
    CHECK(ish_apple_guest_file_remove(
            &request,
            ISH_APPLE_GUEST_FILE_REMOVE_RECURSIVE) == 0,
            "递归删除 guest 根目录会清空内容并保留命名空间锚点");
    request = guest_file_request("/", 1015);
    CHECK(ish_apple_guest_file_stat(&request, &info) == 0 &&
            (info.mode & S_IFMT) == S_IFDIR,
            "清空后 guest 根目录仍可用于后续损坏检测与恢复");
    request = guest_file_request("/bin/sh", 1016);
    CHECK(ish_apple_guest_file_stat(&request, &info) == _ENOENT &&
            current == NULL && no_published_children(),
            "清空 guest 根目录确实删除系统内容并释放 prepared task");
    memset(buffer, 0, sizeof(buffer));
    CHECK(snprintf(
            path,
            sizeof(path),
            "%s/from-host.txt",
            fixture.documents) < (int) sizeof(path),
            "生成根目录清空后的宿主挂载检查路径");
    length = read_host_path(path, buffer, sizeof(buffer));
    CHECK(length == (ssize_t) (sizeof(shared_from_host) - 1) &&
            memcmp(buffer, shared_from_host, sizeof(shared_from_host) - 1) == 0,
            "清空 guest 根目录不会递归删除独立挂载中的用户数据");

    fixture.runtime_stopped = stop_runtime();
    CHECK(fixture.runtime_stopped, "测试结束时停止 PID 1");
    destroy_fixture(&fixture);

    if (failures != 0) {
        fprintf(stderr, "Watch guest 运行态测试共 %d 项失败\n", failures);
        return 1;
    }
    puts("Watch guest 生产公共入口运行态回归通过");
    return 0;
}
