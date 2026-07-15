#include "debug.h"
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include "kernel/calls.h"
#include "kernel/signal-delivery.h"
#include "kernel/signal.h"
#include "kernel/task.h"
#include "kernel/vdso.h"
#include "emu/interrupt.h"
#include "util/timer.h"

#if is_gcc(9)
#pragma GCC diagnostic ignored "-Waddress-of-packed-member"
#endif

int xsave_extra = 0;
int fxsave_extra = 0;
static void sigmask_set(struct task *task, sigset_t_ set);
static void resume_ptrace_stop_for_sigkill(struct task *task, int sig);
static void resume_group_for_signal(struct task *task, int sig);

static bool signal_is_blockable(int sig) {
    return sig != SIGKILL_ && sig != SIGSTOP_;
}

#define UNBLOCKABLE_MASK (sig_mask(SIGKILL_) | sig_mask(SIGSTOP_))
#define SYNCHRONOUS_MASK (sig_mask(SIGSEGV_) | sig_mask(SIGBUS_) | \
        sig_mask(SIGILL_) | sig_mask(SIGTRAP_) | sig_mask(SIGFPE_) | \
        sig_mask(SIGSYS_))
#define JOB_CONTROL_STOP_MASK \
        (sig_mask(SIGSTOP_) | sig_mask(SIGTSTP_) | \
         sig_mask(SIGTTIN_) | sig_mask(SIGTTOU_))

static bool signal_default_ignored(int sig) {
    return sig == SIGURG_ || sig == SIGCONT_ ||
            sig == SIGCHLD_ || sig == SIGWINCH_;
}

static bool signal_default_dumps_core(int sig) {
    switch (sig) {
        case SIGQUIT_: case SIGILL_: case SIGTRAP_: case SIGABRT_:
        case SIGBUS_: case SIGFPE_: case SIGSEGV_: case SIGXCPU_:
        case SIGXFSZ_: case SIGSYS_:
            return true;
        default:
            return false;
    }
}

static void commit_group_fatal_signal(struct task *task, int sig) {
    struct tgroup *group = task->group;
    lock(&group->lock);
    if (!group->doing_group_exit &&
            atomic_load_explicit(&group->external_fatal_signal,
                    memory_order_relaxed) == 0) {
        // 与显式 exit_group 在同一锁下竞争首次组退出原因。
        group->doing_group_exit = true;
        group->group_exit_code = sig;
        atomic_store_explicit(
                &group->external_fatal_signal, sig, memory_order_release);
    }
    unlock(&group->lock);
}

// 调用方持有 sighand 锁；最早生成的未阻塞默认致死信号决定组退出状态。
static void record_external_fatal_signal_locked(
        struct task *receiver, struct task *fatal_subject, int sig,
        enum signal_delivery_disposition disposition) {
    if (disposition != SIGNAL_DELIVERY_TERMINATE ||
            (signal_is_blockable(sig) &&
                    sigset_has(receiver->blocked, sig)) ||
            signal_default_dumps_core(sig))
        return;
    if (sig != SIGKILL_) {
        lock(&fatal_subject->ptrace.lock);
        bool traced = fatal_subject->ptrace.traced ||
                fatal_subject->ptrace.stopped;
        unlock(&fatal_subject->ptrace.lock);
        if (traced)
            return;
    }
    commit_group_fatal_signal(receiver, sig);
}

// 调用方持有 waiting_cond_lock；通知管道是非阻塞的，满管道本身已表示可读。
static void wake_poll_wait_locked(struct task *task) {
    if (!task->waiting_poll_active)
        return;

    ssize_t written;
    do {
        written = write(task->waiting_poll_notify_fd, "", 1);
    } while (written < 0 && errno == EINTR);
    assert(written == 1 || (written < 0 &&
            (errno == EAGAIN || errno == EWOULDBLOCK)));
}

enum signal_delivery_disposition signal_disposition_locked(
        const struct sighand *sighand, int sig) {
    if (signal_is_blockable(sig)) {
        const struct signal_action *action = &sighand->action[sig];
        if (action->handler == SIG_IGN_)
            return SIGNAL_DELIVERY_IGNORE;
        if (action->handler != SIG_DFL_)
            return SIGNAL_DELIVERY_HANDLER;
    }

    if (signal_default_ignored(sig))
        return SIGNAL_DELIVERY_IGNORE;
    switch (sig) {
        case SIGSTOP_: case SIGTSTP_: case SIGTTIN_: case SIGTTOU_:
            return SIGNAL_DELIVERY_STOP;

        default:
            return SIGNAL_DELIVERY_TERMINATE;
    }
}

static void wake_signal_task_unlocked(struct task *task, int sig) {

    if (sigset_has(task->blocked & ~task->waiting, sig) && signal_is_blockable(sig))
        return;

    if (task != current) {
        pthread_kill(task_thread_load(task), SIGUSR1);

        // wake up any pthread condition waiters
        // actual madness, I hope to god it's correct
        // must release the sighand lock while going insane, to avoid a deadlock
        unlock(&task->sighand->lock);
        bool poll_notified = false;
retry:
        lock(&task->waiting_cond_lock);
        if (!poll_notified) {
            wake_poll_wait_locked(task);
            poll_notified = true;
        }
        if (task->waiting_cond != NULL) {
            bool mine = false;
            if (trylock(task->waiting_lock) == EBUSY) {
                if (lock_owned_by_current(task->waiting_lock))
                    mine = true;
                if (!mine) {
                    unlock(&task->waiting_cond_lock);
                    goto retry;
                }
            }
            notify(task->waiting_cond);
            if (!mine)
                unlock(task->waiting_lock);
        }
        unlock(&task->waiting_cond_lock);
        lock(&task->sighand->lock);
    }
}

static int deliver_signal_unlocked(struct task *task, int sig,
        struct siginfo_ info, enum signal_queue_policy policy,
        uid_t_ uid, rlim_t_ limit) {
    int result = signal_enqueue_locked(
            task, sig, info, policy, uid, limit);
    if (result == SIGNAL_ENQUEUE_QUEUED ||
            result == SIGNAL_ENQUEUE_BIT_ONLY)
        wake_signal_task_unlocked(task, sig);
    return result < 0 ? result : 0;
}

// 调用方持有 pids_lock 与共享 sighand 锁。
static void discard_group_signal_locked(
        struct task *subject, int sig) {
    struct tgroup *group = subject->group;
    if (group == NULL) {
        signal_discard_pending_locked(subject, sig);
        return;
    }
    signal_discard_group_pending_locked(group, sig);
    if (list_null(&group->threads)) {
        signal_discard_pending_locked(subject, sig);
        return;
    }
    struct task *member;
    list_for_each_entry(&group->threads, member, group_links)
        signal_discard_pending_locked(member, sig);
}

// stop/continue 的进程级副作用发生在生成时，与动作和阻塞状态无关。
static void prepare_job_control_signal_locked(
        struct task *subject, int sig) {
    if (sigset_has(JOB_CONTROL_STOP_MASK, sig)) {
        discard_group_signal_locked(subject, SIGCONT_);
        return;
    }
    if (sig != SIGCONT_)
        return;
    for (int stop = 1; stop <= NUM_SIGS; stop++)
        if (sigset_has(JOB_CONTROL_STOP_MASK, stop))
            discard_group_signal_locked(subject, stop);
}

static int enqueue_external_signal_locked(struct task *task,
        struct task *receiver, struct task *fatal_subject,
        int sig, struct siginfo_ info,
        enum signal_queue_policy policy, rlim_t_ limit,
        bool process_directed) {
    enum signal_delivery_disposition disposition =
            signal_disposition_locked(task->sighand, sig);
    if (disposition == SIGNAL_DELIVERY_IGNORE &&
            !sigset_has(task->blocked, sig))
        return 0;

    int result = process_directed ?
            signal_enqueue_process_locked(
                    task, sig, info, policy, task->uid, limit) :
            signal_enqueue_locked(
                    task, sig, info, policy, task->uid, limit);
    if (result < 0)
        return result;
    if (result == SIGNAL_ENQUEUE_QUEUED ||
            result == SIGNAL_ENQUEUE_BIT_ONLY) {
        // complete_signal 在唤醒前不可撤销地提交默认非 core 组退出。
        if (receiver != NULL)
            record_external_fatal_signal_locked(
                    receiver, fatal_subject, sig, disposition);
        if (receiver != NULL)
            wake_signal_task_unlocked(receiver, sig);
    }
    return 0;
}

static bool task_live_for_process_signal(const struct task *task) {
    return task != NULL && !task->zombie && !task->exiting;
}

// 调用方持有共享 sighand 与 pids_lock；近似 Linux wants_signal。
static bool task_wants_signal_locked(
        struct task *task, int sig, bool group_stopped) {
    if (!task_live_for_process_signal(task))
        return false;
    if (sig == SIGKILL_)
        return true;
    sigset_t_ effective_blocked = task->blocked & ~task->waiting;
    if (sigset_has(effective_blocked, sig) || group_stopped ||
            (task != current &&
                    (task->pending & ~effective_blocked) != 0))
        return false;

    lock(&task->ptrace.lock);
    bool ptrace_stopped = task->ptrace.stopped;
    unlock(&task->ptrace.lock);
    return !ptrace_stopped;
}

// 队列归线程组所有；这里仅选择需要立即唤醒的执行线程。
static struct task *select_process_signal_receiver_locked(
        struct task *suggested, int sig) {
    struct tgroup *group = suggested->group;
    lock(&group->lock);
    bool group_stopped = group->stopped;
    unlock(&group->lock);

    struct task *preferred[] = {
        group->exec_task,
        suggested,
        group->leader,
    };
    for (size_t index = 0; index < array_size(preferred); index++) {
        if (task_wants_signal_locked(
                    preferred[index], sig, group_stopped))
            return preferred[index];
    }
    struct task *thread;
    list_for_each_entry(&group->threads, thread, group_links) {
        if (task_wants_signal_locked(
                    thread, sig, group_stopped))
            return thread;
    }
    return NULL;
}

void signal_retarget_shared_pending_locked(
        struct task *task, sigset_t_ which) {
    if (task == NULL || task->group == NULL ||
            list_null(&task->group->threads))
        return;
    sigset_t_ pending = task->group->shared_pending & which;
    while (pending != 0) {
        int sig = __builtin_ctzll(pending) + 1;
        struct task *receiver =
                select_process_signal_receiver_locked(task, sig);
        if (receiver != NULL)
            wake_signal_task_unlocked(receiver, sig);
        sigset_del(&pending, sig);
    }
}

void signal_retarget_shared_pending(
        struct task *task, sigset_t_ which) {
    if (task == NULL || task->group == NULL ||
            task->sighand == NULL || which == 0)
        return;
    lock(&pids_lock);
    lock(&task->sighand->lock);
    signal_retarget_shared_pending_locked(task, which);
    unlock(&task->sighand->lock);
    unlock(&pids_lock);
}

// 调用方持有 pids_lock；选择、排队与组致死判定共享同一个 sighand 临界区。
static int send_process_signal_locked(struct task *suggested, int sig,
        struct siginfo_ info, enum signal_queue_policy policy) {
    if (sig == 0)
        return 0;

    struct task *representative =
            task_process_representative_locked(suggested);
    if (representative == NULL)
        return 0;
    struct sighand *sighand = representative->sighand;
    assert(sighand != NULL);
    lock(&sighand->lock);
    prepare_job_control_signal_locked(representative, sig);
    resume_group_for_signal(representative, sig);
    struct task *receiver =
            select_process_signal_receiver_locked(suggested, sig);
    assert(receiver == NULL || receiver->sighand == sighand);
    rlim_t_ limit = policy == SIGNAL_QUEUE_EXPLICIT ||
            sig >= SIGRTMIN_ ?
            rlimit_task(representative, RLIMIT_SIGPENDING_) :
            RLIM_INFINITY_;
    int error = enqueue_external_signal_locked(
            representative, receiver, suggested,
            sig, info, policy, limit, true);
    unlock(&sighand->lock);
    struct task *resume_target = receiver != NULL ?
            receiver : representative;
    resume_ptrace_stop_for_sigkill(resume_target, sig);
    return error;
}

void send_process_signal(
        struct task *suggested, int sig, struct siginfo_ info) {
    int error = send_process_signal_locked(
            suggested, sig, info, SIGNAL_QUEUE_LEGACY);
    assert(error == 0);
}

static void resume_ptrace_stop_for_sigkill(struct task *task, int sig) {
    if (sig != SIGKILL_)
        return;
    lock(&task->ptrace.lock);
    task->ptrace.stopped = false;
    notify(&task->ptrace.cond);
    unlock(&task->ptrace.lock);
}

static void resume_group_for_signal(struct task *task, int sig) {
    if ((sig != SIGCONT_ && sig != SIGKILL_) || task->group == NULL)
        return;
    lock(&task->group->lock);
    bool resumed = sig == SIGCONT_ && task->group->stopped;
    task->group->stopped = false;
    task->group->stop_code = 0;
    if (resumed) {
        task->group->continued = true;
        task->group->continue_notification_pending = true;
    }
    notify(&task->group->stopped_cond);
    unlock(&task->group->lock);
}

void deliver_signal_locked(
        struct task *task, int sig, struct siginfo_ info) {
    lock(&task->sighand->lock);
    prepare_job_control_signal_locked(task, sig);
    resume_group_for_signal(task, sig);
    deliver_signal_unlocked(task, sig, info,
            SIGNAL_QUEUE_FORCE, task->uid, RLIM_INFINITY_);
    unlock(&task->sighand->lock);
    resume_ptrace_stop_for_sigkill(task, sig);
}

void deliver_signal(struct task *task, int sig, struct siginfo_ info) {
    lock(&pids_lock);
    deliver_signal_locked(task, sig, info);
    unlock(&pids_lock);
}

void send_signal_locked(struct task *task, int sig,
        struct siginfo_ info) {
    // signal zero is for testing whether a process exists
    if (sig == 0)
        return;
    if (task->zombie || task->exiting)
        return;

    struct sighand *sighand = task->sighand;
    lock(&sighand->lock);
    prepare_job_control_signal_locked(task, sig);
    resume_group_for_signal(task, sig);
    lock(&task->group->lock);
    bool group_stopped = task->group->stopped;
    unlock(&task->group->lock);
    struct task *fatal_candidate =
            task_wants_signal_locked(task, sig, group_stopped) ?
            task : NULL;
    rlim_t_ limit = sig >= SIGRTMIN_ ?
            rlimit_task(task, RLIMIT_SIGPENDING_) : RLIM_INFINITY_;
    (void) enqueue_external_signal_locked(
            task, fatal_candidate, task, sig, info,
            SIGNAL_QUEUE_LEGACY, limit, false);
    unlock(&sighand->lock);
    resume_ptrace_stop_for_sigkill(task, sig);
}

void send_signal(struct task *task, int sig, struct siginfo_ info) {
    lock(&pids_lock);
    send_signal_locked(task, sig, info);
    unlock(&pids_lock);
}

bool task_sigkill_pending(struct task *task) {
    lock(&task->sighand->lock);
    bool pending = sigset_has(task->pending, SIGKILL_);
    unlock(&task->sighand->lock);
    return pending;
}

int signal_forced_exit_locked(struct task *task) {
    assert(task != NULL && task->group != NULL);
    int external_fatal = task_group_fatal_signal(task);
    lock(&task->group->lock);
    bool group_exit = task->group->doing_group_exit;
    bool exec_peer = task->group->exec_task != NULL &&
            task->group->exec_task != task;
    unlock(&task->group->lock);
    if (external_fatal != 0)
        return external_fatal;
    return group_exit || exec_peer ? SIGKILL_ : 0;
}

// 调用方持有 pids_lock，动作读取遵循 pids_lock -> sighand->lock。
static struct signal_action signal_parent_action_locked(
        struct task *parent, int signal) {
    struct task *representative =
            task_process_representative_locked(parent);
    if (representative == NULL)
        return (struct signal_action) {.handler = SIG_DFL_};
    struct sighand *sighand = representative->sighand;
    lock(&sighand->lock);
    struct signal_action action = sighand->action[signal];
    unlock(&sighand->lock);
    return action;
}

void signal_notify_parent_child_state(struct task *task) {
    lock(&pids_lock);
    struct task *leader = task->group->leader;
    struct task *parent = leader->parent;
    if (parent != NULL) {
        notify(&parent->group->child_exit);

        bool send_state_signal = leader->exit_signal != 0;
        if (leader->exit_signal == SIGCHLD_) {
            struct signal_action action =
                    signal_parent_action_locked(parent, SIGCHLD_);
            send_state_signal = action.handler != SIG_IGN_ &&
                    !(action.flags & SA_NOCLDSTOP_);
        }
        if (send_state_signal)
            send_process_signal(
                    parent, leader->exit_signal, SIGINFO_NIL);
    }
    unlock(&pids_lock);
}

bool signal_parent_child_exit_policy_locked(
        struct task *parent, int exit_signal, bool *send_exit_signal) {
    assert(parent != NULL && send_exit_signal != NULL);
    *send_exit_signal = exit_signal != 0;
    if (exit_signal != SIGCHLD_)
        return false;

    struct signal_action action =
            signal_parent_action_locked(parent, SIGCHLD_);
    bool explicitly_ignored = action.handler == SIG_IGN_;
    *send_exit_signal = !explicitly_ignored;
    return explicitly_ignored || (action.flags & SA_NOCLDWAIT_);
}

void signal_notify_group_continue(struct task *task) {
    lock(&task->group->lock);
    bool pending = task->group->continue_notification_pending;
    unlock(&task->group->lock);
    if (!pending)
        return;

    lock(&task->group->lock);
    bool should_notify = task->group->continue_notification_pending;
    if (should_notify)
        task->group->continue_notification_pending = false;
    unlock(&task->group->lock);
    if (should_notify)
        signal_notify_parent_child_state(task);
}

bool try_self_signal(int sig) {
    assert(sig == SIGTTIN_ || sig == SIGTTOU_);

    lock(&pids_lock);
    struct sighand *sighand = current->sighand;
    lock(&sighand->lock);
    bool can_send = signal_disposition_locked(sighand, sig) !=
            SIGNAL_DELIVERY_IGNORE &&
        !sigset_has(current->blocked, sig);
    if (can_send) {
        prepare_job_control_signal_locked(current, sig);
        deliver_signal_unlocked(current, sig, SIGINFO_NIL,
                SIGNAL_QUEUE_FORCE, current->uid, RLIM_INFINITY_);
    }
    unlock(&sighand->lock);
    unlock(&pids_lock);
    return can_send;
}

int send_group_signal(dword_t pgid, int sig, struct siginfo_ info) {
    lock(&pids_lock);
    struct pid *pid = pid_get(pgid);
    if (pid == NULL) {
        unlock(&pids_lock);
        return _ESRCH;
    }
    struct tgroup *tgroup;
    list_for_each_entry(&pid->pgroup, tgroup, pgroup) {
        (void) send_process_signal_locked(
                tgroup->leader, sig, info, SIGNAL_QUEUE_LEGACY);
    }
    unlock(&pids_lock);
    return 0;
}

static addr_t sigreturn_trampoline(const char *name) {
    addr_t sigreturn_addr = vdso_symbol(name);
    if (sigreturn_addr == 0) {
        die("sigreturn not found in vdso, this should never happen");
    }
    return current->mm->vdso + sigreturn_addr;
}

static void setup_sigcontext(struct sigcontext_ *sc, struct cpu_state *cpu) {
    sc->ax = cpu->eax;
    sc->bx = cpu->ebx;
    sc->cx = cpu->ecx;
    sc->dx = cpu->edx;
    sc->di = cpu->edi;
    sc->si = cpu->esi;
    sc->bp = cpu->ebp;
    sc->sp = sc->sp_at_signal = cpu->esp;
    sc->ip = cpu->eip;
    collapse_flags(cpu);
    sc->flags = cpu->eflags;
    sc->trapno = cpu->trapno;
    if (cpu->trapno == INT_GPF)
        sc->cr2 = cpu->segfault_addr;
    // TODO more shit
    sc->oldmask = current->blocked & 0xffffffff;
}

static void setup_sigframe(struct siginfo_ *info, struct sigframe_ *frame) {
    frame->restorer = sigreturn_trampoline("__kernel_sigreturn");
    frame->sig = info->sig;
    setup_sigcontext(&frame->sc, &current->cpu);
    frame->extramask = current->blocked >> 32;

    static const struct {
        uint16_t popmov;
        uint32_t nr_sigreturn;
        uint16_t int80;
    } __attribute__((packed)) retcode = {
        .popmov = 0xb858,
        .nr_sigreturn = 113,
        .int80 = 0x80cd,
    };
    memcpy(frame->retcode, &retcode, sizeof(retcode));
}

static void setup_rt_sigframe(struct siginfo_ *info, struct rt_sigframe_ *frame) {
    frame->restorer = sigreturn_trampoline("__kernel_rt_sigreturn");
    frame->sig = info->sig;
    frame->info = pack_i386_siginfo(info);
    frame->uc.flags = 0;
    frame->uc.link = 0;
    // 返回帧保存可恢复的配置标志，而不是动态计算的 ONSTACK 状态。
    frame->uc.stack = (struct stack_t_) {
        .stack = (addr_t) current->altstack.stack,
        .flags = current->altstack.flags,
        .size = (dword_t) current->altstack.size,
    };
    setup_sigcontext(&frame->uc.mcontext, &current->cpu);
    frame->uc.sigmask = current->blocked;

    static const struct {
        uint8_t mov;
        uint32_t nr_rt_sigreturn;
        uint16_t int80;
        uint8_t pad;
    } __attribute__((packed)) rt_retcode = {
        .mov = 0xb8,
        .nr_rt_sigreturn = 173,
        .int80 = 0x80cd,
    };
    memcpy(frame->retcode, &rt_retcode, sizeof(rt_retcode));
}

static void force_signal_info_locked(
        struct sighand *sighand, int signal, bool reset_action,
        const struct siginfo_ *precise_info) {
    assert(current != NULL && current->sighand == sighand &&
            signal >= 1 && signal <= NUM_SIGS);
    struct signal_action *action = &sighand->action[signal];
    bool was_blocked = sigset_has(current->blocked, signal);
    if (reset_action || was_blocked || action->handler == SIG_IGN_) {
        *action = (struct signal_action) {
            .handler = SIG_DFL_,
        };
    }
    sigset_del(&current->blocked, signal);
    deliver_signal_unlocked(current, signal,
            precise_info != NULL ? *precise_info : SIGINFO_NIL,
            SIGNAL_QUEUE_FORCE, current->uid, RLIM_INFINITY_);

    // 同步故障必须先于队列里已有的异步信号处理。
    struct sigqueue *forced;
    list_for_each_entry(&current->queue, forced, queue) {
        if (forced->info.sig == signal) {
            // 强制故障必须覆盖合并前的信息，才能保留准确故障地址。
            if (precise_info != NULL) {
                forced->info = *precise_info;
                forced->info.sig = signal;
            }
            list_remove(&forced->queue);
            list_add(&current->queue, &forced->queue);
            return;
        }
    }
    // 内存耗尽时保留 bit-only 强制信号；后续派送会合成 SI_USER 信息。
}

static void force_sigsegv_locked(
        struct sighand *sighand, int failed_signal,
        const struct siginfo_ *precise_info) {
    force_signal_info_locked(sighand, SIGSEGV_,
            failed_signal == SIGSEGV_, precise_info);
}

void signal_force_sigsegv_locked(
        struct sighand *sighand, int failed_signal) {
    force_sigsegv_locked(sighand, failed_signal, NULL);
}

void signal_force_sigsegv_info_locked(
        struct sighand *sighand, int failed_signal,
        const struct siginfo_ *info) {
    assert(info != NULL);
    force_sigsegv_locked(sighand, failed_signal, info);
}

void signal_force_sync_info_locked(
        struct sighand *sighand, int signal,
        const struct siginfo_ *info) {
    assert(info != NULL && info->sig == signal &&
            sigset_has(SYNCHRONOUS_MASK, signal));
    force_signal_info_locked(sighand, signal, false, info);
}

static bool receive_signal(struct sighand *sighand, struct siginfo_ *info) {
    int sig = info->sig;
    STRACE("%d receiving signal %d\n", current->pid, sig);

    switch (signal_disposition_locked(sighand, sig)) {
        case SIGNAL_DELIVERY_IGNORE:
            return true;

        case SIGNAL_DELIVERY_STOP:
            lock(&current->group->lock);
            current->group->stopped = true;
            current->group->continued = false;
            current->group->continue_notification_pending = false;
            current->group->stop_code = sig << 8 | 0x7f;
            unlock(&current->group->lock);
            return true;

        case SIGNAL_DELIVERY_TERMINATE:
            unlock(&sighand->lock); // do_exit must be called without this lock
            do_exit_group(sig);

        case SIGNAL_DELIVERY_HANDLER:
            break;
    }

    struct signal_action *action = &sighand->action[info->sig];
    bool need_siginfo = action->flags & SA_SIGINFO_;

    // setup the frame
    union {
        struct sigframe_ sigframe;
        struct rt_sigframe_ rt_sigframe;
    } frame = {};
    size_t frame_size;
    if (need_siginfo) {
        setup_rt_sigframe(info, &frame.rt_sigframe);
        frame_size = sizeof(frame.rt_sigframe);
    } else {
        setup_sigframe(info, &frame.sigframe);
        frame_size = sizeof(frame.sigframe);
    }

    dword_t sp;
    if (!i386_signal_frame_pointer(current, action,
            current->cpu.esp, frame_size, &sp)) {
        printk("signal frame for %d does not fit on the guest stack\n", sig);
        signal_force_sigsegv_locked(sighand, sig);
        return false;
    }

    // these have to be filled in after the location of the frame is known
    if (need_siginfo) {
        frame.rt_sigframe.pinfo = sp + offsetof(struct rt_sigframe_, info);
        frame.rt_sigframe.puc = sp + offsetof(struct rt_sigframe_, uc);
    }

    // install frame
    if (user_write(sp, &frame, frame_size)) {
        printk("failed to install frame for %d at %#x\n", info->sig, sp);
        signal_force_sigsegv_locked(sighand, sig);
        return false;
    }

    // 只有完整帧落地后，handler 寄存器和掩码才对 guest 可见。
    current->cpu.eax = info->sig;
    current->cpu.eip = (dword_t) action->handler;
    current->cpu.esp = sp;
    if (need_siginfo) {
        current->cpu.edx = frame.rt_sigframe.pinfo;
        current->cpu.ecx = frame.rt_sigframe.puc;
    }
    if (!(action->flags & SA_NODEFER_))
        sigset_add(&current->blocked, info->sig);
    current->blocked |= action->mask;

    if (action->flags & SA_RESETHAND_)
        *action = (struct signal_action) {.handler = SIG_DFL_};
    return true;
}

// 调用前后均持有 sighand；通知父任务与睡眠期间会暂时释放它。
void signal_ptrace_stop_locked(
        int sig, const struct siginfo_ *info) {
    if (sigset_has(current->pending, SIGKILL_))
        return;

    lock(&current->ptrace.lock);
    current->ptrace.stopped = true;
    current->ptrace.signal = sig;
    current->ptrace.info = *info;
    unlock(&current->ptrace.lock);

    // ptrace 父子可能共享 sighand，通知前必须先释放它。
    unlock(&current->sighand->lock);

    lock(&pids_lock);
    struct task *parent = current->parent;
    if (parent != NULL) {
        notify(&parent->group->child_exit);
        // TODO add siginfo
        send_process_signal(parent,
                current->group->leader->exit_signal, SIGINFO_NIL);
    }
    unlock(&pids_lock);

    lock(&current->ptrace.lock);
    while (current->ptrace.stopped)
        wait_for_ignore_signals(
                &current->ptrace.cond, &current->ptrace.lock, NULL);
    unlock(&current->ptrace.lock);
    lock(&current->sighand->lock);
}

void receive_signals(void) {
    lock(&current->group->lock);
    bool was_stopped = current->group->stopped;
    unlock(&current->group->lock);

    struct sighand *sighand = current->sighand;
    lock(&sighand->lock);
    sigset_t_ previous_blocked = current->blocked;

    sigset_t_ blocked = signal_prepare_delivery_locked(current);

    while (true) {
        int forced_exit = signal_forced_exit_locked(current);
        if (forced_exit != 0) {
            if (forced_exit == SIGKILL_ &&
                    sigset_has(current->pending, SIGKILL_))
                signal_discard_pending_locked(current, SIGKILL_);
            unlock(&sighand->lock);
            do_exit_group(forced_exit);
        }

        struct siginfo_ info;
        if (!signal_take_unblocked_locked(
                current, blocked, &info))
            break;

        int sig = info.sig;

        if (current->ptrace.traced && sig != SIGKILL_) {
            // This notifies the parent, goes to sleep, and waits for the
            // parent to tell it to continue.
            // Any signals received while waiting are left on the queue, except
            // for SIGKILL_, which causes an immediate exit.
            signal_ptrace_stop_locked(sig, &info);
        } else if (!receive_signal(sighand, &info)) {
            // 返回 guest 前立即消费强制故障；二次建帧失败会转为致命处置。
            sigset_del(&blocked, SIGSEGV_);
            continue;
        } else {
            // handler 新增的阻塞位必须立即约束本轮后续派送。
            blocked = current->blocked;
        }
    }

    sigset_t_ newly_blocked =
            current->blocked & ~previous_blocked;
    unlock(&sighand->lock);
    signal_retarget_shared_pending(current, newly_blocked);

    // this got moved out of the switch case in receive_signal to fix locking problems
    if (!was_stopped) {
        lock(&current->group->lock);
        bool now_stopped = current->group->stopped;
        unlock(&current->group->lock);
        if (now_stopped)
            signal_notify_parent_child_state(current);
    }
}

static void restore_sigcontext(struct sigcontext_ *context, struct cpu_state *cpu) {
    cpu->eax = context->ax;
    cpu->ebx = context->bx;
    cpu->ecx = context->cx;
    cpu->edx = context->dx;
    cpu->edi = context->di;
    cpu->esi = context->si;
    cpu->ebp = context->bp;
    cpu->esp = context->sp;
    cpu->eip = context->ip;
    collapse_flags(cpu);

    // Use AC, RF, OF, DF, TF, SF, ZF, AF, PF, CF
#define USE_FLAGS 0b1010000110111010101
    cpu->eflags = (context->flags & USE_FLAGS) | (cpu->eflags & ~USE_FLAGS);
}

dword_t sys_rt_sigreturn(void) {
    struct cpu_state *cpu = &current->cpu;
    struct rt_sigframe_ frame;
    // esp points past the first field of the frame
    if (user_get(cpu->esp - offsetof(struct rt_sigframe_, sig), frame)) {
        deliver_signal(current, SIGSEGV_, SIGINFO_NIL);
        return _EFAULT;
    }
    restore_sigcontext(&frame.uc.mcontext, cpu);

    struct signal_altstack restored_altstack = {
        .stack = frame.uc.stack.stack,
        .size = frame.uc.stack.size,
        .flags = frame.uc.stack.flags,
    };
    (void) task_sigaltstack(current, cpu->esp,
            &restored_altstack, NULL, MINSIGSTKSZ_, UINT32_MAX);
    lock(&current->sighand->lock);
    sigset_t_ previous_blocked = current->blocked;
    sigmask_set(current, frame.uc.sigmask);
    sigset_t_ newly_blocked = current->blocked & ~previous_blocked;
    unlock(&current->sighand->lock);
    signal_retarget_shared_pending(current, newly_blocked);
    return cpu->eax;
}

dword_t sys_sigreturn(void) {
    struct cpu_state *cpu = &current->cpu;
    struct sigframe_ frame;
    // esp points past the first two fields of the frame
    if (user_get(cpu->esp - offsetof(struct sigframe_, sc), frame)) {
        deliver_signal(current, SIGSEGV_, SIGINFO_NIL);
        return _EFAULT;
    }
    restore_sigcontext(&frame.sc, cpu);

    lock(&current->sighand->lock);
    sigset_t_ previous_blocked = current->blocked;
    sigset_t_ oldmask = ((sigset_t_) frame.extramask << 32) | frame.sc.oldmask;
    sigmask_set(current, oldmask);
    sigset_t_ newly_blocked = current->blocked & ~previous_blocked;
    unlock(&current->sighand->lock);
    signal_retarget_shared_pending(current, newly_blocked);
    return cpu->eax;
}

struct sighand *sighand_new(void) {
    struct sighand *sighand = malloc(sizeof(struct sighand));
    if (sighand == NULL)
        return NULL;
    memset(sighand, 0, sizeof(struct sighand));
    sighand->refcount = 1;
    lock_init(&sighand->lock);
    return sighand;
}

struct sighand *sighand_copy(struct sighand *sighand) {
    struct sighand *new_sighand = sighand_new();
    if (new_sighand == NULL)
        return NULL;
    lock(&sighand->lock);
    memcpy(new_sighand->action, sighand->action, sizeof(new_sighand->action));
    unlock(&sighand->lock);
    return new_sighand;
}

void sighand_release(struct sighand *sighand) {
    if (--sighand->refcount == 0) {
        free(sighand);
    }
}

static bool signal_action_discards_pending(
        int sig, const struct signal_action *action) {
    if (action->handler == SIG_IGN_)
        return true;
    return action->handler == SIG_DFL_ && signal_default_ignored(sig);
}

int task_sigaction(struct task *task, int sig,
        const struct signal_action *action,
        struct signal_action *oldaction) {
    if (sig < 1 || sig > NUM_SIGS)
        return _EINVAL;
    if (action != NULL && !signal_is_blockable(sig))
        return _EINVAL;

    struct signal_action normalized;
    if (action != NULL) {
        normalized = *action;
        normalized.mask &= ~UNBLOCKABLE_MASK;
        action = &normalized;
    }

    bool discard_pending = action != NULL &&
            signal_action_discards_pending(sig, action);
    if (action != NULL)
        lock(&pids_lock);
    struct sighand *sighand = task->sighand;
    lock(&sighand->lock);
    if (oldaction)
        *oldaction = sighand->action[sig];
    if (action) {
        sighand->action[sig] = *action;
        if (discard_pending)
            discard_group_signal_locked(task, sig);
    }
    unlock(&sighand->lock);
    if (action != NULL)
        unlock(&pids_lock);
    return 0;
}

void task_signal_exec_reset_actions(struct task *task) {
    struct sighand *sighand = task->sighand;
    lock(&sighand->lock);
    for (int sig = 1; sig <= NUM_SIGS; sig++) {
        qword_t handler = sighand->action[sig].handler;
        sighand->action[sig] = (struct signal_action) {
            .handler = handler == SIG_IGN_ ? SIG_IGN_ : SIG_DFL_,
        };
    }
    unlock(&sighand->lock);
}

void task_signal_exec_reset(struct task *task) {
    task_altstack_reset(task);
    task_signal_exec_reset_actions(task);
}

static struct signal_action unpack_i386_sigaction(
        const struct i386_sigaction *wire) {
    return (struct signal_action) {
        .handler = wire->handler,
        .flags = wire->flags,
        .restorer = wire->restorer,
        .mask = wire->mask,
    };
}

static struct i386_sigaction pack_i386_sigaction(
        const struct signal_action *action) {
    return (struct i386_sigaction) {
        .handler = (dword_t) action->handler,
        .flags = (dword_t) action->flags,
        .restorer = (dword_t) action->restorer,
        .mask = action->mask,
    };
}

dword_t sys_rt_sigaction(dword_t signum, addr_t action_addr, addr_t oldaction_addr, dword_t sigset_size) {
    if (sigset_size != sizeof(sigset_t_))
        return _EINVAL;
    struct signal_action action = {0}, oldaction;
    if (action_addr != 0) {
        struct i386_sigaction wire_action;
        if (user_get(action_addr, wire_action))
            return _EFAULT;
        action = unpack_i386_sigaction(&wire_action);
    }
    STRACE("rt_sigaction(%d, %#x {handler=%#llx, flags=%#llx, restorer=%#llx, mask=%#llx}, 0x%x, %d)",
            (sdword_t) signum,
            action_addr, (unsigned long long) action.handler,
            (unsigned long long) action.flags,
            (unsigned long long) action.restorer,
            (unsigned long long) action.mask, oldaction_addr,
            (sdword_t) sigset_size);

    int err = task_sigaction(current, (sdword_t) signum,
            action_addr ? &action : NULL,
            oldaction_addr ? &oldaction : NULL);
    if (err < 0)
        return err;

    if (oldaction_addr != 0) {
        struct i386_sigaction wire_oldaction =
                pack_i386_sigaction(&oldaction);
        if (user_put(oldaction_addr, wire_oldaction))
            return _EFAULT;
    }
    return err;
}

dword_t sys_sigaction(dword_t signum, addr_t action_addr, addr_t oldaction_addr) {
    return sys_rt_sigaction(signum, action_addr, oldaction_addr, 1);
}

static void sigmask_set(struct task *task, sigset_t_ set) {
    task->blocked = (set & ~UNBLOCKABLE_MASK);
}

static void sigmask_set_temp_unlocked(
        struct task *task, sigset_t_ mask) {
    task->saved_mask = task->blocked;
    task->has_saved_mask = true;
    sigmask_set(task, mask);
}

void sigmask_set_temp_task(struct task *task, sigset_t_ mask) {
    lock(&task->sighand->lock);
    sigset_t_ previous_blocked = task->blocked;
    sigmask_set_temp_unlocked(task, mask);
    sigset_t_ newly_blocked = task->blocked & ~previous_blocked;
    unlock(&task->sighand->lock);
    signal_retarget_shared_pending(task, newly_blocked);
}

void sigmask_set_temp(sigset_t_ mask) {
    sigmask_set_temp_task(current, mask);
}

void sigmask_restore_temp_task(struct task *task) {
    lock(&task->sighand->lock);
    sigset_t_ previous_blocked = task->blocked;
    if (task->has_saved_mask) {
        task->blocked = task->saved_mask;
        task->has_saved_mask = false;
    }
    sigset_t_ newly_blocked = task->blocked & ~previous_blocked;
    unlock(&task->sighand->lock);
    signal_retarget_shared_pending(task, newly_blocked);
}

sigset_t_ signal_prepare_delivery_locked(struct task *task) {
    sigset_t_ selection_mask = task->blocked;
    if (task->has_saved_mask) {
        task->blocked = task->saved_mask;
        task->has_saved_mask = false;
    }
    return selection_mask;
}

int_t task_sigsuspend(struct task *task, sigset_t_ mask) {
    assert(task != NULL && task == current);
    struct sighand *sighand = task->sighand;
    lock(&sighand->lock);
    sigset_t_ previous_blocked = task->blocked;
    sigmask_set_temp_unlocked(task, mask);
    sigset_t_ newly_blocked = task->blocked & ~previous_blocked;
    unlock(&sighand->lock);
    signal_retarget_shared_pending(task, newly_blocked);
    lock(&sighand->lock);
    while (wait_for(&task->pause, &sighand->lock, NULL) != _EINTR)
        continue;
    unlock(&sighand->lock);
    return _EINTR;
}

int task_sigprocmask(struct task *task, dword_t how,
        const sigset_t_ *set, sigset_t_ *oldset) {
    struct sighand *sighand = task->sighand;
    lock(&sighand->lock);
    sigset_t_ previous_blocked = task->blocked;
    if (oldset != NULL)
        *oldset = task->blocked;

    int error = 0;
    if (set != NULL) {
        if (how == SIG_BLOCK_)
            sigmask_set(task, task->blocked | *set);
        else if (how == SIG_UNBLOCK_)
            sigmask_set(task, task->blocked & ~*set);
        else if (how == SIG_SETMASK_)
            sigmask_set(task, *set);
        else
            error = _EINVAL;
    }
    sigset_t_ newly_blocked = error == 0 ?
            task->blocked & ~previous_blocked : 0;
    unlock(&sighand->lock);
    signal_retarget_shared_pending(task, newly_blocked);
    return error;
}

dword_t sys_rt_sigprocmask(dword_t how, addr_t set_addr, addr_t oldset_addr, dword_t size) {
    if (size != sizeof(sigset_t_))
        return _EINVAL;

    sigset_t_ set = 0;
    if (set_addr != 0)
        if (user_get(set_addr, set))
            return _EFAULT;
    STRACE("rt_sigprocmask(%s, %#llx, %#x, %d)",
            how == SIG_BLOCK_ ? "SIG_BLOCK" :
            how == SIG_UNBLOCK_ ? "SIG_UNBLOCK" :
            how == SIG_SETMASK_ ? "SIG_SETMASK" : "??",
            set_addr != 0 ? (long long) set : -1, oldset_addr, size);

    sigset_t_ oldset;
    int err = task_sigprocmask(current, how,
            set_addr != 0 ? &set : NULL,
            oldset_addr != 0 ? &oldset : NULL);
    if (err < 0)
        return err;
    if (oldset_addr != 0 && user_put(oldset_addr, oldset))
        return _EFAULT;
    return 0;
}

sigset_t_ task_sigpending(struct task *task) {
    struct sighand *sighand = task->sighand;
    lock(&sighand->lock);
    sigset_t_ pending =
            signal_pending_mask_locked(task) & task->blocked;
    unlock(&sighand->lock);
    return pending;
}

int_t sys_rt_sigpending(addr_t set_addr, uint_t size) {
    STRACE("rt_sigpending(%#x, %u)", set_addr, size);
    if (size > sizeof(sigset_t_))
        return _EINVAL;
    if (size != 0 && set_addr > UINT32_MAX - size + 1)
        return _EFAULT;
    sigset_t_ pending = task_sigpending(current);
    if (user_write(set_addr, &pending, size))
        return _EFAULT;
    return 0;
}

int_t sys_rt_sigsuspend(addr_t mask_addr, uint_t size) {
    if (size != sizeof(sigset_t_))
        return _EINVAL;
    sigset_t_ mask;
    if (user_get(mask_addr, mask))
        return _EFAULT;
    STRACE("sigsuspend(0x%llx) = ...\n", (long long) mask);

    int error = task_sigsuspend(current, mask);
    STRACE("%d done sigsuspend", current->pid);
    return error;
}

int_t sys_pause(void) {
    lock(&current->sighand->lock);
    while (wait_for(&current->pause, &current->sighand->lock, NULL) != _EINTR)
        continue;
    unlock(&current->sighand->lock);
    return _EINTR;
}

static bool dequeue_waited_signal(
        struct task *task, sigset_t_ set, struct siginfo_ *info) {
    return signal_take_unblocked_locked(task, ~set, info);
}

struct sigtimedwait_deadline {
    int64_t sec;
    int64_t nsec;
};

static struct sigtimedwait_deadline make_sigtimedwait_deadline(
        struct timespec timeout) {
    struct timespec now = timespec_now(CLOCK_MONOTONIC);
    struct sigtimedwait_deadline deadline = {
        .sec = (int64_t) now.tv_sec + timeout.tv_sec,
        .nsec = (int64_t) now.tv_nsec + timeout.tv_nsec,
    };
    if (deadline.nsec >= INT64_C(1000000000)) {
        deadline.sec++;
        deadline.nsec -= INT64_C(1000000000);
    }
    return deadline;
}

static bool sigtimedwait_remaining(
        struct sigtimedwait_deadline deadline,
        struct timespec *remaining) {
    struct timespec now = timespec_now(CLOCK_MONOTONIC);
    int64_t sec = deadline.sec - (int64_t) now.tv_sec;
    int64_t nsec = deadline.nsec - (int64_t) now.tv_nsec;
    if (nsec < 0) {
        sec--;
        nsec += INT64_C(1000000000);
    }
    if (sec < 0 || (sec == 0 && nsec == 0))
        return false;

    // watchOS arm64_32 的 time_t 为 32 位，绝对截止值必须始终留在 64 位域。
    assert(sec <= INT32_MAX);
    remaining->tv_sec = (time_t) sec;
    remaining->tv_nsec = (long) nsec;
    return true;
}

int_t sys_rt_sigtimedwait(addr_t set_addr, addr_t info_addr, addr_t timeout_addr, uint_t set_size) {
    if (set_size != sizeof(sigset_t_))
        return _EINVAL;
    sigset_t_ set;
    if (user_get(set_addr, set))
        return _EFAULT;
    bool has_timeout = timeout_addr != 0;
    struct timespec timeout = {0};
    if (timeout_addr != 0) {
        struct timespec_ fake_timeout;
        if (user_get(timeout_addr, fake_timeout))
            return _EFAULT;
        if ((sdword_t) fake_timeout.sec < 0 ||
                (sdword_t) fake_timeout.nsec < 0 ||
                fake_timeout.nsec >= UINT32_C(1000000000))
            return _EINVAL;
        timeout = convert_timespec(fake_timeout);
    }
    STRACE("sigtimedwait(%#llx, %#x, %#x) = ...\n", (long long) set, info_addr, timeout_addr);

    set &= ~UNBLOCKABLE_MASK;
    bool zero_timeout = has_timeout && timespec_is_zero(timeout);
    struct sigtimedwait_deadline deadline = {0};
    if (has_timeout && !zero_timeout)
        deadline = make_sigtimedwait_deadline(timeout);

    lock(&current->sighand->lock);
    assert(current->waiting == 0);
    struct siginfo_ info;
    if (dequeue_waited_signal(current, set, &info)) {
        unlock(&current->sighand->lock);
        goto copy_info;
    }
    if (zero_timeout) {
        unlock(&current->sighand->lock);
        return _EAGAIN;
    }

    current->waiting = set;
    int err = 0;
    while (true) {
        if (signal_pending_mask_locked(current) &
                ~current->blocked & ~set) {
            err = _EINTR;
            break;
        }

        struct timespec remaining;
        struct timespec *wait_timeout = NULL;
        if (has_timeout) {
            if (!sigtimedwait_remaining(deadline, &remaining)) {
                err = _EAGAIN;
                break;
            }
            wait_timeout = &remaining;
        }

        int wait_err = wait_for_ignore_signals(
                &current->pause, &current->sighand->lock, wait_timeout);
        if (dequeue_waited_signal(current, set, &info))
            break;
        if (wait_err == _ETIMEDOUT) {
            err = _EAGAIN;
            break;
        }
    }
    current->waiting = 0;
    unlock(&current->sighand->lock);
    if (err < 0)
        return err;

copy_info:
    if (info_addr != 0)
        if (write_i386_siginfo(info_addr, &info))
            return _EFAULT;
    STRACE("done sigtimedwait = %d\n", info.sig);
    return info.sig;
}

static bool signal_task_permitted(
        const struct task *sender, const struct task *target) {
    return sender->euid == 0 ||
            sender->uid == target->uid ||
            sender->uid == target->suid ||
            sender->euid == target->uid ||
            sender->euid == target->suid;
}

static struct siginfo_ user_kill_info(void) {
    return (struct siginfo_) {
        .code = SI_USER_,
        .payload_kind = SIGNAL_INFO_PAYLOAD_KILL,
        .kill.pid = current->pid,
        .kill.uid = current->uid,
    };
}

static int kill_task(struct task *task, dword_t sig) {
    if (!signal_task_permitted(current, task))
        return _EPERM;
    send_signal_locked(task, sig, user_kill_info());
    return 0;
}

// 调用方持有 pids_lock；suggested 可以是正在退场的旧 leader。
static int kill_process(struct task *suggested, dword_t sig) {
    if (!signal_task_permitted(current, suggested))
        return _EPERM;
    int error = send_process_signal_locked(
            suggested, sig, user_kill_info(), SIGNAL_QUEUE_LEGACY);
    assert(error == 0);
    return 0;
}

static int send_explicit_queued_signal(
        struct task *task, int signal, const struct siginfo_ *info) {
    if (signal == 0)
        return 0;
    if (task->zombie || task->exiting)
        return 0;

    struct sighand *sighand = task->sighand;
    lock(&sighand->lock);
    prepare_job_control_signal_locked(task, signal);
    resume_group_for_signal(task, signal);
    lock(&task->group->lock);
    bool group_stopped = task->group->stopped;
    unlock(&task->group->lock);
    struct task *receiver =
            task_wants_signal_locked(task, signal, group_stopped) ?
            task : NULL;
    int error = enqueue_external_signal_locked(
            task, receiver, task, signal, *info,
            SIGNAL_QUEUE_EXPLICIT,
            rlimit_task(task, RLIMIT_SIGPENDING_), false);
    unlock(&sighand->lock);
    resume_ptrace_stop_for_sigkill(task, signal);
    resume_group_for_signal(task, signal);
    if (error < 0)
        return error;
    return 0;
}

static int validate_explicit_queue_info(
        int signal, const struct siginfo_ *info) {
    if (signal < 0 || signal > NUM_SIGS)
        return _EINVAL;
    if (info == NULL || info->code != SI_QUEUE_ ||
            info->payload_kind != SIGNAL_INFO_PAYLOAD_QUEUE)
        return _EPERM;
    return 0;
}

int task_rt_sigqueueinfo(pid_t_ pid, int signal,
        const struct siginfo_ *info) {
    int error = validate_explicit_queue_info(signal, info);
    if (error < 0)
        return error;

    lock(&pids_lock);
    struct task *found = pid > 0 ? pid_get_task_zombie(pid) : NULL;
    if (found == NULL) {
        unlock(&pids_lock);
        return _ESRCH;
    }
    if (!signal_task_permitted(current, found)) {
        unlock(&pids_lock);
        return _EPERM;
    }
    if (signal == 0) {
        unlock(&pids_lock);
        return 0;
    }

    // Linux 对已经存在但完全退出的 zombie 仍把发送视为成功。
    error = send_process_signal_locked(
            found, signal, *info, SIGNAL_QUEUE_EXPLICIT);
    unlock(&pids_lock);
    return error;
}

int task_rt_tgsigqueueinfo(pid_t_ tgid, pid_t_ tid, int signal,
        const struct siginfo_ *info) {
    int error = validate_explicit_queue_info(signal, info);
    if (error < 0)
        return error;
    if (tgid <= 0 || tid <= 0)
        return _EINVAL;

    lock(&pids_lock);
    struct task *target = pid_get_task(tid);
    if (target == NULL || target->tgid != tgid) {
        unlock(&pids_lock);
        return _ESRCH;
    }
    if (!signal_task_permitted(current, target)) {
        unlock(&pids_lock);
        return _EPERM;
    }
    error = send_explicit_queued_signal(target, signal, info);
    unlock(&pids_lock);
    return error;
}

dword_t sys_rt_sigqueueinfo(
        pid_t_ pid, dword_t signal, addr_t info_addr) {
    struct i386_siginfo wire;
    if (user_get(info_addr, wire))
        return _EFAULT;
    struct siginfo_ info = unpack_i386_sigqueueinfo(
            (sdword_t) signal, &wire);
    return task_rt_sigqueueinfo(pid, (sdword_t) signal, &info);
}

dword_t sys_rt_tgsigqueueinfo(pid_t_ tgid, pid_t_ tid,
        dword_t signal, addr_t info_addr) {
    struct i386_siginfo wire;
    if (user_get(info_addr, wire))
        return _EFAULT;
    struct siginfo_ info = unpack_i386_sigqueueinfo(
            (sdword_t) signal, &wire);
    return task_rt_tgsigqueueinfo(
            tgid, tid, (sdword_t) signal, &info);
}

static int kill_group(pid_t_ pgid, dword_t sig) {
    struct pid *pid = pid_get(pgid);
    if (pid == NULL)
        return _ESRCH;
    struct tgroup *tgroup;
    int err = _EPERM;
    list_for_each_entry(&pid->pgroup, tgroup, pgroup) {
        int kill_err = kill_process(tgroup->leader, sig);
        // killing a group should return an error only if no process can be signaled
        if (err == _EPERM)
            err = kill_err;
    }
    return err;
}

static int kill_everything(dword_t sig) {
    int err = _EPERM;
    for (int i = 2; i < MAX_PID; i++) {
        struct task *task = pid_get_task_zombie(i);
        if (task == NULL || task->group == current->group ||
                !task_is_leader(task))
            continue;
        int kill_err = kill_process(task, sig);
        if (err == _EPERM)
            err = kill_err;
    }
    return err;
}

static int do_kill(
        pid_t_ pid, dword_t sig, pid_t_ tgid,
        bool process_directed) {
    STRACE("kill(%d, %d)", pid, sig);
    if (sig > NUM_SIGS)
        return _EINVAL;
    if (pid == 0)
        pid = -current->group->pgid;

    int err;
    lock(&pids_lock);

    if (pid == -1) {
        err = kill_everything(sig);
    } else if (pid < 0) {
        err = kill_group(-pid, sig);
    } else {
        struct task *task = process_directed ?
                pid_get_task_zombie(pid) : pid_get_task(pid);
        if (task == NULL) {
            unlock(&pids_lock);
            return _ESRCH;
        }

        // If tgid is nonzero, it must be correct
        if (tgid != 0 && task->tgid != tgid) {
            unlock(&pids_lock);
            return _ESRCH;
        }

        err = process_directed ?
                kill_process(task, sig) : kill_task(task, sig);
    }

    unlock(&pids_lock);
    return err;
}

dword_t sys_kill(pid_t_ pid, dword_t sig) {
    return do_kill(pid, sig, 0, true);
}
dword_t sys_tgkill(pid_t_ tgid, pid_t_ tid, dword_t sig) {
    if (tid <= 0 || tgid <= 0)
        return _EINVAL;
    return do_kill(tid, sig, tgid, false);
}
dword_t sys_tkill(pid_t_ tid, dword_t sig) {
    if (tid <= 0)
        return _EINVAL;
    return do_kill(tid, sig, 0, false);
}
