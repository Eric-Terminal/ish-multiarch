#include <assert.h>

#include "guest/aarch64/linux-signal-abi.h"
#include "kernel/aarch64-signal-service.h"
#include "kernel/signal-delivery.h"
#include "kernel/signal.h"
#include "kernel/task.h"

static struct guest_linux_signal_poll_result poll_signal(
        const struct guest_linux_signal_context *context,
        guest_linux_signal_installer installer,
        void *installer_opaque) {
    assert(context != NULL);
    struct task *task = context->task_opaque;
    assert(task != NULL && task == current);
    return task_poll_one_signal(task, installer, installer_opaque);
}

static void restore_signal_state(
        const struct guest_linux_signal_context *context,
        const struct guest_linux_signal_restore_request *request) {
    assert(context != NULL && request != NULL);
    struct task *task = context->task_opaque;
    assert(task != NULL && task == current);

    sigset_t_ blocked = request->blocked_mask;
    (void) task_sigprocmask(task, SIG_SETMASK_, &blocked, NULL);

    const struct signal_altstack altstack = {
        .stack = request->altstack.base,
        .size = request->altstack.size,
        .flags = request->altstack.flags,
    };
    // 无效的用户帧栈配置不得回滚已经恢复的信号掩码。
    (void) task_sigaltstack(task, request->stack_pointer,
            &altstack, NULL, AARCH64_LINUX_MINSIGSTKSZ,
            AARCH64_LINUX_USER_ADDRESS_MAX);
}

static void queue_bad_frame(
        const struct guest_linux_signal_context *context,
        qword_t frame_address) {
    assert(context != NULL);
    struct task *task = context->task_opaque;
    assert(task != NULL && task == current);

    const struct siginfo_ info = {
        .sig = SIGSEGV_,
        .code = SEGV_MAPERR_,
        .payload_kind = SIGNAL_INFO_PAYLOAD_FAULT,
        .fault.addr = frame_address,
    };
    lock(&task->sighand->lock);
    signal_force_sigsegv_info_locked(task->sighand, 0, &info);
    unlock(&task->sighand->lock);
}

const struct guest_linux_signal_service ish_aarch64_linux_signal_service = {
    .poll = poll_signal,
    .restore = restore_signal_state,
    .bad_frame = queue_bad_frame,
};
