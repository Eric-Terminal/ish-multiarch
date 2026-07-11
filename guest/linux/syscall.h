#ifndef GUEST_LINUX_SYSCALL_H
#define GUEST_LINUX_SYSCALL_H

#include "misc.h"

#define GUEST_LINUX_SYSCALL_ARGUMENT_COUNT 6

struct guest_linux_syscall {
    qword_t number;
    qword_t arguments[GUEST_LINUX_SYSCALL_ARGUMENT_COUNT];
};

_Static_assert(sizeof(struct guest_linux_syscall) == 56,
        "Linux syscall 请求 ABI 必须固定为七个 64 位字");
_Static_assert(__builtin_offsetof(struct guest_linux_syscall, number) == 0 &&
        __builtin_offsetof(struct guest_linux_syscall, arguments) == 8,
        "Linux syscall 请求字段偏移必须与 guest 选择无关");

#endif
