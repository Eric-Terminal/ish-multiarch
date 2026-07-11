#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#include "kernel/errno.h"
#include "kernel/ptrace.h"
#include "kernel/task.h"

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "任务发布测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        return 1; \
    } \
} while (0)

int main(void) {
    struct task parent = {0};
    struct tgroup group = {0};
    struct sighand sighand = {.refcount = 1};
    list_init(&group.threads);
    list_init(&group.session);
    list_init(&group.pgroup);
    lock_init(&group.lock);
    cond_init(&group.stopped_cond);
    group.leader = &parent;
    lock_init(&sighand.lock);

    list_init(&parent.group_links);
    list_add(&group.threads, &parent.group_links);
    list_init(&parent.children);
    list_init(&parent.siblings);
    list_init(&parent.queue);
    parent.pid = parent.tgid = 42;
    parent.group = &group;
    parent.sighand = &sighand;
    parent.blocked = sig_mask(SIGUSR1_);
    parent.pending = sig_mask(SIGUSR2_);
    parent.waiting = sig_mask(SIGALRM_);
    parent.saved_mask = UINT64_MAX;
    parent.has_saved_mask = true;
    parent.cpu._poked = true;
    parent.cpu.poked_ptr = &parent.cpu._poked;
    parent.exiting = true;
    parent.zombie = true;
    parent.exit_code = UINT32_MAX;
    parent.ptrace.traced = true;

    lock_init(&parent.general_lock);
    lock_init(&parent.waiting_cond_lock);
    cond_init(&parent.pause);
    lock_init(&parent.ptrace.lock);
    cond_init(&parent.ptrace.cond);

    lock(&parent.general_lock);
    lock(&parent.waiting_cond_lock);
    lock(&parent.ptrace.lock);
    struct task *child = task_create_(&parent);
    CHECK(child != NULL, "创建保留 PID 的子任务");
    CHECK(trylock(&child->general_lock) == 0 &&
            trylock(&child->waiting_cond_lock) == 0 &&
            trylock(&child->ptrace.lock) == 0,
            "子任务互斥锁不继承父任务的加锁状态");
    unlock(&child->ptrace.lock);
    unlock(&child->waiting_cond_lock);
    unlock(&child->general_lock);
    unlock(&parent.ptrace.lock);
    unlock(&parent.waiting_cond_lock);
    unlock(&parent.general_lock);

    lock(&pids_lock);
    CHECK(pid_get_task(child->pid) == NULL,
            "保留 PID 在完整发布前不可查询");
    unlock(&pids_lock);
    CHECK(list_empty(&parent.children) &&
            list_size(&group.threads) == 1,
            "未发布任务不进入父子链或线程组链");
    CHECK(child->parent == &parent &&
            child->blocked == parent.blocked && child->pending == 0 &&
            child->waiting == 0 && child->saved_mask == 0 &&
            !child->has_saved_mask && child->cpu.poked_ptr == NULL &&
            !child->cpu._poked && !child->ptrace.traced &&
            !child->exiting && !child->zombie && child->exit_code == 0,
            "构建阶段只继承可继承状态并重置线程私有字段");

    task_publish(child);
    lock(&pids_lock);
    CHECK(pid_get_task(child->pid) == child &&
            list_size(&parent.children) == 1 &&
            list_size(&group.threads) == 2,
            "发布点同时公开 PID、父子链与线程组链");
    child->exiting = true;
    CHECK(pid_get_task(child->pid) == NULL &&
            pid_get_task_zombie(child->pid) == child,
            "退出中的任务不再暴露给普通查询但保留生命周期查找");
    child->exiting = false;
    unlock(&pids_lock);

    current = &parent;
    child->ptrace.stopped = true;
    CHECK(sys_ptrace(PTRACE_PEEKUSER_, (dword_t) child->pid,
            (addr_t) sizeof(struct user_), 0) == (dword_t) _EIO,
            "ptrace 参数错误保持预期返回值");
    CHECK(trylock(&pids_lock) == 0,
            "ptrace 参数错误释放 PID 生命周期锁");
    unlock(&pids_lock);
    CHECK(trylock(&child->ptrace.lock) == 0,
            "ptrace 参数错误释放子任务锁");
    unlock(&child->ptrace.lock);
    CHECK(sys_ptrace(PTRACE_CONT_, (dword_t) child->pid, 0, 0) == 0 &&
            !child->ptrace.stopped,
            "ptrace 成功路径在 PID 生命周期保护下访问子任务");
    CHECK(trylock(&pids_lock) == 0,
            "ptrace 成功路径释放 PID 生命周期锁");
    unlock(&pids_lock);
    CHECK(trylock(&child->ptrace.lock) == 0,
            "ptrace 成功路径释放子任务锁");
    unlock(&child->ptrace.lock);
    CHECK(sys_ptrace(PTRACE_CONT_, 0, 0, 0) == (dword_t) _EPERM,
            "ptrace 未找到子任务时返回 EPERM");
    CHECK(trylock(&pids_lock) == 0,
            "ptrace 未找到子任务时释放 PID 生命周期锁");
    unlock(&pids_lock);

    signal(SIGUSR1, SIG_IGN);
    task_thread_store(child, pthread_self());
    child->ptrace.stopped = true;
    CHECK(sys_ptrace(PTRACE_KILL_, (dword_t) child->pid, 0, 0) == 0 &&
            !child->ptrace.stopped && sigset_has(child->pending, SIGKILL_),
            "ptrace kill 在释放子任务锁后投递信号");
    CHECK(trylock(&pids_lock) == 0,
            "ptrace kill 释放 PID 生命周期锁");
    unlock(&pids_lock);
    CHECK(trylock(&child->ptrace.lock) == 0,
            "ptrace kill 释放子任务锁");
    unlock(&child->ptrace.lock);
    struct sigqueue *queued = list_first_entry(
            &child->queue, struct sigqueue, queue);
    list_remove(&queued->queue);
    free(queued);
    child->pending = 0;
    current = NULL;

    lock(&pids_lock);
    lock(&group.lock);
    list_remove(&child->group_links);
    task_destroy(child);
    unlock(&group.lock);
    unlock(&pids_lock);
    CHECK(list_empty(&parent.children) &&
            list_size(&group.threads) == 1,
            "已发布测试任务可完整撤销关系");

    struct task *aborted = task_create_(&parent);
    CHECK(aborted != NULL, "创建待中止任务");
    pid_t_ aborted_pid = aborted->pid;
    task_abort_create(aborted);
    lock(&pids_lock);
    CHECK(pid_get_task(aborted_pid) == NULL,
            "中止构建后释放保留 PID");
    unlock(&pids_lock);
    CHECK(list_empty(&parent.children) &&
            list_size(&group.threads) == 1,
            "中止构建不产生可观察任务关系");
    bool reused_aborted_pid = false;
    for (int index = 0; index < MAX_PID; index++) {
        struct task *candidate = task_create_(&parent);
        CHECK(candidate != NULL, "循环验证时创建保留 PID");
        if (candidate->pid == aborted_pid)
            reused_aborted_pid = true;
        task_abort_create(candidate);
    }
    CHECK(reused_aborted_pid,
            "中止构建后保留 PID 可被分配器再次使用");

    struct task *leader = task_create_(&parent);
    CHECK(leader != NULL, "创建独立线程组首领");
    struct tgroup process_group = {0};
    list_init(&process_group.threads);
    list_init(&process_group.session);
    list_init(&process_group.pgroup);
    lock_init(&process_group.lock);
    process_group.leader = leader;
    process_group.sid = leader->pid;
    process_group.pgid = leader->pid;
    leader->group = &process_group;
    leader->tgid = leader->pid;

    task_publish(leader);
    lock(&pids_lock);
    struct pid *leader_pid = pid_get(leader->pid);
    CHECK(leader_pid != NULL && leader_pid->task == leader &&
            list_size(&leader_pid->session) == 1 &&
            list_size(&leader_pid->pgroup) == 1 &&
            list_size(&process_group.threads) == 1 &&
            list_size(&parent.children) == 1,
            "线程组首领在同一发布点公开会话、进程组和父子关系");
    lock(&process_group.lock);
    list_remove(&leader->group_links);
    list_remove(&process_group.session);
    list_remove(&process_group.pgroup);
    task_destroy(leader);
    unlock(&process_group.lock);
    unlock(&pids_lock);
    CHECK(list_empty(&parent.children),
            "线程组首领销毁后撤销父子关系");

    cond_destroy(&parent.pause);
    cond_destroy(&parent.ptrace.cond);
    cond_destroy(&group.stopped_cond);
    return 0;
}
