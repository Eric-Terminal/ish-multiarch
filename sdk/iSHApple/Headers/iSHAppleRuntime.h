#ifndef ISH_APPLE_RUNTIME_H
#define ISH_APPLE_RUNTIME_H

#include "iSHAppleDefines.h"
#include "iSHAppleMount.h"

#define ISH_APPLE_RUNTIME_PHASE_IDLE INT32_C(0)
#define ISH_APPLE_RUNTIME_PHASE_PREPARING INT32_C(1)
#define ISH_APPLE_RUNTIME_PHASE_RUNNING INT32_C(2)
#define ISH_APPLE_RUNTIME_PHASE_STOPPED INT32_C(3)
#define ISH_APPLE_RUNTIME_PHASE_FAILED INT32_C(4)

#define ISH_APPLE_RUNTIME_SOCKET_PREFIX_BYTES_MAX UINT32_C(82)
#define ISH_APPLE_RUNTIME_HOSTNAME_BYTES_MAX UINT32_C(64)
#define ISH_APPLE_RUNTIME_BOOT_COMMAND_BYTES_MAX UINT32_C(4095)

#define ISH_APPLE_RUNTIME_CAPABILITY_PTY UINT64_C(1)
#define ISH_APPLE_RUNTIME_CAPABILITY_LIVE_MOUNTS UINT64_C(2)
#define ISH_APPLE_RUNTIME_CAPABILITY_DIAGNOSTICS UINT64_C(4)
#define ISH_APPLE_RUNTIME_CAPABILITY_GUEST_FILES UINT64_C(8)

#define ISH_APPLE_RUNTIME_ARCHITECTURE_AARCH64 UINT32_C(1)

#define ISH_APPLE_RUNTIME_BACKEND_UNKNOWN UINT32_C(0)
#define ISH_APPLE_RUNTIME_BACKEND_C UINT32_C(1)
#define ISH_APPLE_RUNTIME_BACKEND_THREADED UINT32_C(2)

ISH_APPLE_EXTERN_C_BEGIN

struct ish_apple_runtime_spec_v1 {
    uint32_t version;
    uint32_t structure_size;
    uint64_t reserved[2];
    const char *ISH_APPLE_NONNULL root_data;
    const char *ISH_APPLE_NONNULL shared_directory;
    const char *ISH_APPLE_NONNULL socket_prefix;
    const char *ISH_APPLE_NONNULL hostname;
    const char *ISH_APPLE_NONNULL boot_command;
};

struct ish_apple_runtime_spec_v2 {
    uint32_t version;
    uint32_t structure_size;
    uint64_t reserved[2];
    const char *ISH_APPLE_NONNULL root_data;
    const char *ISH_APPLE_NONNULL shared_directory;
    const char *ISH_APPLE_NONNULL socket_prefix;
    const char *ISH_APPLE_NONNULL hostname;
    const char *ISH_APPLE_NONNULL boot_command;
    const struct ish_apple_mount_spec_v1 *ISH_APPLE_NULLABLE mounts;
    uint32_t mount_count;
    uint32_t reserved_0;
};

/* runtime 进入可接受作业的 RUNNING 状态后，此固定宽度快照保持不变。 */
struct ish_apple_runtime_capabilities_v1 {
    uint32_t version;
    uint32_t structure_size;
    uint64_t feature_flags;
    uint32_t guest_architecture;
    uint32_t backend;
    uint32_t public_abi_version;
    uint32_t reserved_0;
    uint64_t reserved[4];
};

/*
 * 每个宿主进程只能启动一个 runtime。所有路径在返回前复制或打开，成功后
 * 同一 Linux runtime 同时服务可见终端和结构化命令。
 */
ISH_APPLE_API int32_t ish_apple_runtime_start(
        const struct ish_apple_runtime_spec_v1 *ISH_APPLE_NONNULL spec);
/* v2 会在 PID 1 接受作业前原子注册并挂载 mounts。 */
ISH_APPLE_API int32_t ish_apple_runtime_start_v2(
        const struct ish_apple_runtime_spec_v2 *ISH_APPLE_NONNULL spec);
ISH_APPLE_API int32_t ish_apple_runtime_current_phase(void);
ISH_APPLE_API int32_t ish_apple_runtime_last_error(void);
ISH_APPLE_API int32_t ish_apple_runtime_copy_capabilities(
        struct ish_apple_runtime_capabilities_v1 *ISH_APPLE_NULLABLE capabilities_out);

ISH_APPLE_EXTERN_C_END

#endif
