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

#define ISH_WATCH_SESSION_LIMIT 4

enum ish_watch_session_phase {
    ISH_WATCH_SESSION_STARTING = 1,
    ISH_WATCH_SESSION_RUNNING = 2,
    ISH_WATCH_SESSION_EXITED = 3,
};

struct ish_watch_session_status {
    int32_t phase;
    int32_t wait_status;
};

// root_data 与 socket_prefix 属于宿主文件系统。成功返回 0，失败返回负 Linux errno。
int ish_watch_runtime_start(
        const char *root_data,
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
// close 使句柄立即失效，并终止仍绑定该 PTY 的全部进程组。
int ish_watch_session_close(ish_watch_session_id session_id);

#ifdef ISH_APPLE_WATCH_RUNTIME_TESTING
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
#endif

#endif
