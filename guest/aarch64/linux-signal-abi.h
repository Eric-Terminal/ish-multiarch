#ifndef GUEST_AARCH64_LINUX_SIGNAL_ABI_H
#define GUEST_AARCH64_LINUX_SIGNAL_ABI_H

#include "misc.h"

#define AARCH64_LINUX_SA_NOCLDSTOP UINT64_C(0x00000001)
#define AARCH64_LINUX_SA_NOCLDWAIT UINT64_C(0x00000002)
#define AARCH64_LINUX_SA_SIGINFO UINT64_C(0x00000004)
#define AARCH64_LINUX_SA_UNSUPPORTED UINT64_C(0x00000400)
#define AARCH64_LINUX_SA_EXPOSE_TAGBITS UINT64_C(0x00000800)
#define AARCH64_LINUX_SA_RESTORER UINT64_C(0x04000000)
#define AARCH64_LINUX_SA_ONSTACK UINT64_C(0x08000000)
#define AARCH64_LINUX_SA_RESTART UINT64_C(0x10000000)
#define AARCH64_LINUX_SA_NODEFER UINT64_C(0x40000000)
#define AARCH64_LINUX_SA_RESETHAND UINT64_C(0x80000000)

#define AARCH64_LINUX_SA_SUPPORTED_FLAGS \
    (AARCH64_LINUX_SA_NOCLDSTOP | AARCH64_LINUX_SA_NOCLDWAIT | \
     AARCH64_LINUX_SA_SIGINFO | AARCH64_LINUX_SA_EXPOSE_TAGBITS | \
     AARCH64_LINUX_SA_RESTORER | AARCH64_LINUX_SA_ONSTACK | \
     AARCH64_LINUX_SA_RESTART | AARCH64_LINUX_SA_NODEFER | \
     AARCH64_LINUX_SA_RESETHAND)

struct aarch64_linux_sigaction {
    qword_t handler;
    qword_t flags;
    qword_t restorer;
    qword_t mask;
} __attribute__((packed, aligned(8)));

_Static_assert(sizeof(struct aarch64_linux_sigaction) == 32 &&
        _Alignof(struct aarch64_linux_sigaction) == 8,
        "AArch64 Linux sigaction ABI 必须固定为 32 字节且按 8 字节对齐");
_Static_assert(__builtin_offsetof(struct aarch64_linux_sigaction, handler) == 0 &&
        __builtin_offsetof(struct aarch64_linux_sigaction, flags) == 8 &&
        __builtin_offsetof(struct aarch64_linux_sigaction, restorer) == 16 &&
        __builtin_offsetof(struct aarch64_linux_sigaction, mask) == 24,
        "AArch64 Linux sigaction 字段偏移不正确");
_Static_assert(AARCH64_LINUX_SA_SUPPORTED_FLAGS == UINT64_C(0xdc000807) &&
        !(AARCH64_LINUX_SA_SUPPORTED_FLAGS & AARCH64_LINUX_SA_UNSUPPORTED),
        "AArch64 Linux sigaction 支持标志集合不正确");

#endif
