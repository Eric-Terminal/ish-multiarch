#ifndef GUEST_AARCH64_LINUX_FUTEX_ABI_H
#define GUEST_AARCH64_LINUX_FUTEX_ABI_H

#include "guest/linux/futex-abi.h"

#define AARCH64_LINUX_FUTEX_WAITERS GUEST_LINUX_FUTEX_WAITERS
#define AARCH64_LINUX_FUTEX_OWNER_DIED GUEST_LINUX_FUTEX_OWNER_DIED
#define AARCH64_LINUX_FUTEX_TID_MASK GUEST_LINUX_FUTEX_TID_MASK

// robust list 节点地址的最低位标记 PI futex，其余位仍是 guest 地址。
#define AARCH64_LINUX_ROBUST_LIST_PI UINT64_C(0x1)
#define AARCH64_LINUX_ROBUST_LIST_MODIFIER_MASK UINT64_C(0x1)
#define AARCH64_LINUX_ROBUST_LIST_ADDRESS_MASK UINT64_C(0xfffffffffffffffe)
#define AARCH64_LINUX_ROBUST_LIST_LIMIT GUEST_LINUX_ROBUST_LIST_LIMIT

// wire 布局必须独立于 host 的 pointer、long 和 size_t，尤其是 arm64_32。
struct aarch64_linux_robust_list_head {
    qword_t next;
    sqword_t futex_offset;
    qword_t list_op_pending;
} __attribute__((packed, aligned(8)));

_Static_assert(sizeof(struct aarch64_linux_robust_list_head) == 24 &&
        _Alignof(struct aarch64_linux_robust_list_head) == 8,
        "AArch64 Linux robust_list_head 必须保持 24 字节并按 8 字节对齐");
_Static_assert(__builtin_offsetof(
                struct aarch64_linux_robust_list_head, next) == 0 &&
        __builtin_offsetof(
                struct aarch64_linux_robust_list_head, futex_offset) == 8 &&
        __builtin_offsetof(
                struct aarch64_linux_robust_list_head,
                list_op_pending) == 16,
        "AArch64 Linux robust_list_head 字段偏移必须保持 LP64 wire 布局");
_Static_assert(sizeof(((struct aarch64_linux_robust_list_head *) 0)->next) == 8 &&
        sizeof(((struct aarch64_linux_robust_list_head *) 0)->futex_offset) == 8 &&
        sizeof(((struct aarch64_linux_robust_list_head *) 0)->
                list_op_pending) == 8 &&
        (sqword_t) -1 < 0,
        "AArch64 Linux robust_list_head 必须使用固定宽度且 offset 必须有符号");
_Static_assert(sizeof(AARCH64_LINUX_FUTEX_WAITERS) == sizeof(dword_t) &&
        sizeof(AARCH64_LINUX_FUTEX_OWNER_DIED) == sizeof(dword_t) &&
        sizeof(AARCH64_LINUX_FUTEX_TID_MASK) == sizeof(dword_t) &&
        sizeof(AARCH64_LINUX_ROBUST_LIST_LIMIT) == sizeof(dword_t) &&
        sizeof(AARCH64_LINUX_ROBUST_LIST_PI) == sizeof(qword_t) &&
        sizeof(AARCH64_LINUX_ROBUST_LIST_MODIFIER_MASK) ==
                sizeof(qword_t) &&
        sizeof(AARCH64_LINUX_ROBUST_LIST_ADDRESS_MASK) == sizeof(qword_t),
        "futex 状态必须为 32 位，robust list 地址修饰符必须为 64 位");
_Static_assert((AARCH64_LINUX_FUTEX_WAITERS |
                AARCH64_LINUX_FUTEX_OWNER_DIED |
                AARCH64_LINUX_FUTEX_TID_MASK) == UINT32_MAX &&
        (AARCH64_LINUX_ROBUST_LIST_PI &
                AARCH64_LINUX_ROBUST_LIST_ADDRESS_MASK) == 0 &&
        AARCH64_LINUX_ROBUST_LIST_PI ==
                AARCH64_LINUX_ROBUST_LIST_MODIFIER_MASK,
        "futex owner 位域和 robust list 地址修饰符不得重叠");

#endif
