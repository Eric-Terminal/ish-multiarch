#ifndef GUEST_AARCH64_LINUX_SYSTEM_ABI_H
#define GUEST_AARCH64_LINUX_SYSTEM_ABI_H

#include "misc.h"

struct aarch64_linux_sysinfo {
    sqword_t uptime;
    qword_t loads[3];
    qword_t totalram;
    qword_t freeram;
    qword_t sharedram;
    qword_t bufferram;
    qword_t totalswap;
    qword_t freeswap;
    word_t procs;
    word_t padding;
    dword_t alignment_padding;
    qword_t totalhigh;
    qword_t freehigh;
    dword_t mem_unit;
    dword_t tail_padding;
} __attribute__((packed, aligned(8)));

_Static_assert(sizeof(struct aarch64_linux_sysinfo) == 112 &&
        _Alignof(struct aarch64_linux_sysinfo) == 8,
        "AArch64 Linux sysinfo ABI 必须固定为 112 字节且按 8 字节对齐");
_Static_assert(__builtin_offsetof(struct aarch64_linux_sysinfo, uptime) == 0 &&
        __builtin_offsetof(struct aarch64_linux_sysinfo, loads) == 8 &&
        __builtin_offsetof(struct aarch64_linux_sysinfo, totalram) == 32 &&
        __builtin_offsetof(struct aarch64_linux_sysinfo, freeram) == 40 &&
        __builtin_offsetof(struct aarch64_linux_sysinfo, sharedram) == 48 &&
        __builtin_offsetof(struct aarch64_linux_sysinfo, bufferram) == 56 &&
        __builtin_offsetof(struct aarch64_linux_sysinfo, totalswap) == 64 &&
        __builtin_offsetof(struct aarch64_linux_sysinfo, freeswap) == 72,
        "AArch64 Linux sysinfo 内存字段偏移必须与 LP64 ABI 一致");
_Static_assert(__builtin_offsetof(struct aarch64_linux_sysinfo, procs) == 80 &&
        __builtin_offsetof(struct aarch64_linux_sysinfo, totalhigh) == 88 &&
        __builtin_offsetof(struct aarch64_linux_sysinfo, freehigh) == 96 &&
        __builtin_offsetof(struct aarch64_linux_sysinfo, mem_unit) == 104,
        "AArch64 Linux sysinfo 尾部字段偏移必须与 LP64 ABI 一致");

#endif
