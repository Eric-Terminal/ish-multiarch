#include <stddef.h>
#include <sys/types.h>

#include "asbestos/asbestos.h"
#include "asbestos/frame.h"
#include "fs/fd.h"
#include "guest/linux/futex-abi.h"

#ifndef EXPECTED_APPLE_WORD_BYTES
#error "必须声明 Apple 宿主 ABI 的预期字宽"
#endif

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

#if EXPECTED_APPLE_WORD_BYTES == 4
#ifndef __ILP32__
#error "watchOS arm64_32 必须使用 ILP32 ABI"
#endif
#ifndef __ARM64_ARCH_8_32__
#error "watchOS arm64_32 必须声明 ARM64 32 位宿主 ABI"
#endif
#elif EXPECTED_APPLE_WORD_BYTES == 8
#ifdef __ILP32__
#error "Apple arm64 切片不得使用 ILP32 ABI"
#endif
#else
#error "Apple 门禁只支持 4 或 8 字节宿主字宽"
#endif
