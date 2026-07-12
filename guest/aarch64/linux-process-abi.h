#ifndef GUEST_AARCH64_LINUX_PROCESS_ABI_H
#define GUEST_AARCH64_LINUX_PROCESS_ABI_H

#include "misc.h"

#define AARCH64_LINUX_CLONE_ARGS_SIZE_VER0 UINT64_C(64)
#define AARCH64_LINUX_CLONE_ARGS_SIZE_VER1 UINT64_C(80)
#define AARCH64_LINUX_CLONE_ARGS_SIZE_VER2 UINT64_C(88)

#define AARCH64_LINUX_CSIGNAL UINT64_C(0x000000ff)
#define AARCH64_LINUX_CLONE_VM UINT64_C(0x00000100)
#define AARCH64_LINUX_CLONE_FS UINT64_C(0x00000200)
#define AARCH64_LINUX_CLONE_FILES UINT64_C(0x00000400)
#define AARCH64_LINUX_CLONE_SIGHAND UINT64_C(0x00000800)
#define AARCH64_LINUX_CLONE_PIDFD UINT64_C(0x00001000)
#define AARCH64_LINUX_CLONE_PTRACE UINT64_C(0x00002000)
#define AARCH64_LINUX_CLONE_VFORK UINT64_C(0x00004000)
#define AARCH64_LINUX_CLONE_PARENT UINT64_C(0x00008000)
#define AARCH64_LINUX_CLONE_THREAD UINT64_C(0x00010000)
#define AARCH64_LINUX_CLONE_NEWNS UINT64_C(0x00020000)
#define AARCH64_LINUX_CLONE_SYSVSEM UINT64_C(0x00040000)
#define AARCH64_LINUX_CLONE_SETTLS UINT64_C(0x00080000)
#define AARCH64_LINUX_CLONE_PARENT_SETTID UINT64_C(0x00100000)
#define AARCH64_LINUX_CLONE_CHILD_CLEARTID UINT64_C(0x00200000)
#define AARCH64_LINUX_CLONE_DETACHED UINT64_C(0x00400000)
#define AARCH64_LINUX_CLONE_UNTRACED UINT64_C(0x00800000)
#define AARCH64_LINUX_CLONE_CHILD_SETTID UINT64_C(0x01000000)
#define AARCH64_LINUX_CLONE_NEWCGROUP UINT64_C(0x02000000)
#define AARCH64_LINUX_CLONE_NEWUTS UINT64_C(0x04000000)
#define AARCH64_LINUX_CLONE_NEWIPC UINT64_C(0x08000000)
#define AARCH64_LINUX_CLONE_NEWUSER UINT64_C(0x10000000)
#define AARCH64_LINUX_CLONE_NEWPID UINT64_C(0x20000000)
#define AARCH64_LINUX_CLONE_NEWNET UINT64_C(0x40000000)
#define AARCH64_LINUX_CLONE_IO UINT64_C(0x80000000)
#define AARCH64_LINUX_CLONE_CLEAR_SIGHAND (UINT64_C(1) << 32)
#define AARCH64_LINUX_CLONE_INTO_CGROUP (UINT64_C(1) << 33)
#define AARCH64_LINUX_CLONE_AUTOREAP (UINT64_C(1) << 34)
#define AARCH64_LINUX_CLONE_NNP (UINT64_C(1) << 35)
#define AARCH64_LINUX_CLONE_PIDFD_AUTOKILL (UINT64_C(1) << 36)
#define AARCH64_LINUX_CLONE_EMPTY_MNTNS (UINT64_C(1) << 37)
#define AARCH64_LINUX_CLONE_NEWTIME UINT64_C(0x00000080)

#define AARCH64_LINUX_CLONE_SUPPORTED_FLAGS ( \
        AARCH64_LINUX_CLONE_VM | AARCH64_LINUX_CLONE_FS | \
        AARCH64_LINUX_CLONE_FILES | AARCH64_LINUX_CLONE_SIGHAND | \
        AARCH64_LINUX_CLONE_VFORK | AARCH64_LINUX_CLONE_THREAD | \
        AARCH64_LINUX_CLONE_SYSVSEM | AARCH64_LINUX_CLONE_SETTLS | \
        AARCH64_LINUX_CLONE_PARENT_SETTID | \
        AARCH64_LINUX_CLONE_CHILD_CLEARTID | \
        AARCH64_LINUX_CLONE_CHILD_SETTID)

struct aarch64_linux_clone_args {
    qword_t flags;
    qword_t pidfd;
    qword_t child_tid;
    qword_t parent_tid;
    qword_t exit_signal;
    qword_t stack;
    qword_t stack_size;
    qword_t tls;
    qword_t set_tid;
    qword_t set_tid_size;
    qword_t cgroup;
} __attribute__((packed, aligned(8)));

_Static_assert(sizeof(struct aarch64_linux_clone_args) == 88 &&
        _Alignof(struct aarch64_linux_clone_args) == 8,
        "AArch64 Linux clone_args 必须保持 88 字节并按 8 字节对齐");
_Static_assert((AARCH64_LINUX_CLONE_SUPPORTED_FLAGS &
                ~UINT64_C(0xffffffff)) == 0,
        "已支持的 clone3 flags 必须能在校验后无损交给旧 clone 核心");
_Static_assert(__builtin_offsetof(
                struct aarch64_linux_clone_args, flags) == 0 &&
        __builtin_offsetof(
                struct aarch64_linux_clone_args, pidfd) == 8 &&
        __builtin_offsetof(
                struct aarch64_linux_clone_args, child_tid) == 16 &&
        __builtin_offsetof(
                struct aarch64_linux_clone_args, parent_tid) == 24 &&
        __builtin_offsetof(
                struct aarch64_linux_clone_args, exit_signal) == 32 &&
        __builtin_offsetof(
                struct aarch64_linux_clone_args, stack) == 40 &&
        __builtin_offsetof(
                struct aarch64_linux_clone_args, stack_size) == 48 &&
        __builtin_offsetof(
                struct aarch64_linux_clone_args, tls) == 56 &&
        __builtin_offsetof(
                struct aarch64_linux_clone_args, set_tid) == 64 &&
        __builtin_offsetof(
                struct aarch64_linux_clone_args, set_tid_size) == 72 &&
        __builtin_offsetof(
                struct aarch64_linux_clone_args, cgroup) == 80,
        "AArch64 Linux clone_args 字段偏移必须全部固定为 64 位 wire 布局");

#endif
