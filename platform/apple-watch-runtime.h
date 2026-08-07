#ifndef PLATFORM_APPLE_WATCH_RUNTIME_H
#define PLATFORM_APPLE_WATCH_RUNTIME_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

// Watch runtime 与现有 iSH 全局内核状态一致：每个宿主进程只能启动一次。
enum ish_watch_runtime_phase {
    ISH_WATCH_RUNTIME_IDLE = 0,
    ISH_WATCH_RUNTIME_PREPARING = 1,
    ISH_WATCH_RUNTIME_RUNNING = 2,
    ISH_WATCH_RUNTIME_STOPPED = 3,
    ISH_WATCH_RUNTIME_FAILED = 4,
};

typedef uint64_t ish_watch_session_id;

enum ish_watch_session_phase {
    ISH_WATCH_SESSION_STARTING = 1,
    ISH_WATCH_SESSION_RUNNING = 2,
    ISH_WATCH_SESSION_EXITED = 3,
};

struct ish_watch_session_status {
    int32_t phase;
    int32_t wait_status;
};

enum ish_watch_guest_file_id {
    ISH_WATCH_GUEST_FILE_REPOSITORIES = 1,
    ISH_WATCH_GUEST_FILE_APK_VERSION = 2,
};

// 公共调用方可据此分配完整读取缓冲；长度不包含额外的字符串终止符。
#define ISH_WATCH_REPOSITORIES_LIMIT 65730
#define ISH_WATCH_APK_VERSION_LIMIT 999

// root_data、documents_directory 与 socket_prefix 属于宿主文件系统。
// documents_directory 唯一挂载到 guest /mnt/shared。
// 成功返回 0，失败返回负 Linux errno。
int ish_watch_runtime_start(
        const char *root_data,
        const char *documents_directory,
        const char *socket_prefix,
        const char *hostname,
        const char *boot_command);

int ish_watch_runtime_current_phase(void);
int ish_watch_runtime_last_error(void);

// 这些兼容 API 只访问 PID 1 的启动 console；可见终端应使用 session API。
size_t ish_watch_runtime_read_output(
        void *buffer, size_t capacity, uint64_t *dropped_bytes);
ssize_t ish_watch_runtime_send_input(const void *bytes, size_t length);
int ish_watch_runtime_set_window_size(uint16_t columns, uint16_t rows);

// 只允许访问 Watch 管理的 APK 仓库与版本标记；路径不由调用方提供。
ssize_t ish_watch_guest_file_read(
        int32_t file_id, void *buffer, size_t capacity);
// remove_file 为 1 时 bytes 必须为 NULL 且 length 为 0。
int ish_watch_guest_file_replace(
        int32_t file_id,
        const void *bytes,
        size_t length,
        int remove_file);

// 每个 session 是同一 Linux runtime 中由 PID 1 派生的独立 PTY 子进程。
int ish_watch_session_create(
        const char *command,
        uint16_t columns,
        uint16_t rows,
        ish_watch_session_id *session_id);
int ish_watch_session_status(
        ish_watch_session_id session_id,
        struct ish_watch_session_status *status);
ssize_t ish_watch_session_read_output(
        ish_watch_session_id session_id,
        void *buffer,
        size_t capacity,
        uint64_t *dropped_bytes);
ssize_t ish_watch_session_send_input(
        ish_watch_session_id session_id,
        const void *bytes,
        size_t length);
int ish_watch_session_set_window_size(
        ish_watch_session_id session_id,
        uint16_t columns,
        uint16_t rows);
// cancel 终止控制终端进程组，但保留句柄供调用方 drain 输出和读取退出状态。
int ish_watch_session_cancel(ish_watch_session_id session_id);
// close 使句柄立即失效，并终止仍绑定该 PTY 的全部进程组。
int ish_watch_session_close(ish_watch_session_id session_id);

#ifdef ISH_APPLE_WATCH_RUNTIME_TESTING
enum ish_watch_runtime_test_directory_stage {
    ISH_WATCH_RUNTIME_TEST_DIRECTORY_AFTER_LSTAT = 1,
    ISH_WATCH_RUNTIME_TEST_DIRECTORY_AFTER_VALIDATION = 2,
    ISH_WATCH_RUNTIME_TEST_DIRECTORY_AFTER_RELEASE = 3,
};

typedef void (*ish_watch_runtime_test_directory_hook)(
        int32_t stage, const char *path, int directory_fd);

void ish_watch_runtime_test_set_directory_hook(
        ish_watch_runtime_test_directory_hook hook);
void ish_watch_runtime_test_append_output(const void *bytes, size_t length);
int ish_watch_runtime_test_add_session(
        int32_t phase, ish_watch_session_id *session_id);
void ish_watch_runtime_test_append_session_output(
        ish_watch_session_id session_id,
        const void *bytes,
        size_t length);
void ish_watch_runtime_test_mark_session_exited(
        ish_watch_session_id session_id,
        int32_t wait_status);
int ish_watch_runtime_test_recycled_transport(void);
int ish_watch_runtime_test_exit_ownership(void);
#endif

#endif
