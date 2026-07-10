#ifndef GUEST_LINUX_SYSCALL_H
#define GUEST_LINUX_SYSCALL_H

#include "misc.h"

#define GUEST_LINUX_SYSCALL_ARGUMENT_COUNT 6

struct guest_linux_syscall {
    qword_t number;
    qword_t arguments[GUEST_LINUX_SYSCALL_ARGUMENT_COUNT];
};

#endif
