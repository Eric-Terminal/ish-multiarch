#ifndef KERNEL_AARCH64_SIGNAL_SERVICE_H
#define KERNEL_AARCH64_SIGNAL_SERVICE_H

#include "guest/linux/signal-service.h"
#include "guest/linux/syscall-service.h"

struct task;

// 调用线程的 current 必须与 signal context 中的 task_opaque 相同。
extern const struct guest_linux_signal_service
        ish_aarch64_linux_signal_service;

qword_t aarch64_linux_dispatch_rt_sigsuspend(
        const struct guest_linux_syscall_context *context,
        const struct guest_linux_syscall *syscall,
        struct task *task, struct guest_linux_user_fault *fault);

#endif
