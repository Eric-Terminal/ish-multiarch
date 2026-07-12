#ifndef KERNEL_AARCH64_TASK_RUNNER_H
#define KERNEL_AARCH64_TASK_RUNNER_H

#include "misc.h"

struct task;
struct guest_linux_user_fault;
struct siginfo_;

enum aarch64_task_event_action {
    AARCH64_TASK_EVENT_CONTINUE,
    AARCH64_TASK_EVENT_EXIT,
    AARCH64_TASK_EVENT_EXIT_GROUP,
    AARCH64_TASK_EVENT_STOP,
    AARCH64_TASK_EVENT_TERMINATE,
    AARCH64_TASK_EVENT_EXEC,
};

struct aarch64_task_event {
    enum aarch64_task_event_action action;
    dword_t status;
};

// 将 memory fault 确定性转换为 Linux 信号；返回信号编号并填充 info。
int aarch64_task_fault_signal(
        const struct guest_linux_user_fault *fault,
        struct siginfo_ *info);
// 两个有限入口用于建立可测试的 guest 指令与信号安全点。
// EXIT/EXIT_GROUP 的 status 是 wait 编码，STOP/TERMINATE 则是原始信号号。
struct aarch64_task_event aarch64_task_run_one(struct task *task);
struct aarch64_task_event aarch64_task_poll_signals(struct task *task);
void aarch64_task_run_current(void);

#endif
