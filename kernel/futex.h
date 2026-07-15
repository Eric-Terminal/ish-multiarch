#ifndef KERNEL_FUTEX_H
#define KERNEL_FUTEX_H

#include <stddef.h>

struct aarch64_linux_process;

int futex_wake(addr_t uaddr, dword_t val);
int futex_wake_aarch64(struct aarch64_linux_process *process,
        qword_t uaddr, dword_t val);
// 常驻故障注入入口仅供测试；SIZE_MAX 表示恢复正常分配。
void futex_test_fail_allocation_at(size_t index);

#endif
