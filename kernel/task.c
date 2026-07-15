#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "guest/aarch64/linux-process.h"
#include "kernel/aarch64-signal-service.h"
#include "kernel/aarch64-syscall-service.h"
#include "kernel/aarch64-task-runner.h"
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/futex.h"
#include "kernel/task.h"
#include "kernel/memory.h"
#include "emu/tlb.h"

__thread struct task *current;

static struct pid pids[MAX_PID + 1] = {};
lock_t pids_lock = LOCK_INITIALIZER;
static atomic_bool task_exec_fail_sighand_reservation =
        ATOMIC_VAR_INIT(false);

void task_exec_test_fail_sighand_reservation_once(void) {
    atomic_store_explicit(&task_exec_fail_sighand_reservation,
            true, memory_order_release);
}

bool task_has_aarch64_process(const struct task *task) {
    return __atomic_load_n(
            &task->aarch64_process, __ATOMIC_ACQUIRE) != NULL;
}

static bool task_accepts_aarch64_process(struct task *task,
        struct aarch64_linux_process *process, pid_t_ tid) {
    return task != NULL && process != NULL &&
            aarch64_linux_process_uses_services(process,
                    tid, task,
                    &ish_aarch64_linux_syscall_service,
                    &ish_aarch64_linux_signal_service);
}

static void task_exec_name_from_path(
        char name[16], const char *executable);

bool task_attach_aarch64_process(struct task *task,
        struct aarch64_linux_process *process) {
    if (task == NULL ||
            !task_accepts_aarch64_process(task, process, task->pid) ||
            task->aarch64_process != NULL ||
            task->exec_transition.process != NULL)
        return false;
    task->aarch64_process = process;
    return true;
}

bool task_stage_aarch64_exec(struct task *task,
        struct aarch64_linux_process *process, struct mm *mm,
        uid_t_ euid, uid_t_ egid, const char *executable) {
    if (task == NULL || executable == NULL ||
            !task_accepts_aarch64_process(task, process, task->tgid) ||
            mm == NULL ||
            mm == task->mm ||
            task->exec_transition.process != NULL ||
            task->exec_transition.mm != NULL ||
            task->exec_transition.sighand != NULL ||
            task->exec_transition.begun ||
            task->exec_transition.ready ||
            task->aarch64_process == process)
        return false;
    task->exec_transition.process = process;
    task->exec_transition.mm = mm;
    task->exec_transition.euid = euid;
    task->exec_transition.egid = egid;
    task_exec_name_from_path(task->exec_transition.comm, executable);
    return true;
}

bool task_has_aarch64_exec_candidate(const struct task *task) {
    return task->exec_transition.process != NULL;
}

static void task_exec_name_from_path(
        char name[16], const char *executable) {
    const char *basename = strrchr(executable, '/');
    basename = basename == NULL ? executable : basename + 1;
    memset(name, 0, 16);
    snprintf(name, 16, "%s", basename);
}

static bool task_exec_is_killed(struct task *task) {
    return task_group_fatal_signal(task) != 0 ||
            task_sigkill_pending(task);
}

static int task_exec_reserve_sighand(struct task *task) {
    struct sighand *old_sighand = task->sighand;
    if (atomic_load_explicit(
                &old_sighand->refcount, memory_order_acquire) == 1)
        return 0;

    bool fail_reservation = atomic_exchange_explicit(
            &task_exec_fail_sighand_reservation,
            false, memory_order_acq_rel);
    struct sighand *private_sighand =
            fail_reservation ? NULL : sighand_new();
    lock(&pids_lock);
    old_sighand = task->sighand;
    bool shared = atomic_load_explicit(
            &old_sighand->refcount, memory_order_acquire) > 1;
    if (private_sighand != NULL && shared) {
        assert(task->exec_transition.sighand == NULL);
        task->exec_transition.sighand = private_sighand;
        private_sighand = NULL;
    }
    unlock(&pids_lock);

    if (private_sighand != NULL)
        sighand_release(private_sighand);
    return shared && task->exec_transition.sighand == NULL ?
            _ENOMEM : 0;
}

int task_begin_aarch64_exec(struct task *task) {
    assert(task != NULL && task == current &&
            task_has_aarch64_exec_candidate(task) &&
            task->exec_transition.mm != NULL &&
            task->exec_transition.sighand == NULL &&
            !task->exec_transition.begun);

    // PONR 必须早于 de-thread；从这里起任何失败都只能终止映像。
    task->exec_transition.begun = true;

    int error = task_exec_dethread(task);
    if (error < 0)
        return error;
    error = task_exec_unshare_files(task);
    if (error < 0)
        return error;
    error = task_exec_reserve_sighand(task);
    if (error < 0)
        return error;

    lock(&pids_lock);
    bool killed = task_exec_is_killed(task);
    unlock(&pids_lock);
    if (killed)
        return _EINTR;
    task->exec_transition.ready = true;
    return 0;
}

static void task_set_exec_name(struct task *task, const char comm[16]) {
    lock(&task->general_lock);
    memcpy(task->comm, comm, sizeof(task->comm));
    unlock(&task->general_lock);
    task->did_exec = true;
}

static void task_commit_exec_credentials(
        struct task *task, uid_t_ euid, uid_t_ egid) {
    // 信号权限检查在 pids_lock 下读取四个字段，必须整体发布。
    lock(&pids_lock);
    task->euid = euid;
    task->egid = egid;
    task->suid = euid;
    task->sgid = egid;
    unlock(&pids_lock);
}

static void task_notify_exec(struct task *task) {
    update_thread_name();
    lock(&task->ptrace.lock);
    bool traced = task->ptrace.traced;
    unlock(&task->ptrace.lock);
    if (traced) {
        lock(&pids_lock);
        // 经典 ptrace exec trap 即使 disposition 为 SIG_IGN 也必须排队。
        deliver_signal_locked(task, SIGTRAP_, (struct siginfo_) {
            .code = SI_USER_,
            .payload_kind = SIGNAL_INFO_PAYLOAD_KILL,
            .kill.pid = task->pid,
            .kill.uid = task->uid,
        });
        unlock(&pids_lock);
    }
}

static void task_finish_exec_state(struct task *task,
        uid_t_ euid, uid_t_ egid, const char comm[16]) {
    // Linux 先关闭敏感 fd，再清替代栈、更新 comm、重置 handler，最后提交凭据。
    fdtable_do_cloexec(task->files);
    task_altstack_reset(task);
    task_set_exec_name(task, comm);
    task_signal_exec_reset_actions(task);
    task_commit_exec_credentials(task, euid, egid);
}

void task_finish_exec(struct task *task, uid_t_ euid, uid_t_ egid,
        const char *executable, struct mm *retired_mm) {
    assert(task != NULL && task == current && retired_mm != NULL &&
            retired_mm != task->mm);
    char comm[16];
    task_exec_name_from_path(comm, executable);

    task_finish_exec_state(task, euid, egid, comm);
    mm_release(retired_mm);
    task_notify_exec(task);
}

void task_commit_aarch64_exec(struct task *task) {
    assert(task != NULL && task == current &&
            task_has_aarch64_exec_candidate(task) &&
            task->exec_transition.mm != NULL &&
            task->exec_transition.begun &&
            task->exec_transition.ready &&
            task_accepts_aarch64_process(task,
                    task->exec_transition.process, task->pid));
    struct task_exec_transition *transition = &task->exec_transition;
    struct aarch64_linux_process *retired = task->aarch64_process;
    uid_t_ euid = transition->euid;
    uid_t_ egid = transition->egid;
    char comm[16];
    memcpy(comm, transition->comm, sizeof(comm));

    // 只在成功提交时清理旧映像；候选失败仍保留原架构注册。
    if (retired != NULL)
        futex_cleanup_task_aarch64(task, retired);
    else
        futex_cleanup_task_i386(task);

    // Linux 在替换 mm 前完成 clear_child_tid/robust 清理并唤醒 vfork 父任务。
    vfork_notify(task);

    // procfs 以 pids_lock 固定 task，再由 general_lock 取得配对的 process/mm。
    lock(&pids_lock);
    lock(&task->general_lock);
    struct mm *retired_mm = task->mm;
    task_set_mm(task, transition->mm);
    transition->mm = NULL;
    // mm 先写，process 的 release-store 是无锁观察者的发布标志。
    __atomic_store_n(&task->aarch64_process,
            transition->process, __ATOMIC_RELEASE);
    transition->process = NULL;
    unlock(&task->general_lock);
    unlock(&pids_lock);

    // timer pending 仍由旧 sighand 保护，必须在最终动作快照前清理。
    if (task->group != NULL)
        tgroup_exec_posix_timers_destroy(task->group);
    signal_flush_exec_timer_pending(task);

    struct sighand *retired_sighand = NULL;
    // 对应 Linux unshare_sighand：在旧动作锁下取得安全点的最终快照。
    lock(&pids_lock);
    if (transition->sighand != NULL) {
        retired_sighand = task->sighand;
        lock(&retired_sighand->lock);
        memcpy(transition->sighand->action,
                retired_sighand->action,
                sizeof(transition->sighand->action));
        task->sighand = transition->sighand;
        transition->sighand = NULL;
        unlock(&retired_sighand->lock);
    } else {
        assert(atomic_load_explicit(
                    &task->sighand->refcount,
                    memory_order_acquire) == 1);
    }
    unlock(&pids_lock);

    if (retired_sighand != NULL)
        sighand_release(retired_sighand);

    assert(atomic_load_explicit(
                &task->files->refcount, memory_order_acquire) == 1);
    task_finish_exec_state(task, euid, egid, comm);

    *transition = (struct task_exec_transition) {};
    aarch64_linux_process_destroy(retired);
    if (retired_mm != NULL)
        mm_release(retired_mm);

    task_notify_exec(task);
}

void task_discard_aarch64_exec(struct task *task) {
    assert(task != NULL);
    assert(!task->exec_transition.begun || task->exiting);
    aarch64_linux_process_destroy(task->exec_transition.process);
    if (task->exec_transition.mm != NULL)
        mm_release(task->exec_transition.mm);
    if (task->exec_transition.sighand != NULL)
        sighand_release(task->exec_transition.sighand);
    task->exec_transition = (struct task_exec_transition) {};
}

struct aarch64_linux_process *task_take_aarch64_process(
        struct task *task) {
    assert(task != NULL);
    return __atomic_exchange_n(
            &task->aarch64_process, NULL, __ATOMIC_ACQ_REL);
}

void task_release_aarch64_process(struct task *task) {
    // 原始析构不产生退出副作用；失败构造与候选回滚也会经过这里。
    aarch64_linux_process_destroy(
            task_take_aarch64_process(task));
}

static bool pid_empty(struct pid *pid) {
    return !pid->reserved && pid->task == NULL &&
            list_empty(&pid->session) && list_empty(&pid->pgroup);
}

static struct pid *pid_slot(dword_t id) {
    if (id >= array_size(pids))
        return NULL;
    return &pids[id];
}

struct pid *pid_get(dword_t id) {
    struct pid *pid = pid_slot(id);
    if (pid == NULL || pid->reserved || pid_empty(pid))
        return NULL;
    return pid;
}

struct task *pid_get_task_zombie(dword_t id) {
    struct pid *pid = pid_get(id);
    if (pid == NULL)
        return NULL;
    struct task *task = pid->task;
    return task;
}

struct task *pid_get_task(dword_t id) {
    struct task *task = pid_get_task_zombie(id);
    if (task != NULL && (task->zombie || task->exiting))
        return NULL;
    return task;
}

struct task *task_process_representative_locked(struct task *task) {
    if (task == NULL)
        return NULL;
    struct tgroup *group = task->group;
    struct task *preferred[] = {
        task,
        group->exec_task,
        group->leader,
    };
    for (size_t index = 0; index < array_size(preferred); index++) {
        struct task *candidate = preferred[index];
        if (candidate != NULL &&
                !candidate->zombie && !candidate->exiting)
            return candidate;
    }
    struct task *member;
    list_for_each_entry(&group->threads, member, group_links) {
        if (!member->zombie && !member->exiting)
            return member;
    }
    return NULL;
}

struct task *pid_get_process_task(dword_t id) {
    struct task *task = pid_get_task_zombie(id);
    if (task == NULL)
        return NULL;
    if (!task->zombie && !task->exiting)
        return task;
    if (task == task->group->leader) {
        struct task *execer = task->group->exec_task;
        if (execer != NULL && !execer->zombie && !execer->exiting)
            return execer;
    }
    return NULL;
}

struct task *task_create_(struct task *parent) {
    struct task *task = malloc(sizeof(struct task));
    if (task == NULL)
        return NULL;
    *task = (struct task) {};
    if (parent != NULL) {
        *task = *parent;
    } else {
        task_altstack_reset(task);
    }
    // opaque process 不可通过 task 的结构体浅拷贝共享。
    task->aarch64_process = NULL;
    task->exec_transition = (struct task_exec_transition) {};

    task_thread_store(task, zero_init(pthread_t));
    atomic_init(&task->start_ready, false);
    task->threadid = 0;
    task->cpu.poked_ptr = NULL;
    task->cpu._poked = false;
    list_init(&task->group_links);
    list_init(&task->children);
    list_init(&task->siblings);
    task->parent = parent;
    task->pending = 0;
    task->pending_bit_only = 0;
    task->pending_timer_bit_only = 0;
    task->waiting = 0;
    list_init(&task->queue);
    task->saved_mask = 0;
    task->has_saved_mask = false;
    task->clear_tid = 0;
    task->robust_list = 0;
    task->aarch64_robust_list = 0;
    task->did_exec = false;
    task->exit_code = 0;
    task->zombie = false;
    task->exiting = false;
    task->vfork = NULL;
    task->exit_signal = 0;

    task->general_lock = (lock_t) {0};
    lock_init(&task->general_lock);
    task->sockrestart = (struct task_sockrestart) {};
    list_init(&task->sockrestart.listen);
    task->waiting_cond = NULL;
    task->waiting_lock = NULL;
    task->waiting_poll_active = false;
    task->waiting_poll_notify_fd = -1;
    task->waiting_cond_lock = (lock_t) {0};
    lock_init(&task->waiting_cond_lock);
    task->pause = (cond_t) {0};
    cond_init(&task->pause);
    task->ptrace = (typeof(task->ptrace)) {0};
    lock_init(&task->ptrace.lock);
    cond_init(&task->ptrace.cond);

    lock(&pids_lock);
    static int cur_pid = 0;
    do {
        cur_pid++;
        if (cur_pid > MAX_PID) cur_pid = 1;
    } while (!pid_empty(&pids[cur_pid]));
    struct pid *pid = &pids[cur_pid];
    pid->id = cur_pid;
    pid->reserved = true;
    list_init(&pid->session);
    list_init(&pid->pgroup);
    task->pid = pid->id;
    unlock(&pids_lock);
    return task;
}

void task_publish_locked(struct task *task) {
    struct pid *pid = pid_slot(task->pid);
    assert(pid != NULL && pid->reserved && pid->task == NULL);

    struct tgroup *group = task->group;
    if (task_is_leader(task)) {
        struct pid *session = pid_slot(group->sid);
        struct pid *pgroup = pid_slot(group->pgid);
        assert(session != NULL && pgroup != NULL);
        list_add(&session->session, &group->session);
        list_add(&pgroup->pgroup, &group->pgroup);
    }
    list_add(&group->threads, &task->group_links);
    if (task->parent != NULL)
        list_add(&task->parent->children, &task->siblings);

    pid->task = task;
    pid->reserved = false;
}

void task_publish(struct task *task) {
    lock(&pids_lock);
    lock(&task->group->lock);
    task_publish_locked(task);
    unlock(&task->group->lock);
    unlock(&pids_lock);
}

void task_abort_create(struct task *task) {
    lock(&pids_lock);
    struct pid *pid = pid_slot(task->pid);
    assert(pid != NULL && pid->reserved && pid->task == NULL);
    pid->reserved = false;
    unlock(&pids_lock);
    task_discard_aarch64_exec(task);
    task_release_aarch64_process(task);
    cond_destroy(&task->pause);
    cond_destroy(&task->ptrace.cond);
    free(task);
}

void task_destroy(struct task *task) {
    assert(task->aarch64_process == NULL &&
            task->exec_transition.process == NULL &&
            task->exec_transition.mm == NULL &&
            task->exec_transition.sighand == NULL &&
            !task->exec_transition.begun &&
            !task->exec_transition.ready);
    list_remove(&task->siblings);
    struct pid *pid = pid_slot(task->pid);
    assert(pid != NULL && !pid->reserved && pid->task == task);
    pid->task = NULL;
    free(task);
}

static bool task_group_has_other_threads(
        const struct tgroup *group, const struct task *task) {
    struct task *member;
    list_for_each_entry(&group->threads, member, group_links) {
        if (member != task)
            return true;
    }
    return false;
}

int task_group_fatal_signal(const struct task *task) {
    return atomic_load_explicit(
            &task->group->external_fatal_signal, memory_order_acquire);
}

// 调用方持有 pids_lock；marker 的发布与清除都串在共享 sighand 内。
static void task_exec_clear_marker_locked(struct task *task) {
    lock(&task->sighand->lock);
    lock(&task->group->lock);
    assert(task->group->exec_task == task);
    task->group->exec_task = NULL;
    unlock(&task->group->lock);
    unlock(&task->sighand->lock);
}

int task_exec_unshare_files(struct task *task) {
    assert(task != NULL && task->files != NULL);
    struct fdtable *shared_files = task->files;
    struct fdtable *private_files = NULL;
    if (atomic_load_explicit(
                &shared_files->refcount, memory_order_acquire) > 1) {
        private_files = fdtable_copy(shared_files);
        if (IS_ERR(private_files))
            return (int) PTR_ERR(private_files);
    }

    lock(&pids_lock);
    bool killed = task_exec_is_killed(task);
    if (!killed && private_files != NULL) {
        assert(task->files == shared_files);
        task->files = private_files;
        private_files = NULL;
    }
    unlock(&pids_lock);

    if (private_files != NULL)
        fdtable_release(private_files);
    if (task->files != shared_files)
        fdtable_release(shared_files);
    return killed ? _EINTR : 0;
}

// 成功后线程组只保留 task；等待期间不会吞掉外部 SIGKILL。
int task_exec_dethread(struct task *task) {
    assert(task != NULL);
    struct tgroup *group = task->group;

    lock(&pids_lock);
    lock(&task->sighand->lock);
    lock(&group->lock);
    int external_fatal = task_group_fatal_signal(task);
    if (group->doing_group_exit || group->exec_task != NULL ||
            external_fatal != 0) {
        bool killed = external_fatal != 0 ||
                sigset_has(task->pending, SIGKILL_);
        unlock(&group->lock);
        unlock(&task->sighand->lock);
        unlock(&pids_lock);
        return killed ? _EINTR : _EAGAIN;
    }
    group->exec_task = task;
    group->stopped = false;
    group->stop_code = 0;
    group->continued = false;
    group->continue_notification_pending = false;
    notify(&group->stopped_cond);
    unlock(&group->lock);
    unlock(&task->sighand->lock);

    // 强制信号只负责唤醒 peer；do_exit_group 会识别 exec 协调态。
    struct task *peer;
    list_for_each_entry(&group->threads, peer, group_links) {
        if (peer != task && !peer->exiting) {
            deliver_signal_locked(peer, SIGKILL_, SIGINFO_NIL);
        }
    }
    while (task_group_has_other_threads(group, task)) {
        if (task_sigkill_pending(task) ||
                task_group_fatal_signal(task) != 0) {
            task_exec_clear_marker_locked(task);
            unlock(&pids_lock);
            return _EINTR;
        }
        wait_for_ignore_signals(&group->child_exit, &pids_lock, NULL);
        if (task_sigkill_pending(task) ||
                task_group_fatal_signal(task) != 0) {
            task_exec_clear_marker_locked(task);
            unlock(&pids_lock);
            return _EINTR;
        }
    }

    struct task *old_leader = group->leader;
    if (old_leader != task) {
        assert(old_leader->exiting &&
                list_null(&old_leader->group_links));
        struct pid *leader_pid = pid_slot(old_leader->pid);
        struct pid *thread_pid = pid_slot(task->pid);
        assert(leader_pid != NULL && thread_pid != NULL &&
                leader_pid->task == old_leader &&
                thread_pid->task == task);

        // 当前线程取代旧 leader 在父进程子链中的位置。
        list_remove_safe(&task->siblings);
        task->parent = old_leader->parent;
        if (task->parent != NULL)
            list_add_before(&old_leader->siblings, &task->siblings);
        else
            list_init(&task->siblings);

        pid_t_ thread_id = task->pid;
        task->pid = old_leader->pid;
        old_leader->pid = thread_id;
        leader_pid->task = task;
        thread_pid->task = old_leader;
        task->tgid = task->pid;
        group->leader = task;
        old_leader->exit_signal = 0;
        task_destroy(old_leader);
    }
    task->exit_signal = SIGCHLD_;
    task_exec_clear_marker_locked(task);
    unlock(&pids_lock);
    return 0;
}

static void task_run_i386_current(void) {
    struct cpu_state *cpu = &current->cpu;
    struct tlb tlb = {};
    tlb_refresh(&tlb, &current->mem->mmu);
    while (!task_has_aarch64_exec_candidate(current)) {
        read_wrlock(&current->mem->lock);
        int interrupt = cpu_run_to_interrupt(cpu, &tlb);
        read_wrunlock(&current->mem->lock);
        handle_interrupt(interrupt);
    }
    task_commit_aarch64_exec(current);
}

void task_run_current(void) {
    while (true) {
        if (task_has_aarch64_process(current))
            aarch64_task_run_current();
        else
            task_run_i386_current();
    }
}

static void *task_thread(void *task) {
    current = task;
    while (!atomic_load_explicit(&current->start_ready, memory_order_acquire))
        sched_yield();
    update_thread_name();
    task_run_current();
    die("task_thread returned"); // above function call should never return
}

static pthread_attr_t task_thread_attr;
__attribute__((constructor)) static void create_attr(void) {
    pthread_attr_init(&task_thread_attr);
    pthread_attr_setdetachstate(&task_thread_attr, PTHREAD_CREATE_DETACHED);
}

void task_start_suspended(struct task *task) {
    atomic_store_explicit(&task->start_ready, false, memory_order_relaxed);
    lock(&task->sighand->lock);
    pthread_t thread;
    if (pthread_create(&thread, &task_thread_attr, task_thread, task) != 0)
        die("could not create thread");
    task_thread_store(task, thread);
    unlock(&task->sighand->lock);
}

void task_release_start(struct task *task) {
    atomic_store_explicit(&task->start_ready, true, memory_order_release);
}

void task_start(struct task *task) {
    lock(&pids_lock);
    task_start_suspended(task);
    unlock(&pids_lock);
    task_release_start(task);
}

int_t sys_sched_yield(void) {
    STRACE("sched_yield()");
    sched_yield();
    return 0;
}

void update_thread_name(void) {
    char name[16]; // As long as Linux will let us make this
    snprintf(name, sizeof(name), "-%d", current->pid);
    size_t pid_width = strlen(name);
    size_t name_width = snprintf(name, sizeof(name), "%s", current->comm);
    sprintf(name + (name_width < sizeof(name) - 1 - pid_width ? name_width : sizeof(name) - 1 - pid_width), "-%d", current->pid);
#if __APPLE__
    pthread_setname_np(name);
#else
    pthread_setname_np(pthread_self(), name);
#endif
}
