#ifndef ISH_APPLE_TERMINAL_H
#define ISH_APPLE_TERMINAL_H

#include "iSHAppleDefines.h"

#define ISH_APPLE_TERMINAL_IO_BYTES_MAX UINT32_C(2147483647)

#define ISH_APPLE_TERMINAL_COMPLETION_EXITED INT32_C(1)
#define ISH_APPLE_TERMINAL_COMPLETION_SIGNALED INT32_C(2)
#define ISH_APPLE_TERMINAL_COMPLETION_CANCELLED INT32_C(3)
#define ISH_APPLE_TERMINAL_COMPLETION_RUNTIME_FAILURE INT32_C(4)

ISH_APPLE_EXTERN_C_BEGIN

typedef struct ish_apple_terminal_session ish_apple_terminal_session;

/*
 * arguments 必须包含 argv[0]；environment 是完整 guest 环境，不继承宿主。
 * columns/rows 是初始 PTY 尺寸，后续 resize 会触发 Linux SIGWINCH 语义。
 */
struct ish_apple_terminal_spec_v1 {
    uint32_t version;
    uint32_t structure_size;
    uint16_t columns;
    uint16_t rows;
    uint32_t reserved_0;
    uint64_t terminal_id;
    uint64_t reserved[2];
    const char *ISH_APPLE_NONNULL executable;
    const char *ISH_APPLE_NONNULL const *ISH_APPLE_NONNULL arguments;
    const char *ISH_APPLE_NONNULL const *ISH_APPLE_NULLABLE environment;
    const char *ISH_APPLE_NULLABLE working_directory;
    uint32_t argument_count;
    uint32_t environment_count;
};

struct ish_apple_terminal_result_v1 {
    uint32_t version;
    uint32_t structure_size;
    uint64_t terminal_id;
    int32_t reason;
    int32_t exit_code;
    int32_t termination_signal;
    int32_t error;
    uint64_t output_bytes;
    uint64_t dropped_bytes;
    uint64_t elapsed_milliseconds;
    uint64_t reserved[2];
};

ISH_APPLE_API int32_t ish_apple_terminal_session_start(
        const struct ish_apple_terminal_spec_v1 *ISH_APPLE_NONNULL spec,
        ish_apple_terminal_session *ISH_APPLE_NULLABLE *ISH_APPLE_NONNULL session_out);
ISH_APPLE_API ish_apple_terminal_session *ISH_APPLE_NULLABLE
ish_apple_terminal_session_retain(
        ish_apple_terminal_session *ISH_APPLE_NULLABLE session);
ISH_APPLE_API void ish_apple_terminal_session_release(
        ish_apple_terminal_session *ISH_APPLE_NULLABLE session);

/*
 * read_output 是非阻塞 raw PTY 读取；没有可用字节时 count_out 为 0。
 * dropped_out 返回底层终端在本次读取前因内存压力丢弃的字节数。
 */
ISH_APPLE_API int32_t ish_apple_terminal_session_read_output(
        ish_apple_terminal_session *ISH_APPLE_NONNULL session,
        void *ISH_APPLE_NULLABLE bytes,
        uint32_t capacity,
        uint32_t *ISH_APPLE_NONNULL count_out,
        uint64_t *ISH_APPLE_NONNULL dropped_out);
ISH_APPLE_API int32_t ish_apple_terminal_session_write_input(
        ish_apple_terminal_session *ISH_APPLE_NONNULL session,
        const void *ISH_APPLE_NULLABLE bytes,
        uint32_t length,
        uint32_t *ISH_APPLE_NONNULL accepted_out);
ISH_APPLE_API int32_t ish_apple_terminal_session_finish_input(
        ish_apple_terminal_session *ISH_APPLE_NONNULL session);
ISH_APPLE_API int32_t ish_apple_terminal_session_resize(
        ish_apple_terminal_session *ISH_APPLE_NONNULL session,
        uint16_t columns,
        uint16_t rows);
ISH_APPLE_API int32_t ish_apple_terminal_session_interrupt(
        ish_apple_terminal_session *ISH_APPLE_NONNULL session);
ISH_APPLE_API int32_t ish_apple_terminal_session_cancel(
        ish_apple_terminal_session *ISH_APPLE_NONNULL session);

/*
 * 终端仍运行时返回 ISH_APPLE_LINUX_EAGAIN；完成后可重复取得同一结果。
 * 调用方应先 drain raw output，再读取最终结果。
 */
ISH_APPLE_API int32_t ish_apple_terminal_session_copy_result(
        ish_apple_terminal_session *ISH_APPLE_NONNULL session,
        struct ish_apple_terminal_result_v1 *ISH_APPLE_NONNULL result_out);

ISH_APPLE_EXTERN_C_END

#endif
