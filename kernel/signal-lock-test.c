#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "kernel/signal.h"
#include "kernel/task.h"

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "信号锁回归测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        return 1; \
    } \
} while (0)

struct signal_fixture {
    struct task child;
    struct task parent;
    struct tgroup child_group;
    struct tgroup parent_group;
    struct sighand child_sighand;
    struct sighand parent_sighand;
};

static void init_group(struct tgroup *group, struct task *leader) {
    list_init(&group->threads);
    list_init(&group->session);
    list_init(&group->pgroup);
    lock_init(&group->lock);
    cond_init(&group->child_exit);
    cond_init(&group->stopped_cond);
    group->leader = leader;
}

static void init_task_links(struct task *task) {
    list_init(&task->group_links);
    list_init(&task->children);
    list_init(&task->siblings);
    list_init(&task->queue);
}

static void init_fixture(struct signal_fixture *fixture,
        bool share_sighand, int exit_signal) {
    *fixture = (struct signal_fixture) {0};
    lock_init(&fixture->child_sighand.lock);
    lock_init(&fixture->parent_sighand.lock);
    init_group(&fixture->child_group, &fixture->child);
    init_group(&fixture->parent_group, &fixture->parent);
    init_task_links(&fixture->child);
    init_task_links(&fixture->parent);

    fixture->child.group = &fixture->child_group;
    fixture->child.sighand = &fixture->child_sighand;
    fixture->child.parent = &fixture->parent;
    fixture->child.pid = fixture->child.tgid = 200;
    fixture->child.exit_signal = exit_signal;
    lock_init(&fixture->child.general_lock);
    lock_init(&fixture->child.waiting_cond_lock);
    cond_init(&fixture->child.pause);
    lock_init(&fixture->child.ptrace.lock);
    cond_init(&fixture->child.ptrace.cond);
    list_add(&fixture->child_group.threads,
            &fixture->child.group_links);

    fixture->parent.group = &fixture->parent_group;
    fixture->parent.sighand = share_sighand ?
            &fixture->child_sighand : &fixture->parent_sighand;
    fixture->parent.pid = fixture->parent.tgid = 100;
    list_add(&fixture->parent_group.threads,
            &fixture->parent.group_links);
    current = &fixture->child;
}

struct stop_controller {
    struct task *task;
    int ignored_signal;
    atomic_int action_result;
};

static void *continue_traced_task(void *opaque) {
    struct stop_controller *controller = opaque;
    while (true) {
        lock(&controller->task->ptrace.lock);
        bool stopped = controller->task->ptrace.stopped;
        unlock(&controller->task->ptrace.lock);
        if (stopped)
            break;
        sched_yield();
    }

    // traced 停顿建立后再改动作，稳定命中 receive_signals 暂时释放 sighand 的窗口。
    if (controller->ignored_signal != 0) {
        const struct signal_action ignore = {.handler = SIG_IGN_};
        int result = task_sigaction(controller->task,
                controller->ignored_signal, &ignore, NULL);
        atomic_store(&controller->action_result, result);
    }

    lock(&controller->task->ptrace.lock);
    controller->task->ptrace.stopped = false;
    notify(&controller->task->ptrace.cond);
    unlock(&controller->task->ptrace.lock);
    return NULL;
}

static int test_shared_sighand_parent_notification(void) {
    struct signal_fixture fixture;
    init_fixture(&fixture, true, SIGCHLD_);
    fixture.child.ptrace.traced = true;
    deliver_signal(&fixture.child, SIGUSR1_,
            (struct siginfo_) {.code = SI_USER_});

    struct stop_controller controller = {.task = &fixture.child};
    atomic_init(&controller.action_result, 0);
    pthread_t controller_thread;
    CHECK(pthread_create(&controller_thread, NULL,
                    continue_traced_task, &controller) == 0,
            "创建 traced 子任务恢复线程");

    receive_signals();
    CHECK(pthread_join(controller_thread, NULL) == 0,
            "回收 traced 子任务恢复线程");
    CHECK(fixture.child.pending == 0 &&
            list_empty(&fixture.child.queue) &&
            !fixture.child.ptrace.stopped &&
            fixture.parent.pending == 0 &&
            list_empty(&fixture.parent.queue),
            "共享 sighand 的父通知不递归加锁且保持队列一致");
    return 0;
}

static int test_concurrent_ignore_removes_successor(void) {
    struct signal_fixture fixture;
    init_fixture(&fixture, false, SIGCHLD_);
    fixture.child.ptrace.traced = true;
    deliver_signal(&fixture.child, SIGUSR1_,
            (struct siginfo_) {.code = SI_USER_});
    deliver_signal(&fixture.child, SIGUSR2_,
            (struct siginfo_) {.code = SI_USER_});

    struct stop_controller controller = {
        .task = &fixture.child,
        .ignored_signal = SIGUSR2_,
    };
    atomic_init(&controller.action_result, INT_MIN);
    pthread_t controller_thread;
    CHECK(pthread_create(&controller_thread, NULL,
                    continue_traced_task, &controller) == 0,
            "创建并发 SIG_IGN 控制线程");

    receive_signals();
    CHECK(pthread_join(controller_thread, NULL) == 0,
            "回收并发 SIG_IGN 控制线程");
    CHECK(atomic_load(&controller.action_result) == 0 &&
            fixture.child_sighand.action[SIGUSR2_].handler == SIG_IGN_ &&
            fixture.child.pending == 0 &&
            list_empty(&fixture.child.queue) &&
            !fixture.child.ptrace.stopped,
            "删除后继信号节点后遍历不访问释放内存且状态一致");
    return 0;
}

static int test_default_stop_action(void) {
    struct signal_fixture fixture;
    init_fixture(&fixture, false, 0);
    deliver_signal(&fixture.child, SIGTSTP_,
            (struct siginfo_) {.code = SI_USER_});

    receive_signals();
    CHECK(fixture.child_group.stopped &&
            fixture.child_group.stop_code ==
                    (dword_t) (SIGTSTP_ << 8 | 0x7f) &&
            fixture.child.pending == 0 &&
            list_empty(&fixture.child.queue),
            "普通 SIGTSTP 设置线程组停止状态与 wait 状态码");

    fixture.child_group.stopped = false;
    fixture.child_group.stop_code = 0;
    fixture.child.parent = NULL;
    deliver_signal(&fixture.child, SIGTTIN_,
            (struct siginfo_) {.code = SI_USER_});
    receive_signals();
    CHECK(fixture.child_group.stopped &&
            fixture.child_group.stop_code ==
                    (dword_t) (SIGTTIN_ << 8 | 0x7f) &&
            fixture.child.pending == 0 &&
            list_empty(&fixture.child.queue),
            "无父任务仍可完成默认停止而不解引用空指针");
    return 0;
}

static int test_sigkill_releases_ptrace_stop(void) {
    struct signal_fixture fixture;
    init_fixture(&fixture, false, 0);

    fixture.child.ptrace.stopped = true;
    deliver_signal(&fixture.child, SIGKILL_, SIGINFO_NIL);
    CHECK(!fixture.child.ptrace.stopped &&
            sigset_has(fixture.child.pending, SIGKILL_) &&
            list_size(&fixture.child.queue) == 1,
            "首次 SIGKILL 解除 ptrace 停顿并进入队列");

    fixture.child.ptrace.stopped = true;
    send_signal(&fixture.child, SIGKILL_, SIGINFO_NIL);
    CHECK(!fixture.child.ptrace.stopped &&
            sigset_has(fixture.child.pending, SIGKILL_) &&
            list_size(&fixture.child.queue) == 1,
            "重复 SIGKILL 即使合并也必须再次解除 ptrace 停顿");
    return 0;
}

#define REALTIME_SENDERS 4
#define REALTIME_SIGNALS_PER_SENDER 64

struct realtime_sender {
    struct task *task;
    int sender;
};

static void *send_realtime_signals(void *opaque) {
    struct realtime_sender *sender = opaque;
    for (int sequence = 0;
            sequence < REALTIME_SIGNALS_PER_SENDER; sequence++) {
        deliver_signal(sender->task, SIGRTMIN_, (struct siginfo_) {
            .code = SI_USER_,
            .payload_kind = SIGNAL_INFO_PAYLOAD_KILL,
            .kill = {.pid = sender->sender, .uid = (uid_t_) sequence},
        });
    }
    return NULL;
}

static int test_concurrent_realtime_queue(void) {
    struct signal_fixture fixture;
    init_fixture(&fixture, false, 0);
    fixture.child.blocked = sig_mask(SIGRTMIN_);

    pthread_t threads[REALTIME_SENDERS];
    struct realtime_sender senders[REALTIME_SENDERS];
    for (int index = 0; index < REALTIME_SENDERS; index++) {
        senders[index] = (struct realtime_sender) {
            .task = &fixture.child,
            .sender = index,
        };
        CHECK(pthread_create(&threads[index], NULL,
                        send_realtime_signals, &senders[index]) == 0,
                "创建并发实时信号发送线程");
    }
    for (int index = 0; index < REALTIME_SENDERS; index++) {
        CHECK(pthread_join(threads[index], NULL) == 0,
                "回收并发实时信号发送线程");
    }

    const unsigned long expected =
            REALTIME_SENDERS * REALTIME_SIGNALS_PER_SENDER;
    CHECK(fixture.child.pending == sig_mask(SIGRTMIN_) &&
            list_size(&fixture.child.queue) == expected,
            "并发发送的同号实时信号无折叠或丢失");

    bool pending_consistent = true;
    unsigned long remaining = expected;
    lock(&fixture.child.sighand->lock);
    while (!list_empty(&fixture.child.queue)) {
        struct sigqueue *queued = list_first_entry(
                &fixture.child.queue, struct sigqueue, queue);
        signal_dequeue_locked(&fixture.child, queued);
        remaining--;
        pending_consistent = pending_consistent &&
                sigset_has(fixture.child.pending, SIGRTMIN_) ==
                        (remaining != 0);
    }
    unlock(&fixture.child.sighand->lock);
    CHECK(pending_consistent && remaining == 0 && fixture.child.pending == 0,
            "并发队列逐项消费时 pending 位保持一致");
    return 0;
}

typedef int (*isolated_test_fn)(void);

// 锁回归可能死锁或触发 UAF；host 子进程与 alarm 将其转换成有界测试失败。
static bool run_isolated(const char *name, isolated_test_fn test) {
    pid_t child = fork();
    if (child < 0) {
        fprintf(stderr, "信号锁回归测试无法 fork：%s\n", strerror(errno));
        return false;
    }
    if (child == 0) {
        alarm(2);
        _exit(test() == 0 ? 0 : 1);
    }

    int status;
    pid_t waited;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited < 0) {
        fprintf(stderr, "信号锁回归测试等待 %s 失败：%s\n",
                name, strerror(errno));
        return false;
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        return true;
    if (WIFSIGNALED(status)) {
        fprintf(stderr, "信号锁回归测试失败：%s 被 host 信号 %d 终止\n",
                name, WTERMSIG(status));
    } else {
        fprintf(stderr, "信号锁回归测试失败：%s 返回状态 %d\n",
                name, WEXITSTATUS(status));
    }
    return false;
}

int main(void) {
    unsigned failures = 0;
    failures += !run_isolated("共享 sighand 父通知",
            test_shared_sighand_parent_notification);
    failures += !run_isolated("并发 SIG_IGN 删除后继节点",
            test_concurrent_ignore_removes_successor);
    failures += !run_isolated("默认停止动作", test_default_stop_action);
    failures += !run_isolated("SIGKILL 解除 ptrace 停顿",
            test_sigkill_releases_ptrace_stop);
    failures += !run_isolated("并发实时信号排队",
            test_concurrent_realtime_queue);
    return failures == 0 ? 0 : 1;
}
