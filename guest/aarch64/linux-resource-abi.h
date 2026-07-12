#ifndef GUEST_AARCH64_LINUX_RESOURCE_ABI_H
#define GUEST_AARCH64_LINUX_RESOURCE_ABI_H

#include "misc.h"

struct aarch64_linux_timeval {
    sqword_t sec;
    sqword_t usec;
} __attribute__((packed, aligned(8)));

struct aarch64_linux_rusage {
    struct aarch64_linux_timeval utime;
    struct aarch64_linux_timeval stime;
    sqword_t maxrss;
    sqword_t ixrss;
    sqword_t idrss;
    sqword_t isrss;
    sqword_t minflt;
    sqword_t majflt;
    sqword_t nswap;
    sqword_t inblock;
    sqword_t oublock;
    sqword_t msgsnd;
    sqword_t msgrcv;
    sqword_t nsignals;
    sqword_t nvcsw;
    sqword_t nivcsw;
} __attribute__((packed, aligned(8)));

_Static_assert(sizeof(struct aarch64_linux_timeval) == 16 &&
        _Alignof(struct aarch64_linux_timeval) == 8 &&
        __builtin_offsetof(struct aarch64_linux_timeval, sec) == 0 &&
        __builtin_offsetof(struct aarch64_linux_timeval, usec) == 8,
        "AArch64 Linux timeval ABI 必须固定为 16 字节且按 8 字节对齐");
_Static_assert(sizeof(struct aarch64_linux_rusage) == 144 &&
        _Alignof(struct aarch64_linux_rusage) == 8,
        "AArch64 Linux rusage ABI 必须固定为 144 字节且按 8 字节对齐");
_Static_assert(__builtin_offsetof(struct aarch64_linux_rusage, utime) == 0 &&
        __builtin_offsetof(struct aarch64_linux_rusage, stime) == 16 &&
        __builtin_offsetof(struct aarch64_linux_rusage, maxrss) == 32 &&
        __builtin_offsetof(struct aarch64_linux_rusage, ixrss) == 40 &&
        __builtin_offsetof(struct aarch64_linux_rusage, idrss) == 48 &&
        __builtin_offsetof(struct aarch64_linux_rusage, isrss) == 56 &&
        __builtin_offsetof(struct aarch64_linux_rusage, minflt) == 64 &&
        __builtin_offsetof(struct aarch64_linux_rusage, majflt) == 72 &&
        __builtin_offsetof(struct aarch64_linux_rusage, nswap) == 80 &&
        __builtin_offsetof(struct aarch64_linux_rusage, inblock) == 88 &&
        __builtin_offsetof(struct aarch64_linux_rusage, oublock) == 96 &&
        __builtin_offsetof(struct aarch64_linux_rusage, msgsnd) == 104 &&
        __builtin_offsetof(struct aarch64_linux_rusage, msgrcv) == 112 &&
        __builtin_offsetof(struct aarch64_linux_rusage, nsignals) == 120 &&
        __builtin_offsetof(struct aarch64_linux_rusage, nvcsw) == 128 &&
        __builtin_offsetof(struct aarch64_linux_rusage, nivcsw) == 136,
        "AArch64 Linux rusage 字段偏移必须与内核 ABI 一致");

#endif
