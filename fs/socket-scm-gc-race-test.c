#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "fs/fd.h"
#include "fs/sock.h"
#include "kernel/calls.h"
#include "kernel/mm.h"
#include "kernel/resource.h"
#include "kernel/task.h"

#define USER_SOCKETPAIR UINT32_C(0x1000)

#ifndef __has_feature
#define __has_feature(feature) 0
#endif

#if defined(__SANITIZE_THREAD__) || __has_feature(thread_sanitizer)
#define RETAIN_ITERATIONS 1200
#define MESSAGE_COUNT 12
#define THREAD_WAIT_MS 15000
#else
#define RETAIN_ITERATIONS 12000
#define MESSAGE_COUNT 16
#define THREAD_WAIT_MS 5000
#endif

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "SCM GC 并发测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        passed = false; \
        goto cleanup; \
    } \
} while (0)

struct fixture {
    struct task task;
    struct tgroup group;
    fd_t sender_number;
    fd_t receiver_number;
    bool locks_initialized;
};

struct start_gate {
    pthread_mutex_t lock;
    pthread_cond_t condition;
    unsigned ready;
    unsigned expected;
    bool open;
};

struct retain_race {
    struct fixture *fixture;
    struct start_gate *gate;
    struct fd *receiver;
    uid_t_ uid;
    atomic_bool retainer_finished;
    atomic_bool collector_finished;
    atomic_bool pause_collector;
    atomic_bool collector_paused;
    atomic_bool root_released;
    atomic_bool collected;
    atomic_bool stop_collector;
    atomic_uint retain_failures;
    atomic_uint collector_passes;
};

struct blocked_collection {
    struct fixture *fixture;
    atomic_bool finished;
};

enum io_failure {
    IO_FAILURE_NONE,
    IO_FAILURE_PEEK,
    IO_FAILURE_PEEK_RIGHT,
    IO_FAILURE_RECEIVE,
    IO_FAILURE_RECEIVE_RIGHT,
    IO_FAILURE_CLOSE,
};

struct io_race {
    struct fixture *fixture;
    struct start_gate *gate;
    struct socket_ref receiver;
    atomic_bool first_peek_ready;
    atomic_bool guest_close_done;
    atomic_bool io_finished;
    atomic_bool closer_finished;
    atomic_bool collector_finished;
    atomic_bool stop_collector;
    atomic_uint collector_passes;
    atomic_uint consumed;
    atomic_int failure;
};

static bool wait_for_flag(atomic_bool *flag, unsigned timeout_ms) {
    const struct timespec interval = {.tv_nsec = 1000000};
    for (unsigned elapsed = 0; elapsed < timeout_ms; elapsed++) {
        if (atomic_load_explicit(flag, memory_order_acquire))
            return true;
        nanosleep(&interval, NULL);
    }
    return atomic_load_explicit(flag, memory_order_acquire);
}

static bool wait_for_uint_at_least(
        atomic_uint *value, unsigned minimum, unsigned timeout_ms) {
    const struct timespec interval = {.tv_nsec = 1000000};
    for (unsigned elapsed = 0; elapsed < timeout_ms; elapsed++) {
        if (atomic_load_explicit(value, memory_order_acquire) >= minimum)
            return true;
        nanosleep(&interval, NULL);
    }
    return atomic_load_explicit(value, memory_order_acquire) >= minimum;
}

static bool wait_for_blocked_refcount(
        struct fd *fd, unsigned references, unsigned timeout_ms) {
    const struct timespec interval = {.tv_nsec = 1000000};
    for (unsigned elapsed = 0; elapsed < timeout_ms; elapsed++) {
        unsigned state = atomic_load_explicit(
                &fd->refcount, memory_order_acquire);
        if ((state & FD_REFCOUNT_ACQUIRE_BLOCKED) != 0 &&
                (state & FD_REFCOUNT_VALUE_MASK) == references)
            return true;
        nanosleep(&interval, NULL);
    }
    unsigned state = atomic_load_explicit(
            &fd->refcount, memory_order_acquire);
    return (state & FD_REFCOUNT_ACQUIRE_BLOCKED) != 0 &&
            (state & FD_REFCOUNT_VALUE_MASK) == references;
}

static void fail_stuck_thread(const char *operation) {
    fprintf(stderr, "SCM GC 并发测试失败：%s线程未在超时内退出\n",
            operation);
    fflush(stderr);
    _Exit(1);
}

static bool start_gate_init(
        struct start_gate *gate, unsigned expected) {
    memset(gate, 0, sizeof(*gate));
    gate->expected = expected;
    if (pthread_mutex_init(&gate->lock, NULL) != 0)
        return false;
    if (pthread_cond_init(&gate->condition, NULL) != 0) {
        pthread_mutex_destroy(&gate->lock);
        return false;
    }
    return true;
}

static void start_gate_destroy(struct start_gate *gate) {
    pthread_cond_destroy(&gate->condition);
    pthread_mutex_destroy(&gate->lock);
}

static void start_gate_wait(struct start_gate *gate) {
    pthread_mutex_lock(&gate->lock);
    gate->ready++;
    pthread_cond_broadcast(&gate->condition);
    while (!gate->open)
        pthread_cond_wait(&gate->condition, &gate->lock);
    pthread_mutex_unlock(&gate->lock);
}

static bool start_gate_open(struct start_gate *gate) {
    const struct timespec interval = {.tv_nsec = 1000000};
    bool ready = false;
    for (unsigned elapsed = 0; elapsed < THREAD_WAIT_MS; elapsed++) {
        pthread_mutex_lock(&gate->lock);
        ready = gate->ready == gate->expected;
        pthread_mutex_unlock(&gate->lock);
        if (ready)
            break;
        nanosleep(&interval, NULL);
    }
    pthread_mutex_lock(&gate->lock);
    gate->open = true;
    pthread_cond_broadcast(&gate->condition);
    pthread_mutex_unlock(&gate->lock);
    return ready;
}

static void fixture_destroy(struct fixture *fixture) {
    current = &fixture->task;
    if (fixture->sender_number >= 0) {
        (void) f_close_task(&fixture->task, fixture->sender_number);
        fixture->sender_number = -1;
    }
    if (fixture->receiver_number >= 0) {
        (void) f_close_task(&fixture->task, fixture->receiver_number);
        fixture->receiver_number = -1;
    }
    if (fixture->task.files != NULL && !IS_ERR(fixture->task.files)) {
        fdtable_release(fixture->task.files);
        fixture->task.files = NULL;
    }
    socket_scm_collect_now();
    if (fixture->task.mm != NULL) {
        mm_release(fixture->task.mm);
        fixture->task.mm = NULL;
    }
    if (fixture->locks_initialized) {
        pthread_mutex_destroy(&fixture->task.waiting_cond_lock.m);
        pthread_mutex_destroy(&fixture->group.lock.m);
    }
    current = NULL;
}

static bool fixture_init(struct fixture *fixture,
        uid_t_ uid, dword_t socket_type) {
    memset(fixture, 0, sizeof(*fixture));
    fixture->sender_number = -1;
    fixture->receiver_number = -1;
    lock_init(&fixture->group.lock);
    lock_init(&fixture->task.waiting_cond_lock);
    fixture->locks_initialized = true;
    list_init(&fixture->group.threads);
    fixture->group.limits[RLIMIT_NOFILE_] =
            (struct rlimit_) {512, 512};
    fixture->task.group = &fixture->group;
    fixture->task.uid = uid;
    fixture->task.euid = uid;
    fixture->task.suid = uid;
    fixture->task.waiting_poll_notify_fd = -1;
    fixture->task.files = fdtable_new(32);
    if (IS_ERR(fixture->task.files)) {
        fprintf(stderr, "SCM GC 并发测试失败：创建 fd 表\n");
        return false;
    }

    struct mm *memory = mm_new();
    if (memory == NULL) {
        fprintf(stderr, "SCM GC 并发测试失败：创建用户地址空间\n");
        return false;
    }
    task_set_mm(&fixture->task, memory);
    write_wrlock(&fixture->task.mem->lock);
    int error = pt_map_nothing(
            fixture->task.mem, PAGE(USER_SOCKETPAIR), 1, P_RWX);
    write_wrunlock(&fixture->task.mem->lock);
    if (error < 0) {
        fprintf(stderr, "SCM GC 并发测试失败：映射 socketpair 写回页\n");
        return false;
    }

    current = &fixture->task;
    fd_t numbers[2];
    if (sys_socketpair(AF_LOCAL_, socket_type | SOCK_NONBLOCK_, 0,
                    USER_SOCKETPAIR) < 0 ||
            user_read(USER_SOCKETPAIR, numbers, sizeof(numbers)) != 0) {
        fprintf(stderr, "SCM GC 并发测试失败：创建 socketpair\n");
        return false;
    }
    fixture->sender_number = numbers[0];
    fixture->receiver_number = numbers[1];
    return true;
}

static bool send_self_right(struct fixture *fixture,
        const struct socket_ref *sender, byte_t payload) {
    struct scm *scm = NULL;
    int error = socket_scm_create_task(&fixture->task,
            &fixture->receiver_number, 1, &scm);
    if (error < 0)
        return false;
    ssize_t sent = socket_sendmsg_ref(sender,
            &payload, sizeof(payload),
            MSG_DONTWAIT_ | MSG_NOSIGNAL_, NULL, &scm);
    if (scm != NULL)
        socket_scm_release(scm);
    return sent == 1 && scm == NULL;
}

static bool enqueue_self_rights(
        struct fixture *fixture, unsigned count) {
    struct socket_ref sender = {0};
    if (socket_ref_get_task(&fixture->task,
                    fixture->sender_number, &sender) < 0)
        return false;
    bool sent = true;
    for (unsigned index = 0; index < count; index++) {
        if (!send_self_right(fixture, &sender, (byte_t) index)) {
            fprintf(stderr,
                    "SCM GC 并发测试失败：第 %u 条预置消息发送失败\n",
                    index + 1);
            sent = false;
            break;
        }
    }
    socket_ref_release(&sender);
    return sent;
}

static bool host_fd_is_open(int host_fd) {
    return host_fd >= 0 && fcntl(host_fd, F_GETFD) >= 0;
}

static bool host_fd_is_closed(int host_fd) {
    errno = 0;
    return host_fd >= 0 && fcntl(host_fd, F_GETFD) < 0 && errno == EBADF;
}

static void *retain_racer(void *opaque) {
    struct retain_race *race = opaque;
    current = &race->fixture->task;
    start_gate_wait(race->gate);
    for (unsigned iteration = 0;
            iteration < RETAIN_ITERATIONS; iteration++) {
        struct fd *retained = fd_try_retain(race->receiver);
        if (retained == NULL) {
            atomic_fetch_add_explicit(&race->retain_failures, 1,
                    memory_order_relaxed);
            break;
        }
        if ((iteration & 7) == 0)
            sched_yield();
        fd_close(retained);
    }
    atomic_store_explicit(
            &race->retainer_finished, true, memory_order_release);
    current = NULL;
    return NULL;
}

static void *retain_collector(void *opaque) {
    struct retain_race *race = opaque;
    current = &race->fixture->task;
    start_gate_wait(race->gate);
    while (!atomic_load_explicit(
            &race->stop_collector, memory_order_acquire)) {
        if (atomic_load_explicit(
                &race->pause_collector, memory_order_acquire)) {
            atomic_store_explicit(
                    &race->collector_paused, true, memory_order_release);
            while (atomic_load_explicit(
                            &race->pause_collector,
                            memory_order_acquire) &&
                    !atomic_load_explicit(
                            &race->stop_collector,
                            memory_order_acquire))
                sched_yield();
            atomic_store_explicit(
                    &race->collector_paused, false,
                    memory_order_release);
            continue;
        }
        socket_scm_collect_now();
        atomic_fetch_add_explicit(
                &race->collector_passes, 1, memory_order_release);
        if (atomic_load_explicit(
                    &race->root_released, memory_order_acquire) &&
                socket_scm_inflight_count(race->uid) == 0)
            atomic_store_explicit(
                    &race->collected, true, memory_order_release);
        sched_yield();
    }
    atomic_store_explicit(
            &race->collector_finished, true, memory_order_release);
    current = NULL;
    return NULL;
}

static void *blocked_collector(void *opaque) {
    struct blocked_collection *collection = opaque;
    current = &collection->fixture->task;
    socket_scm_collect_now();
    atomic_store_explicit(
            &collection->finished, true, memory_order_release);
    current = NULL;
    return NULL;
}

static bool test_try_retain_races_with_collection(void) {
    const uid_t_ uid = 6300;
    bool passed = true;
    struct fixture fixture;
    struct fd *receiver = NULL;
    struct start_gate gate;
    bool gate_initialized = false;
    pthread_t retainer_thread;
    pthread_t collector_thread;
    bool retainer_started = false;
    bool collector_started = false;
    struct retain_race race = {0};

    if (!fixture_init(&fixture, uid, SOCK_STREAM_)) {
        fixture_destroy(&fixture);
        return false;
    }
    receiver = f_get_task_retain(
            &fixture.task, fixture.receiver_number);
    CHECK(receiver != NULL, "取得外部接收端强引用");
    int receiver_host_fd = receiver->real_fd;
    CHECK(enqueue_self_rights(&fixture, 1), "建立弱获取竞态自环");
    CHECK(f_close_task(&fixture.task, fixture.sender_number) == 0,
            "关闭竞态自环发送端 guest fd");
    fixture.sender_number = -1;
    CHECK(f_close_task(&fixture.task, fixture.receiver_number) == 0,
            "关闭竞态自环接收端 guest fd");
    fixture.receiver_number = -1;
    CHECK(socket_scm_inflight_count(uid) == 1,
            "竞态开始前自环恰好持有一个 inflight 槽");

    CHECK(start_gate_init(&gate, 2), "初始化弱获取竞态闸门");
    gate_initialized = true;
    race.fixture = &fixture;
    race.gate = &gate;
    race.receiver = receiver;
    race.uid = uid;
    atomic_init(&race.retainer_finished, false);
    atomic_init(&race.collector_finished, false);
    atomic_init(&race.pause_collector, false);
    atomic_init(&race.collector_paused, false);
    atomic_init(&race.root_released, false);
    atomic_init(&race.collected, false);
    atomic_init(&race.stop_collector, false);
    atomic_init(&race.retain_failures, 0);
    atomic_init(&race.collector_passes, 0);

    retainer_started = pthread_create(
            &retainer_thread, NULL, retain_racer, &race) == 0;
    collector_started = retainer_started && pthread_create(
            &collector_thread, NULL, retain_collector, &race) == 0;
    CHECK(retainer_started && collector_started,
            "创建弱获取与 GC 竞态线程");
    CHECK(start_gate_open(&gate), "弱获取与 GC 线程同时就绪");
    if (!wait_for_flag(&race.retainer_finished, THREAD_WAIT_MS))
        fail_stuck_thread("fd_try_retain 竞态");
    pthread_join(retainer_thread, NULL);
    retainer_started = false;

    CHECK(wait_for_uint_at_least(
                    &race.collector_passes, 2, THREAD_WAIT_MS),
            "外部根存活期间 GC 至少完成两轮");
    atomic_store_explicit(
            &race.pause_collector, true, memory_order_release);
    CHECK(wait_for_flag(&race.collector_paused, THREAD_WAIT_MS),
            "暂停 GC 以核对外部根");
    CHECK(atomic_load_explicit(
                    &race.retain_failures, memory_order_acquire) == 0,
            "外部强引用存活时弱获取不会被错误拒绝");
    CHECK(socket_scm_inflight_count(uid) == 1 &&
                    host_fd_is_open(receiver_host_fd),
            "并发 GC 不会提前回收外部强引用可达的环");

    atomic_store_explicit(
            &race.pause_collector, false, memory_order_release);
    fd_close(receiver);
    receiver = NULL;
    atomic_store_explicit(
            &race.root_released, true, memory_order_release);
    CHECK(wait_for_flag(&race.collected, THREAD_WAIT_MS),
            "GC 与最后外部引用释放交错后回收自环");
    atomic_store_explicit(
            &race.stop_collector, true, memory_order_release);
    if (!wait_for_flag(&race.collector_finished, THREAD_WAIT_MS))
        fail_stuck_thread("弱获取竞态 GC");
    pthread_join(collector_thread, NULL);
    collector_started = false;
    CHECK(socket_scm_inflight_count(uid) == 0 &&
                    host_fd_is_closed(receiver_host_fd),
            "弱获取停止后最终环析构且 inflight 归零");

cleanup:
    if (retainer_started || collector_started) {
        atomic_store_explicit(
                &race.pause_collector, false, memory_order_release);
        atomic_store_explicit(
                &race.stop_collector, true, memory_order_release);
        if (gate_initialized) {
            pthread_mutex_lock(&gate.lock);
            gate.open = true;
            pthread_cond_broadcast(&gate.condition);
            pthread_mutex_unlock(&gate.lock);
        }
        if (retainer_started) {
            if (!wait_for_flag(&race.retainer_finished, THREAD_WAIT_MS))
                fail_stuck_thread("清理 fd_try_retain 竞态");
            pthread_join(retainer_thread, NULL);
        }
        if (collector_started) {
            if (!wait_for_flag(&race.collector_finished, THREAD_WAIT_MS))
                fail_stuck_thread("清理弱获取竞态 GC");
            pthread_join(collector_thread, NULL);
        }
    }
    if (gate_initialized)
        start_gate_destroy(&gate);
    if (receiver != NULL)
        fd_close(receiver);
    fixture_destroy(&fixture);
    return passed;
}

static bool test_try_retain_rejects_blocked_sweep(void) {
    const uid_t_ first_uid = 6302;
    const uid_t_ second_uid = 6303;
    bool passed = true;
    struct fixture first_fixture;
    struct fixture second_fixture;
    bool first_initialized = false;
    bool second_initialized = false;
    struct fd *first_root = NULL;
    struct fd *second_root = NULL;
    struct fd *first_raw = NULL;
    struct fd *second_raw = NULL;
    struct fd *unexpected_retain = NULL;
    int first_host_fd = -1;
    int second_host_fd = -1;
    bool first_poll_locked = false;
    pthread_t collector_thread;
    bool collector_started = false;
    struct blocked_collection collection = {0};

    if (!fixture_init(&first_fixture, first_uid, SOCK_STREAM_)) {
        fixture_destroy(&first_fixture);
        return false;
    }
    first_initialized = true;
    if (!fixture_init(&second_fixture, second_uid, SOCK_STREAM_)) {
        passed = false;
        goto cleanup;
    }
    second_initialized = true;

    first_root = f_get_task_retain(&first_fixture.task,
            first_fixture.receiver_number);
    second_root = f_get_task_retain(&second_fixture.task,
            second_fixture.receiver_number);
    CHECK(first_root != NULL && second_root != NULL,
            "取得两组封锁测试外部根");
    first_raw = first_root;
    second_raw = second_root;
    first_host_fd = first_raw->real_fd;
    second_host_fd = second_raw->real_fd;

    current = &first_fixture.task;
    CHECK(enqueue_self_rights(&first_fixture, 1),
            "建立首个封锁测试自环");
    current = &second_fixture.task;
    CHECK(enqueue_self_rights(&second_fixture, 1),
            "建立第二个封锁测试自环");

    CHECK(f_close_task(&first_fixture.task,
                    first_fixture.sender_number) == 0,
            "关闭首个自环发送端 guest fd");
    first_fixture.sender_number = -1;
    CHECK(f_close_task(&first_fixture.task,
                    first_fixture.receiver_number) == 0,
            "关闭首个自环接收端 guest fd");
    first_fixture.receiver_number = -1;
    CHECK(f_close_task(&second_fixture.task,
                    second_fixture.sender_number) == 0,
            "关闭第二个自环发送端 guest fd");
    second_fixture.sender_number = -1;
    CHECK(f_close_task(&second_fixture.task,
                    second_fixture.receiver_number) == 0,
            "关闭第二个自环接收端 guest fd");
    second_fixture.receiver_number = -1;

    // 卡住首个对象的最终 poll 清理，使第二个对象稳定停在 BLOCKED|1。
    lock(&first_raw->poll_lock);
    first_poll_locked = true;
    fd_close(first_root);
    first_root = NULL;
    fd_close(second_root);
    second_root = NULL;

    collection.fixture = &first_fixture;
    atomic_init(&collection.finished, false);
    collector_started = pthread_create(&collector_thread, NULL,
            blocked_collector, &collection) == 0;
    CHECK(collector_started, "创建确定性封锁 GC 线程");
    CHECK(wait_for_blocked_refcount(
                    first_raw, 0, THREAD_WAIT_MS),
            "首个对象在最终 poll 清理前停于 BLOCKED|0");

    unsigned second_state = atomic_load_explicit(
            &second_raw->refcount, memory_order_acquire);
    CHECK((second_state & FD_REFCOUNT_ACQUIRE_BLOCKED) != 0 &&
                    (second_state & FD_REFCOUNT_VALUE_MASK) == 1,
            "第二个对象在 sweep 释放前停于 BLOCKED|1");
    unexpected_retain = fd_try_retain(second_raw);
    CHECK(unexpected_retain == NULL,
            "弱获取不能穿透 GC 高位封锁");

    unlock(&first_raw->poll_lock);
    first_poll_locked = false;
    if (!wait_for_flag(&collection.finished, THREAD_WAIT_MS))
        fail_stuck_thread("确定性封锁 GC");
    pthread_join(collector_thread, NULL);
    collector_started = false;

    CHECK(socket_scm_inflight_count(first_uid) == 0 &&
                    socket_scm_inflight_count(second_uid) == 0,
            "封锁 sweep 后两组 inflight 均归零");
    CHECK(host_fd_is_closed(first_host_fd) &&
                    host_fd_is_closed(second_host_fd),
            "封锁 sweep 后两个宿主 fd 均最终关闭");

cleanup:
    if (first_poll_locked) {
        unlock(&first_raw->poll_lock);
    }
    if (unexpected_retain != NULL)
        fd_close(unexpected_retain);
    if (collector_started) {
        if (!wait_for_flag(&collection.finished, THREAD_WAIT_MS))
            fail_stuck_thread("清理确定性封锁 GC");
        pthread_join(collector_thread, NULL);
    }
    if (first_root != NULL)
        fd_close(first_root);
    if (second_root != NULL)
        fd_close(second_root);
    if (second_initialized)
        fixture_destroy(&second_fixture);
    if (first_initialized)
        fixture_destroy(&first_fixture);
    return passed;
}

static void io_fail(struct io_race *race, enum io_failure failure) {
    int expected = IO_FAILURE_NONE;
    atomic_compare_exchange_strong_explicit(&race->failure,
            &expected, failure, memory_order_release,
            memory_order_relaxed);
}

static bool received_self_right(struct io_race *race,
        dword_t flags, byte_t expected_payload, struct scm **scm) {
    byte_t payload = UINT8_MAX;
    dword_t message_flags = UINT32_MAX;
    ssize_t received = socket_recvmsg_ref(&race->receiver,
            &payload, sizeof(payload), flags | MSG_DONTWAIT_,
            NULL, &message_flags, scm);
    return received == 1 && payload == expected_payload &&
            message_flags == 0 && *scm != NULL &&
            (*scm)->num_fds == 1;
}

static void *peek_receive_racer(void *opaque) {
    struct io_race *race = opaque;
    current = &race->fixture->task;
    start_gate_wait(race->gate);
    for (unsigned index = 0; index < MESSAGE_COUNT - 1; index++) {
        struct scm *peeked = NULL;
        if (!received_self_right(
                    race, MSG_PEEK_, (byte_t) index, &peeked)) {
            if (peeked != NULL)
                socket_scm_release(peeked);
            io_fail(race, IO_FAILURE_PEEK);
            atomic_store_explicit(
                    &race->first_peek_ready, true,
                    memory_order_release);
            break;
        }
        if (peeked->fds[0] != race->receiver.fd) {
            socket_scm_release(peeked);
            io_fail(race, IO_FAILURE_PEEK_RIGHT);
            atomic_store_explicit(
                    &race->first_peek_ready, true,
                    memory_order_release);
            break;
        }
        if (index == 0) {
            // 保持克隆存活，确定性覆盖 guest close 与 PEEK 根并存窗口。
            atomic_store_explicit(
                    &race->first_peek_ready, true,
                    memory_order_release);
            if (!wait_for_flag(&race->guest_close_done, THREAD_WAIT_MS))
                fail_stuck_thread("等待 guest fd 关闭");
        }
        socket_scm_release(peeked);

        struct scm *received = NULL;
        if (!received_self_right(
                    race, 0, (byte_t) index, &received)) {
            if (received != NULL)
                socket_scm_release(received);
            io_fail(race, IO_FAILURE_RECEIVE);
            break;
        }
        if (received->fds[0] != race->receiver.fd) {
            socket_scm_release(received);
            io_fail(race, IO_FAILURE_RECEIVE_RIGHT);
            break;
        }
        socket_scm_release(received);
        atomic_fetch_add_explicit(
                &race->consumed, 1, memory_order_release);
        if ((index & 3) == 0)
            sched_yield();
    }
    atomic_store_explicit(
            &race->first_peek_ready, true, memory_order_release);
    atomic_store_explicit(
            &race->io_finished, true, memory_order_release);
    current = NULL;
    return NULL;
}

static void *guest_closer(void *opaque) {
    struct io_race *race = opaque;
    current = &race->fixture->task;
    start_gate_wait(race->gate);
    if (!wait_for_flag(&race->first_peek_ready, THREAD_WAIT_MS))
        fail_stuck_thread("等待首个 MSG_PEEK");
    if (race->fixture->sender_number >= 0) {
        if (f_close_task(&race->fixture->task,
                    race->fixture->sender_number) != 0)
            io_fail(race, IO_FAILURE_CLOSE);
        race->fixture->sender_number = -1;
    }
    if (race->fixture->receiver_number >= 0) {
        if (f_close_task(&race->fixture->task,
                    race->fixture->receiver_number) != 0)
            io_fail(race, IO_FAILURE_CLOSE);
        race->fixture->receiver_number = -1;
    }
    atomic_store_explicit(
            &race->guest_close_done, true, memory_order_release);
    atomic_store_explicit(
            &race->closer_finished, true, memory_order_release);
    current = NULL;
    return NULL;
}

static void *io_collector(void *opaque) {
    struct io_race *race = opaque;
    current = &race->fixture->task;
    start_gate_wait(race->gate);
    while (!atomic_load_explicit(
            &race->stop_collector, memory_order_acquire)) {
        socket_scm_collect_now();
        atomic_fetch_add_explicit(
                &race->collector_passes, 1, memory_order_release);
        sched_yield();
    }
    atomic_store_explicit(
            &race->collector_finished, true, memory_order_release);
    current = NULL;
    return NULL;
}

static const char *io_failure_description(int failure) {
    switch (failure) {
        case IO_FAILURE_PEEK:
            return "MSG_PEEK 未返回预期 payload 与 SCM_RIGHTS";
        case IO_FAILURE_PEEK_RIGHT:
            return "MSG_PEEK 克隆没有保留同一接收对象";
        case IO_FAILURE_RECEIVE:
            return "正式 recv 未消费与 PEEK 相同的消息";
        case IO_FAILURE_RECEIVE_RIGHT:
            return "正式 recv 没有返回同一接收对象";
        case IO_FAILURE_CLOSE:
            return "并发关闭 guest socket 描述符失败";
        default:
            return "未知 I/O 竞态错误";
    }
}

static bool test_peek_receive_close_races_with_collection(void) {
    const uid_t_ uid = 6301;
    bool passed = true;
    struct fixture fixture;
    struct start_gate gate;
    bool gate_initialized = false;
    struct io_race race = {0};
    pthread_t io_thread;
    pthread_t closer_thread;
    pthread_t collector_thread;
    bool io_started = false;
    bool closer_started = false;
    bool collector_started = false;

    if (!fixture_init(&fixture, uid, SOCK_DGRAM_)) {
        fixture_destroy(&fixture);
        return false;
    }
    CHECK(socket_ref_get_task(&fixture.task,
                    fixture.receiver_number, &race.receiver) == 0,
            "取得 PEEK/recv 竞态接收端强引用");
    int receiver_host_fd = race.receiver.fd->real_fd;
    CHECK(enqueue_self_rights(&fixture, MESSAGE_COUNT),
            "预置 PEEK/recv 竞态 SCM 消息");
    CHECK(socket_scm_inflight_count(uid) == MESSAGE_COUNT,
            "并发 I/O 前 inflight 槽数量准确");
    CHECK(start_gate_init(&gate, 3), "初始化并发 I/O 闸门");
    gate_initialized = true;

    race.fixture = &fixture;
    race.gate = &gate;
    atomic_init(&race.first_peek_ready, false);
    atomic_init(&race.guest_close_done, false);
    atomic_init(&race.io_finished, false);
    atomic_init(&race.closer_finished, false);
    atomic_init(&race.collector_finished, false);
    atomic_init(&race.stop_collector, false);
    atomic_init(&race.collector_passes, 0);
    atomic_init(&race.consumed, 0);
    atomic_init(&race.failure, IO_FAILURE_NONE);

    io_started = pthread_create(
            &io_thread, NULL, peek_receive_racer, &race) == 0;
    closer_started = io_started && pthread_create(
            &closer_thread, NULL, guest_closer, &race) == 0;
    collector_started = closer_started && pthread_create(
            &collector_thread, NULL, io_collector, &race) == 0;
    CHECK(io_started && closer_started && collector_started,
            "创建 PEEK、关闭与 GC 并发线程");
    CHECK(start_gate_open(&gate), "三个并发 I/O 线程同时就绪");
    if (!wait_for_flag(&race.io_finished, THREAD_WAIT_MS))
        fail_stuck_thread("MSG_PEEK/recv 竞态");
    if (!wait_for_flag(&race.closer_finished, THREAD_WAIT_MS))
        fail_stuck_thread("guest close 竞态");
    pthread_join(io_thread, NULL);
    pthread_join(closer_thread, NULL);
    io_started = false;
    closer_started = false;
    CHECK(wait_for_uint_at_least(
                    &race.collector_passes, 2, THREAD_WAIT_MS),
            "I/O 期间 GC 至少完成两轮");
    atomic_store_explicit(
            &race.stop_collector, true, memory_order_release);
    if (!wait_for_flag(&race.collector_finished, THREAD_WAIT_MS))
        fail_stuck_thread("PEEK/recv 竞态 GC");
    pthread_join(collector_thread, NULL);
    collector_started = false;

    int failure = atomic_load_explicit(
            &race.failure, memory_order_acquire);
    if (failure != IO_FAILURE_NONE) {
        fprintf(stderr, "SCM GC 并发测试失败：%s\n",
                io_failure_description(failure));
        passed = false;
        goto cleanup;
    }
    CHECK(atomic_load_explicit(
                    &race.consumed, memory_order_acquire) ==
                    MESSAGE_COUNT - 1,
            "并发 PEEK 与 recv 逐条配对且保留最后一条环边");
    CHECK(socket_scm_inflight_count(uid) == 1 &&
                    host_fd_is_open(receiver_host_fd),
            "guest close 与并发 GC 不会越过 socket_ref 外部根");

    socket_ref_release(&race.receiver);
    socket_scm_collect_now();
    CHECK(socket_scm_inflight_count(uid) == 0 &&
                    host_fd_is_closed(receiver_host_fd),
            "释放 I/O 外部根后最终消息环被回收且 inflight 归零");

cleanup:
    if (io_started || closer_started || collector_started) {
        atomic_store_explicit(
                &race.first_peek_ready, true, memory_order_release);
        atomic_store_explicit(
                &race.guest_close_done, true, memory_order_release);
        atomic_store_explicit(
                &race.stop_collector, true, memory_order_release);
        if (gate_initialized) {
            pthread_mutex_lock(&gate.lock);
            gate.open = true;
            pthread_cond_broadcast(&gate.condition);
            pthread_mutex_unlock(&gate.lock);
        }
        if (io_started) {
            if (!wait_for_flag(&race.io_finished, THREAD_WAIT_MS))
                fail_stuck_thread("清理 MSG_PEEK/recv 竞态");
            pthread_join(io_thread, NULL);
        }
        if (closer_started) {
            if (!wait_for_flag(&race.closer_finished, THREAD_WAIT_MS))
                fail_stuck_thread("清理 guest close 竞态");
            pthread_join(closer_thread, NULL);
        }
        if (collector_started) {
            if (!wait_for_flag(&race.collector_finished, THREAD_WAIT_MS))
                fail_stuck_thread("清理 PEEK/recv 竞态 GC");
            pthread_join(collector_thread, NULL);
        }
    }
    if (gate_initialized)
        start_gate_destroy(&gate);
    if (race.receiver.fd != NULL)
        socket_ref_release(&race.receiver);
    fixture_destroy(&fixture);
    return passed;
}

int main(void) {
    bool passed = true;
    passed = test_try_retain_races_with_collection() && passed;
    passed = test_try_retain_rejects_blocked_sweep() && passed;
    passed = test_peek_receive_close_races_with_collection() && passed;
    return passed ? 0 : 1;
}
