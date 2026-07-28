#ifndef GUEST_AARCH64_LINUX_TIME_ABI_H
#define GUEST_AARCH64_LINUX_TIME_ABI_H

#include "misc.h"

struct aarch64_linux_timespec {
    sqword_t sec;
    sqword_t nsec;
} __attribute__((packed, aligned(8)));

_Static_assert(sizeof(struct aarch64_linux_timespec) == 16 &&
        _Alignof(struct aarch64_linux_timespec) == 8 &&
        __builtin_offsetof(struct aarch64_linux_timespec, sec) == 0 &&
        __builtin_offsetof(struct aarch64_linux_timespec, nsec) == 8,
        "AArch64 timespec 必须由两个连续的 64 位有符号字段组成");

struct aarch64_linux_itimerspec {
    struct aarch64_linux_timespec interval;
    struct aarch64_linux_timespec value;
} __attribute__((packed, aligned(8)));

_Static_assert(sizeof(struct aarch64_linux_itimerspec) == 32 &&
        _Alignof(struct aarch64_linux_itimerspec) == 8 &&
        __builtin_offsetof(
                struct aarch64_linux_itimerspec, interval) == 0 &&
        __builtin_offsetof(
                struct aarch64_linux_itimerspec, interval.sec) == 0 &&
        __builtin_offsetof(
                struct aarch64_linux_itimerspec, interval.nsec) == 8 &&
        __builtin_offsetof(
                struct aarch64_linux_itimerspec, value) == 16 &&
        __builtin_offsetof(
                struct aarch64_linux_itimerspec, value.sec) == 16 &&
        __builtin_offsetof(
                struct aarch64_linux_itimerspec, value.nsec) == 24,
        "AArch64 itimerspec 必须按 interval、value 排列四个 64 位字段");

#endif
