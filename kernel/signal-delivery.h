#ifndef KERNEL_SIGNAL_DELIVERY_H
#define KERNEL_SIGNAL_DELIVERY_H

#include "guest/linux/signal-service.h"

struct sighand;
struct siginfo_;
struct task;

enum signal_delivery_disposition {
    SIGNAL_DELIVERY_IGNORE,
    SIGNAL_DELIVERY_TERMINATE,
    SIGNAL_DELIVERY_HANDLER,
    SIGNAL_DELIVERY_STOP,
};

// 调用方持有 sighand->lock，返回值只描述该锁保护下的动作快照。
enum signal_delivery_disposition signal_disposition_locked(
        const struct sighand *sighand, int signal);

// 下列函数调用前后均持有 current->sighand->lock；ptrace 等待期间可暂时释放。
void signal_force_sigsegv_locked(
        struct sighand *sighand, int failed_signal);
void signal_force_sigsegv_info_locked(
        struct sighand *sighand, int failed_signal,
        const struct siginfo_ *info);
// 强制同步异常会解除阻塞，并在被阻塞或忽略时恢复默认动作；精确信息移到队首。
void signal_force_sync_info_locked(
        struct sighand *sighand, int signal,
        const struct siginfo_ *info);
void signal_ptrace_stop_locked(
        int signal, const struct siginfo_ *info);

// 调用方不得预持任务锁；函数只为当前线程的 task 派送一个可见动作。
struct guest_linux_signal_poll_result task_poll_one_signal(
        struct task *task, guest_linux_signal_installer installer,
        void *installer_opaque);

#endif
