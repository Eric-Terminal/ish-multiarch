#include <pthread.h>
#include <signal.h>
#include <string.h>
#include "guest/aarch64/linux-process.h"
#include "kernel/calls.h"
#include "kernel/mm.h"
#include "kernel/futex.h"
#include "kernel/ptrace.h"
#include "fs/fd.h"
#include "fs/tty.h"

static void halt_system(void);

static bool exit_tgroup(struct task *task) {
    struct tgroup *group = task->group;
    list_remove(&task->group_links);
    // de-thread 与 wait 共用该条件，谓词均在 pids_lock 下复查。
    notify(&group->child_exit);
    bool group_dead = list_empty(&group->threads);
    if (group_dead) {
        // don't need to lock the group since the only pointers to it come from:
        // - other threads' current->group, but there are none left thanks to that list_empty call
        // - locking pids_lock first, which do_exit did
        // The group will be removed from its group and session by reap_if_zombie,
        // because fish tries to set the pgid to that of an exited but not reaped
        // task.
        // https://github.com/Microsoft/WSL/issues/2786
    }
    return group_dead;
}

void (*exit_hook)(struct task *task, int code) = NULL;

// 调用方持有 pids_lock；返回的 tty 引用必须在退出进程表临界区后释放。
static struct tty *release_reaped_task_locked(struct task *task) {
    cond_destroy(&task->group->child_exit);
    cond_destroy(&task->group->stopped_cond);
    struct tty *tty = task_leave_session(task);
    list_remove(&task->group->pgroup);
    free(task->group);
    task_destroy(task);
    return tty;
}

static void release_detached_tty(struct tty *tty) {
    if (tty == NULL)
        return;
    assert(!lock_owned_by_current(&pids_lock));
    lock(&ttys_lock);
    tty_release(tty);
    unlock(&ttys_lock);
}

static struct task *find_new_parent(struct task *task) {
    struct task *new_parent;
    list_for_each_entry(&task->group->threads, new_parent, group_links) {
        if (!new_parent->exiting)
            return new_parent;
    }
    return pid_get_task_zombie(1);
}

noreturn void do_exit(int status) {
    // has to happen before mm_release
    if (task_has_aarch64_process(current)) {
        struct aarch64_linux_process *process =
                current->aarch64_process;
        futex_cleanup_task_aarch64(current, process);
    } else {
        futex_cleanup_task_i386(current);
    }

    // 最后一个进入退出流程的线程负责在资源仍完整时退休组定时器。
    lock(&pids_lock);
    lock(&current->sighand->lock);
    current->exiting = true;
    signal_retarget_shared_pending_locked(
            current, ~current->blocked);
    bool all_exiting = true;
    struct task *group_task;
    list_for_each_entry(
            &current->group->threads, group_task, group_links) {
        if (!group_task->exiting) {
            all_exiting = false;
            break;
        }
    }
    unlock(&current->sighand->lock);
    unlock(&pids_lock);
    if (all_exiting)
        tgroup_timers_destroy(current->group);

    // release all our resources
    task_discard_aarch64_exec(current);
    task_release_aarch64_process(current);
    mm_release(current->mm);
    current->mm = NULL;
    fdtable_release(current->files);
    current->files = NULL;
    fs_info_release(current->fs);
    current->fs = NULL;
    // sighand must be released below so it can be protected by pids_lock
    // since it can be accessed by other threads

    // save things that our parent might be interested in
    current->exit_code = status; // FIXME locking
    struct rusage_ rusage = rusage_get_current();
    lock(&current->group->lock);
    rusage_add(&current->group->rusage, &rusage);
    struct rusage_ group_rusage = current->group->rusage;
    unlock(&current->group->lock);

    // the actual freeing needs pids_lock
    lock(&pids_lock);
    // release the sighand
    sighand_release(current->sighand);
    current->sighand = NULL;
    signal_flush_pending(current);
    struct task *leader = current->group->leader;

    // reparent children
    struct task *new_parent = find_new_parent(current);
    struct task *child, *tmp;
    list_for_each_entry_safe(&current->children, child, tmp, siblings) {
        child->parent = new_parent;
        list_remove(&child->siblings);
        list_add(&new_parent->children, &child->siblings);
    }

    bool group_dead = exit_tgroup(current);
    if (group_dead)
        signal_flush_group_pending(current->group);
    bool auto_reap = false;
    if (group_dead) {
        // notify parent that we died
        struct task *parent = leader->parent;
        if (parent == NULL) {
            // init died
            halt_system();
        } else {
            bool send_exit_signal;
            auto_reap = signal_parent_child_exit_policy_locked(
                    parent, leader->exit_signal, &send_exit_signal);
            leader->zombie = !auto_reap;
            notify(&parent->group->child_exit);
            dword_t exit_code = leader->exit_code;
            if (leader->group->doing_group_exit)
                exit_code = leader->group->group_exit_code;
            struct siginfo_ info = {
                .code = SI_KERNEL_,
                .payload_kind = SIGNAL_INFO_PAYLOAD_CHILD,
                .child.pid = leader->pid,
                .child.uid = leader->uid,
                .child.status = exit_code,
                .child.utime = clock_from_timeval(group_rusage.utime),
                .child.stime = clock_from_timeval(group_rusage.stime),
            };
            if (send_exit_signal)
                send_process_signal(parent, leader->exit_signal, info);
        }

        if (exit_hook != NULL)
            exit_hook(current, status);
    }

    vfork_notify(current);
    if (current != leader) {
        task_destroy(current);
        // 后续 auto-reap 仍会取得锁，LOCK_DEBUG 不得观察已释放的 TLS task。
        current = NULL;
    }
    struct tty *reaped_tty = NULL;
    if (group_dead && auto_reap)
        reaped_tty = release_reaped_task_locked(leader);
    unlock(&pids_lock);
    // 此线程不再执行 guest 工作；auto-reap 还可能已经释放 TLS 指向的 task。
    current = NULL;
    release_detached_tty(reaped_tty);

    pthread_exit(NULL);
}

noreturn void do_exit_group(int status) {
    struct tgroup *group = current->group;
    lock(&pids_lock);
    struct sighand *sighand = current->sighand;
    lock(&sighand->lock);
    lock(&group->lock);
    int external_fatal = task_group_fatal_signal(current);
    if (group->exec_task != NULL && group->exec_task != current &&
            !group->doing_group_exit && external_fatal == 0) {
        unlock(&group->lock);
        unlock(&sighand->lock);
        unlock(&pids_lock);
        // exec 发出的致命信号只清理当前 peer，不能反杀执行线程。
        do_exit(0);
    }
    if (external_fatal != 0)
        status = external_fatal;
    if (!group->doing_group_exit) {
        group->doing_group_exit = true;
        group->group_exit_code = status;
    } else {
        status = group->group_exit_code;
    }
    unlock(&group->lock);
    unlock(&sighand->lock);

    // kill everyone else in the group
    struct task *task;
    list_for_each_entry(&group->threads, task, group_links) {
        deliver_signal_locked(task, SIGKILL_, SIGINFO_NIL);
    }

    lock(&group->lock);
    group->stopped = false;
    group->stop_code = 0;
    notify(&group->stopped_cond);
    unlock(&group->lock);
    unlock(&pids_lock);
    do_exit(status);
}

// 仅由持有 pids_lock 的 init 退出路径调用，并在返回前重新持有该锁。
static void halt_system(void) {
    for (int state = 0; state < 3; state++) {
        int tasks_found = 0;
        for (int i = 2; i < MAX_PID; i++) {
            struct task *task = pid_get_task_zombie(i);
            if (task != NULL && !task->zombie) {
                tasks_found++;
                if (task->exiting)
                    continue;
                switch (state) {
                case 0:
                    deliver_signal_locked(task, SIGTERM_, SIGINFO_NIL);
                    break;
                case 1:
                    send_signal_locked(task, SIGKILL_, SIGINFO_NIL);
                    break;
                case 2:
                    pthread_kill(task_thread_load(task), SIGTERM);
                }
            }
        }
        if (tasks_found == 0)
            break;
        if (state != 2) {
            unlock(&pids_lock);
            sleep(1);
            lock(&pids_lock);
        }
    }

    // unmount all filesystems
    lock(&mounts_lock);
    struct mount *mount, *tmp;
    list_for_each_entry_safe(&mounts, mount, tmp, mounts) {
        mount_remove(mount);
    }
    unlock(&mounts_lock);
}

dword_t sys_exit(dword_t status) {
    STRACE("exit(%d)\n", status);
    do_exit(status << 8);
}

dword_t sys_exit_group(dword_t status) {
    STRACE("exit_group(%d)\n", status);
    do_exit_group(status << 8);
}

#define WNOHANG_ (1 << 0)
#define WUNTRACED_ (1 << 1)
#define WEXITED_ (1 << 2)
#define WCONTINUED_ (1 << 3)
#define WNOWAIT_ (1 << 24)
#define __WALL_ (1 << 30)
#define WAIT4_OPTIONS_ (WNOHANG_|WUNTRACED_|WCONTINUED_|__WALL_)

#define P_ALL_ 0
#define P_PID_ 1
#define P_PGID_ 2

// returns false if the task cannot be reaped and true if the task was reaped
static bool reap_if_zombie(struct task *task, struct siginfo_ *info_out,
        struct rusage_ *rusage_out, int options,
        struct tty **released_tty) {
    if (!task->zombie)
        return false;
    lock(&task->group->lock);

    dword_t exit_code = task->exit_code;
    if (task->group->doing_group_exit)
        exit_code = task->group->group_exit_code;
    info_out->child.status = exit_code;

    struct rusage_ rusage = task->group->rusage;
    if (!(options & WNOWAIT_)) {
        lock(&current->group->lock);
        rusage_add(&current->group->children_rusage, &rusage);
        unlock(&current->group->lock);
    }
    if (rusage_out != NULL)
        *rusage_out = rusage;

    unlock(&task->group->lock);

    // WNOWAIT means don't destroy the child, instead leave it so it could be waited for again.
    if (options & WNOWAIT_)
        return true;

    *released_tty = release_reaped_task_locked(task);
    return true;
}

static bool notify_if_stopped(struct task *task,
        struct siginfo_ *info_out, int options) {
    lock(&task->group->lock);
    bool stopped = task->group->stopped &&
            task->group->stop_code != 0;
    if (stopped) {
        info_out->child.status = task->group->stop_code;
        if (!(options & WNOWAIT_))
            task->group->stop_code = 0;
    }
    unlock(&task->group->lock);
    return stopped;
}

static bool notify_if_continued(struct task *task,
        struct siginfo_ *info_out, int options) {
    lock(&task->group->lock);
    bool continued = task->group->continued;
    if (continued && !(options & WNOWAIT_))
        task->group->continued = false;
    unlock(&task->group->lock);
    if (continued)
        info_out->child.status = UINT32_C(0xffff);
    return continued;
}

static bool reap_if_needed(struct task *task, struct siginfo_ *info_out,
        struct rusage_ *rusage_out, int options,
        struct tty **released_tty) {
    assert(task_is_leader(task));
    if ((options & WEXITED_ && reap_if_zombie(task, info_out,
            rusage_out, options, released_tty)) ||
        (options & WUNTRACED_ &&
                notify_if_stopped(task, info_out, options)) ||
        (options & WCONTINUED_ &&
                notify_if_continued(task, info_out, options))) {
        info_out->sig = SIGCHLD_;
        info_out->payload_kind = SIGNAL_INFO_PAYLOAD_CHILD;
        return true;
    }
    lock(&task->ptrace.lock);
    if (task->ptrace.stopped && task->ptrace.signal) {
        // I had this code here because it made something work, but it's now
        // making GDB think we support events (we don't). I can't remember what
        // it fixed but until then commenting it out for now.
        info_out->child.status = /* task->ptrace.trap_event << 16 |*/ task->ptrace.signal << 8 | 0x7f;
        task->ptrace.signal = 0;
        info_out->sig = SIGCHLD_;
        info_out->payload_kind = SIGNAL_INFO_PAYLOAD_CHILD;
        unlock(&task->ptrace.lock);
        return true;
    }
    unlock(&task->ptrace.lock);
    return false;
}

int do_wait(int idtype, pid_t_ id, struct siginfo_ *info, struct rusage_ *rusage, int options) {
    if (idtype != P_ALL_ && idtype != P_PID_ && idtype != P_PGID_)
        return _EINVAL;
    if (options & ~(WNOHANG_|WUNTRACED_|WEXITED_|WCONTINUED_|WNOWAIT_|__WALL_))
        return _EINVAL;

    lock(&pids_lock);
    int err;
    bool got_signal = false;
    struct tty *released_tty = NULL;

retry:
    if (idtype != P_PID_) {
        // look for a zombie child
        bool no_children = true;
        struct task *parent;
        list_for_each_entry(&current->group->threads, parent, group_links) {
            struct task *task;
            list_for_each_entry(&parent->children, task, siblings) {
                if (!task_is_leader(task))
                    continue;
                if (idtype == P_PGID_ && task->group->pgid != id)
                    continue;
                no_children = false;
                info->child.pid = task->pid;
                if (reap_if_needed(task, info, rusage,
                        options, &released_tty))
                    goto found_something;
            }
        }
        err = _ECHILD;
        if (no_children)
            goto error;
    } else {
        // check if this child is a zombie
        struct task *task = pid_get_task_zombie(id);
        err = _ECHILD;
        if (task == NULL || task->parent == NULL || task->parent->group != current->group)
            goto error;
        task = task->group->leader;
        info->child.pid = id;
        if (reap_if_needed(task, info, rusage,
                options, &released_tty))
            goto found_something;
    }

    // WNOHANG leaves the info in an implementation-defined state. set the pid
    // to 0 so wait4 can pass that along correctly.
    info->child.pid = 0;
    if (options & WNOHANG_) {
        info->sig = SIGCHLD_;
        goto found_something;
    }

    err = _EINTR;
    if (got_signal)
        goto error;

    // no matching zombie found, wait for one
    if (wait_for(&current->group->child_exit, &pids_lock, NULL)) {
        // maybe we got a SIGCHLD! go through the loop one more time to make
        // sure the newly exited process is returned in that case.
        got_signal = true;
        goto retry;
    }
    goto retry;

found_something:
    unlock(&pids_lock);
    release_detached_tty(released_tty);
    return 0;

error:
    unlock(&pids_lock);
    return err;
}

dword_t sys_waitid(int_t idtype, pid_t_ id, addr_t info_addr, int_t options) {
    STRACE("waitid(%d, %d, %#x, %#x)", idtype, id, info_addr, options);
    struct siginfo_ info = {};
    int_t res = do_wait(idtype, id, &info, NULL, options);
    if (res < 0 || (res == 0 && info.child.pid == 0))
        return res;
    if (info_addr != 0 && write_i386_siginfo(info_addr, &info))
        return _EFAULT;
    return 0;
}

sdword_t do_wait4(pid_t_ id, dword_t options,
        struct wait4_result *result) {
    assert(result != NULL);
    *result = (struct wait4_result) {0};
    if (options & ~WAIT4_OPTIONS_)
        return _EINVAL;
    // Linux 明确定义该边界为 ESRCH，且不能计算未定义的 -INT_MIN。
    if (id == INT32_MIN)
        return _ESRCH;

    int idtype;
    if (id > 0)
        idtype = P_PID_;
    else if (id == -1)
        idtype = P_ALL_;
    else {
        idtype = P_PGID_;
        if (id == 0)
            id = current->group->pgid;
        else
            id = -id;
    }

    struct siginfo_ info = {0};
    int_t res = do_wait(idtype, id, &info,
            &result->rusage, options | WEXITED_);
    if (res < 0)
        return res;
    result->status = info.child.status;
    return info.child.pid;
}

dword_t sys_wait4(pid_t_ id, addr_t status_addr, dword_t options, addr_t rusage_addr) {
    STRACE("wait4(%d, %#x, %#x, %#x)", id, status_addr, options, rusage_addr);
    struct wait4_result result;
    sdword_t waited = do_wait4(id, options, &result);
    if (waited <= 0)
        return (dword_t) waited;
    if (status_addr != 0 && user_put(status_addr, result.status))
        return _EFAULT;
    if (rusage_addr != 0 && user_put(rusage_addr, result.rusage))
        return _EFAULT;
    return (dword_t) waited;
}

dword_t sys_waitpid(pid_t_ pid, addr_t status_addr, dword_t options) {
    return sys_wait4(pid, status_addr, options, 0);
}
