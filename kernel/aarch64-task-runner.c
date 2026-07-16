#include <assert.h>

#include "guest/aarch64/linux-process.h"
#include "guest/memory/address-space.h"
#include "kernel/aarch64-task-runner.h"
#include "kernel/calls.h"
#include "kernel/signal-delivery.h"
#include "kernel/signal.h"
#include "kernel/task.h"

static struct aarch64_task_event task_event(
        enum aarch64_task_event_action action, dword_t status) {
    return (struct aarch64_task_event) {
        .action = action,
        .status = status,
    };
}

static struct aarch64_task_event translate_process_event(
        const struct aarch64_linux_process_result *result) {
    switch ((enum aarch64_linux_process_status) result->status) {
        case AARCH64_LINUX_PROCESS_RUNNABLE:
            return task_event(AARCH64_TASK_EVENT_CONTINUE, 0);
        case AARCH64_LINUX_PROCESS_EXIT:
            return task_event(AARCH64_TASK_EVENT_EXIT,
                    result->exit_status << 8);
        case AARCH64_LINUX_PROCESS_EXIT_GROUP:
            return task_event(AARCH64_TASK_EVENT_EXIT_GROUP,
                    result->exit_status << 8);
        case AARCH64_LINUX_PROCESS_STOP:
            return task_event(AARCH64_TASK_EVENT_STOP,
                    (dword_t) result->signal);
        case AARCH64_LINUX_PROCESS_TERMINATE:
            return task_event(AARCH64_TASK_EVENT_TERMINATE,
                    (dword_t) result->signal);
        case AARCH64_LINUX_PROCESS_FETCH_FAULT:
        case AARCH64_LINUX_PROCESS_DATA_FAULT:
        case AARCH64_LINUX_PROCESS_UNDEFINED:
            break;
        case AARCH64_LINUX_PROCESS_EXEC:
            return task_event(AARCH64_TASK_EVENT_EXEC, 0);
    }
    assert(false && "同步异常必须在转换 task 事件前处理");
    return task_event(AARCH64_TASK_EVENT_TERMINATE, SIGKILL_);
}

static void force_fault_signal(struct task *task,
        int signal, const struct siginfo_ *info) {
    lock(&task->sighand->lock);
    signal_force_sync_info_locked(
            task->sighand, signal, info);
    unlock(&task->sighand->lock);
}

int aarch64_task_fault_signal(
        const struct guest_linux_user_fault *fault,
        struct siginfo_ *info) {
    assert(fault != NULL && info != NULL);
    int signal;
    int code;
    switch ((enum guest_memory_fault_kind) fault->kind) {
        case GUEST_MEMORY_FAULT_ADDRESS_SIZE:
            // Linux arm64 将地址尺寸异常作为不可捕获的内核故障终止。
            *info = SIGINFO_NIL;
            info->sig = SIGKILL_;
            return SIGKILL_;
        case GUEST_MEMORY_FAULT_OUT_OF_MEMORY:
            // COW 缺页无法取得宿主页时不能伪装成地址或权限错误。
            *info = SIGINFO_NIL;
            info->sig = SIGKILL_;
            return SIGKILL_;
        case GUEST_MEMORY_FAULT_UNMAPPED:
            signal = SIGSEGV_;
            code = SEGV_MAPERR_;
            break;
        case GUEST_MEMORY_FAULT_PERMISSION:
            signal = SIGSEGV_;
            code = SEGV_ACCERR_;
            break;
        case GUEST_MEMORY_FAULT_ALIGNMENT:
            signal = SIGBUS_;
            code = BUS_ADRALN_;
            break;
        case GUEST_MEMORY_FAULT_NONE:
            assert(false &&
                    "AArch64 fault 事件必须携带有效故障类型");
            *info = SIGINFO_NIL;
            info->sig = SIGKILL_;
            return SIGKILL_;
    }

    *info = (struct siginfo_) {
        .sig = signal,
        .code = code,
        .payload_kind = SIGNAL_INFO_PAYLOAD_FAULT,
        .fault.addr = fault->address,
    };
    return signal;
}

static void force_memory_fault(struct task *task,
        const struct guest_linux_user_fault *fault) {
    struct siginfo_ info;
    int signal = aarch64_task_fault_signal(fault, &info);
    if (signal == SIGKILL_)
        deliver_signal(task, signal, info);
    else
        force_fault_signal(task, signal, &info);
}

struct aarch64_task_event aarch64_task_poll_signals(
        struct task *task) {
    assert(task != NULL && task == current &&
            task_has_aarch64_process(task));
    struct aarch64_linux_process_result result =
            aarch64_linux_process_poll_signals(
                    task->aarch64_process);
    return translate_process_event(&result);
}

struct aarch64_task_event aarch64_task_run_one(struct task *task) {
    assert(task != NULL && task == current &&
            task_has_aarch64_process(task));
    struct aarch64_linux_process_result result =
            aarch64_linux_process_run_one(
                    task->aarch64_process);
    switch ((enum aarch64_linux_process_status) result.status) {
        case AARCH64_LINUX_PROCESS_FETCH_FAULT:
        case AARCH64_LINUX_PROCESS_DATA_FAULT:
            force_memory_fault(task, &result.fault);
            return aarch64_task_poll_signals(task);
        case AARCH64_LINUX_PROCESS_UNDEFINED:
            force_fault_signal(task, SIGILL_, &(struct siginfo_) {
                .sig = SIGILL_,
                .code = ILL_ILLOPC_,
                .payload_kind = SIGNAL_INFO_PAYLOAD_FAULT,
                .fault.addr = result.fault.address,
            });
            return aarch64_task_poll_signals(task);
        case AARCH64_LINUX_PROCESS_RUNNABLE:
        case AARCH64_LINUX_PROCESS_EXIT:
        case AARCH64_LINUX_PROCESS_EXIT_GROUP:
        case AARCH64_LINUX_PROCESS_STOP:
        case AARCH64_LINUX_PROCESS_TERMINATE:
            return translate_process_event(&result);
        case AARCH64_LINUX_PROCESS_EXEC:
            return translate_process_event(&result);
    }
    assert(false && "未知 AArch64 process 状态");
    return task_event(AARCH64_TASK_EVENT_TERMINATE, SIGKILL_);
}

static struct aarch64_task_event wait_until_continued(
        struct task *task) {
    struct tgroup *group = task->group;
    lock(&group->lock);
    while (group->stopped)
        wait_for_ignore_signals(
                &group->stopped_cond, &group->lock, NULL);
    unlock(&group->lock);
    signal_notify_group_continue(task);
    // SIGKILL 会解除停止；恢复 guest 前必须先消费它。
    return aarch64_task_poll_signals(task);
}

void aarch64_task_run_current(void) {
    assert(current != NULL && task_has_aarch64_process(current));
    struct aarch64_task_event event =
            aarch64_task_poll_signals(current);
    while (true) {
        switch (event.action) {
            case AARCH64_TASK_EVENT_CONTINUE:
                event = aarch64_task_run_one(current);
                break;
            case AARCH64_TASK_EVENT_EXIT:
                do_exit((int) event.status);
            case AARCH64_TASK_EVENT_EXIT_GROUP:
                do_exit_group((int) event.status);
            case AARCH64_TASK_EVENT_STOP:
                event = wait_until_continued(current);
                break;
            case AARCH64_TASK_EVENT_TERMINATE:
                do_exit_group((int) event.status);
            case AARCH64_TASK_EVENT_EXEC:
                task_commit_aarch64_exec(current);
                return;
        }
    }
}
