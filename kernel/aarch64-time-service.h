#ifndef KERNEL_AARCH64_TIME_SERVICE_H
#define KERNEL_AARCH64_TIME_SERVICE_H

#include "guest/linux/syscall-service.h"

struct task;

qword_t aarch64_linux_dispatch_clock_gettime(
        const struct guest_linux_syscall_context *context,
        const struct guest_linux_syscall *syscall,
        struct guest_linux_user_fault *fault);
qword_t aarch64_linux_dispatch_nanosleep(
        const struct guest_linux_syscall_context *context,
        const struct guest_linux_syscall *syscall,
        struct task *task, struct guest_linux_user_fault *fault);
qword_t aarch64_linux_dispatch_timerfd_create(
        const struct guest_linux_syscall *syscall, struct task *task);
qword_t aarch64_linux_dispatch_timerfd_settime(
        const struct guest_linux_syscall_context *context,
        const struct guest_linux_syscall *syscall,
        struct task *task, struct guest_linux_user_fault *fault);
qword_t aarch64_linux_dispatch_timerfd_gettime(
        const struct guest_linux_syscall_context *context,
        const struct guest_linux_syscall *syscall,
        struct task *task, struct guest_linux_user_fault *fault);

#endif
