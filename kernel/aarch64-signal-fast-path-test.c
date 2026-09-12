#include <assert.h>
#include <sched.h>
#include <time.h>

#include "kernel/aarch64-signal-service.h"
#include "kernel/signal.h"
#include "kernel/task.h"

struct fixture {
    struct task task;
    struct tgroup group;
    struct sighand sighand;
    unsigned delivered;
};

static void init_fixture(struct fixture *fixture) {
    *fixture = (struct fixture) {0};
    fixture->task.group = &fixture->group;
    fixture->task.sighand = &fixture->sighand;
    fixture->task.pid = fixture->task.tgid = 1234;
    fixture->group.leader = &fixture->task;
    atomic_init(&fixture->task.signal_poll_needed, true);
    atomic_init(&fixture->group.signal_poll_state, 0);
    atomic_init(&fixture->group.external_fatal_signal, 0);
    atomic_init(&fixture->sighand.refcount, 1);
    task_thread_store(&fixture->task, pthread_self());
    list_init(&fixture->task.queue);
    list_init(&fixture->group.shared_queue);
    list_init(&fixture->group.threads);
    list_add_tail(&fixture->group.threads, &fixture->task.group_links);
    lock_init(&fixture->group.lock);
    lock_init(&fixture->sighand.lock);
    lock_init(&fixture->task.ptrace.lock);
    lock_init(&fixture->task.waiting_cond_lock);
    cond_init(&fixture->task.ptrace.cond);
    cond_init(&fixture->group.stopped_cond);
    fixture->sighand.action[SIGUSR1_] = (struct signal_action) {
        .handler = UINT64_C(0x4000), .flags = SA_NODEFER_,
    };
    fixture->sighand.action[SIGRTMIN_] = (struct signal_action) {
        .handler = UINT64_C(0x4000), .flags = SA_NODEFER_,
    };
    current = &fixture->task;
}

static void destroy_fixture(struct fixture *fixture) {
    signal_flush_pending(&fixture->task);
    signal_flush_group_pending(&fixture->group);
    cond_destroy(&fixture->task.ptrace.cond);
    cond_destroy(&fixture->group.stopped_cond);
    pthread_mutex_destroy(&fixture->group.lock.m);
    pthread_mutex_destroy(&fixture->sighand.lock.m);
    pthread_mutex_destroy(&fixture->task.ptrace.lock.m);
    pthread_mutex_destroy(&fixture->task.waiting_cond_lock.m);
    current = NULL;
}

static bool pending(struct task *task) {
    const struct guest_linux_signal_context context = {.task_opaque = task};
    return ish_aarch64_linux_signal_service.may_have_pending(&context);
}

static enum guest_linux_signal_install_status installed(
        void *opaque, const struct guest_linux_signal_delivery *delivery) {
    struct fixture *fixture = opaque;
    assert(delivery->info.signal == SIGUSR1_ || delivery->info.signal == SIGRTMIN_);
    fixture->delivered++;
    return GUEST_LINUX_SIGNAL_INSTALL_COMPLETE;
}

static struct guest_linux_signal_poll_result poll_signal(struct fixture *fixture) {
    const struct guest_linux_signal_context context = {.task_opaque = &fixture->task};
    return ish_aarch64_linux_signal_service.poll(&context, installed, fixture);
}

static void enqueue(struct fixture *fixture, int signal, bool shared) {
    lock(&fixture->sighand.lock);
    int status = shared ? signal_enqueue_process_locked(
            &fixture->task, signal, SIGINFO_NIL, SIGNAL_QUEUE_FORCE, 0, RLIM_INFINITY_) :
            signal_enqueue_locked(
                    &fixture->task, signal, SIGINFO_NIL, SIGNAL_QUEUE_FORCE, 0, RLIM_INFINITY_);
    assert(status >= 0);
    unlock(&fixture->sighand.lock);
}

static void test_idle_masks_and_shared_pending(void) {
    struct fixture fixture;
    init_fixture(&fixture);
    assert(pending(&fixture.task));
    assert(poll_signal(&fixture).status == GUEST_LINUX_SIGNAL_POLL_IDLE);
    // 已持锁时仍可查询，保证空闲路径不会再获取这两把锁。
    lock(&fixture.sighand.lock);
    lock(&fixture.group.lock);
    assert(!pending(&fixture.task));
    unlock(&fixture.group.lock);
    unlock(&fixture.sighand.lock);

    enqueue(&fixture, SIGUSR1_, false);
    assert(pending(&fixture.task));
    assert(poll_signal(&fixture).status == GUEST_LINUX_SIGNAL_POLL_HANDLER);
    assert(fixture.delivered == 1 && !pending(&fixture.task));

    sigset_t_ blocked = sig_mask(SIGUSR1_);
    assert(task_sigprocmask(&fixture.task, SIG_BLOCK_, &blocked, NULL) == 0);
    enqueue(&fixture, SIGUSR1_, true);
    struct task peer = {.group = &fixture.group};
    atomic_init(&peer.signal_poll_needed, false);
    assert(pending(&peer));
    assert(poll_signal(&fixture).status == GUEST_LINUX_SIGNAL_POLL_IDLE);
    assert(!pending(&fixture.task));
    assert(pending(&peer));
    assert(task_sigprocmask(&fixture.task, SIG_UNBLOCK_, &blocked, NULL) == 0);
    assert(pending(&fixture.task));
    assert(poll_signal(&fixture).status == GUEST_LINUX_SIGNAL_POLL_HANDLER);
    assert(fixture.delivered == 2 && !pending(&fixture.task));
    // 其他成员仍须确认它未观察过的世代，即使信号已被本线程领取。
    assert(pending(&peer));

    sigmask_set_temp_task(&fixture.task, blocked);
    assert(pending(&fixture.task));
    assert(poll_signal(&fixture).status == GUEST_LINUX_SIGNAL_POLL_IDLE);
    assert(!fixture.task.has_saved_mask && fixture.task.blocked == 0);
    assert(!pending(&fixture.task));

    // 默认致死信号须使同组其他线程也离开空闲快速路径。
    peer.observed_signal_poll_state = atomic_load(&fixture.group.signal_poll_state);
    assert(!pending(&peer));
    send_signal(&fixture.task, SIGTERM_, SIGINFO_NIL);
    assert(pending(&peer));
    assert(poll_signal(&fixture).status == GUEST_LINUX_SIGNAL_POLL_TERMINATE);
    assert(pending(&peer));
    destroy_fixture(&fixture);
}

#define CONCURRENT_SIGNALS 512

struct producer {
    struct fixture *fixture;
    bool shared;
    atomic_bool done;
};

static void *produce_signals(void *opaque) {
    struct producer *producer = opaque;
    for (unsigned index = 0; index < CONCURRENT_SIGNALS; index++) {
        enqueue(producer->fixture, SIGRTMIN_, producer->shared);
        sched_yield();
    }
    atomic_store(&producer->done, true);
    return NULL;
}

static uint64_t monotonic_seconds(void) {
    struct timespec now;
    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return (uint64_t) now.tv_sec;
}

static void test_concurrent_publication(bool shared) {
    struct fixture fixture;
    init_fixture(&fixture);
    assert(poll_signal(&fixture).status == GUEST_LINUX_SIGNAL_POLL_IDLE);
    struct producer producer = {.fixture = &fixture, .shared = shared};
    atomic_init(&producer.done, false);
    pthread_t thread;
    assert(pthread_create(&thread, NULL, produce_signals, &producer) == 0);
    uint64_t deadline = monotonic_seconds() + 10;
    // 实时信号逐个计数；若并发清除提示丢失了置位，测试会超时失败。
    while (fixture.delivered != CONCURRENT_SIGNALS) {
        assert(monotonic_seconds() < deadline);
        if (pending(&fixture.task))
            assert(poll_signal(&fixture).status == GUEST_LINUX_SIGNAL_POLL_HANDLER);
        else
            sched_yield();
    }
    assert(pthread_join(thread, NULL) == 0);
    assert(!pending(&fixture.task));
    destroy_fixture(&fixture);
}

static void test_blocked_shared_peers(void) {
    struct fixture first, peer;
    init_fixture(&first);
    init_fixture(&peer);
    peer.task.group = &first.group;
    peer.task.sighand = &first.sighand;
    sigset_t_ blocked = sig_mask(SIGUSR1_);
    assert(task_sigprocmask(&first.task, SIG_BLOCK_, &blocked, NULL) == 0);
    assert(task_sigprocmask(&peer.task, SIG_BLOCK_, &blocked, NULL) == 0);
    enqueue(&first, SIGUSR1_, true);
    current = &first.task;
    assert(poll_signal(&first).status == GUEST_LINUX_SIGNAL_POLL_IDLE);
    assert(!pending(&first.task) && pending(&peer.task));
    current = &peer.task;
    assert(poll_signal(&peer).status == GUEST_LINUX_SIGNAL_POLL_IDLE);
    for (unsigned index = 0; index < 10000; index++)
        assert(!pending(&first.task) && !pending(&peer.task));
    assert(task_sigprocmask(&peer.task, SIG_UNBLOCK_, &blocked, NULL) == 0);
    assert(pending(&peer.task));
    assert(poll_signal(&peer).status == GUEST_LINUX_SIGNAL_POLL_HANDLER);
    assert(peer.delivered == 1 && !pending(&first.task) && !pending(&peer.task));

    // 已确认旧世代的线程仍能看见后续共享事件。
    enqueue(&first, SIGUSR1_, true);
    assert(pending(&first.task) && pending(&peer.task));
    current = &first.task;
    assert(poll_signal(&first).status == GUEST_LINUX_SIGNAL_POLL_IDLE);
    current = &peer.task;
    assert(poll_signal(&peer).status == GUEST_LINUX_SIGNAL_POLL_HANDLER);
    assert(peer.delivered == 2);
    peer.task.group = &peer.group;
    peer.task.sighand = &peer.sighand;
    destroy_fixture(&peer);
    destroy_fixture(&first);
}

static void test_concurrent_blocked_publication(bool shared) {
    struct fixture fixture;
    init_fixture(&fixture);
    sigset_t_ blocked = sig_mask(SIGRTMIN_);
    assert(task_sigprocmask(&fixture.task, SIG_BLOCK_, &blocked, NULL) == 0);
    struct producer producer = {.fixture = &fixture, .shared = shared};
    atomic_init(&producer.done, false);
    pthread_t thread;
    assert(pthread_create(&thread, NULL, produce_signals, &producer) == 0);
    uint64_t deadline = monotonic_seconds() + 10;
    while (!atomic_load(&producer.done)) {
        assert(monotonic_seconds() < deadline);
        if (pending(&fixture.task))
            assert(poll_signal(&fixture).status == GUEST_LINUX_SIGNAL_POLL_IDLE);
        sched_yield();
    }
    assert(pthread_join(thread, NULL) == 0);
    assert(poll_signal(&fixture).status == GUEST_LINUX_SIGNAL_POLL_IDLE);
    assert(!pending(&fixture.task) && fixture.delivered == 0);
    assert(task_sigprocmask(&fixture.task, SIG_UNBLOCK_, &blocked, NULL) == 0);
    for (unsigned index = 0; index < CONCURRENT_SIGNALS; index++) {
        assert(pending(&fixture.task));
        assert(poll_signal(&fixture).status == GUEST_LINUX_SIGNAL_POLL_HANDLER);
    }
    assert(fixture.delivered == CONCURRENT_SIGNALS && !pending(&fixture.task));
    destroy_fixture(&fixture);
}

int main(void) {
    test_idle_masks_and_shared_pending();
    test_concurrent_publication(false);
    test_concurrent_publication(true);
    test_blocked_shared_peers();
    test_concurrent_blocked_publication(false);
    test_concurrent_blocked_publication(true);
    return 0;
}
