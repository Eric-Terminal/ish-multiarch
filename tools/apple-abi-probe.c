#include <stddef.h>
#include <sys/types.h>

#include "asbestos/asbestos.h"
#include "asbestos/frame.h"
#include "fs/fd.h"
#include "guest/linux/futex-abi.h"
#include "sdk/iSHApple/Headers/iSHApple.h"

#ifndef EXPECTED_APPLE_WORD_BYTES
#error "必须声明 Apple 宿主 ABI 的预期字宽"
#endif

#define ASSERT_APPLE_OFFSET(type, field, expected) \
    _Static_assert(offsetof(type, field) == (expected), \
            #type "." #field " 的公共 ABI 偏移发生变化")
#define ASSERT_APPLE_ALIGNMENT(type, expected) \
    _Static_assert(_Alignof(type) == (expected), \
            #type " 的公共 ABI 对齐发生变化")

_Static_assert(sizeof(void *) == EXPECTED_APPLE_WORD_BYTES,
        "Apple 指针宽度与目标切片不一致");
typedef void (*apple_function_pointer)(void);
_Static_assert(sizeof(apple_function_pointer) == EXPECTED_APPLE_WORD_BYTES,
        "Apple 函数指针宽度与目标切片不一致");
_Static_assert(sizeof(long) == EXPECTED_APPLE_WORD_BYTES,
        "Apple long 宽度与目标切片不一致");
_Static_assert(sizeof(size_t) == EXPECTED_APPLE_WORD_BYTES,
        "Apple size_t 宽度与目标切片不一致");
_Static_assert(sizeof(ssize_t) == EXPECTED_APPLE_WORD_BYTES,
        "Apple ssize_t 宽度与目标切片不一致");
_Static_assert(sizeof(off_t) == 8,
        "Apple off_t 必须保留 64 位文件位置");
_Static_assert(sizeof(off_t_) == 8,
        "guest off_t_ 必须保留 64 位文件位置");
_Static_assert(sizeof(((struct fd *) 0)->offset) == 8,
        "fd 顺序位置不得跟随 arm64_32 宿主字宽收窄");
_Static_assert(sizeof(((struct cpu_state *) 0)->poked_ptr) ==
                EXPECTED_APPLE_WORD_BYTES,
        "CPU 宿主指针字段与目标切片不一致");
_Static_assert(sizeof(((struct fiber_frame *) 0)->last_block) ==
                EXPECTED_APPLE_WORD_BYTES,
        "fiber 宿主指针字段与目标切片不一致");
_Static_assert(sizeof(((struct fiber_frame *) 0)->ret_cache[0]) == 8,
        "fiber 返回缓存必须保持 64 位单元");
_Static_assert(sizeof(((struct fiber_block *) 0)->code[0]) == 8,
        "fiber 代码流必须保持 64 位单元");
_Static_assert(sizeof(struct guest_linux_futex_waitv) == 24 &&
        __builtin_offsetof(struct guest_linux_futex_waitv, address) == 8 &&
        __builtin_offsetof(struct guest_linux_futex_waitv, flags) == 16 &&
        __builtin_offsetof(struct guest_linux_futex_waitv, reserved) == 20,
        "futex_waitv wire 布局不得跟随 Apple 宿主字宽变化");
_Static_assert(sizeof(struct guest_linux_kernel_timespec) == 16 &&
        sizeof(((struct guest_linux_kernel_timespec *) 0)->sec) == 8 &&
        sizeof(((struct guest_linux_kernel_timespec *) 0)->nsec) == 8,
        "futex_waitv time64 不能在 arm64_32 宿主上收窄");
_Static_assert(sizeof(struct ish_apple_command_result_v1) == 72,
        "公共命令结果必须保持固定宽度布局");
ASSERT_APPLE_ALIGNMENT(struct ish_apple_command_result_v1, 8);
ASSERT_APPLE_OFFSET(struct ish_apple_command_result_v1, version, 0);
ASSERT_APPLE_OFFSET(
        struct ish_apple_command_result_v1, structure_size, 4);
ASSERT_APPLE_OFFSET(struct ish_apple_command_result_v1, request_id, 8);
ASSERT_APPLE_OFFSET(struct ish_apple_command_result_v1, reason, 16);
ASSERT_APPLE_OFFSET(struct ish_apple_command_result_v1, exit_code, 20);
ASSERT_APPLE_OFFSET(
        struct ish_apple_command_result_v1, termination_signal, 24);
ASSERT_APPLE_OFFSET(struct ish_apple_command_result_v1, error, 28);
ASSERT_APPLE_OFFSET(
        struct ish_apple_command_result_v1, stdout_bytes, 32);
ASSERT_APPLE_OFFSET(
        struct ish_apple_command_result_v1, stderr_bytes, 40);
ASSERT_APPLE_OFFSET(
        struct ish_apple_command_result_v1, elapsed_milliseconds, 48);
ASSERT_APPLE_OFFSET(struct ish_apple_command_result_v1, reserved, 56);

ASSERT_APPLE_OFFSET(struct ish_apple_runtime_spec_v1, version, 0);
ASSERT_APPLE_OFFSET(
        struct ish_apple_runtime_spec_v1, structure_size, 4);
ASSERT_APPLE_OFFSET(struct ish_apple_runtime_spec_v1, reserved, 8);
ASSERT_APPLE_OFFSET(struct ish_apple_runtime_spec_v1, root_data, 24);

ASSERT_APPLE_OFFSET(struct ish_apple_command_spec_v1, version, 0);
ASSERT_APPLE_OFFSET(
        struct ish_apple_command_spec_v1, structure_size, 4);
ASSERT_APPLE_OFFSET(
        struct ish_apple_command_spec_v1, timeout_milliseconds, 8);
ASSERT_APPLE_OFFSET(
        struct ish_apple_command_spec_v1, reserved_0, 12);
ASSERT_APPLE_OFFSET(
        struct ish_apple_command_spec_v1, request_id, 16);
ASSERT_APPLE_OFFSET(
        struct ish_apple_command_spec_v1, output_byte_limit, 24);
ASSERT_APPLE_OFFSET(struct ish_apple_command_spec_v1, reserved, 32);
ASSERT_APPLE_OFFSET(struct ish_apple_command_spec_v1, executable, 48);

ASSERT_APPLE_OFFSET(struct ish_apple_command_callbacks_v1, version, 0);
ASSERT_APPLE_OFFSET(
        struct ish_apple_command_callbacks_v1, structure_size, 4);
ASSERT_APPLE_OFFSET(struct ish_apple_command_callbacks_v1, context, 8);

#if EXPECTED_APPLE_WORD_BYTES == 4
#ifndef __ILP32__
#error "watchOS arm64_32 必须使用 ILP32 ABI"
#endif
#ifndef __ARM64_ARCH_8_32__
#error "watchOS arm64_32 必须声明 ARM64 32 位宿主 ABI"
#endif
_Static_assert(sizeof(struct ish_apple_runtime_spec_v1) == 48,
        "arm64_32 公共 runtime 配置布局漂移");
ASSERT_APPLE_ALIGNMENT(struct ish_apple_runtime_spec_v1, 8);
ASSERT_APPLE_OFFSET(
        struct ish_apple_runtime_spec_v1, shared_directory, 28);
ASSERT_APPLE_OFFSET(
        struct ish_apple_runtime_spec_v1, socket_prefix, 32);
ASSERT_APPLE_OFFSET(struct ish_apple_runtime_spec_v1, hostname, 36);
ASSERT_APPLE_OFFSET(
        struct ish_apple_runtime_spec_v1, boot_command, 40);
_Static_assert(sizeof(struct ish_apple_command_spec_v1) == 72,
        "arm64_32 公共命令配置布局漂移");
ASSERT_APPLE_ALIGNMENT(struct ish_apple_command_spec_v1, 8);
ASSERT_APPLE_OFFSET(
        struct ish_apple_command_spec_v1, arguments, 52);
ASSERT_APPLE_OFFSET(
        struct ish_apple_command_spec_v1, environment, 56);
ASSERT_APPLE_OFFSET(
        struct ish_apple_command_spec_v1, working_directory, 60);
ASSERT_APPLE_OFFSET(
        struct ish_apple_command_spec_v1, argument_count, 64);
ASSERT_APPLE_OFFSET(
        struct ish_apple_command_spec_v1, environment_count, 68);
_Static_assert(sizeof(struct ish_apple_command_callbacks_v1) == 40,
        "arm64_32 公共命令回调布局漂移");
ASSERT_APPLE_ALIGNMENT(struct ish_apple_command_callbacks_v1, 8);
ASSERT_APPLE_OFFSET(
        struct ish_apple_command_callbacks_v1, stream, 12);
ASSERT_APPLE_OFFSET(
        struct ish_apple_command_callbacks_v1, completed, 16);
ASSERT_APPLE_OFFSET(
        struct ish_apple_command_callbacks_v1, reserved, 24);
#elif EXPECTED_APPLE_WORD_BYTES == 8
#ifdef __ILP32__
#error "Apple arm64 切片不得使用 ILP32 ABI"
#endif
_Static_assert(sizeof(struct ish_apple_runtime_spec_v1) == 64,
        "LP64 公共 runtime 配置布局漂移");
ASSERT_APPLE_ALIGNMENT(struct ish_apple_runtime_spec_v1, 8);
ASSERT_APPLE_OFFSET(
        struct ish_apple_runtime_spec_v1, shared_directory, 32);
ASSERT_APPLE_OFFSET(
        struct ish_apple_runtime_spec_v1, socket_prefix, 40);
ASSERT_APPLE_OFFSET(struct ish_apple_runtime_spec_v1, hostname, 48);
ASSERT_APPLE_OFFSET(
        struct ish_apple_runtime_spec_v1, boot_command, 56);
_Static_assert(sizeof(struct ish_apple_command_spec_v1) == 88,
        "LP64 公共命令配置布局漂移");
ASSERT_APPLE_ALIGNMENT(struct ish_apple_command_spec_v1, 8);
ASSERT_APPLE_OFFSET(
        struct ish_apple_command_spec_v1, arguments, 56);
ASSERT_APPLE_OFFSET(
        struct ish_apple_command_spec_v1, environment, 64);
ASSERT_APPLE_OFFSET(
        struct ish_apple_command_spec_v1, working_directory, 72);
ASSERT_APPLE_OFFSET(
        struct ish_apple_command_spec_v1, argument_count, 80);
ASSERT_APPLE_OFFSET(
        struct ish_apple_command_spec_v1, environment_count, 84);
_Static_assert(sizeof(struct ish_apple_command_callbacks_v1) == 48,
        "LP64 公共命令回调布局漂移");
ASSERT_APPLE_ALIGNMENT(struct ish_apple_command_callbacks_v1, 8);
ASSERT_APPLE_OFFSET(
        struct ish_apple_command_callbacks_v1, stream, 16);
ASSERT_APPLE_OFFSET(
        struct ish_apple_command_callbacks_v1, completed, 24);
ASSERT_APPLE_OFFSET(
        struct ish_apple_command_callbacks_v1, reserved, 32);
#else
#error "Apple 门禁只支持 4 或 8 字节宿主字宽"
#endif

#undef ASSERT_APPLE_ALIGNMENT
#undef ASSERT_APPLE_OFFSET
