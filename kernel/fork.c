#include "debug.h"
#include <string.h>
#include "kernel/task.h"
#include "fs/fd.h"
#include "guest/aarch64/linux-process.h"
#include "kernel/calls.h"
#include "fs/tty.h"
#include "kernel/mm.h"
#include "kernel/ptrace.h"

#define CSIGNAL_ 0x000000ff
#define CLONE_VM_ 0x00000100
#define CLONE_FS_ 0x00000200
#define CLONE_FILES_ 0x00000400
#define CLONE_SIGHAND_ 0x00000800
#define CLONE_PTRACE_ 0x00002000
#define CLONE_VFORK_ 0x00004000
#define CLONE_PARENT_ 0x00008000
#define CLONE_THREAD_ 0x00010000
#define CLONE_NEWNS_ 0x00020000
#define CLONE_SYSVSEM_ 0x00040000
#define CLONE_SETTLS_ 0x00080000
#define CLONE_PARENT_SETTID_ 0x00100000
#define CLONE_CHILD_CLEARTID_ 0x00200000
#define CLONE_DETACHED_ 0x00400000
#define CLONE_UNTRACED_ 0x00800000
#define CLONE_CHILD_SETTID_ 0x01000000
#define CLONE_NEWCGROUP_ 0x02000000
#define CLONE_NEWUTS_ 0x04000000
#define CLONE_NEWIPC_ 0x08000000
#define CLONE_NEWUSER_ 0x10000000
#define CLONE_NEWPID_ 0x20000000
#define CLONE_NEWNET_ 0x40000000
#define CLONE_IO_ 0x80000000
#define IMPLEMENTED_FLAGS (CLONE_VM_|CLONE_FILES_|CLONE_FS_|CLONE_SIGHAND_|CLONE_SYSVSEM_|CLONE_VFORK_|CLONE_THREAD_|\
        CLONE_SETTLS_|CLONE_CHILD_SETTID_|CLONE_PARENT_SETTID_|CLONE_CHILD_CLEARTID_|CLONE_DETACHED_)

static void tgroup_init_copy(
        struct tgroup *group, struct tgroup *old_group) {
    *group = *old_group;
    list_init(&group->threads);
    list_init(&group->pgroup);
    list_init(&group->session);
    group->itimer = NULL;
    memset(group->posix_timers, 0, sizeof(group->posix_timers));
    group->doing_group_exit = false;
    group->exec_task = NULL;
    atomic_init(&group->external_fatal_signal, 0);
    group->shared_pending = 0;
    group->shared_bit_only = 0;
    group->shared_timer_bit_only = 0;
    list_init(&group->shared_queue);
    group->stop_code = 0;
    group->continued = false;
    group->continue_notification_pending = false;
    group->children_rusage = (struct rusage_) {};
    group->child_exit = (cond_t) {0};
    cond_init(&group->child_exit);
    group->stopped_cond = (cond_t) {0};
    cond_init(&group->stopped_cond);
    group->lock = (lock_t) {0};
    lock_init(&group->lock);
}

static int copy_task(struct task *task, dword_t flags, qword_t stack,
        qword_t ptid_addr, qword_t tls_addr, qword_t ctid_addr,
        struct guest_linux_user_fault *fault) {
    bool is_aarch64 = task_has_aarch64_process(task);
    if (!is_aarch64 && stack != 0)
        task->cpu.esp = (addr_t) stack;

    int err;
    struct mm *mm = task->mm;
    if (flags & CLONE_VM_) {
        mm_retain(mm);
    } else {
        struct mm *new_mm = mm_copy(mm);
        if (new_mm == NULL)
            return _ENOMEM;
        task_set_mm(task, new_mm);
    }
    task_altstack_on_clone(task,
            (flags & CLONE_VM_) != 0, (flags & CLONE_VFORK_) != 0);

    if (flags & CLONE_FILES_) {
        task->files->refcount++;
    } else {
        task->files = fdtable_copy(task->files);
        if (IS_ERR(task->files)) {
            err = (int) PTR_ERR(task->files);
            goto fail_free_mem;
        }
    }

    err = _ENOMEM;
    if (flags & CLONE_FS_) {
        task->fs->refcount++;
    } else {
        task->fs = fs_info_copy(task->fs);
        if (task->fs == NULL)
            goto fail_free_files;
    }

    if (flags & CLONE_SIGHAND_) {
        task->sighand->refcount++;
    } else {
        task->sighand = sighand_copy(task->sighand);
        if (task->sighand == NULL)
            goto fail_free_fs;
    }

    struct tgroup *old_group = task->group;
    struct tgroup *new_group = NULL;
    if (!(flags & CLONE_THREAD_)) {
        new_group = malloc(sizeof(*new_group));
        if (new_group == NULL) {
            err = _ENOMEM;
            goto fail_free_sighand;
        }
    }

    if ((flags & CLONE_SETTLS_) && !is_aarch64) {
        err = task_set_thread_area(task, (addr_t) tls_addr);
        if (err < 0)
            goto fail_free_sighand;
    }

    if ((flags & CLONE_CHILD_CLEARTID_) && !is_aarch64)
        task->clear_tid = (addr_t) ctid_addr;
    task->exit_signal = flags & CSIGNAL_;

    lock(&pids_lock);
    lock(&old_group->lock);
    bool group_action = old_group->doing_group_exit ||
            old_group->exec_task != NULL;
    unlock(&old_group->lock);
    bool killed = task_sigkill_pending(current) ||
            task_group_fatal_signal(current) != 0;
    if (group_action || killed) {
        unlock(&pids_lock);
        err = killed ? _EINTR : _EAGAIN;
        goto fail_free_sighand;
    }
    err = _EFAULT;
    if (flags & CLONE_CHILD_SETTID_) {
        bool failed = is_aarch64 ?
                !aarch64_linux_process_write_u32(
                        task->aarch64_process, ctid_addr,
                        (dword_t) task->pid, fault) :
                user_put_task(task, (addr_t) ctid_addr, task->pid);
        if (failed) {
            unlock(&pids_lock);
            goto fail_free_sighand;
        }
    }
    if (flags & CLONE_PARENT_SETTID_) {
        bool failed = is_aarch64 ?
                !aarch64_linux_process_write_u32(
                        current->aarch64_process, ptid_addr,
                        (dword_t) task->pid, fault) :
                user_put((addr_t) ptid_addr, task->pid);
        if (failed) {
            unlock(&pids_lock);
            goto fail_free_sighand;
        }
    }
    if (new_group != NULL) {
        // 整体快照包含由旧组 sighand 保护的共享 pending 字段。
        lock(&current->sighand->lock);
        lock(&old_group->lock);
        tgroup_init_copy(new_group, old_group);
        struct tty *inherited_tty = new_group->tty;
        unlock(&old_group->lock);
        unlock(&current->sighand->lock);
        // pids_lock 保证 controlling tty 不会被摘除；避免 group→tty 锁序反转。
        if (inherited_tty != NULL) {
            lock(&inherited_tty->lock);
            inherited_tty->refcount++;
            unlock(&inherited_tty->lock);
        }
        task->group = new_group;
        task->group->leader = task;
        task->tgid = task->pid;
    } else {
        // 同组线程与 leader 具有同一外部父任务，避免 leader 退出时自环。
        task->parent = old_group->leader->parent;
    }
    // 检查、启动与发布共用 pids_lock，组动作不会漏掉新子任务。
    task_start_suspended(task);
    lock(&task->group->lock);
    task_publish_locked(task);
    unlock(&task->group->lock);
    unlock(&pids_lock);

    // remember to do CLONE_SYSVSEM
    return 0;

fail_free_sighand:
    free(new_group);
    sighand_release(task->sighand);
fail_free_fs:
    fs_info_release(task->fs);
fail_free_files:
    fdtable_release(task->files);
fail_free_mem:
    mm_release(task->mm);
    return err;
}

static dword_t clone_task(dword_t flags, qword_t stack, qword_t ptid,
        qword_t tls, qword_t ctid,
        struct guest_linux_user_fault *fault) {
    if (flags & ~CSIGNAL_ & ~IMPLEMENTED_FLAGS) {
        FIXME("unimplemented clone flags 0x%x", flags & ~CSIGNAL_ & ~IMPLEMENTED_FLAGS);
        return _EINVAL;
    }
    if (flags & CLONE_SIGHAND_ && !(flags & CLONE_VM_))
        return _EINVAL;
    if (flags & CLONE_THREAD_ && !(flags & CLONE_SIGHAND_))
        return _EINVAL;
    bool is_aarch64 = task_has_aarch64_process(current);

    struct task *task = task_create_(current);
    if (task == NULL)
        return _ENOMEM;
    pid_t_ pid = task->pid;

    if (is_aarch64) {
        // copy_task 会在末尾发布 PID；opaque process 必须在此之前完整接入。
        struct aarch64_linux_process_error process_error;
        struct aarch64_linux_process *process;
        qword_t clear_child_tid =
                (flags & CLONE_CHILD_CLEARTID_) != 0 ? ctid : 0;
        if ((flags & CLONE_VM_) != 0) {
            const struct aarch64_linux_process_thread_config
                    process_config = {
                .tid = pid,
                .set_tls = (flags & CLONE_SETTLS_) != 0,
                .task_opaque = task,
                .stack_pointer = stack,
                .tls = tls,
                .clear_child_tid = clear_child_tid,
            };
            process = aarch64_linux_process_clone_thread(
                    current->aarch64_process,
                    &process_config, &process_error);
        } else {
            const struct aarch64_linux_process_fork_config
                    process_config = {
                .tid = pid,
                .set_tls = (flags & CLONE_SETTLS_) != 0,
                .task_opaque = task,
                .stack_pointer = stack,
                .tls = tls,
                .clear_child_tid = clear_child_tid,
            };
            process = aarch64_linux_process_fork(
                    current->aarch64_process,
                    &process_config, &process_error);
        }
        if (process == NULL) {
            task_abort_create(task);
            return process_error.stage ==
                    AARCH64_LINUX_PROCESS_ERROR_ALLOCATION ?
                    _ENOMEM : _EINVAL;
        }
        if (!task_attach_aarch64_process(task, process)) {
            aarch64_linux_process_destroy(process);
            task_abort_create(task);
            return _EINVAL;
        }
    }

    struct vfork_info vfork;
    if (flags & CLONE_VFORK_) {
        lock_init(&vfork.lock);
        cond_init(&vfork.cond);
        vfork.done = false;
        task->vfork = &vfork;
    }
    task->cpu.eax = 0;

    int err = copy_task(task, flags, stack, ptid, tls, ctid, fault);
    if (err < 0) {
        if (flags & CLONE_VFORK_)
            cond_destroy(&vfork.cond);
        task_abort_create(task);
        return err;
    }

    if (current->ptrace.traced) {
        current->ptrace.trap_event = PTRACE_EVENT_FORK_;
        send_signal(current, SIGTRAP_, SIGINFO_NIL);
    }
    task_release_start(task);

    if (flags & CLONE_VFORK_) {
        lock(&vfork.lock);
        while (!vfork.done)
            // FIXME this should stop waiting if a fatal signal is received
            wait_for_ignore_signals(&vfork.cond, &vfork.lock, NULL);
        unlock(&vfork.lock);
        cond_destroy(&vfork.cond);
    }

    return pid;
}

dword_t sys_clone(dword_t flags, addr_t stack, addr_t ptid,
        addr_t tls, addr_t ctid) {
    STRACE("clone(0x%x, 0x%x, 0x%x, 0x%x, 0x%x)",
            flags, stack, ptid, tls, ctid);
    return clone_task(flags, stack, ptid, tls, ctid, NULL);
}

dword_t sys_clone_aarch64(dword_t flags, qword_t stack, qword_t ptid,
        qword_t tls, qword_t ctid,
        struct guest_linux_user_fault *fault) {
    STRACE("clone(0x%x, 0x%llx, 0x%llx, 0x%llx, 0x%llx)",
            flags, (unsigned long long) stack,
            (unsigned long long) ptid, (unsigned long long) tls,
            (unsigned long long) ctid);
    assert(task_has_aarch64_process(current));
    return clone_task(flags, stack, ptid, tls, ctid, fault);
}

dword_t sys_fork(void) {
    return sys_clone(SIGCHLD_, 0, 0, 0, 0);
}

dword_t sys_vfork(void) {
    return sys_clone(CLONE_VFORK_ | CLONE_VM_ | SIGCHLD_, 0, 0, 0, 0);
}

void vfork_notify(struct task *task) {
    lock(&task->general_lock);
    struct vfork_info *vfork = task->vfork;
    if (vfork != NULL) {
        task->vfork = NULL;
        lock(&vfork->lock);
        vfork->done = true;
        notify(&vfork->cond);
        unlock(&vfork->lock);
    }
    unlock(&task->general_lock);
}
