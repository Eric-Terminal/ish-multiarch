#ifndef GUEST_AARCH64_LINUX_SYSCALL_NAME_H
#define GUEST_AARCH64_LINUX_SYSCALL_NAME_H

#include "misc.h"

/* 返回 Linux asm-generic 的稳定名称；保留号与未知新号返回 NULL。 */
const char *aarch64_linux_syscall_name(qword_t number);

#endif
