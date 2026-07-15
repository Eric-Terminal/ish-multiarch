#ifndef KERNEL_AARCH64_SYSCALL_SERVICE_H
#define KERNEL_AARCH64_SYSCALL_SERVICE_H

#include <stddef.h>

#include "guest/linux/syscall-service.h"

// 调用线程的 current 必须与 syscall context 中的 task_opaque 相同。
extern const struct guest_linux_syscall_service
        ish_aarch64_linux_syscall_service;

// 仅供 PONR 线程退出回归确认 host exec 参数已由清理域释放。
size_t ish_aarch64_execve_test_live_host_buffer_sets(void);

#endif
