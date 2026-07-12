#include <assert.h>
#include <stdlib.h>

#include "kernel/signal-delivery.h"
#include "kernel/signal.h"
#include "kernel/task.h"

#define SIGNAL_UNBLOCKABLE_MASK \
    (sig_mask(SIGKILL_) | sig_mask(SIGSTOP_))

static struct sigqueue *find_queued_signal(
        struct task *task, int signal) {
    struct sigqueue *queued;
    list_for_each_entry(&task->queue, queued, queue) {
        if (queued->info.sig == signal)
            return queued;
    }
    return NULL;
}

struct sigqueue *signal_select_unblocked_locked(
        struct task *task, sigset_t_ blocked) {
    struct sigqueue *selected_realtime = NULL;
    struct sigqueue *queued;
    list_for_each_entry(&task->queue, queued, queue) {
        int signal = queued->info.sig;
        if (sigset_has(blocked, signal))
            continue;
        if (signal < SIGRTMIN_)
            return queued;
        if (selected_realtime == NULL ||
                signal < selected_realtime->info.sig) {
            selected_realtime = queued;
        }
    }
    return selected_realtime;
}

void signal_dequeue_locked(
        struct task *task, struct sigqueue *queued) {
    int signal = queued->info.sig;
    list_remove(&queued->queue);
    if (find_queued_signal(task, signal) == NULL)
        sigset_del(&task->pending, signal);
    free(queued);
}

static void signal_notify_group_stop(
        struct task *task, bool was_stopped) {
    if (was_stopped)
        return;
    lock(&task->group->lock);
    bool now_stopped = task->group->stopped;
    unlock(&task->group->lock);
    if (!now_stopped)
        return;

    lock(&pids_lock);
    struct task *leader = task->group->leader;
    struct task *parent = leader->parent;
    if (parent != NULL) {
        notify(&parent->group->child_exit);
        send_signal(parent,
                leader->exit_signal, SIGINFO_NIL);
    }
    unlock(&pids_lock);
}

static struct guest_linux_signal_info export_signal_info(
        const struct siginfo_ *info) {
    struct guest_linux_signal_info exported = {
        .signal = info->sig,
        .error = info->sig_errno,
        .code = info->code,
    };
    switch (info->payload_kind) {
        case SIGNAL_INFO_PAYLOAD_KILL:
            exported.payload_kind = GUEST_LINUX_SIGNAL_PAYLOAD_KILL;
            exported.kill.pid = info->kill.pid;
            exported.kill.uid = info->kill.uid;
            break;
        case SIGNAL_INFO_PAYLOAD_TIMER:
            exported.payload_kind = GUEST_LINUX_SIGNAL_PAYLOAD_TIMER;
            exported.timer.timer = info->timer.timer;
            exported.timer.overrun = info->timer.overrun;
            exported.timer.value = info->timer.value;
            exported.timer.private_value = info->timer._private;
            break;
        case SIGNAL_INFO_PAYLOAD_CHILD:
            exported.payload_kind = GUEST_LINUX_SIGNAL_PAYLOAD_CHILD;
            exported.child.pid = info->child.pid;
            exported.child.uid = info->child.uid;
            exported.child.status = info->child.status;
            exported.child.utime = info->child.utime;
            exported.child.stime = info->child.stime;
            break;
        case SIGNAL_INFO_PAYLOAD_FAULT:
            exported.payload_kind = GUEST_LINUX_SIGNAL_PAYLOAD_FAULT;
            exported.fault.address = info->fault.addr;
            break;
        case SIGNAL_INFO_PAYLOAD_SIGSYS:
            exported.payload_kind = GUEST_LINUX_SIGNAL_PAYLOAD_SIGSYS;
            exported.sigsys.address = info->sigsys.addr;
            exported.sigsys.syscall = info->sigsys.syscall;
            exported.sigsys.architecture = info->sigsys.arch;
            break;
        case SIGNAL_INFO_PAYLOAD_NONE:
        default:
            exported.payload_kind = GUEST_LINUX_SIGNAL_PAYLOAD_NONE;
            break;
    }
    return exported;
}

static struct guest_linux_signal_delivery make_delivery(
        const struct task *task, const struct siginfo_ *info,
        const struct signal_action *action) {
    return (struct guest_linux_signal_delivery) {
        .info = export_signal_info(info),
        .action = {
            .handler = action->handler,
            .flags = action->flags,
            .restorer = action->restorer,
            .mask = action->mask,
        },
        .blocked_mask = task->blocked,
        .altstack = {
            .base = task->altstack.stack,
            .size = task->altstack.size,
            .flags = task->altstack.flags,
        },
    };
}

static struct sigqueue *select_signal(
        struct task *task, sigset_t_ blocked) {
    // SIGKILL 已经 pending 时不得先把任务停驻或返回用户 handler。
    if (sigset_has(task->pending, SIGKILL_)) {
        struct sigqueue *fatal = find_queued_signal(task, SIGKILL_);
        assert(fatal != NULL);
        return fatal;
    }

    return signal_select_unblocked_locked(task, blocked);
}

static struct guest_linux_signal_poll_result poll_result(
        enum guest_linux_signal_poll_status status, int signal) {
    return (struct guest_linux_signal_poll_result) {
        .status = (dword_t) status,
        .signal = (sdword_t) signal,
    };
}

struct guest_linux_signal_poll_result task_poll_one_signal(
        struct task *task, guest_linux_signal_installer installer,
        void *installer_opaque) {
    assert(task != NULL && task == current && installer != NULL);

    lock(&task->group->lock);
    bool was_stopped = task->group->stopped;
    unlock(&task->group->lock);

    struct sighand *sighand = task->sighand;
    lock(&sighand->lock);
    sigset_t_ selection_mask =
            signal_prepare_delivery_locked(task);

    while (true) {
        struct sigqueue *queued = select_signal(task, selection_mask);
        if (queued == NULL) {
            unlock(&sighand->lock);
            return poll_result(GUEST_LINUX_SIGNAL_POLL_IDLE, 0);
        }

        struct siginfo_ info = queued->info;
        int signal = info.sig;
        signal_dequeue_locked(task, queued);

        if (task->ptrace.traced && signal != SIGKILL_) {
            signal_ptrace_stop_locked(signal, &info);
            continue;
        }

        enum signal_delivery_disposition disposition =
                signal_disposition_locked(sighand, signal);
        if (disposition == SIGNAL_DELIVERY_IGNORE)
            continue;
        if (disposition == SIGNAL_DELIVERY_TERMINATE) {
            unlock(&sighand->lock);
            return poll_result(
                    GUEST_LINUX_SIGNAL_POLL_TERMINATE, signal);
        }
        if (disposition == SIGNAL_DELIVERY_STOP) {
            lock(&task->group->lock);
            task->group->stopped = true;
            task->group->continued = false;
            task->group->continue_notification_pending = false;
            task->group->stop_code =
                    (dword_t) signal << 8 | UINT32_C(0x7f);
            unlock(&task->group->lock);
            unlock(&sighand->lock);
            signal_notify_group_stop(task, was_stopped);
            return poll_result(GUEST_LINUX_SIGNAL_POLL_STOP, signal);
        }

        struct signal_action *action = &sighand->action[signal];
        struct guest_linux_signal_delivery delivery =
                make_delivery(task, &info, action);
        enum guest_linux_signal_install_status installed =
                installer(installer_opaque, &delivery);
        if (installed == GUEST_LINUX_SIGNAL_INSTALL_FRAME_FAULT) {
            signal_force_sigsegv_locked(sighand, signal);
            sigset_del(&selection_mask, SIGSEGV_);
            continue;
        }
        assert(installed == GUEST_LINUX_SIGNAL_INSTALL_COMPLETE);

        sigset_t_ next_mask = task->blocked | action->mask;
        if (!(action->flags & SA_NODEFER_))
            sigset_add(&next_mask, signal);
        task->blocked = next_mask & ~SIGNAL_UNBLOCKABLE_MASK;
        if (action->flags & SA_RESETHAND_) {
            *action = (struct signal_action) {
                .handler = SIG_DFL_,
            };
        }
        unlock(&sighand->lock);
        return poll_result(GUEST_LINUX_SIGNAL_POLL_HANDLER, signal);
    }
}
