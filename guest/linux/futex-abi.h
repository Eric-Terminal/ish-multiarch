#ifndef GUEST_LINUX_FUTEX_ABI_H
#define GUEST_LINUX_FUTEX_ABI_H

#include "misc.h"

// futex owner word 在所有已支持的 Linux guest ABI 中都固定为 32 位。
#define GUEST_LINUX_FUTEX_WAITERS UINT32_C(0x80000000)
#define GUEST_LINUX_FUTEX_OWNER_DIED UINT32_C(0x40000000)
#define GUEST_LINUX_FUTEX_TID_MASK UINT32_C(0x3fffffff)
#define GUEST_LINUX_ROBUST_LIST_LIMIT UINT32_C(2048)

// futex_waitv 在 32/64 位 Linux guest 上共用同一固定宽度 wire ABI。
#define GUEST_LINUX_FUTEX_WAITV_MAX UINT32_C(128)
#define GUEST_LINUX_FUTEX2_SIZE_U32 UINT32_C(0x02)
#define GUEST_LINUX_FUTEX_PRIVATE_FLAG UINT32_C(0x80)
#define GUEST_LINUX_FUTEX_WAITV_SUPPORTED_FLAGS \
    (GUEST_LINUX_FUTEX2_SIZE_U32 | GUEST_LINUX_FUTEX_PRIVATE_FLAG)

struct guest_linux_futex_waitv {
    qword_t value;
    qword_t address;
    dword_t flags;
    dword_t reserved;
} __attribute__((packed, aligned(8)));

// i386 的 futex_waitv 也使用 time64 线协议，不能退回旧 32 位 timespec。
struct guest_linux_kernel_timespec {
    sqword_t sec;
    sqword_t nsec;
} __attribute__((packed, aligned(8)));

// i386 robust list 指针的最低位标记 PI futex，其余 31 位是 guest 地址。
#define I386_LINUX_ROBUST_LIST_PI UINT32_C(0x1)
#define I386_LINUX_ROBUST_LIST_MODIFIER_MASK UINT32_C(0x1)
#define I386_LINUX_ROBUST_LIST_ADDRESS_MASK UINT32_C(0xfffffffe)

// wire 布局必须独立于 host 的 pointer、long 和 size_t。
struct i386_linux_robust_list_head {
    dword_t next;
    sdword_t futex_offset;
    dword_t list_op_pending;
} __attribute__((packed, aligned(4)));

_Static_assert(sizeof(struct i386_linux_robust_list_head) == 12 &&
        _Alignof(struct i386_linux_robust_list_head) == 4,
        "i386 Linux robust_list_head 必须保持 12 字节并按 4 字节对齐");
_Static_assert(sizeof(struct guest_linux_futex_waitv) == 24 &&
        _Alignof(struct guest_linux_futex_waitv) == 8 &&
        __builtin_offsetof(struct guest_linux_futex_waitv, value) == 0 &&
        __builtin_offsetof(struct guest_linux_futex_waitv, address) == 8 &&
        __builtin_offsetof(struct guest_linux_futex_waitv, flags) == 16 &&
        __builtin_offsetof(struct guest_linux_futex_waitv, reserved) == 20,
        "Linux futex_waitv 元素必须保持 24 字节固定 wire 布局");
_Static_assert(sizeof(struct guest_linux_kernel_timespec) == 16 &&
        _Alignof(struct guest_linux_kernel_timespec) == 8 &&
        __builtin_offsetof(struct guest_linux_kernel_timespec, sec) == 0 &&
        __builtin_offsetof(struct guest_linux_kernel_timespec, nsec) == 8 &&
        (sqword_t) -1 < 0,
        "Linux __kernel_timespec 必须由两个连续的有符号 64 位字段组成");
_Static_assert(GUEST_LINUX_FUTEX_WAITV_MAX == UINT32_C(128) &&
        (GUEST_LINUX_FUTEX_WAITV_SUPPORTED_FLAGS &
                ~UINT32_C(0x82)) == 0,
        "futex_waitv 上限和当前支持标志必须落在 Linux UAPI 范围内");
_Static_assert(__builtin_offsetof(
                struct i386_linux_robust_list_head, next) == 0 &&
        __builtin_offsetof(
                struct i386_linux_robust_list_head, futex_offset) == 4 &&
        __builtin_offsetof(
                struct i386_linux_robust_list_head,
                list_op_pending) == 8,
        "i386 Linux robust_list_head 字段偏移必须保持 ILP32 wire 布局");
_Static_assert(sizeof(((struct i386_linux_robust_list_head *) 0)->next) == 4 &&
        sizeof(((struct i386_linux_robust_list_head *) 0)->futex_offset) == 4 &&
        sizeof(((struct i386_linux_robust_list_head *) 0)->
                list_op_pending) == 4 &&
        (sdword_t) -1 < 0,
        "i386 Linux robust_list_head 必须使用固定宽度且 offset 必须有符号");
_Static_assert(sizeof(GUEST_LINUX_FUTEX_WAITERS) == sizeof(dword_t) &&
        sizeof(GUEST_LINUX_FUTEX_OWNER_DIED) == sizeof(dword_t) &&
        sizeof(GUEST_LINUX_FUTEX_TID_MASK) == sizeof(dword_t) &&
        sizeof(GUEST_LINUX_ROBUST_LIST_LIMIT) == sizeof(dword_t),
        "Linux futex 状态与 robust list 上限必须保持 32 位");
_Static_assert((GUEST_LINUX_FUTEX_WAITERS |
                GUEST_LINUX_FUTEX_OWNER_DIED |
                GUEST_LINUX_FUTEX_TID_MASK) == UINT32_MAX &&
        (I386_LINUX_ROBUST_LIST_PI &
                I386_LINUX_ROBUST_LIST_ADDRESS_MASK) == 0 &&
        I386_LINUX_ROBUST_LIST_PI ==
                I386_LINUX_ROBUST_LIST_MODIFIER_MASK,
        "futex owner 位域和 i386 robust list 地址修饰符不得重叠");

#endif
