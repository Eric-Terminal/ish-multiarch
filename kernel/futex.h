#ifndef KERNEL_FUTEX_H
#define KERNEL_FUTEX_H

#include <stddef.h>

struct aarch64_linux_process;
struct task;

int futex_wake(addr_t uaddr, dword_t val);
int futex_wake_aarch64(struct aarch64_linux_process *process,
        qword_t uaddr, dword_t val);
// 仅供已提交的 i386 任务退出或成功 exec；调用时旧地址空间仍须存活。
void futex_cleanup_task_i386(struct task *task);
// 仅供已提交任务退出或成功 exec；调用时旧地址空间仍须存活。
void futex_cleanup_task_aarch64(
        struct task *task, struct aarch64_linux_process *process);
// process 为 NULL 时只清除注册；否则在旧地址空间仍存活时完成退出修复。
void futex_cleanup_robust_list_aarch64(
        struct task *task, struct aarch64_linux_process *process);
// 常驻故障注入入口仅供测试；SIZE_MAX 表示恢复正常分配。
void futex_test_fail_allocation_at(size_t index);
// 常驻生命周期计数仅供测试在所有等待者退出后的静止点读取。
unsigned futex_test_live_count(void);

#endif
