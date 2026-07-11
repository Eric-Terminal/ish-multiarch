#ifndef KERNEL_AARCH64_SYSCALL_SERVICE_H
#define KERNEL_AARCH64_SYSCALL_SERVICE_H

#include "guest/linux/syscall-service.h"

// 调用线程的 current 必须与 syscall context 中的 task_opaque 相同。
extern const struct guest_linux_syscall_service
        ish_aarch64_linux_syscall_service;

#endif
