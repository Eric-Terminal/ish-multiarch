#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#include "kernel/calls.h"
#include "kernel/signal.h"
#include "kernel/task.h"

/*
 * 直接纳入 fork 实现以覆盖私有的新线程组复制点；公开入口改名，避免与
 * 被链接的内核库重复定义。
 */
#define sys_clone host_job_test_sys_clone
#define sys_clone_aarch64 host_job_test_sys_clone_aarch64
#define sys_fork host_job_test_sys_fork
#define sys_vfork host_job_test_sys_vfork
#define vfork_notify host_job_test_vfork_notify
#include "kernel/fork.c"
#undef vfork_notify
#undef sys_vfork
#undef sys_fork
#undef sys_clone_aarch64
#undef sys_clone

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "宿主作业身份测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        return 1; \
    } \
} while (0)

static void initialize_group(
        struct tgroup *group, struct task *leader,
        pid_t_ sid, pid_t_ pgid) {
    *group = (struct tgroup) {0};
    list_init(&group->threads);
    list_init(&group->session);
    list_init(&group->pgroup);
    list_init(&group->shared_queue);
    lock_init(&group->lock);
    cond_init(&group->child_exit);
    cond_init(&group->stopped_cond);
    atomic_init(&group->external_fatal_signal, 0);
    group->leader = leader;
    group->sid = sid;
    group->pgid = pgid;
    group->limits[RLIMIT_SIGPENDING_] =
            (struct rlimit_) {1024, 1024};
}

static struct task *make_initial_process(
        uint64_t host_job_id, struct tgroup **group_out) {
    struct task *leader = task_create_(NULL);
    if (leader == NULL)
        return NULL;

    struct tgroup *group = malloc(sizeof(*group));
    if (group == NULL) {
        task_abort_create(leader);
        return NULL;
    }
    initialize_group(
            group, leader, leader->pid, leader->pid);
    leader->group = group;
    leader->tgid = leader->pid;
    leader->sighand = sighand_new();
    if (leader->sighand == NULL) {
        free(group);
        task_abort_create(leader);
        return NULL;
    }
    leader->blocked =
            sig_mask(SIGUSR1_) | sig_mask(SIGUSR2_);

    if (!task_set_host_job_id(
                leader, host_job_id)) {
        sighand_release(leader->sighand);
        free(group);
        task_abort_create(leader);
        return NULL;
    }
    if (!task_set_host_diagnostic_context(
                leader,
                TASK_HOST_DIAGNOSTIC_COMMAND,
                host_job_id)) {
        sighand_release(leader->sighand);
        free(group);
        task_abort_create(leader);
        return NULL;
    }
    task_publish(leader);
    *group_out = group;
    return leader;
}

static struct task *make_forked_process(
        struct task *parent, struct tgroup **group_out) {
    struct task *child = task_create_(parent);
    if (child == NULL)
        return NULL;

    struct tgroup *group = malloc(sizeof(*group));
    if (group == NULL) {
        task_abort_create(child);
        return NULL;
    }

    lock(&pids_lock);
    lock(&parent->sighand->lock);
    lock(&parent->group->lock);
    tgroup_init_copy(group, parent->group);
    unlock(&parent->group->lock);
    unlock(&parent->sighand->lock);
    unlock(&pids_lock);

    child->group = group;
    group->leader = child;
    child->tgid = child->pid;
    child->exit_signal = SIGCHLD_;
    task_publish(child);
    *group_out = group;
    return child;
}

static struct task *make_group_thread(struct task *leader) {
    struct task *thread = task_create_(leader);
    if (thread == NULL)
        return NULL;
    thread->parent = leader->parent;
    thread->group = leader->group;
    thread->tgid = leader->tgid;
    thread->exit_signal = 0;
    if (task_set_host_job_id(
                thread, UINT64_C(0xeeee)))
        return NULL;
    task_publish(thread);
    return thread;
}

static bool group_has_pending(
        struct task *task, sigset_t_ signals) {
    lock(&task->sighand->lock);
    bool matches =
            task->group->shared_pending == signals;
    unlock(&task->sighand->lock);
    return matches;
}

int main(void) {
    const uint64_t command_job = UINT64_C(0x123456789abcdef0);
    const uint64_t independent_job =
            UINT64_C(0x0fedcba987654321);

    struct tgroup *root_group;
    struct task *root =
            make_initial_process(command_job, &root_group);
    CHECK(root != NULL &&
            root_group->host_job_id == command_job &&
            root_group->host_diagnostic_scope ==
                    TASK_HOST_DIAGNOSTIC_COMMAND &&
            root_group->host_diagnostic_request_id == command_job,
            "初始进程在发布前接受作业与诊断身份");
    CHECK(!task_set_host_job_id(
                    root, independent_job) &&
            root_group->host_job_id == command_job,
            "发布后的线程组身份保持不可变");
    CHECK(!task_set_host_diagnostic_context(
                    root,
                    TASK_HOST_DIAGNOSTIC_TERMINAL,
                    independent_job) &&
            root_group->host_diagnostic_scope ==
                    TASK_HOST_DIAGNOSTIC_COMMAND &&
            root_group->host_diagnostic_request_id == command_job,
            "发布后的诊断归属保持不可变");
    current = root;

    struct tgroup *pgroup_child_group;
    struct task *pgroup_child =
            make_forked_process(root, &pgroup_child_group);
    CHECK(pgroup_child != NULL &&
            pgroup_child_group->host_job_id == command_job &&
            pgroup_child_group->host_diagnostic_scope ==
                    TASK_HOST_DIAGNOSTIC_COMMAND &&
            pgroup_child_group->host_diagnostic_request_id ==
                    command_job,
            "fork 建立的新线程组继承作业与诊断身份");

    struct task *thread = make_group_thread(pgroup_child);
    CHECK(thread != NULL &&
            thread->group == pgroup_child_group &&
            thread->group->host_job_id == command_job &&
            thread->group->host_diagnostic_request_id ==
                    command_job,
            "CLONE_THREAD 语义共享同一个作业与诊断身份");
    CHECK(sys_setpgid(
                    pgroup_child->pid, pgroup_child->pid) == 0 &&
            pgroup_child_group->pgid == pgroup_child->pid &&
            pgroup_child_group->host_job_id == command_job,
            "改变 guest 进程组不能逃离宿主作业");

    struct tgroup *session_child_group;
    struct task *session_child =
            make_forked_process(root, &session_child_group);
    CHECK(session_child != NULL &&
            task_setsid(session_child) == session_child->pid &&
            session_child_group->sid == session_child->pid &&
            session_child_group->host_job_id == command_job,
            "创建 guest 会话不能逃离宿主作业");

    struct tgroup *independent_group;
    struct task *independent = make_initial_process(
            independent_job, &independent_group);
    CHECK(independent != NULL &&
            independent_group->host_job_id == independent_job,
            "独立初始进程可以属于另一个宿主作业");

    CHECK(task_signal_host_job(command_job, SIGUSR1_) == 3,
            "无锁包装恰好命中作业内三个线程组");
    CHECK(group_has_pending(root, sig_mask(SIGUSR1_)) &&
            group_has_pending(
                    pgroup_child, sig_mask(SIGUSR1_)) &&
            group_has_pending(
                    session_child, sig_mask(SIGUSR1_)) &&
            group_has_pending(independent, 0),
            "信号覆盖 setsid/setpgid 后代且隔离其他作业");

    lock(&pids_lock);
    size_t locked_count = task_signal_host_job_locked(
            command_job, SIGUSR2_);
    unlock(&pids_lock);
    CHECK(locked_count == 3 &&
            group_has_pending(root,
                    sig_mask(SIGUSR1_) | sig_mask(SIGUSR2_)) &&
            group_has_pending(pgroup_child,
                    sig_mask(SIGUSR1_) | sig_mask(SIGUSR2_)) &&
            group_has_pending(session_child,
                    sig_mask(SIGUSR1_) | sig_mask(SIGUSR2_)) &&
            group_has_pending(independent, 0),
            "持锁入口不重复计算同组线程并保持作业隔离");
    CHECK(task_signal_host_job(0, SIGUSR1_) == 0,
            "零身份不匹配任何线程组");

    current = NULL;
    return 0;
}
