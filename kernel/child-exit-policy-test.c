#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "fs/fd.h"
#include "kernel/calls.h"
#include "kernel/fs.h"
#include "kernel/mm.h"
#include "kernel/signal.h"
#include "kernel/task.h"

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "子进程退出策略测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        return 1; \
    } \
} while (0)

struct child_exit_thread {
    struct task *task;
    int status;
};

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
    if (mm == NULL || IS_ERR(files) || fs == NULL || sighand == NULL)
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

static struct task *make_child(
        struct task *parent, int exit_signal,
        time_t_ accounted_seconds) {
    struct task *child = task_create_(parent);
    if (child == NULL)
        return NULL;
    struct tgroup *group = malloc(sizeof(*group));
    if (group == NULL) {
        task_abort_create(child);
        return NULL;
    }
    init_group(group, child,
            parent->group->sid, parent->group->pgid);
    group->rusage.utime.sec = accounted_seconds;
    child->group = group;
    child->tgid = child->pid;
    child->exit_signal = exit_signal;

    mm_retain(child->mm);
    child->files->refcount++;
    child->fs->refcount++;
    child->sighand->refcount++;
    task_publish(child);
    return child;
}

static struct task *make_group_thread(struct task *leader) {
    struct task *thread = task_create_(leader);
    if (thread == NULL)
        return NULL;
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

static void *exit_child(void *opaque) {
    struct child_exit_thread *thread = opaque;
    current = thread->task;
    task_thread_store(current, pthread_self());
    do_exit(thread->status);
}

static bool run_child_exit(
        struct task *child, int status) {
    struct child_exit_thread thread = {
        .task = child,
        .status = status,
    };
    pthread_t host_thread;
    if (pthread_create(&host_thread, NULL, exit_child, &thread) != 0)
        return false;
    return pthread_join(host_thread, NULL) == 0;
}

static bool child_is_fully_reaped(
        struct task *parent, pid_t_ pid) {
    lock(&pids_lock);
    bool reaped = pid_get_task_zombie(pid) == NULL &&
            list_empty(&parent->children);
    unlock(&pids_lock);
    return reaped;
}

static bool queued_signal_matches(
        struct task *task, int signal, pid_t_ child_pid, int status) {
    lock(&task->sighand->lock);
    bool matches = task->pending == 0 && list_empty(&task->queue) &&
            task->group->shared_pending == sig_mask(signal) &&
            list_size(&task->group->shared_queue) == 1;
    if (matches) {
        struct sigqueue *queued = list_first_entry(
                &task->group->shared_queue, struct sigqueue, queue);
        matches = queued->info.sig == signal &&
                queued->info.payload_kind ==
                        SIGNAL_INFO_PAYLOAD_CHILD &&
                queued->info.child.pid == child_pid &&
                queued->info.child.status == status;
    }
    unlock(&task->sighand->lock);
    return matches;
}

static bool no_pending_signals(struct task *task) {
    lock(&task->sighand->lock);
    bool empty = task->pending == 0 && list_empty(&task->queue) &&
            task->group->shared_pending == 0 &&
            list_empty(&task->group->shared_queue);
    unlock(&task->sighand->lock);
    return empty;
}

static void destroy_parent(
        struct task *parent, struct tgroup *group) {
    signal_flush_pending(parent);
    signal_flush_group_pending(group);
    mm_release(parent->mm);
    fdtable_release(parent->files);
    fs_info_release(parent->fs);
    sighand_release(parent->sighand);
    parent->mm = NULL;
    parent->files = NULL;
    parent->fs = NULL;
    parent->sighand = NULL;
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
}

static int run_exit_policy_scenario(void) {
    struct tgroup parent_group;
    struct task *parent = make_parent(&parent_group);
    CHECK(parent != NULL, "创建父任务");
    parent->blocked = sig_mask(SIGCHLD_) | sig_mask(SIGUSR1_);

    const struct signal_action no_wait_handler = {
        .handler = UINT64_C(0x1234),
        .flags = SA_NOCLDWAIT_,
    };
    CHECK(task_sigaction(parent, SIGCHLD_,
                    &no_wait_handler, NULL) == 0,
            "安装带 handler 的 SA_NOCLDWAIT");
    struct task *handler_child =
            make_child(parent, SIGCHLD_, 11);
    CHECK(handler_child != NULL, "创建 SA_NOCLDWAIT 子任务");
    pid_t_ handler_pid = handler_child->pid;
    int handler_status = 23 << 8;
    CHECK(run_child_exit(handler_child, handler_status),
            "等待 SA_NOCLDWAIT 子任务退出");
    struct wait4_result wait_result;
    CHECK(child_is_fully_reaped(parent, handler_pid) &&
            do_wait4(handler_pid, 0, &wait_result) == _ECHILD,
            "SA_NOCLDWAIT 自动完成 PID、父子链和组清理");
    CHECK(queued_signal_matches(parent, SIGCHLD_,
                    handler_pid, handler_status),
            "SA_NOCLDWAIT 的自定义 handler 仍收到 SIGCHLD");
    CHECK(parent_group.children_rusage.utime.sec == 0 &&
            parent_group.children_rusage.utime.usec == 0,
            "SA_NOCLDWAIT 自动回收不累计 RUSAGE_CHILDREN");

    const struct signal_action ignore = {.handler = SIG_IGN_};
    CHECK(task_sigaction(parent, SIGCHLD_, &ignore, NULL) == 0 &&
            no_pending_signals(parent),
            "显式忽略 SIGCHLD 并清除旧队列");
    struct task *ignored_child =
            make_child(parent, SIGCHLD_, 13);
    CHECK(ignored_child != NULL, "创建 SIG_IGN 子任务");
    pid_t_ ignored_pid = ignored_child->pid;
    CHECK(run_child_exit(ignored_child, 31 << 8),
            "等待 SIG_IGN 子任务退出");
    CHECK(child_is_fully_reaped(parent, ignored_pid) &&
            do_wait4(ignored_pid, 0, &wait_result) == _ECHILD &&
            no_pending_signals(parent),
            "显式 SIG_IGN 自动回收且不生成排队信号");
    CHECK(parent_group.children_rusage.utime.sec == 0 &&
            parent_group.children_rusage.utime.usec == 0,
            "SIG_IGN 自动回收不累计 RUSAGE_CHILDREN");

    struct task *other_signal_child =
            make_child(parent, SIGUSR1_, 17);
    CHECK(other_signal_child != NULL, "创建非 SIGCHLD 子任务");
    pid_t_ other_signal_pid = other_signal_child->pid;
    int other_status = 47 << 8;
    CHECK(run_child_exit(other_signal_child, other_status),
            "等待非 SIGCHLD 子任务退出");
    lock(&pids_lock);
    struct task *zombie = pid_get_task_zombie(other_signal_pid);
    bool retained = zombie != NULL && zombie->zombie &&
            !list_empty(&parent->children);
    unlock(&pids_lock);
    CHECK(retained && queued_signal_matches(parent, SIGUSR1_,
                    other_signal_pid, other_status),
            "SIGCHLD 的忽略策略不影响其他退出信号");
    CHECK(do_wait4(other_signal_pid, 0, &wait_result) ==
                    other_signal_pid &&
            wait_result.status == (dword_t) other_status &&
            child_is_fully_reaped(parent, other_signal_pid),
            "非 SIGCHLD 子任务仍由 wait 完整回收");
    CHECK(parent_group.children_rusage.utime.sec >= 17,
            "正常 wait 回收仍累计 RUSAGE_CHILDREN");

    const struct signal_action ignore_usr1 = {.handler = SIG_IGN_};
    CHECK(task_sigaction(parent, SIGUSR1_,
                    &ignore_usr1, NULL) == 0 &&
            no_pending_signals(parent),
            "清理非 SIGCHLD 测试信号");

    struct rusage_ accounted_before_threaded_auto_reap =
            parent_group.children_rusage;
    struct task *threaded_leader =
            make_child(parent, SIGCHLD_, 19);
    CHECK(threaded_leader != NULL, "创建多线程组 leader");
    struct task *last_thread = make_group_thread(threaded_leader);
    CHECK(last_thread != NULL, "创建多线程组末线程");
    pid_t_ threaded_leader_pid = threaded_leader->pid;
    pid_t_ last_thread_pid = last_thread->pid;
    CHECK(run_child_exit(threaded_leader, 53 << 8),
            "等待多线程组 leader 先退出");
    lock(&pids_lock);
    struct task *retained_leader =
            pid_get_task_zombie(threaded_leader_pid);
    bool group_retained = retained_leader != NULL &&
            !retained_leader->zombie &&
            pid_get_task(last_thread_pid) == last_thread;
    unlock(&pids_lock);
    CHECK(group_retained,
            "leader 先退出时保留线程组直到最后线程退出");
    CHECK(run_child_exit(last_thread, 59 << 8),
            "等待多线程组末线程退出");
    lock(&pids_lock);
    bool threaded_group_reaped =
            pid_get_task_zombie(threaded_leader_pid) == NULL &&
            pid_get_task_zombie(last_thread_pid) == NULL &&
            list_empty(&parent->children);
    unlock(&pids_lock);
    CHECK(threaded_group_reaped &&
            do_wait4(threaded_leader_pid, 0, &wait_result) == _ECHILD &&
            no_pending_signals(parent),
            "自动回收完整释放先退 leader 与最后线程");
    CHECK(memcmp(&parent_group.children_rusage,
                    &accounted_before_threaded_auto_reap,
                    sizeof(parent_group.children_rusage)) == 0,
            "多线程组自动回收不追加 RUSAGE_CHILDREN");

    destroy_parent(parent, &parent_group);
    return 0;
}

int main(void) {
    pid_t child = fork();
    if (child < 0) {
        fprintf(stderr, "子进程退出策略测试无法 fork：%s\n",
                strerror(errno));
        return 1;
    }
    if (child == 0) {
        signal(SIGUSR1, SIG_IGN);
        alarm(10);
        _exit(run_exit_policy_scenario());
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
        fprintf(stderr, "子进程退出策略测试被 host 信号 %d 终止\n",
                WTERMSIG(status));
    } else {
        fprintf(stderr, "子进程退出策略测试返回状态 %d\n",
                waited == child && WIFEXITED(status) ?
                        WEXITSTATUS(status) : -1);
    }
    return 1;
}
