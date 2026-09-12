#include <assert.h>

#include "guest/aarch64/linux-signal-abi.h"
#include "guest/memory/address-space.h"
#include "kernel/aarch64-signal-service.h"
#include "kernel/errno.h"
#include "kernel/signal-delivery.h"
#include "kernel/signal.h"
#include "kernel/task.h"

#define AARCH64_SIGNAL_USER_ADDRESS_LIMIT \
    (AARCH64_LINUX_USER_ADDRESS_MAX + UINT64_C(1))

static bool may_have_pending_signal(
        const struct guest_linux_signal_context *context) {
    const struct task *task = context->task_opaque;
    if (atomic_load_explicit(&task->signal_poll_needed, memory_order_acquire))
        return true;
    uint64_t state = atomic_load_explicit(
            &task->group->signal_poll_state, memory_order_acquire);
    return (state & SIGNAL_POLL_FORCE) != 0 || state != task->observed_signal_poll_state;
}

static struct guest_linux_signal_poll_result poll_signal(
        const struct guest_linux_signal_context *context,
        guest_linux_signal_installer installer,
        void *installer_opaque) {
    assert(context != NULL);
    struct task *task = context->task_opaque;
    assert(task != NULL && task == current);
    struct guest_linux_signal_poll_result result =
            task_poll_one_signal(task, installer, installer_opaque);

    // 在生产者的锁内确认这一世代。屏蔽中的共享信号只令每个成员检查一次，
    // 不能由一个成员清掉其他成员尚未看见的新信号通知。
    lock(&task->sighand->lock);
    atomic_store_explicit(&task->signal_poll_needed,
            ((task->pending | task->group->shared_pending) & ~task->blocked) != 0 ||
                    task->has_saved_mask,
            memory_order_release);
    lock(&task->group->lock);
    uint64_t state = atomic_load_explicit(
            &task->group->signal_poll_state, memory_order_relaxed) & ~SIGNAL_POLL_FORCE;
    if (task->group->doing_group_exit || task->group->exec_task != NULL)
        state |= SIGNAL_POLL_FORCE;
    atomic_store_explicit(&task->group->signal_poll_state, state, memory_order_release);
    task->observed_signal_poll_state = state;
    unlock(&task->group->lock);
    unlock(&task->sighand->lock);
    return result;
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
    .may_have_pending = may_have_pending_signal,
};

static qword_t signal_syscall_result(int result) {
    return (qword_t) (sqword_t) result;
}

static bool signal_user_range_fits(qword_t address, qword_t size) {
    return address <= AARCH64_SIGNAL_USER_ADDRESS_LIMIT &&
            size <= AARCH64_SIGNAL_USER_ADDRESS_LIMIT - address;
}

qword_t aarch64_linux_dispatch_rt_sigsuspend(
        const struct guest_linux_syscall_context *context,
        const struct guest_linux_syscall *syscall,
        struct task *task, struct guest_linux_user_fault *fault) {
    if (syscall->arguments[1] != sizeof(sigset_t_))
        return signal_syscall_result(_EINVAL);

    qword_t address = syscall->arguments[0];
    if (!signal_user_range_fits(address, sizeof(sigset_t_))) {
        *fault = (struct guest_linux_user_fault) {
            .address = address,
            .access = GUEST_MEMORY_READ,
            .kind = GUEST_MEMORY_FAULT_ADDRESS_SIZE,
        };
        return signal_syscall_result(_EFAULT);
    }

    sigset_t_ mask;
    assert(context->user.read != NULL);
    if (!context->user.read(context->user.opaque,
            address, &mask, sizeof(mask), fault))
        return signal_syscall_result(_EFAULT);
    return signal_syscall_result(task_sigsuspend(task, mask));
}
