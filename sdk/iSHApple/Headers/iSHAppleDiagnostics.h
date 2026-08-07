#ifndef ISH_APPLE_DIAGNOSTICS_H
#define ISH_APPLE_DIAGNOSTICS_H

#include "iSHAppleDefines.h"

#define ISH_APPLE_DIAGNOSTIC_SCOPE_RUNTIME UINT32_C(1)
#define ISH_APPLE_DIAGNOSTIC_SCOPE_COMMAND UINT32_C(2)
#define ISH_APPLE_DIAGNOSTIC_SCOPE_TERMINAL UINT32_C(3)
#define ISH_APPLE_DIAGNOSTIC_SCOPE_GUEST_FILE UINT32_C(4)

#define ISH_APPLE_DIAGNOSTIC_CATEGORY_INSTRUCTION UINT32_C(1)
#define ISH_APPLE_DIAGNOSTIC_CATEGORY_SYSCALL UINT32_C(2)
#define ISH_APPLE_DIAGNOSTIC_CATEGORY_FILESYSTEM UINT32_C(3)
#define ISH_APPLE_DIAGNOSTIC_CATEGORY_RUNTIME UINT32_C(4)

#define ISH_APPLE_DIAGNOSTIC_INSTRUCTION_UNDEFINED UINT32_C(1)
#define ISH_APPLE_DIAGNOSTIC_SYSCALL_UNSUPPORTED UINT32_C(1)
#define ISH_APPLE_DIAGNOSTIC_FILESYSTEM_UNSUPPORTED UINT32_C(1)
#define ISH_APPLE_DIAGNOSTIC_RUNTIME_START_FAILED UINT32_C(1)
#define ISH_APPLE_DIAGNOSTIC_RUNTIME_BRIDGE_FAILED UINT32_C(2)

#define ISH_APPLE_DIAGNOSTIC_ARCHITECTURE_AARCH64 UINT32_C(1)

#define ISH_APPLE_DIAGNOSTIC_BACKEND_UNKNOWN UINT32_C(0)
#define ISH_APPLE_DIAGNOSTIC_BACKEND_C UINT32_C(1)
#define ISH_APPLE_DIAGNOSTIC_BACKEND_THREADED UINT32_C(2)

#define ISH_APPLE_DIAGNOSTIC_SYSCALL_NAME_BYTES_MAX UINT32_C(32)
#define ISH_APPLE_DIAGNOSTIC_BUILD_IDENTITY_BYTES_MAX UINT32_C(64)

ISH_APPLE_EXTERN_C_BEGIN

/*
 * 所有字段均为固定宽度，arm64_32 与 LP64 共用同一布局。linux_error 使用
 * iSHAppleLinuxErrno.h 的负 Linux errno；不适用的字段保持 0。
 */
struct ish_apple_diagnostic_event_v1 {
    uint32_t version;
    uint32_t structure_size;
    uint32_t category;
    uint32_t kind;
    uint32_t scope;
    uint32_t architecture;
    uint32_t backend;
    int32_t linux_error;
    int32_t signal;
    uint32_t opcode;
    uint64_t sequence;
    uint64_t request_id;
    uint64_t guest_pc;
    uint64_t syscall_number;
    char syscall_name[ISH_APPLE_DIAGNOSTIC_SYSCALL_NAME_BYTES_MAX];
    char build_identity[ISH_APPLE_DIAGNOSTIC_BUILD_IDENTITY_BYTES_MAX];
    uint64_t reserved[4];
};

/*
 * runtime scope 使用 request_id == 0；其他 scope 要求非零 request_id。
 * events == NULL 且 capacity == 0 时只查询待消费数量。提供缓冲区时会原子
 * 移除最多 capacity 条匹配事件；其他请求的事件保持原顺序。
 */
ISH_APPLE_API int32_t ish_apple_diagnostics_drain(
        uint32_t scope,
        uint64_t request_id,
        struct ish_apple_diagnostic_event_v1 *ISH_APPLE_NULLABLE events,
        uint32_t capacity,
        uint32_t *ISH_APPLE_NONNULL count_out);

/* 清理尚未消费的匹配事件，并返回实际清理数量。 */
ISH_APPLE_API int32_t ish_apple_diagnostics_clear(
        uint32_t scope,
        uint64_t request_id,
        uint32_t *ISH_APPLE_NONNULL cleared_out);

ISH_APPLE_EXTERN_C_END

#endif
