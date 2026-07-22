#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "fs/fd.h"
#include "fs/tty.h"
#include "kernel/aarch64-syscall-service.h"
#include "kernel/calls.h"
#include "kernel/fs.h"
#include "kernel/mm.h"
#include "kernel/signal.h"
#include "kernel/task.h"

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "会话终端生命周期测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        return false; \
    } \
} while (0)

struct exit_thread {
    struct task *task;
    int status;
};

struct cleanup_probe {
    atomic_uint calls;
    atomic_bool held_pids_lock;
    atomic_bool missed_ttys_lock;
    atomic_bool current_was_null;
    atomic_int session;
};

static struct cleanup_probe cleanup_probe;

struct signal_echo_gate {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    bool entered;
    bool allow_return;
    unsigned calls;
};

static struct signal_echo_gate *active_signal_echo_gate;

static int signal_echo_write(
        struct tty *UNUSED(tty), const void *UNUSED(buffer),
        size_t size, bool UNUSED(blocking)) {
    struct signal_echo_gate *gate = active_signal_echo_gate;
    pthread_mutex_lock(&gate->mutex);
    gate->calls++;
    bool block = gate->calls == 1;
    if (block) {
        gate->entered = true;
        pthread_cond_broadcast(&gate->changed);
    }
    while (block && !gate->allow_return)
        pthread_cond_wait(&gate->changed, &gate->mutex);
    pthread_mutex_unlock(&gate->mutex);
    return (int) size;
}

static const struct tty_driver_ops signal_echo_ops = {
    .write = signal_echo_write,
};

static struct tty_driver signal_echo_driver = {
    .ops = &signal_echo_ops,
};

struct signal_input_call {
    struct task *task;
    struct tty *tty;
    char input;
    ssize_t result;
};

static void *run_signal_input(void *opaque) {
    struct signal_input_call *call = opaque;
    current = call->task;
    call->result = tty_input(
            call->tty, &call->input, 1, false);
    current = NULL;
    return NULL;
}

static void destroy_live_group(
        struct task *parent, struct task *leader,
        struct task *thread);

static int lifecycle_tty_write(
        struct tty *UNUSED(tty), const void *UNUSED(buffer),
        size_t size, bool UNUSED(blocking)) {
    return (int) size;
}

static void lifecycle_tty_cleanup(struct tty *tty) {
    atomic_fetch_add_explicit(
            &cleanup_probe.calls, 1, memory_order_relaxed);
    if (lock_owned_by_current(&pids_lock))
        atomic_store_explicit(
                &cleanup_probe.held_pids_lock, true,
                memory_order_relaxed);
    if (!lock_owned_by_current(&ttys_lock))
        atomic_store_explicit(
                &cleanup_probe.missed_ttys_lock, true,
                memory_order_relaxed);
    atomic_store_explicit(
            &cleanup_probe.current_was_null, current == NULL,
            memory_order_relaxed);
    atomic_store_explicit(
            &cleanup_probe.session, tty->session,
            memory_order_relaxed);
}

static const struct tty_driver_ops lifecycle_tty_ops = {
    .write = lifecycle_tty_write,
    .cleanup = lifecycle_tty_cleanup,
};

static struct tty_driver lifecycle_tty_driver = {
    .ops = &lifecycle_tty_ops,
};

static void reset_cleanup_probe(void) {
    atomic_store_explicit(
            &cleanup_probe.calls, 0, memory_order_relaxed);
    atomic_store_explicit(
            &cleanup_probe.held_pids_lock, false,
            memory_order_relaxed);
    atomic_store_explicit(
            &cleanup_probe.missed_ttys_lock, false,
            memory_order_relaxed);
    atomic_store_explicit(
            &cleanup_probe.current_was_null, false,
            memory_order_relaxed);
    atomic_store_explicit(
            &cleanup_probe.session, -1, memory_order_relaxed);
}

static bool cleanup_ran_once_outside_pids(void) {
    return atomic_load_explicit(
                    &cleanup_probe.calls, memory_order_relaxed) == 1 &&
            !atomic_load_explicit(
                    &cleanup_probe.held_pids_lock,
                    memory_order_relaxed) &&
            !atomic_load_explicit(
                    &cleanup_probe.missed_ttys_lock,
                    memory_order_relaxed) &&
            atomic_load_explicit(
                    &cleanup_probe.session, memory_order_relaxed) == 0;
}

static void init_group(
        struct tgroup *group, struct task *leader,
        pid_t_ sid, pid_t_ pgid) {
    *group = (struct tgroup) {0};
    list_init(&group->threads);
    list_init(&group->session);
    list_init(&group->pgroup);
    lock_init(&group->lock);
    cond_init(&group->child_exit);
    cond_init(&group->stopped_cond);
    group->leader = leader;
    group->sid = sid;
    group->pgid = pgid;
    group->limits[RLIMIT_NOFILE_] =
            (struct rlimit_) {16, 16};
}

static struct task *make_parent(struct tgroup *group) {
    struct task *parent = task_create_(NULL);
    if (parent == NULL)
        return NULL;
    init_group(group, parent, parent->pid, parent->pid);
    parent->group = group;
    parent->tgid = parent->pid;

    struct mm *mm = mm_new();
    struct fdtable *files = fdtable_new(1);
    struct fs_info *fs = fs_info_new();
    struct sighand *sighand = sighand_new();
    if (mm == NULL || IS_ERR(files) ||
            fs == NULL || sighand == NULL)
        return NULL;
    task_set_mm(parent, mm);
    parent->files = files;
    parent->fs = fs;
    parent->sighand = sighand;
    task_thread_store(parent, pthread_self());
    task_publish(parent);
    current = parent;
    return parent;
}

static struct task *make_process(
        struct task *parent, pid_t_ sid, pid_t_ pgid) {
    struct task *task = task_create_(parent);
    if (task == NULL)
        return NULL;
    struct tgroup *group = malloc(sizeof(*group));
    if (group == NULL) {
        task_abort_create(task);
        return NULL;
    }
    init_group(group, task, sid, pgid);
    task->group = group;
    task->tgid = task->pid;
    task->exit_signal = SIGCHLD_;

    mm_retain(task->mm);
    task->files->refcount++;
    task->fs->refcount++;
    task->sighand->refcount++;
    task_publish(task);
    return task;
}

static struct task *make_group_thread(struct task *leader) {
    struct task *thread = task_create_(leader);
    if (thread == NULL)
        return NULL;
    thread->parent = leader->parent;
    thread->group = leader->group;
    thread->tgid = leader->tgid;
    thread->exit_signal = 0;
    task_publish(thread);
    return thread;
}

static struct task *make_exiting_group_thread(struct task *leader) {
    struct task *thread = task_create_(leader);
    if (thread == NULL)
        return NULL;
    thread->parent = leader->parent;
    thread->group = leader->group;
    thread->tgid = leader->tgid;
    thread->exit_signal = 0;
    mm_retain(thread->mm);
    thread->files->refcount++;
    thread->fs->refcount++;
    thread->sighand->refcount++;
    task_publish(thread);
    return thread;
}

static struct tty *attach_new_tty(struct tgroup *group) {
    struct tty *tty = pty_open_fake(&lifecycle_tty_driver);
    if (IS_ERR(tty))
        return NULL;
    lock(&group->lock);
    group->tty = tty;
    unlock(&group->lock);
    lock(&tty->lock);
    tty->session = group->sid;
    tty->fg_group = group->pgid;
    unlock(&tty->lock);
    return tty;
}

static void share_tty(struct tgroup *group, struct tty *tty) {
    lock(&tty->lock);
    tty->refcount++;
    unlock(&tty->lock);
    lock(&group->lock);
    group->tty = tty;
    unlock(&group->lock);
}

static void *exit_task(void *opaque) {
    struct exit_thread *exit = opaque;
    current = exit->task;
    task_thread_store(current, pthread_self());
    do_exit(exit->status);
}

static bool run_task_exit(struct task *task, int status) {
    struct exit_thread exit = {
        .task = task,
        .status = status,
    };
    pthread_t thread;
    if (pthread_create(&thread, NULL, exit_task, &exit) != 0)
        return false;
    return pthread_join(thread, NULL) == 0;
}

static bool wait_for_child(struct task *parent, pid_t_ pid) {
    current = parent;
    struct wait4_result result;
    return do_wait4(pid, 0, &result) == pid;
}

static bool enter_own_session(
        struct task *task, struct task *parent) {
    current = task;
    pid_t_ result = task_setsid(task);
    current = parent;
    return result == task->pid;
}

static qword_t invoke_aarch64_session_syscall(
        struct task *task, qword_t number,
        qword_t argument0, qword_t argument1) {
    const struct guest_linux_syscall_context context = {
        .task_opaque = task,
    };
    const struct guest_linux_syscall syscall = {
        .number = number,
        .arguments = {argument0, argument1},
    };
    struct guest_linux_user_fault fault;
    current = task;
    return ish_aarch64_linux_syscall_service.dispatch(
            &context, &syscall, &fault);
}

static bool test_aarch64_session_syscalls(
        struct task *parent) {
    enum {
        aarch64_sys_setpgid = 154,
        aarch64_sys_getpgid = 155,
        aarch64_sys_getsid = 156,
        aarch64_sys_setsid = 157,
    };
    const qword_t noisy_zero = UINT64_C(0xabcdef0100000000);
    const qword_t noisy_negative_one = UINT64_C(0x12345678ffffffff);
    const qword_t esrch = (qword_t) (sqword_t) _ESRCH;
    const qword_t eperm = (qword_t) (sqword_t) _EPERM;

    struct task *child = make_process(parent,
            parent->group->sid, parent->group->pgid);
    CHECK(child != NULL, "创建 AArch64 会话系统调用子进程");
    pid_t_ child_pid = child->pid;
    pid_t_ parent_pid = parent->pid;

    CHECK(invoke_aarch64_session_syscall(child,
                    aarch64_sys_getpgid, noisy_zero, 0) ==
                    (qword_t) parent_pid,
            "getpgid(0) 返回当前进程组并忽略参数高位");
    CHECK(invoke_aarch64_session_syscall(child,
                    aarch64_sys_getsid, noisy_zero, 0) ==
                    (qword_t) parent_pid,
            "getsid(0) 返回当前会话并忽略参数高位");
    CHECK(invoke_aarch64_session_syscall(child,
                    aarch64_sys_getpgid,
                    noisy_negative_one, 0) == esrch &&
            invoke_aarch64_session_syscall(child,
                    aarch64_sys_getsid,
                    noisy_negative_one, 0) == esrch,
            "进程查询按有符号低 32 位返回完整 64 位 ESRCH");

    CHECK(invoke_aarch64_session_syscall(child,
                    aarch64_sys_setpgid,
                    noisy_zero, noisy_zero) == 0 &&
            child->group->pgid == child_pid,
            "setpgid(0, 0) 建立当前进程组并忽略参数高位");
    CHECK(invoke_aarch64_session_syscall(child,
                    aarch64_sys_setsid, 0, 0) == eperm,
            "进程组 leader 的 setsid 错误被完整符号扩展");
    CHECK(invoke_aarch64_session_syscall(child,
                    aarch64_sys_setpgid, noisy_zero,
                    UINT64_C(0x1234567800000000) |
                            (dword_t) parent_pid) == 0,
            "setpgid 可把当前进程移回同会话的既有进程组");
    qword_t setsid_result = invoke_aarch64_session_syscall(child,
            aarch64_sys_setsid, 0, 0);
    CHECK(setsid_result == (qword_t) child_pid,
            "setsid 返回新建会话 ID");
    CHECK(child->group->sid == child_pid &&
            child->group->pgid == child_pid,
            "setsid 把会话与进程组锚定到线程组 leader");

    CHECK(invoke_aarch64_session_syscall(parent,
                    aarch64_sys_getsid,
                    UINT64_C(0x8765432100000000) |
                            (dword_t) child_pid, 0) ==
                    (qword_t) child_pid &&
            invoke_aarch64_session_syscall(parent,
                    aarch64_sys_getpgid,
                    UINT64_C(0xfeedface00000000) |
                            (dword_t) child_pid, 0) ==
                    (qword_t) child_pid,
            "非零 PID 查询返回目标进程的会话与进程组");

    CHECK(run_task_exit(child, 0) &&
            wait_for_child(parent, child_pid),
            "正规退出并回收 AArch64 会话系统调用子进程");
    return true;
}

// 测试夹具直接建立内核已验证过的同会话进程组关系，避免依赖其他 syscall 边界。
static bool move_group_to_pgid(
        struct tgroup *group, pid_t_ pgid) {
    lock(&pids_lock);
    struct pid *pid = pid_get(pgid);
    if (pid == NULL) {
        unlock(&pids_lock);
        return false;
    }
    list_remove(&group->pgroup);
    list_add(&pid->pgroup, &group->pgroup);
    group->pgid = pgid;
    unlock(&pids_lock);
    return true;
}

static bool test_signal_target_is_input_snapshot(
        struct task *parent) {
    struct task *first = make_process(parent,
            parent->group->sid, parent->group->pgid);
    struct task *second = make_process(parent,
            parent->group->sid, parent->group->pgid);
    CHECK(first != NULL && second != NULL,
            "创建前台组快照测试的两个进程");
    CHECK(move_group_to_pgid(first->group, first->pid) &&
            move_group_to_pgid(second->group, second->pid),
            "建立前台组快照测试的独立进程组");
    first->blocked |= sig_mask(SIGINT_);
    second->blocked |= sig_mask(SIGINT_);

    struct signal_echo_gate gate = {};
    CHECK(pthread_mutex_init(&gate.mutex, NULL) == 0 &&
            pthread_cond_init(&gate.changed, NULL) == 0,
            "初始化前台组快照回显闸门");
    active_signal_echo_gate = &gate;

    current = parent;
    struct tty *tty = pty_open_fake(&signal_echo_driver);
    CHECK(tty != NULL && !IS_ERR(tty),
            "创建前台组快照测试 tty");
    int tty_num = tty->num;
    lock(&tty->lock);
    tty->fg_group = first->pid;
    char interrupt = tty->termios.cc[VINTR_];
    unlock(&tty->lock);

    struct signal_input_call call = {
        .task = parent,
        .tty = tty,
        .input = interrupt,
        .result = -1,
    };
    pthread_t input_thread;
    CHECK(pthread_create(
            &input_thread, NULL, run_signal_input, &call) == 0,
            "建立前台组快照输入线程");

    pthread_mutex_lock(&gate.mutex);
    while (!gate.entered)
        pthread_cond_wait(&gate.changed, &gate.mutex);
    pthread_mutex_unlock(&gate.mutex);

    // VINTR 已按 first 入队但尚未投递；回显窗口切换前台组不得改变本批信号目标。
    lock(&tty->lock);
    tty->fg_group = second->pid;
    unlock(&tty->lock);

    pthread_mutex_lock(&gate.mutex);
    gate.allow_return = true;
    pthread_cond_broadcast(&gate.changed);
    pthread_mutex_unlock(&gate.mutex);
    CHECK(pthread_join(input_thread, NULL) == 0 &&
            call.result == 1,
            "前台组切换后输入线程有界完成");

    lock(&pids_lock);
    lock(&first->sighand->lock);
    bool first_received = sigset_has(
            first->group->shared_pending, SIGINT_);
    bool second_received = sigset_has(
            second->group->shared_pending, SIGINT_);
    unlock(&first->sighand->lock);
    unlock(&pids_lock);
    CHECK(first_received && !second_received,
            "控制字符信号只投递给输入开始时的前台进程组");

    current = parent;
    lock(&ttys_lock);
    tty_release(tty);
    unlock(&ttys_lock);
    CHECK(signal_echo_driver.ttys[tty_num] == NULL &&
            !signal_echo_driver.reserved[tty_num],
            "前台组快照测试完整回收 tty 槽位");

    active_signal_echo_gate = NULL;
    pthread_cond_destroy(&gate.changed);
    pthread_mutex_destroy(&gate.mutex);
    destroy_live_group(parent, first, NULL);
    destroy_live_group(parent, second, NULL);
    current = parent;
    return true;
}

static bool test_wait_reap_cleanup(struct task *parent) {
    struct task *child = make_process(parent,
            parent->group->sid, parent->group->pgid);
    CHECK(child != NULL, "创建 wait 回收子任务");
    CHECK(enter_own_session(child, parent),
            "为 wait 回收子任务建立独立会话");

    reset_cleanup_probe();
    CHECK(attach_new_tty(child->group) != NULL,
            "为 wait 回收子任务绑定 controlling tty");
    pid_t_ pid = child->pid;
    CHECK(run_task_exit(child, 17 << 8),
            "等待带 controlling tty 的子任务退出");
    CHECK(atomic_load_explicit(
                    &cleanup_probe.calls,
                    memory_order_relaxed) == 0,
            "僵尸在 wait 前保留 controlling tty 引用");
    CHECK(wait_for_child(parent, pid),
            "wait 完整回收带 controlling tty 的子任务");
    CHECK(cleanup_ran_once_outside_pids(),
            "wait 在 pids_lock 外恰好清理一次 tty");
    return true;
}

static bool old_session_has_expected_members(
        pid_t_ sid, pid_t_ holder_pgid,
        struct tgroup *last_group) {
    lock(&pids_lock);
    struct pid *session = pid_get(sid);
    struct pid *holder = pid_get(holder_pgid);
    bool valid = session != NULL && session->task == NULL &&
            list_size(&session->session) == 1 &&
            list_first_entry(&session->session,
                    struct tgroup, session) == last_group &&
            list_empty(&session->pgroup) &&
            holder != NULL && holder->task == NULL &&
            list_size(&holder->pgroup) == 1 &&
            list_first_entry(&holder->pgroup,
                    struct tgroup, pgroup) == last_group;
    unlock(&pids_lock);
    return valid;
}

static bool new_session_is_anchored_to_leader(
        pid_t_ leader_pid, pid_t_ thread_pid,
        pid_t_ old_sid, pid_t_ old_pgid,
        struct tgroup *group) {
    lock(&pids_lock);
    struct pid *leader = pid_get(leader_pid);
    struct pid *thread = pid_get(thread_pid);
    bool valid = group->sid == leader_pid &&
            group->pgid == leader_pid &&
            leader != NULL &&
            list_size(&leader->session) == 1 &&
            list_first_entry(&leader->session,
                    struct tgroup, session) == group &&
            list_size(&leader->pgroup) == 1 &&
            list_first_entry(&leader->pgroup,
                    struct tgroup, pgroup) == group &&
            thread != NULL &&
            list_empty(&thread->session) &&
            list_empty(&thread->pgroup) &&
            pid_get(old_sid) == NULL &&
            pid_get(old_pgid) == NULL;
    unlock(&pids_lock);
    return valid;
}

static void destroy_live_group(
        struct task *parent, struct task *leader,
        struct task *thread) {
    current = parent;
    signal_flush_pending(leader);
    if (thread != NULL)
        signal_flush_pending(thread);
    signal_flush_group_pending(leader->group);
    mm_release(leader->mm);
    fdtable_release(leader->files);
    fs_info_release(leader->fs);
    sighand_release(leader->sighand);

    struct tgroup *group = leader->group;
    lock(&pids_lock);
    lock(&group->lock);
    if (thread != NULL) {
        list_remove(&thread->group_links);
        task_destroy(thread);
    }
    list_remove(&leader->group_links);
    list_remove(&group->session);
    list_remove(&group->pgroup);
    task_destroy(leader);
    unlock(&group->lock);
    unlock(&pids_lock);
    cond_destroy(&group->stopped_cond);
    cond_destroy(&group->child_exit);
    lock_destroy(&group->lock);
    free(group);
}

static bool test_reaped_leader_and_thread_setsid(
        struct task *parent) {
    struct task *session_leader = make_process(parent,
            parent->group->sid, parent->group->pgid);
    CHECK(session_leader != NULL, "创建旧会话 leader");
    CHECK(enter_own_session(session_leader, parent),
            "建立待回收的旧会话");

    reset_cleanup_probe();
    struct tty *tty = attach_new_tty(session_leader->group);
    CHECK(tty != NULL, "为旧会话绑定 controlling tty");

    struct task *pgroup_holder = make_process(
            session_leader, session_leader->pid,
            session_leader->pid);
    struct task *last_member = make_process(
            session_leader, session_leader->pid,
            session_leader->pid);
    CHECK(pgroup_holder != NULL && last_member != NULL,
            "创建旧会话剩余成员");
    share_tty(pgroup_holder->group, tty);
    share_tty(last_member->group, tty);

    current = pgroup_holder;
    CHECK(sys_setpgid(0, 0) == 0,
            "为旧会话建立独立进程组");
    current = parent;
    CHECK(move_group_to_pgid(
                    last_member->group, pgroup_holder->pid),
            "把最后成员迁入独立进程组");

    pid_t_ old_sid = session_leader->pid;
    pid_t_ old_pgid = pgroup_holder->pid;
    pid_t_ leader_pid = last_member->pid;
    CHECK(run_task_exit(session_leader, 23 << 8) &&
            wait_for_child(parent, old_sid),
            "退出并回收旧 session leader");
    CHECK(run_task_exit(pgroup_holder, 29 << 8) &&
            wait_for_child(parent, old_pgid),
            "退出并回收旧进程组 leader");
    CHECK(atomic_load_explicit(
                    &cleanup_probe.calls,
                    memory_order_relaxed) == 0,
            "最后成员继续持有 controlling tty");
    CHECK(old_session_has_expected_members(
                    old_sid, old_pgid,
                    last_member->group),
            "旧 SID 仅由 pgid 不同的最后成员保留");

    struct task *thread = make_group_thread(last_member);
    CHECK(thread != NULL, "创建非 leader setsid 调用线程");
    pid_t_ thread_pid = thread->pid;
    current = thread;
    pid_t_ result = task_setsid(thread);
    current = parent;
    CHECK(result == leader_pid,
            "非 leader 线程成功为线程组建立新会话");
    CHECK(cleanup_ran_once_outside_pids(),
            "setsid 在 pids_lock 外恰好清理一次 tty");
    CHECK(new_session_is_anchored_to_leader(
                    leader_pid, thread_pid,
                    old_sid, old_pgid,
                    last_member->group),
            "非 leader setsid 把 session 与 pgroup 锚定到 TGID");

    destroy_live_group(parent, last_member, thread);
    return true;
}

static bool proposed_pgid_is_occupied(
        pid_t_ proposed_sid, struct tgroup *holder_group) {
    lock(&pids_lock);
    struct pid *pid = pid_get(proposed_sid);
    bool occupied = pid != NULL &&
            list_size(&pid->pgroup) == 1 &&
            list_first_entry(&pid->pgroup,
                    struct tgroup, pgroup) == holder_group;
    unlock(&pids_lock);
    return occupied;
}

static bool test_setsid_rejects_occupied_pgid(
        struct task *parent) {
    struct task *candidate = make_process(parent,
            parent->group->sid, parent->group->pgid);
    struct task *holder = make_process(parent,
            parent->group->sid, parent->group->pgid);
    CHECK(candidate != NULL && holder != NULL,
            "创建拟用 SID 与旧 PGID 测试任务");

    current = candidate;
    CHECK(sys_setpgid(0, 0) == 0,
            "先建立与拟用 SID 同号的进程组");
    current = parent;
    CHECK(move_group_to_pgid(
                    holder->group, candidate->pid),
            "让另一进程保留拟用 SID 的旧进程组");
    CHECK(move_group_to_pgid(
                    candidate->group, parent->group->pgid),
            "候选进程离开同号旧进程组");
    CHECK(proposed_pgid_is_occupied(
                    candidate->pid, holder->group),
            "拟用 SID 的旧进程组仅由另一进程保留");

    pid_t_ candidate_pid = candidate->pid;
    pid_t_ holder_pid = holder->pid;
    current = candidate;
    pid_t_ result = task_setsid(candidate);
    current = parent;
    CHECK(result == _EPERM,
            "拟用 SID 已被旧 PGID 占用时 setsid 返回 EPERM");
    CHECK(candidate->group->sid == parent->group->sid &&
            candidate->group->pgid == parent->group->pgid &&
            proposed_pgid_is_occupied(
                    candidate_pid, holder->group),
            "setsid 拒绝路径不改写 session 与 pgroup 链");

    CHECK(run_task_exit(candidate, 31 << 8) &&
            wait_for_child(parent, candidate_pid),
            "退出并回收 setsid 候选进程");
    CHECK(run_task_exit(holder, 37 << 8) &&
            wait_for_child(parent, holder_pid),
            "退出并回收旧 PGID 保留进程");
    return true;
}

static bool test_auto_reap_cleanup(struct task *parent) {
    const struct signal_action no_wait = {
        .handler = UINT64_C(0x1234),
        .flags = SA_NOCLDWAIT_,
    };
    CHECK(task_sigaction(parent, SIGCHLD_,
                    &no_wait, NULL) == 0,
            "安装 SA_NOCLDWAIT 动作");

    struct task *child = make_process(parent,
            parent->group->sid, parent->group->pgid);
    CHECK(child != NULL, "创建自动回收子任务");
    CHECK(enter_own_session(child, parent),
            "为自动回收子任务建立独立会话");
    reset_cleanup_probe();
    CHECK(attach_new_tty(child->group) != NULL,
            "为自动回收子任务绑定 controlling tty");
    pid_t_ pid = child->pid;
    CHECK(run_task_exit(child, 41 << 8),
            "等待 SA_NOCLDWAIT 子任务退出");
    CHECK(cleanup_ran_once_outside_pids(),
            "自动回收在 pids_lock 外恰好清理一次 tty");

    lock(&pids_lock);
    bool reaped = pid_get(pid) == NULL;
    unlock(&pids_lock);
    CHECK(reaped, "SA_NOCLDWAIT 同步完成任务与 PID 回收");

    const struct signal_action default_action = {0};
    CHECK(task_sigaction(parent, SIGCHLD_,
                    &default_action, NULL) == 0,
            "恢复默认 SIGCHLD 动作");
    return true;
}

static bool test_nonleader_auto_reap_cleanup(
        struct task *parent) {
    const struct signal_action no_wait = {
        .handler = UINT64_C(0x5678),
        .flags = SA_NOCLDWAIT_,
    };
    CHECK(task_sigaction(parent, SIGCHLD_,
                    &no_wait, NULL) == 0,
            "为多线程组安装 SA_NOCLDWAIT 动作");

    struct task *leader = make_process(parent,
            parent->group->sid, parent->group->pgid);
    CHECK(leader != NULL, "创建自动回收多线程组 leader");
    CHECK(enter_own_session(leader, parent),
            "为自动回收多线程组建立独立会话");
    struct task *last_thread = make_exiting_group_thread(leader);
    CHECK(last_thread != NULL, "创建最后退出的非 leader 线程");

    reset_cleanup_probe();
    CHECK(attach_new_tty(leader->group) != NULL,
            "为自动回收多线程组绑定 controlling tty");
    pid_t_ leader_pid = leader->pid;
    pid_t_ thread_pid = last_thread->pid;
    CHECK(run_task_exit(leader, 43 << 8),
            "让多线程组 leader 先退出");

    lock(&pids_lock);
    bool retained = pid_get_task_zombie(leader_pid) == leader &&
            pid_get_task(thread_pid) == last_thread &&
            list_size(&leader->group->threads) == 1 &&
            list_first_entry(&leader->group->threads,
                    struct task, group_links) == last_thread;
    unlock(&pids_lock);
    CHECK(retained && atomic_load_explicit(
                    &cleanup_probe.calls,
                    memory_order_relaxed) == 0,
            "leader 退出后由非 leader 保留线程组与 tty");

    CHECK(run_task_exit(last_thread, 47 << 8),
            "最后一个非 leader 线程触发自动回收");
    CHECK(cleanup_ran_once_outside_pids() &&
            atomic_load_explicit(
                    &cleanup_probe.current_was_null,
                    memory_order_relaxed),
            "nonleader auto-reap 清理 tty 时 current 已清空");

    lock(&pids_lock);
    bool reaped = pid_get(leader_pid) == NULL &&
            pid_get(thread_pid) == NULL;
    unlock(&pids_lock);
    CHECK(reaped, "nonleader auto-reap 完整释放线程组 PID");

    const struct signal_action default_action = {0};
    CHECK(task_sigaction(parent, SIGCHLD_,
                    &default_action, NULL) == 0,
            "恢复多线程自动回收后的 SIGCHLD 动作");
    return true;
}

static void destroy_parent(
        struct task *parent, struct tgroup *group) {
    signal_flush_pending(parent);
    signal_flush_group_pending(group);
    mm_release(parent->mm);
    fdtable_release(parent->files);
    fs_info_release(parent->fs);
    sighand_release(parent->sighand);
    current = NULL;

    cond_destroy(&parent->pause);
    cond_destroy(&parent->ptrace.cond);
    lock(&pids_lock);
    lock(&group->lock);
    list_remove(&parent->group_links);
    list_remove(&group->session);
    list_remove(&group->pgroup);
    task_destroy(parent);
    unlock(&group->lock);
    unlock(&pids_lock);
    cond_destroy(&group->stopped_cond);
    cond_destroy(&group->child_exit);
    lock_destroy(&group->lock);
}

static int run_lifecycle_scenarios(void) {
    struct tgroup parent_group;
    struct task *parent = make_parent(&parent_group);
    if (parent == NULL) {
        fprintf(stderr, "会话终端生命周期测试失败：创建父任务\n");
        return 1;
    }
    parent->blocked = sig_mask(SIGCHLD_);

    if (!test_aarch64_session_syscalls(parent) ||
            !test_wait_reap_cleanup(parent) ||
            !test_reaped_leader_and_thread_setsid(parent) ||
            !test_setsid_rejects_occupied_pgid(parent) ||
            !test_auto_reap_cleanup(parent) ||
            !test_nonleader_auto_reap_cleanup(parent) ||
            !test_signal_target_is_input_snapshot(parent))
        return 1;

    destroy_parent(parent, &parent_group);
    return 0;
}

int main(void) {
    pid_t child = fork();
    if (child < 0) {
        fprintf(stderr, "会话终端生命周期测试无法 fork：%s\n",
                strerror(errno));
        return 1;
    }
    if (child == 0) {
        signal(SIGUSR1, SIG_IGN);
        alarm(20);
        _exit(run_lifecycle_scenarios());
    }

    int status;
    pid_t waited;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited == child && WIFEXITED(status) &&
            WEXITSTATUS(status) == 0)
        return 0;
    if (waited == child && WIFSIGNALED(status)) {
        fprintf(stderr, "会话终端生命周期测试被 host 信号 %d 终止\n",
                WTERMSIG(status));
    } else {
        fprintf(stderr, "会话终端生命周期测试返回状态 %d\n",
                waited == child && WIFEXITED(status) ?
                        WEXITSTATUS(status) : -1);
    }
    return 1;
}
