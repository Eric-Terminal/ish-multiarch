#ifndef KERNEL_AARCH64_FD_SERVICE_H
#define KERNEL_AARCH64_FD_SERVICE_H

#include "guest/linux/syscall-service.h"

struct task;

qword_t aarch64_linux_dispatch_dup(
        const struct guest_linux_syscall *syscall, struct task *task);
qword_t aarch64_linux_dispatch_dup3(
        const struct guest_linux_syscall *syscall, struct task *task);
qword_t aarch64_linux_dispatch_fcntl(
        const struct guest_linux_syscall *syscall, struct task *task);
qword_t aarch64_linux_dispatch_pipe2(
        const struct guest_linux_syscall_context *context,
        const struct guest_linux_syscall *syscall,
        struct task *task, struct guest_linux_user_fault *fault);

#endif
