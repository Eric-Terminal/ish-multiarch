#ifndef ISH_APPLE_COMMAND_H
#define ISH_APPLE_COMMAND_H

#include "iSHAppleDefines.h"

#define ISH_APPLE_COMMAND_ACTIVE_SESSION_MAX UINT32_C(4)
#define ISH_APPLE_COMMAND_ARGUMENT_COUNT_MAX UINT32_C(4096)
#define ISH_APPLE_COMMAND_ARGUMENT_BYTES_MAX UINT32_C(131072)
#define ISH_APPLE_COMMAND_PATH_BYTES_MAX UINT32_C(4096)
#define ISH_APPLE_COMMAND_OUTPUT_CHUNK_MAX UINT32_C(16384)
#define ISH_APPLE_COMMAND_STDIN_WRITE_BYTES_MAX UINT32_C(2147483647)
#define ISH_APPLE_COMMAND_OUTPUT_BYTES_DEFAULT UINT64_C(8388608)
#define ISH_APPLE_COMMAND_OUTPUT_BYTES_MAX UINT64_C(67108864)
#define ISH_APPLE_COMMAND_TIMEOUT_MS_DEFAULT UINT32_C(300000)
#define ISH_APPLE_COMMAND_TIMEOUT_MS_MAX UINT32_C(3600000)

#define ISH_APPLE_COMMAND_STREAM_STDOUT UINT32_C(1)
#define ISH_APPLE_COMMAND_STREAM_STDERR UINT32_C(2)

#define ISH_APPLE_COMMAND_COMPLETION_EXITED INT32_C(1)
#define ISH_APPLE_COMMAND_COMPLETION_SIGNALED INT32_C(2)
#define ISH_APPLE_COMMAND_COMPLETION_CANCELLED INT32_C(3)
#define ISH_APPLE_COMMAND_COMPLETION_TIMED_OUT INT32_C(4)
#define ISH_APPLE_COMMAND_COMPLETION_OUTPUT_LIMIT INT32_C(5)
#define ISH_APPLE_COMMAND_COMPLETION_RUNTIME_FAILURE INT32_C(6)

ISH_APPLE_EXTERN_C_BEGIN

typedef struct ish_apple_command_session ish_apple_command_session;

/*
 * arguments 必须包含 argv[0]；environment 是完整 guest 环境，不继承宿主。
 * timeout_milliseconds 和 output_byte_limit 为 0 时分别使用公共默认值。
 */
struct ish_apple_command_spec_v1 {
    uint32_t version;
    uint32_t structure_size;
    uint32_t timeout_milliseconds;
    uint32_t reserved_0;
    uint64_t request_id;
    uint64_t output_byte_limit;
    uint64_t reserved[2];
    const char *ISH_APPLE_NONNULL executable;
    const char *ISH_APPLE_NONNULL const *ISH_APPLE_NONNULL arguments;
    const char *ISH_APPLE_NONNULL const *ISH_APPLE_NULLABLE environment;
    const char *ISH_APPLE_NULLABLE working_directory;
    uint32_t argument_count;
    uint32_t environment_count;
};

struct ish_apple_command_result_v1 {
    uint32_t version;
    uint32_t structure_size;
    uint64_t request_id;
    int32_t reason;
    int32_t exit_code;
    int32_t termination_signal;
    int32_t error;
    uint64_t stdout_bytes;
    uint64_t stderr_bytes;
    uint64_t elapsed_milliseconds;
    uint64_t reserved[2];
};

/*
 * 同一 session 的回调不会重叠，不持有 session 内部锁。bytes 仅在当前回调
 * 返回前有效；length 为 0 时 terminal_error 描述该流的结束原因。
 */
typedef void (*ish_apple_command_stream_callback)(
        void *ISH_APPLE_NULLABLE context,
        ish_apple_command_session *ISH_APPLE_NONNULL session,
        uint64_t request_id,
        uint32_t stream,
        const void *ISH_APPLE_NULLABLE bytes,
        uint32_t length,
        int32_t terminal_error);

/*
 * 两路流结束后只调用一次。result 仅在回调返回前有效；需要保存时复制结构。
 */
typedef void (*ish_apple_command_completion_callback)(
        void *ISH_APPLE_NULLABLE context,
        ish_apple_command_session *ISH_APPLE_NONNULL session,
        const struct ish_apple_command_result_v1 *ISH_APPLE_NONNULL result);

struct ish_apple_command_callbacks_v1 {
    uint32_t version;
    uint32_t structure_size;
    void *ISH_APPLE_NULLABLE context;
    ish_apple_command_stream_callback ISH_APPLE_NONNULL stream;
    ish_apple_command_completion_callback ISH_APPLE_NONNULL completed;
    uint64_t reserved[2];
};

/*
 * 成功返回一个调用方引用。回调可能在线程上早于 start 返回，但开始回调前
 * session_out 已经写入。失败返回负 Linux errno，且不发布任何句柄或回调。
 */
ISH_APPLE_API int32_t ish_apple_command_session_start(
        const struct ish_apple_command_spec_v1 *ISH_APPLE_NONNULL spec,
        const struct ish_apple_command_callbacks_v1 *ISH_APPLE_NONNULL callbacks,
        ish_apple_command_session *ISH_APPLE_NULLABLE *ISH_APPLE_NONNULL session_out);

ISH_APPLE_API ish_apple_command_session *ISH_APPLE_NULLABLE
ish_apple_command_session_retain(
        ish_apple_command_session *ISH_APPLE_NULLABLE session);
ISH_APPLE_API void ish_apple_command_session_release(
        ish_apple_command_session *ISH_APPLE_NULLABLE session);

/*
 * stdin 是非阻塞管道；成功时 accepted_out 可小于 length。
 * ISH_APPLE_LINUX_EAGAIN 表示调用方应稍后重试；超过跨 ABI 固定的
 * ISH_APPLE_COMMAND_STDIN_WRITE_BYTES_MAX 则返回
 * ISH_APPLE_LINUX_EMSGSIZE。close_stdin 与 cancel 可重复调用。
 */
ISH_APPLE_API int32_t ish_apple_command_session_write_stdin(
        ish_apple_command_session *ISH_APPLE_NONNULL session,
        const void *ISH_APPLE_NULLABLE bytes,
        uint32_t length,
        uint32_t *ISH_APPLE_NONNULL accepted_out);
ISH_APPLE_API int32_t ish_apple_command_session_close_stdin(
        ish_apple_command_session *ISH_APPLE_NONNULL session);
ISH_APPLE_API int32_t ish_apple_command_session_interrupt(
        ish_apple_command_session *ISH_APPLE_NONNULL session);
ISH_APPLE_API int32_t ish_apple_command_session_cancel(
        ish_apple_command_session *ISH_APPLE_NONNULL session);

/*
 * wait 可重复调用并复制相同结果。任一回调内同步 wait 返回
 * ISH_APPLE_LINUX_EDEADLK。
 */
ISH_APPLE_API int32_t ish_apple_command_session_wait(
        ish_apple_command_session *ISH_APPLE_NONNULL session,
        struct ish_apple_command_result_v1 *ISH_APPLE_NONNULL result_out);

ISH_APPLE_EXTERN_C_END

#endif
