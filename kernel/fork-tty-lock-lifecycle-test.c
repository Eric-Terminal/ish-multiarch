#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "debug.h"
#include "fs/fd.h"
#include "fs/tty.h"
#include "guest/aarch64/linux-process.h"
#include "kernel/calls.h"
#include "kernel/fs.h"
#include "kernel/mm.h"
#include "kernel/ptrace.h"
#include "kernel/task.h"

struct copy_gate {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    unsigned cond_init_calls;
    bool snapshot_entered;
    bool allow_snapshot;
};

static struct copy_gate *active_copy_gate;
static void fork_tty_lock_cond_init(cond_t *cond);

// 直接编译被测实现，并只重命名公开入口，测试才能在不增加生产 hook 的情况下
// 把新 tgroup 初始化精确停在旧 group 锁内。
#define cond_init fork_tty_lock_cond_init
#define sys_clone fork_tty_lock_sys_clone
#define sys_clone_aarch64 fork_tty_lock_sys_clone_aarch64
#define sys_fork fork_tty_lock_sys_fork
#define sys_vfork fork_tty_lock_sys_vfork
#define vfork_notify fork_tty_lock_vfork_notify
#include "kernel/fork.c"
#undef vfork_notify
#undef sys_vfork
#undef sys_fork
#undef sys_clone_aarch64
#undef sys_clone
#undef cond_init

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "fork/TTY 锁序测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        return 1; \
    } \
} while (0)

static void fork_tty_lock_cond_init(cond_t *cond) {
    cond_init(cond);
    struct copy_gate *gate = active_copy_gate;
    if (gate == NULL)
        return;

    pthread_mutex_lock(&gate->mutex);
    gate->cond_init_calls++;
    if (gate->cond_init_calls == 1) {
        gate->snapshot_entered = true;
        pthread_cond_broadcast(&gate->changed);
        while (!gate->allow_snapshot)
            pthread_cond_wait(&gate->changed, &gate->mutex);
    }
    pthread_mutex_unlock(&gate->mutex);
}

static const struct fd_ops metadata_fd_ops = {0};
static const struct tty_driver_ops test_tty_ops = {0};
static struct tty_driver test_tty_driver = {
    .ops = &test_tty_ops,
};

struct fixture {
    struct tgroup group;
    struct task *parent;
    struct task *child;
    struct tty *tty;
    struct fd tty_fd;
};

static bool fixture_init(struct fixture *fixture) {
    *fixture = (struct fixture) {0};
    struct task *parent = task_create_(NULL);
    if (parent == NULL)
        return false;

    struct tgroup *group = &fixture->group;
    list_init(&group->threads);
    list_init(&group->session);
    list_init(&group->pgroup);
    lock_init(&group->lock);
    cond_init(&group->child_exit);
    cond_init(&group->stopped_cond);
    atomic_init(&group->external_fatal_signal, 0);
    group->leader = parent;
    group->sid = parent->pid;
    group->pgid = parent->pid;
    group->limits[RLIMIT_NOFILE_] = (struct rlimit_) {16, 16};
    parent->group = group;
    parent->tgid = parent->pid;

    struct mm *mm = mm_new();
    struct fdtable *files = fdtable_new(1);
    struct fs_info *fs = fs_info_new();
    struct sighand *sighand = sighand_new();
    struct fd *metadata = fd_create(&metadata_fd_ops);
    if (mm == NULL || IS_ERR(files) || fs == NULL ||
            sighand == NULL || metadata == NULL)
        return false;
    mm->exefile = fd_retain(metadata);
    fs->root = fd_retain(metadata);
    fs->pwd = fd_retain(metadata);
    fd_close(metadata);
    task_set_mm(parent, mm);
    parent->files = files;
    parent->fs = fs;
    parent->sighand = sighand;
    task_thread_store(parent, pthread_self());
    task_publish(parent);

    struct tty *tty = tty_alloc(&test_tty_driver, 240, parent->pid);
    if (tty == NULL)
        return false;
    tty->refcount = 2;
    tty->open_count = 1;
    tty->ever_opened = true;
    tty->session = parent->pid;
    tty->fg_group = parent->pid;
    group->tty = tty;

    struct task *child = task_create_(parent);
    if (child == NULL)
        return false;

    fixture->parent = parent;
    fixture->child = child;
    fixture->tty = tty;
    fixture->tty_fd.tty = tty;
    current = parent;
    return true;
}

struct copy_call {
    struct task *parent;
    struct task *child;
    int result;
};

static void *run_copy(void *opaque) {
    struct copy_call *call = opaque;
    current = call->parent;
    call->result = copy_task(
            call->child, SIGCHLD_, 0, 0, 0, 0, NULL);
    current = NULL;
    return NULL;
}

struct ioctl_call {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    struct task *task;
    struct fd *fd;
    int command;
    pid_t_ value;
    bool ready;
    bool start;
    bool attempting;
    int result;
};

static void *run_ioctl(void *opaque) {
    struct ioctl_call *call = opaque;
    current = call->task;
    pthread_mutex_lock(&call->mutex);
    call->ready = true;
    pthread_cond_broadcast(&call->changed);
    while (!call->start)
        pthread_cond_wait(&call->changed, &call->mutex);
    call->attempting = true;
    pthread_cond_broadcast(&call->changed);
    pthread_mutex_unlock(&call->mutex);

    call->result = tty_dev.fd.ioctl(
            call->fd, call->command, &call->value);
    current = NULL;
    return NULL;
}

static bool lock_owned_by_thread(lock_t *lock, pthread_t thread) {
    pthread_t owner = atomic_load_explicit(
            &lock->owner, memory_order_acquire);
    pthread_t empty = zero_init(pthread_t);
    return memcmp(&owner, &empty, sizeof(owner)) != 0 &&
            pthread_equal(owner, thread);
}

static int run_lock_order_scenario(int command) {
    struct fixture fixture;
    CHECK(fixture_init(&fixture), "初始化任务与 controlling tty 夹具");

    struct copy_gate copy_gate = {0};
    CHECK(pthread_mutex_init(&copy_gate.mutex, NULL) == 0 &&
            pthread_cond_init(&copy_gate.changed, NULL) == 0,
            "初始化 fork 快照闸门");
    active_copy_gate = &copy_gate;

    struct copy_call copy = {
        .parent = fixture.parent,
        .child = fixture.child,
        .result = -1,
    };
    pthread_t copy_thread;
    CHECK(pthread_create(&copy_thread, NULL, run_copy, &copy) == 0,
            "建立 fork 复制线程");

    pthread_mutex_lock(&copy_gate.mutex);
    while (!copy_gate.snapshot_entered)
        pthread_cond_wait(&copy_gate.changed, &copy_gate.mutex);
    pthread_mutex_unlock(&copy_gate.mutex);
    CHECK(lock_owned_by_thread(&fixture.group.lock, copy_thread),
            "fork 快照闸门位于旧 group 锁内");

    struct ioctl_call ioctl = {
        .task = fixture.parent,
        .fd = &fixture.tty_fd,
        .command = command,
        .value = command == TIOCSPGRP_ ?
                fixture.parent->pid + 1000 : 0,
        .result = -1,
    };
    CHECK(pthread_mutex_init(&ioctl.mutex, NULL) == 0 &&
            pthread_cond_init(&ioctl.changed, NULL) == 0,
            "初始化 ioctl 启动闸门");
    pthread_t ioctl_thread;
    CHECK(pthread_create(&ioctl_thread, NULL, run_ioctl, &ioctl) == 0,
            "建立并发 TTY ioctl 线程");
    pthread_mutex_lock(&ioctl.mutex);
    while (!ioctl.ready)
        pthread_cond_wait(&ioctl.changed, &ioctl.mutex);
    ioctl.start = true;
    pthread_cond_broadcast(&ioctl.changed);
    while (!ioctl.attempting)
        pthread_cond_wait(&ioctl.changed, &ioctl.mutex);
    pthread_mutex_unlock(&ioctl.mutex);

    if (command == TIOCGPGRP_) {
        while (!lock_owned_by_thread(&fixture.tty->lock, ioctl_thread))
            sched_yield();
    }

    pthread_mutex_lock(&copy_gate.mutex);
    copy_gate.allow_snapshot = true;
    pthread_cond_broadcast(&copy_gate.changed);
    pthread_mutex_unlock(&copy_gate.mutex);

    CHECK(pthread_join(ioctl_thread, NULL) == 0,
            "并发 TTY ioctl 在 fork 复制期间完成");
    CHECK(pthread_join(copy_thread, NULL) == 0,
            "fork 复制在 TTY ioctl 后完成");
    active_copy_gate = NULL;

    CHECK(copy.result == 0,
            "fork 复制 controlling tty 引用成功");
    CHECK(ioctl.result == 0,
            command == TIOCGPGRP_ ?
            "TIOCGPGRP 与 fork 并发完成" :
            "TIOCSPGRP 与 fork 并发完成");
    CHECK(fixture.child->group != &fixture.group &&
            fixture.child->group->tty == fixture.tty,
            "子进程组继承同一 controlling tty");
    lock(&fixture.tty->lock);
    unsigned tty_refs = fixture.tty->refcount;
    pid_t_ foreground_group = fixture.tty->fg_group;
    unlock(&fixture.tty->lock);
    CHECK(tty_refs == 3,
            "fork 恰好提升一份 controlling tty 引用");
    CHECK(command != TIOCGPGRP_ ||
            ioctl.value == fixture.parent->pid,
            "TIOCGPGRP 读取并发快照的前台组");
    CHECK(command != TIOCSPGRP_ ||
            foreground_group == ioctl.value,
            "TIOCSPGRP 提交并发更新的前台组");
    return 0;
}

static bool run_isolated_scenario(int command, const char *name) {
    pid_t host_child = fork();
    if (host_child < 0) {
        fprintf(stderr, "fork/TTY 锁序测试无法建立 %s 子进程：%s\n",
                name, strerror(errno));
        return false;
    }
    if (host_child == 0) {
        signal(SIGUSR1, SIG_IGN);
        alarm(10);
        _exit(run_lock_order_scenario(command));
    }

    int status;
    pid_t waited;
    do {
        waited = waitpid(host_child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited == host_child && WIFEXITED(status) &&
            WEXITSTATUS(status) == 0)
        return true;
    if (waited == host_child && WIFSIGNALED(status)) {
        fprintf(stderr, "fork/TTY 锁序测试的 %s 场景被 host 信号 %d 终止\n",
                name, WTERMSIG(status));
    } else {
        fprintf(stderr, "fork/TTY 锁序测试的 %s 场景返回状态 %d\n",
                name, waited == host_child && WIFEXITED(status) ?
                WEXITSTATUS(status) : -1);
    }
    return false;
}

int main(void) {
    if (!run_isolated_scenario(TIOCGPGRP_, "TIOCGPGRP") ||
            !run_isolated_scenario(TIOCSPGRP_, "TIOCSPGRP"))
        return 1;
    return 0;
}
