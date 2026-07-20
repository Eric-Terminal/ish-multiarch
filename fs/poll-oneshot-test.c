#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "fs/fd.h"
#include "fs/poll.h"
#include "fs/real.h"
#include "fs/sock.h"
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/resource.h"
#include "kernel/task.h"

extern const struct fd_ops socket_fdops;

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "poll 测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        return 1; \
    } \
} while (0)

struct callback_result {
    int calls;
    int types;
    uint64_t info;
};

struct capacity_result {
    int calls;
    int delivered;
    int capacity;
    uint64_t info;
};

static int record_event(
        void *opaque, int types, union poll_fd_info info) {
    struct callback_result *result = opaque;
    result->calls++;
    result->types = types;
    result->info = info.num;
    return 1;
}

static int reject_event(
        void *opaque, int types, union poll_fd_info info) {
    struct callback_result *result = opaque;
    result->calls++;
    result->types = types;
    result->info = info.num;
    return 0;
}

static int record_with_capacity(
        void *opaque, int types, union poll_fd_info info) {
    struct capacity_result *result = opaque;
    use(types);
    result->calls++;
    if (result->delivered >= result->capacity)
        return 0;
    result->delivered++;
    result->info = info.num;
    return 1;
}

static int fake_ready(struct fd *fd) {
    return *(int *) fd->data;
}

static const struct fd_ops fake_ops = {
    .poll = fake_ready,
};

static const struct fd_ops invalid_real_ops = {
    .poll = realfs_poll,
};

static bool restart_socket_create(struct fd **fd, int *peer) {
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) < 0)
        return false;

    struct fd *created = fd_create(&socket_fdops);
    if (created == NULL) {
        close(sockets[0]);
        close(sockets[1]);
        return false;
    }
    created->real_fd = sockets[0];
    // 测试只需要真实宿主套接字的 poll 与重启语义。
    created->socket.domain = AF_INET_;
    atomic_init(&created->socket.guest_shutdown, 0);
    atomic_init(&created->socket.unix_route_generation, 0);
    atomic_init(&created->socket.unix_capacity_generation, 0);
    atomic_init(&created->socket.guest_error, 0);
    *fd = created;
    *peer = sockets[1];
    return true;
}

struct poll_gate {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    bool entered;
    bool released;
    int readiness;
};

static void poll_gate_init(struct poll_gate *gate, int readiness) {
    *gate = (struct poll_gate) {.readiness = readiness};
    pthread_mutex_init(&gate->lock, NULL);
    pthread_cond_init(&gate->cond, NULL);
}

static void poll_gate_destroy(struct poll_gate *gate) {
    pthread_cond_destroy(&gate->cond);
    pthread_mutex_destroy(&gate->lock);
}

static void poll_gate_wait_until_entered(struct poll_gate *gate) {
    pthread_mutex_lock(&gate->lock);
    while (!gate->entered)
        pthread_cond_wait(&gate->cond, &gate->lock);
    pthread_mutex_unlock(&gate->lock);
}

static void poll_gate_release(struct poll_gate *gate) {
    pthread_mutex_lock(&gate->lock);
    gate->released = true;
    pthread_cond_broadcast(&gate->cond);
    pthread_mutex_unlock(&gate->lock);
}

static int gated_ready(struct fd *fd) {
    struct poll_gate *gate = fd->data;
    pthread_mutex_lock(&gate->lock);
    gate->entered = true;
    pthread_cond_broadcast(&gate->cond);
    while (!gate->released)
        pthread_cond_wait(&gate->cond, &gate->lock);
    int readiness = gate->readiness;
    pthread_mutex_unlock(&gate->lock);
    return readiness;
}

static const struct fd_ops gated_ops = {
    .poll = gated_ready,
};

static void init_task(struct task *task, struct sighand *sighand) {
    memset(task, 0, sizeof(*task));
    memset(sighand, 0, sizeof(*sighand));
    atomic_init(&sighand->refcount, 1);
    lock_init(&sighand->lock);
    lock_init(&task->waiting_cond_lock);
    task->sighand = sighand;
}

static void init_current_task(struct task *task, struct sighand *sighand) {
    init_task(task, sighand);
    current = task;
}

struct wait_context {
    struct poll *poll;
    struct task task;
    struct sighand sighand;
    struct callback_result callback;
    int result;
};

struct timeout_wait_context {
    struct poll *poll;
    struct task task;
    struct sighand sighand;
    struct callback_result callback;
    atomic_bool finished;
    struct timespec elapsed;
    int result;
};

static void *wait_for_event(void *opaque) {
    struct wait_context *context = opaque;
    current = &context->task;
    struct timespec timeout = {.tv_sec = 2};
    context->result = poll_wait(context->poll, record_event,
            &context->callback, &timeout);
    current = NULL;
    return NULL;
}

static void *wait_forever_for_event(void *opaque) {
    struct wait_context *context = opaque;
    current = &context->task;
    context->result = poll_wait(context->poll, record_event,
            &context->callback, NULL);
    current = NULL;
    return NULL;
}

static void *wait_with_repeated_notifications(void *opaque) {
    struct timeout_wait_context *context = opaque;
    current = &context->task;
    struct timespec timeout = {.tv_nsec = 300000000};
    struct timespec started;
    struct timespec finished;
    clock_gettime(CLOCK_MONOTONIC, &started);
    context->result = poll_wait(context->poll, record_event,
            &context->callback, &timeout);
    clock_gettime(CLOCK_MONOTONIC, &finished);
    context->elapsed.tv_sec = finished.tv_sec - started.tv_sec;
    context->elapsed.tv_nsec = finished.tv_nsec - started.tv_nsec;
    if (context->elapsed.tv_nsec < 0) {
        context->elapsed.tv_sec--;
        context->elapsed.tv_nsec += 1000000000;
    }
    atomic_store_explicit(&context->finished, true, memory_order_release);
    current = NULL;
    return NULL;
}

static bool wait_until_blocked(struct poll *poll) {
    for (unsigned attempt = 0; attempt < 100000; attempt++) {
        lock(&poll->lock);
        bool waiting = poll->waiters != 0;
        unlock(&poll->lock);
        if (waiting)
            return true;
        sched_yield();
    }
    return false;
}

static bool wait_until_waiter_count(struct poll *poll, int expected) {
    for (unsigned attempt = 0; attempt < 100000; attempt++) {
        lock(&poll->lock);
        bool reached = poll->waiters == expected;
        unlock(&poll->lock);
        if (reached)
            return true;
        sched_yield();
    }
    return false;
}

struct destroy_context {
    struct poll *poll;
    atomic_bool started;
};

static void *destroy_poll(void *opaque) {
    struct destroy_context *context = opaque;
    current = NULL;
    atomic_store_explicit(&context->started, true, memory_order_release);
    poll_destroy(context->poll);
    return NULL;
}

struct close_context {
    struct fd *fd;
    int result;
};

struct delete_context {
    struct poll *poll;
    struct fd *fd;
    int result;
};

static void *delete_poll_fd(void *opaque) {
    struct delete_context *context = opaque;
    current = NULL;
    context->result = poll_del_fd(context->poll, context->fd);
    return NULL;
}

static void *close_fd(void *opaque) {
    struct close_context *context = opaque;
    current = NULL;
    context->result = fd_close(context->fd);
    return NULL;
}

static bool wait_until_fd_poll_locked(struct fd *fd) {
    for (unsigned attempt = 0; attempt < 100000; attempt++) {
        if (trylock(&fd->poll_lock) != 0)
            return true;
        unlock(&fd->poll_lock);
        sched_yield();
    }
    return false;
}

static bool wait_until_started(atomic_bool *started) {
    for (unsigned attempt = 0; attempt < 100000; attempt++) {
        if (atomic_load_explicit(started, memory_order_acquire))
            return true;
        sched_yield();
    }
    return false;
}

struct epoll_fixture {
    struct task task;
    struct tgroup group;
    struct sighand sighand;
    struct task *previous;
};

struct epoll_registration {
    fd_t epoll_number;
    fd_t target_number;
    struct fd *epoll;
    struct fd *target;
};

static bool epoll_fixture_init(struct epoll_fixture *fixture) {
    struct task *previous = current;
    memset(fixture, 0, sizeof(*fixture));
    fixture->previous = previous;
    init_task(&fixture->task, &fixture->sighand);
    lock_init(&fixture->group.lock);
    fixture->group.limits[RLIMIT_NOFILE_] =
            (struct rlimit_) {8, 8};
    fixture->task.group = &fixture->group;
    fixture->task.files = fdtable_new(8);
    if (IS_ERR(fixture->task.files))
        return false;
    current = &fixture->task;
    return true;
}

static void epoll_fixture_destroy(struct epoll_fixture *fixture) {
    current = NULL;
    fdtable_release(fixture->task.files);
    pthread_mutex_destroy(&fixture->task.waiting_cond_lock.m);
    pthread_mutex_destroy(&fixture->sighand.lock.m);
    pthread_mutex_destroy(&fixture->group.lock.m);
    current = fixture->previous;
}

static bool epoll_registration_create(struct epoll_registration *registration,
        const struct fd_ops *target_ops, void *target_data) {
    *registration = (struct epoll_registration) {
        .epoll_number = sys_epoll_create(0),
        .target_number = -1,
    };
    if (registration->epoll_number < 0)
        return false;

    registration->target = fd_create(target_ops);
    if (registration->target == NULL)
        return false;
    registration->target->data = target_data;
    registration->target_number = f_install(registration->target, 0);
    if (registration->target_number < 0)
        return false;

    struct fd *epoll = f_get_task_retain(
            current, registration->epoll_number);
    if (epoll == NULL)
        return false;
    int result = poll_add_fd_unique(epoll->epollfd.poll,
            registration->target, POLL_READ,
            (union poll_fd_info) {.num = 1});
    registration->epoll = epoll;
    fd_close(epoll);
    return result == 0;
}

struct epoll_call_context {
    struct task *task;
    fd_t epoll_number;
    fd_t target_number;
    int result;
};

static void *epoll_delete(void *opaque) {
    struct epoll_call_context *context = opaque;
    current = context->task;
    context->result = sys_epoll_ctl(context->epoll_number,
            2, context->target_number, 0);
    current = NULL;
    return NULL;
}

static void *epoll_wait_immediate(void *opaque) {
    struct epoll_call_context *context = opaque;
    current = context->task;
    context->result = sys_epoll_wait(
            context->epoll_number, 0, 1, 0);
    current = NULL;
    return NULL;
}

static bool wait_until_epoll_call_retained(
        const struct epoll_registration *registration) {
    for (unsigned attempt = 0; attempt < 100000; attempt++) {
        if (atomic_load_explicit(&registration->epoll->refcount,
                    memory_order_acquire) == 2 &&
                atomic_load_explicit(&registration->target->refcount,
                    memory_order_acquire) == 2)
            return true;
        sched_yield();
    }
    return false;
}

static int test_duplicate_poll_entries(void) {
    int readiness = POLL_READ;
    struct fd *fd = fd_create(&fake_ops);
    CHECK(fd != NULL, "创建重复登记测试文件对象");
    fd->data = &readiness;

    struct poll *poll = poll_create();
    CHECK(!IS_ERR(poll), "创建重复登记 poll 实例");
    CHECK(poll_add_fd(poll, fd, POLL_READ,
            (union poll_fd_info) {.num = 1}) == 0,
            "普通 poll 接受首次登记");
    CHECK(poll_add_fd(poll, fd, POLL_READ,
            (union poll_fd_info) {.num = 2}) == 0,
            "普通 poll 保留重复文件描述符语义");

    struct timespec immediate = {0};
    struct callback_result result = {0};
    CHECK(poll_wait(poll, record_event, &result, &immediate) == 2 &&
            result.calls == 2, "重复登记分别投递事件");

    poll_destroy(poll);
    CHECK(fd_close(fd) == 0, "释放重复登记测试文件对象");
    return 0;
}

static int test_invalid_timeout(void) {
    struct poll *poll = poll_create();
    CHECK(!IS_ERR(poll), "创建非法超时测试 poll 实例");
    const struct timespec invalid[] = {
        {.tv_sec = -1},
        {.tv_nsec = -1},
        {.tv_nsec = 1000000000},
    };
    for (unsigned index = 0; index < array_size(invalid); index++) {
        struct timespec timeout = invalid[index];
        struct callback_result callback = {0};
        CHECK(poll_wait(poll, record_event,
                &callback, &timeout) == _EINVAL,
                "拒绝超出规范范围的 timespec");
        CHECK(callback.calls == 0 && poll->waiters == 0 &&
                poll->notify_pipe[0] == -1 && poll->notify_pipe[1] == -1,
                "非法超时不会建立宿主等待资源");
    }
    poll_destroy(poll);
    return 0;
}

static int test_notifications_preserve_deadline(void) {
    int readiness = 0;
    struct fd *fd = fd_create(&fake_ops);
    CHECK(fd != NULL, "创建截止时间测试文件对象");
    fd->data = &readiness;

    struct poll *poll = poll_create();
    CHECK(!IS_ERR(poll), "创建截止时间测试 poll 实例");
    CHECK(poll_add_fd_unique(poll, fd, POLL_READ,
            (union poll_fd_info) {.num = 1}) == 0,
            "登记始终未就绪的测试文件对象");

    struct timeout_wait_context context = {
        .poll = poll,
    };
    init_task(&context.task, &context.sighand);
    atomic_init(&context.finished, false);
    pthread_t waiter;
    CHECK(pthread_create(&waiter, NULL,
            wait_with_repeated_notifications, &context) == 0,
            "启动固定截止时间等待线程");
    CHECK(wait_until_blocked(poll),
            "发送通知前等待线程已进入宿主后端");

    struct timespec interval = {.tv_nsec = 40000000};
    int notify_result = 0;
    for (unsigned attempt = 0; attempt < 25; attempt++) {
        nanosleep(&interval, NULL);
        if (atomic_load_explicit(
                &context.finished, memory_order_acquire))
            break;
        notify_result = poll_mod_fd(poll, fd, POLL_READ,
                (union poll_fd_info) {.num = attempt + 2});
        if (notify_result < 0)
            break;
    }
    int join_result = pthread_join(waiter, NULL);
    int64_t elapsed_millis = context.elapsed.tv_sec * INT64_C(1000) +
            context.elapsed.tv_nsec / 1000000;

    CHECK(notify_result == 0 && join_result == 0,
            "重复无就绪通知与等待线程均正常完成");
    CHECK(context.result == 0 && context.callback.calls == 0,
            "重复通知不会伪造就绪事件");
    CHECK(elapsed_millis >= 200 && elapsed_millis < 800,
            "重复通知不会从头复用相对超时时长");

    pthread_mutex_destroy(&context.task.waiting_cond_lock.m);
    pthread_mutex_destroy(&context.sighand.lock.m);
    poll_destroy(poll);
    CHECK(fd_close(fd) == 0, "释放截止时间测试文件对象");
    return 0;
}

static int test_registration_semantics(void) {
    int readiness = POLL_READ;
    struct fd *fd = fd_create(&fake_ops);
    CHECK(fd != NULL, "创建测试文件对象");
    fd->data = &readiness;

    struct poll *poll = poll_create();
    CHECK(!IS_ERR(poll), "创建 poll 实例");
    union poll_fd_info first_info = {.num = 0x1234};
    CHECK(poll_add_fd_unique(poll, fd, POLL_READ | POLL_ONESHOT,
            first_info) == 0, "首次登记成功");
    CHECK(poll_has_fd(poll, fd), "登记后能够查询到文件对象");
    CHECK(poll_add_fd_unique(poll, fd, POLL_READ | POLL_ONESHOT,
            first_info) == _EEXIST, "重复 ADD 返回 EEXIST");

    struct timespec immediate = {0};
    struct callback_result rejected = {0};
    CHECK(poll_wait(poll, reject_event, &rejected, &immediate) == 0 &&
            rejected.calls == 1, "回调容量不足时不计入已投递事件");
    CHECK(poll_has_fd(poll, fd), "未投递事件不会禁用单次触发登记");

    struct callback_result first = {0};
    CHECK(poll_wait(poll, record_event, &first, &immediate) == 1,
            "首次就绪事件被投递");
    CHECK(first.calls == 1 && first.types == POLL_READ &&
            first.info == first_info.num, "首次事件保留类型与用户数据");
    CHECK(poll_has_fd(poll, fd), "单次触发后仍保留登记");

    struct callback_result disabled = {0};
    CHECK(poll_wait(poll, record_event, &disabled, &immediate) == 0 &&
            disabled.calls == 0, "未重置前不重复投递");

    union poll_fd_info second_info = {.num = 0x5678};
    CHECK(poll_mod_fd(poll, fd, POLL_READ | POLL_ONESHOT,
            second_info) == 0, "MOD 重置单次触发登记");
    struct callback_result second = {0};
    CHECK(poll_wait(poll, record_event, &second, &immediate) == 1,
            "重置后再次投递就绪事件");
    CHECK(second.calls == 1 && second.types == POLL_READ &&
            second.info == second_info.num, "重置同时更新用户数据");
    CHECK(poll_del_fd(poll, fd) == 0, "禁用状态仍可删除登记");
    CHECK(!poll_has_fd(poll, fd) && poll_del_fd(poll, fd) == _ENOENT,
            "删除后登记消失且重复删除返回 ENOENT");

    poll_destroy(poll);
    CHECK(fd_close(fd) == 0, "释放测试文件对象");
    return 0;
}

static int test_internal_wakeup_preserves_guest_edge_state(void) {
    int readiness = POLL_READ;
    struct fd *fd = fd_create(&fake_ops);
    CHECK(fd != NULL, "创建内部唤醒边沿测试文件对象");
    fd->data = &readiness;

    struct poll *guest = poll_create();
    struct poll *internal = poll_create();
    CHECK(!IS_ERR(guest) && !IS_ERR(internal),
            "创建 guest 与内部 poll 实例");
    CHECK(poll_add_fd(guest, fd,
            POLL_READ | POLL_EDGETRIGGERED,
            (union poll_fd_info) {.num = 1}) == 0 &&
            poll_add_fd_wake(internal, fd, POLL_READ,
            (union poll_fd_info) {.num = 2}) == 0,
            "登记 guest 边沿与内部唤醒事件");

    struct timespec immediate = {0};
    struct callback_result first_guest = {0};
    CHECK(poll_wait(guest, record_event,
            &first_guest, &immediate) == 1 &&
            first_guest.calls == 1 &&
            first_guest.types == POLL_READ,
            "guest 首次读取边沿正常投递");

    poll_wakeup_internal(fd, POLL_READ);
    struct callback_result rejected = {0};
    CHECK(poll_wait(internal, reject_event,
            &rejected, &immediate) == 0 && rejected.calls == 1,
            "内部显式事件允许回调暂时拒收");
    struct callback_result internal_event = {0};
    CHECK(poll_wait(internal, record_event,
            &internal_event, &immediate) == 1 &&
            internal_event.calls == 1 &&
            internal_event.types == POLL_READ &&
            internal_event.info == 2,
            "被拒收的内部显式事件留待下一轮投递");

    struct callback_result no_guest_edge = {0};
    CHECK(poll_wait(guest, record_event,
            &no_guest_edge, &immediate) == 0 &&
            no_guest_edge.calls == 0,
            "内部唤醒不会给 guest 制造伪边沿");

    struct fd *host_fd = NULL;
    int host_peer = -1;
    CHECK(restart_socket_create(&host_fd, &host_peer),
            "创建 host-only 内部唤醒测试套接字");
    struct poll *host = poll_create();
    CHECK(!IS_ERR(host) && poll_add_fd_host(host, host_fd, POLL_READ,
            (union poll_fd_info) {.num = 3}) == 0,
            "登记 host-only 内部唤醒事件");
    struct callback_result host_before_wakeup = {0};
    CHECK(poll_wait(host, record_event,
            &host_before_wakeup, &immediate) == 0 &&
            host_before_wakeup.calls == 0,
            "宿主未就绪时 host-only 不会伪造事件");
    poll_wakeup_internal(host_fd, POLL_READ);
    struct callback_result host_event = {0};
    CHECK(poll_wait(host, record_event,
            &host_event, &immediate) == 1 &&
            host_event.calls == 1 &&
            host_event.types == POLL_READ &&
            host_event.info == 3,
            "路由变化能显式唤醒 host-only 发送等待");
    poll_destroy(host);
    CHECK(fd_close(host_fd) == 0 && close(host_peer) == 0,
            "释放 host-only 内部唤醒测试套接字");

    poll_wakeup(fd, POLL_READ);
    struct callback_result rearmed_guest = {0};
    CHECK(poll_wait(guest, record_event,
            &rearmed_guest, &immediate) == 1 &&
            rearmed_guest.calls == 1 &&
            rearmed_guest.types == POLL_READ,
            "普通唤醒仍能重新开放 guest 边沿");

    poll_destroy(internal);
    poll_destroy(guest);
    CHECK(fd_close(fd) == 0, "释放内部唤醒边沿测试文件对象");
    return 0;
}

static int test_wake_only_cleanup_leaves_safe_tombstone(void) {
    int readiness = 0;
    struct fd *closed = fd_create(&fake_ops);
    CHECK(closed != NULL, "创建 wake-only 生命周期测试文件对象");
    closed->data = &readiness;

    struct poll *poll = poll_create();
    CHECK(!IS_ERR(poll), "创建 wake-only 生命周期 poll 实例");
    CHECK(poll_add_fd_wake(poll, closed, POLL_WRITE,
            (union poll_fd_info) {.num = 0xc10}) == 0,
            "登记 wake-only 生命周期事件");

    struct wait_context context = {.poll = poll};
    init_task(&context.task, &context.sighand);
    pthread_t waiter;
    CHECK(pthread_create(&waiter, NULL,
            wait_for_event, &context) == 0,
            "启动 wake-only 生命周期等待线程");
    CHECK(wait_until_blocked(poll),
            "最终关闭前等待线程已进入宿主后端");
    CHECK(fd_close(closed) == 0,
            "最终关闭 wake-only 关联文件对象");
    CHECK(pthread_join(waiter, NULL) == 0 &&
            context.result == 1 && context.callback.calls == 1 &&
            context.callback.types == POLL_HUP &&
            context.callback.info == 0xc10,
            "tombstone 安全投递最终关闭通知");

    struct fd *unrelated = fd_create(&fake_ops);
    CHECK(unrelated != NULL, "创建 tombstone 遍历测试文件对象");
    unrelated->data = &readiness;
    CHECK(!poll_has_fd(poll, unrelated) &&
            poll_mod_fd(poll, unrelated, POLL_READ,
            (union poll_fd_info) {.num = 1}) == _ENOENT &&
            poll_del_fd(poll, unrelated) == _ENOENT,
            "HAS、MOD 与 DEL 安全跳过 tombstone");
    CHECK(poll_add_fd_wake(poll, unrelated, POLL_READ,
            (union poll_fd_info) {.num = 2}) == 0 &&
            poll_has_fd(poll, unrelated),
            "tombstone 不妨碍后续登记与查询");
    CHECK(poll_mod_fd(poll, unrelated, POLL_WRITE,
            (union poll_fd_info) {.num = 3}) == 0 &&
            poll_del_fd(poll, unrelated) == 0 &&
            !poll_has_fd(poll, unrelated),
            "tombstone 不妨碍后续 MOD 与 DEL");

    pthread_mutex_destroy(&context.task.waiting_cond_lock.m);
    pthread_mutex_destroy(&context.sighand.lock.m);
    poll_destroy(poll);
    CHECK(fd_close(unrelated) == 0,
            "释放 tombstone 遍历测试文件对象");
    return 0;
}

static int test_capacity_rejection_preserves_ready_event(void) {
    int readiness[2] = {POLL_READ, POLL_READ};
    struct fd *fds[2] = {
        fd_create(&fake_ops),
        fd_create(&fake_ops),
    };
    CHECK(fds[0] != NULL && fds[1] != NULL,
            "创建容量测试文件对象");
    fds[0]->data = &readiness[0];
    fds[1]->data = &readiness[1];

    struct poll *poll = poll_create();
    CHECK(!IS_ERR(poll), "创建容量测试 poll 实例");
    for (unsigned index = 0; index < 2; index++) {
        CHECK(poll_add_fd_unique(poll, fds[index],
                POLL_READ | POLL_ONESHOT,
                (union poll_fd_info) {.num = index + 1}) == 0,
                "登记容量测试就绪事件");
    }

    struct timespec immediate = {0};
    struct capacity_result first = {.capacity = 1};
    CHECK(poll_wait(poll, record_with_capacity, &first, &immediate) == 1 &&
            first.calls == 2 && first.delivered == 1,
            "首轮容量不足时只消费一个事件");

    struct capacity_result second = {.capacity = 1};
    CHECK(poll_wait(poll, record_with_capacity, &second, &immediate) == 1 &&
            second.calls == 1 && second.delivered == 1 &&
            second.info != first.info,
            "容量拒绝的事件在下一轮继续投递");

    struct capacity_result exhausted = {.capacity = 1};
    CHECK(poll_wait(poll, record_with_capacity,
            &exhausted, &immediate) == 0 && exhausted.calls == 0,
            "两个单次触发事件均已消费");

    poll_destroy(poll);
    CHECK(fd_close(fds[0]) == 0 && fd_close(fds[1]) == 0,
            "释放容量测试文件对象");
    return 0;
}

static int test_ready_add_wakes_waiter(void) {
    int readiness = POLL_READ;
    struct fd *fd = fd_create(&fake_ops);
    CHECK(fd != NULL, "创建 ADD 唤醒测试文件对象");
    fd->data = &readiness;

    struct poll *poll = poll_create();
    CHECK(!IS_ERR(poll), "创建 ADD 唤醒测试 poll 实例");
    struct wait_context context = {.poll = poll};
    init_task(&context.task, &context.sighand);
    pthread_t waiter;
    CHECK(pthread_create(&waiter, NULL, wait_for_event, &context) == 0,
            "启动 ADD 阻塞等待线程");

    bool blocked = wait_until_blocked(poll);
    int add_result = poll_add_fd_unique(poll, fd, POLL_READ,
            (union poll_fd_info) {.num = 0xadd});
    int join_result = pthread_join(waiter, NULL);
    CHECK(blocked, "ADD 前等待线程已进入阻塞状态");
    CHECK(add_result == 0, "向阻塞 poll 添加已就绪登记");
    CHECK(join_result == 0 && context.result == 1 &&
            context.callback.calls == 1 &&
            context.callback.types == POLL_READ &&
            context.callback.info == 0xadd,
            "已就绪 ADD 立即唤醒等待线程");

    pthread_mutex_destroy(&context.task.waiting_cond_lock.m);
    pthread_mutex_destroy(&context.sighand.lock.m);
    poll_destroy(poll);
    CHECK(fd_close(fd) == 0, "释放 ADD 唤醒测试文件对象");
    return 0;
}

static int test_ready_mod_wakes_waiter(void) {
    int readiness = POLL_READ;
    struct fd *fd = fd_create(&fake_ops);
    CHECK(fd != NULL, "创建 MOD 唤醒测试文件对象");
    fd->data = &readiness;

    struct poll *poll = poll_create();
    CHECK(!IS_ERR(poll), "创建 MOD 唤醒测试 poll 实例");
    CHECK(poll_add_fd_unique(poll, fd, POLL_READ | POLL_ONESHOT,
            (union poll_fd_info) {.num = 1}) == 0,
            "登记 MOD 唤醒测试事件");
    struct timespec immediate = {0};
    struct callback_result initial = {0};
    CHECK(poll_wait(poll, record_event, &initial, &immediate) == 1,
            "先消费单次触发事件使登记禁用");

    struct wait_context context = {.poll = poll};
    init_task(&context.task, &context.sighand);
    pthread_t waiter;
    CHECK(pthread_create(&waiter, NULL, wait_for_event, &context) == 0,
            "启动 MOD 阻塞等待线程");

    bool blocked = wait_until_blocked(poll);
    int mod_result = poll_mod_fd(poll, fd,
            POLL_READ | POLL_ONESHOT,
            (union poll_fd_info) {.num = 2});
    int join_result = pthread_join(waiter, NULL);
    CHECK(blocked, "MOD 前等待线程已进入阻塞状态");
    CHECK(mod_result == 0, "重置阻塞 poll 中的已就绪登记");
    CHECK(join_result == 0 && context.result == 1 &&
            context.callback.calls == 1 &&
            context.callback.types == POLL_READ &&
            context.callback.info == 2,
            "已就绪 MOD 立即唤醒等待线程");

    pthread_mutex_destroy(&context.task.waiting_cond_lock.m);
    pthread_mutex_destroy(&context.sighand.lock.m);
    poll_destroy(poll);
    CHECK(fd_close(fd) == 0, "释放 MOD 唤醒测试文件对象");
    return 0;
}

static int test_listen_wait_snapshot_ignores_add(void) {
    struct fd *first;
    struct fd *added;
    int first_peer;
    int added_peer;
    CHECK(restart_socket_create(&first, &first_peer) &&
            restart_socket_create(&added, &added_peer),
            "创建 ADD 快照测试套接字");

    struct poll *poll = poll_create();
    CHECK(!IS_ERR(poll), "创建 ADD 快照测试 poll 实例");
    CHECK(poll_add_fd(poll, first, POLL_READ,
            (union poll_fd_info) {.num = 1}) == 0,
            "登记 ADD 快照的初始套接字");

    struct wait_context context = {.poll = poll};
    init_task(&context.task, &context.sighand);
    pthread_t waiter;
    CHECK(pthread_create(&waiter, NULL, wait_for_event, &context) == 0,
            "启动 ADD 快照等待线程");
    CHECK(wait_until_blocked(poll),
            "ADD 前等待线程已建立 begin 快照");

    CHECK(poll_add_fd(poll, added, POLL_READ,
            (union poll_fd_info) {.num = 2}) == 0,
            "等待期间动态添加套接字");
    CHECK(write(first_peer, "a", 1) == 1,
            "唤醒 ADD 快照等待线程");
    CHECK(pthread_join(waiter, NULL) == 0 && context.result == 1,
            "ADD 快照等待正常投递事件");
    CHECK(context.task.sockrestart.count == 0,
            "新增登记不会执行未配对的 end");

    pthread_mutex_destroy(&context.task.waiting_cond_lock.m);
    pthread_mutex_destroy(&context.sighand.lock.m);
    poll_destroy(poll);
    CHECK(fd_close(first) == 0 && fd_close(added) == 0,
            "释放 ADD 快照测试文件对象");
    CHECK(close(first_peer) == 0 && close(added_peer) == 0,
            "释放 ADD 快照测试对端");
    return 0;
}

static int test_listen_wait_snapshot_survives_delete(void) {
    struct fd *first;
    struct fd *deleted;
    int first_peer;
    int deleted_peer;
    CHECK(restart_socket_create(&first, &first_peer) &&
            restart_socket_create(&deleted, &deleted_peer),
            "创建 DEL 快照测试套接字");

    struct poll *poll = poll_create();
    CHECK(!IS_ERR(poll), "创建 DEL 快照测试 poll 实例");
    CHECK(poll_add_fd(poll, first, POLL_READ,
            (union poll_fd_info) {.num = 1}) == 0 &&
            poll_add_fd(poll, deleted, POLL_READ,
            (union poll_fd_info) {.num = 2}) == 0,
            "登记 DEL 快照测试套接字");

    struct wait_context context = {.poll = poll};
    init_task(&context.task, &context.sighand);
    pthread_t waiter;
    CHECK(pthread_create(&waiter, NULL, wait_for_event, &context) == 0,
            "启动 DEL 快照等待线程");
    CHECK(wait_until_blocked(poll),
            "DEL 前等待线程已建立 begin 快照");

    struct delete_context deletion = {
        .poll = poll,
        .fd = deleted,
    };
    pthread_t deleter;
    CHECK(pthread_create(&deleter, NULL,
            delete_poll_fd, &deletion) == 0,
            "启动并发 DEL 线程");
    CHECK(pthread_join(deleter, NULL) == 0 && deletion.result == 0,
            "等待期间 DEL 完成登记拆除");
    CHECK(write(first_peer, "d", 1) == 1,
            "唤醒 DEL 快照等待线程");
    CHECK(pthread_join(waiter, NULL) == 0 && context.result == 1,
            "DEL 快照等待正常投递事件");
    CHECK(context.task.sockrestart.count == 0,
            "已删除登记仍执行快照中配对的 end");

    pthread_mutex_destroy(&context.task.waiting_cond_lock.m);
    pthread_mutex_destroy(&context.sighand.lock.m);
    poll_destroy(poll);
    CHECK(fd_close(first) == 0 && fd_close(deleted) == 0,
            "释放 DEL 快照测试文件对象");
    CHECK(close(first_peer) == 0 && close(deleted_peer) == 0,
            "释放 DEL 快照测试对端");
    return 0;
}

static int test_listen_wait_snapshot_survives_cleanup(void) {
    struct fd *first;
    struct fd *closed;
    int first_peer;
    int closed_peer;
    CHECK(restart_socket_create(&first, &first_peer) &&
            restart_socket_create(&closed, &closed_peer),
            "创建 cleanup 快照测试套接字");

    struct poll *poll = poll_create();
    CHECK(!IS_ERR(poll), "创建 cleanup 快照测试 poll 实例");
    CHECK(poll_add_fd(poll, first, POLL_READ,
            (union poll_fd_info) {.num = 1}) == 0 &&
            poll_add_fd(poll, closed, POLL_READ,
            (union poll_fd_info) {.num = 2}) == 0,
            "登记 cleanup 快照测试套接字");

    struct wait_context context = {.poll = poll};
    init_task(&context.task, &context.sighand);
    pthread_t waiter;
    CHECK(pthread_create(&waiter, NULL, wait_for_event, &context) == 0,
            "启动 cleanup 快照等待线程");
    CHECK(wait_until_blocked(poll),
            "cleanup 前等待线程已建立 begin 快照");

    CHECK(fd_close(closed) == 0,
            "等待期间关闭最后一个外部引用");
    CHECK(write(first_peer, "c", 1) == 1,
            "唤醒 cleanup 快照等待线程");
    CHECK(pthread_join(waiter, NULL) == 0 && context.result == 1,
            "cleanup 快照等待正常投递事件");
    CHECK(context.task.sockrestart.count == 0,
            "最终关闭在快照 end 后才释放文件对象");

    pthread_mutex_destroy(&context.task.waiting_cond_lock.m);
    pthread_mutex_destroy(&context.sighand.lock.m);
    poll_destroy(poll);
    CHECK(fd_close(first) == 0,
            "释放 cleanup 快照测试文件对象");
    CHECK(close(first_peer) == 0 && close(closed_peer) == 0,
            "释放 cleanup 快照测试对端");
    return 0;
}

static int test_destroy_wakes_active_waiter(void) {
    struct poll_gate gate;
    poll_gate_init(&gate, 0);
    struct fd *fd = fd_create(&gated_ops);
    CHECK(fd != NULL, "创建销毁等待者测试文件对象");
    fd->data = &gate;

    struct poll *poll = poll_create();
    CHECK(!IS_ERR(poll), "创建销毁等待者测试 poll 实例");
    CHECK(poll_add_fd(poll, fd, POLL_READ,
            (union poll_fd_info) {.num = 1}) == 0,
            "登记会阻塞就绪检查的文件对象");

    struct wait_context waiter_context = {.poll = poll};
    init_task(&waiter_context.task, &waiter_context.sighand);
    pthread_t waiter;
    CHECK(pthread_create(&waiter, NULL, wait_for_event,
            &waiter_context) == 0, "启动待销毁 poll 的等待线程");
    poll_gate_wait_until_entered(&gate);

    struct destroy_context destroy_context = {.poll = poll};
    atomic_init(&destroy_context.started, false);
    pthread_t destroyer;
    CHECK(pthread_create(&destroyer, NULL, destroy_poll,
            &destroy_context) == 0, "启动并发 poll 销毁线程");
    bool destroy_started = wait_until_started(&destroy_context.started);
    poll_gate_release(&gate);

    int waiter_join = pthread_join(waiter, NULL);
    int destroyer_join = pthread_join(destroyer, NULL);
    CHECK(destroy_started, "销毁线程已开始执行");
    CHECK(waiter_join == 0 && destroyer_join == 0,
            "等待与销毁线程都正常结束");
    CHECK(waiter_context.result == _EBADF,
            "销毁会唤醒活动等待者并返回关闭错误");

    pthread_mutex_destroy(&waiter_context.task.waiting_cond_lock.m);
    pthread_mutex_destroy(&waiter_context.sighand.lock.m);
    CHECK(fd_close(fd) == 0, "释放销毁等待者测试文件对象");
    poll_gate_destroy(&gate);
    return 0;
}

static int test_destroy_wakes_all_waiters(void) {
    enum { waiter_count = 4 };
    struct poll *poll = poll_create();
    CHECK(!IS_ERR(poll), "创建多等待者销毁测试 poll 实例");
    struct wait_context contexts[waiter_count] = {0};
    pthread_t waiters[waiter_count];
    for (unsigned index = 0; index < waiter_count; index++) {
        contexts[index].poll = poll;
        init_task(&contexts[index].task, &contexts[index].sighand);
        CHECK(pthread_create(&waiters[index], NULL,
                wait_forever_for_event, &contexts[index]) == 0,
                "启动永久阻塞的 poll 等待线程");
    }
    CHECK(wait_until_waiter_count(poll, waiter_count),
            "四个等待线程都已进入宿主后端");

    struct destroy_context destroy_context = {.poll = poll};
    atomic_init(&destroy_context.started, false);
    pthread_t destroyer;
    CHECK(pthread_create(&destroyer, NULL, destroy_poll,
            &destroy_context) == 0, "启动多等待者 poll 销毁线程");
    int join_result = 0;
    for (unsigned index = 0; index < waiter_count; index++) {
        join_result |= pthread_join(waiters[index], NULL);
        join_result |= contexts[index].result != _EBADF;
        pthread_mutex_destroy(
                &contexts[index].task.waiting_cond_lock.m);
        pthread_mutex_destroy(&contexts[index].sighand.lock.m);
    }
    join_result |= pthread_join(destroyer, NULL);
    CHECK(join_result == 0,
            "一个未消费的电平通知会唤醒并排空全部等待者");
    return 0;
}

static int test_destroy_races_with_fd_cleanup(void) {
    int readiness = 0;
    struct fd *fd = fd_create(&fake_ops);
    CHECK(fd != NULL, "创建并发清理测试文件对象");
    fd->data = &readiness;
    struct poll *poll = poll_create();
    CHECK(!IS_ERR(poll), "创建并发清理测试 poll 实例");
    CHECK(poll_add_fd(poll, fd, POLL_READ,
            (union poll_fd_info) {.num = 1}) == 0,
            "登记并发清理测试文件对象");

    // 先让最终 fd 关闭拿到 fd 锁并停在 poll 锁上，再并发销毁 poll。
    lock(&poll->lock);
    struct close_context close_context = {.fd = fd};
    pthread_t closer;
    CHECK(pthread_create(&closer, NULL, close_fd,
            &close_context) == 0, "启动最终 fd 关闭线程");
    bool cleanup_blocked = wait_until_fd_poll_locked(fd);
    if (!cleanup_blocked) {
        unlock(&poll->lock);
        pthread_join(closer, NULL);
        poll_destroy(poll);
        CHECK(false, "最终 fd 关闭已持有登记锁");
    }

    struct destroy_context destroy_context = {.poll = poll};
    atomic_init(&destroy_context.started, false);
    pthread_t destroyer;
    int create_result = pthread_create(&destroyer, NULL,
            destroy_poll, &destroy_context);
    bool destroy_started = create_result == 0 &&
            wait_until_started(&destroy_context.started);
    for (unsigned attempt = 0; attempt < 1000 && destroy_started; attempt++)
        sched_yield();
    unlock(&poll->lock);

    int close_join = pthread_join(closer, NULL);
    int destroy_join = create_result == 0 ?
            pthread_join(destroyer, NULL) : create_result;
    CHECK(create_result == 0 && destroy_started,
            "并发清理期间销毁线程已开始执行");
    CHECK(close_join == 0 && destroy_join == 0 &&
            close_context.result == 0,
            "最终 fd 清理与 poll 销毁只拆除一次登记");
    return 0;
}

static int test_epoll_ctl_retains_both_fds(void) {
    struct epoll_fixture fixture;
    CHECK(epoll_fixture_init(&fixture), "初始化 epoll ctl 生命周期夹具");
    int readiness = 0;
    struct epoll_registration registration;
    CHECK(epoll_registration_create(&registration,
            &fake_ops, &readiness), "创建 epoll ctl 测试登记");

    // 让 DEL 停在目标 fd 锁上，确认系统调用已经取得两个独立引用。
    lock(&registration.target->poll_lock);
    struct epoll_call_context call = {
        .task = &fixture.task,
        .epoll_number = registration.epoll_number,
        .target_number = registration.target_number,
    };
    pthread_t caller;
    int create_result = pthread_create(&caller, NULL, epoll_delete, &call);
    bool retained = create_result == 0 &&
            wait_until_epoll_call_retained(&registration);
    int epoll_close_result = _EBADF;
    int target_close_result = _EBADF;
    bool refs_survived_close = false;
    if (retained) {
        epoll_close_result = f_close_task(
                &fixture.task, registration.epoll_number);
        target_close_result = f_close_task(
                &fixture.task, registration.target_number);
        refs_survived_close =
                atomic_load_explicit(&registration.epoll->refcount,
                    memory_order_acquire) == 1 &&
                atomic_load_explicit(&registration.target->refcount,
                    memory_order_acquire) == 1;
    }
    unlock(&registration.target->poll_lock);
    int join_result = create_result == 0 ?
            pthread_join(caller, NULL) : create_result;

    if (!retained) {
        f_close_task(&fixture.task, registration.target_number);
        f_close_task(&fixture.task, registration.epoll_number);
    }
    epoll_fixture_destroy(&fixture);
    CHECK(create_result == 0 && retained,
            "epoll ctl 在进入登记锁前保留 epoll 与目标 fd");
    CHECK(epoll_close_result == 0 && target_close_result == 0 &&
            refs_survived_close,
            "并发关闭表项后系统调用引用继续维持两个对象");
    CHECK(join_result == 0 && call.result == 0,
            "DEL 在并发关闭两个表项后安全完成");
    return 0;
}

static int test_epoll_wait_retains_epoll_fd(void) {
    struct epoll_fixture fixture;
    CHECK(epoll_fixture_init(&fixture), "初始化 epoll wait 生命周期夹具");
    struct poll_gate gate;
    poll_gate_init(&gate, 0);
    struct epoll_registration registration;
    CHECK(epoll_registration_create(&registration,
            &gated_ops, &gate), "创建 epoll wait 测试登记");

    struct epoll_call_context call = {
        .task = &fixture.task,
        .epoll_number = registration.epoll_number,
        .target_number = registration.target_number,
    };
    pthread_t waiter;
    CHECK(pthread_create(&waiter, NULL,
            epoll_wait_immediate, &call) == 0,
            "启动 epoll wait 生命周期线程");
    poll_gate_wait_until_entered(&gate);
    bool retained = atomic_load_explicit(&registration.epoll->refcount,
            memory_order_acquire) == 2;
    int close_result = f_close_task(
            &fixture.task, registration.epoll_number);
    bool survived_close = retained && close_result == 0 &&
            atomic_load_explicit(&registration.epoll->refcount,
                memory_order_acquire) == 1;
    poll_gate_release(&gate);
    int join_result = pthread_join(waiter, NULL);

    int target_close_result = f_close_task(
            &fixture.task, registration.target_number);
    epoll_fixture_destroy(&fixture);
    poll_gate_destroy(&gate);
    CHECK(retained && survived_close,
            "epoll wait 在阻塞检查期间保留 epoll fd");
    CHECK(join_result == 0 && call.result == 0,
            "关闭表项后 epoll wait 仍安全返回超时");
    CHECK(target_close_result == 0,
            "释放 epoll wait 测试目标文件对象");
    return 0;
}

static int test_real_fd_cleanup(void) {
    int pipe_fds[2];
    CHECK(pipe(pipe_fds) == 0, "创建宿主管道");
    struct fd *read_end = fd_create(&realfs_fdops);
    CHECK(read_end != NULL, "创建真实文件对象");
    read_end->real_fd = pipe_fds[0];

    struct poll *poll = poll_create();
    CHECK(!IS_ERR(poll), "为真实文件创建 poll 实例");
    CHECK(poll_add_fd(poll, read_end, POLL_READ | POLL_ONESHOT,
            (union poll_fd_info) {.num = 7}) == 0,
            "登记真实管道读端");
    CHECK(write(pipe_fds[1], "x", 1) == 1, "使真实管道进入就绪状态");

    struct timespec immediate = {0};
    struct callback_result first = {0};
    CHECK(poll_wait(poll, record_event, &first, &immediate) == 1 &&
            first.calls == 1, "真实后端投递首次事件");
    struct callback_result disabled = {0};
    CHECK(poll_wait(poll, record_event, &disabled, &immediate) == 0 &&
            disabled.calls == 0, "真实后端在重置前保持禁用");
    CHECK(poll_mod_fd(poll, read_end, POLL_READ | POLL_ONESHOT,
            (union poll_fd_info) {.num = 8}) == 0,
            "真实后端可通过 MOD 重置");
    struct callback_result second = {0};
    CHECK(poll_wait(poll, record_event, &second, &immediate) == 1 &&
            second.calls == 1 && second.info == 8,
            "真实后端重置后再次投递并更新数据");

    CHECK(fd_close(read_end) == 0, "关闭禁用中的真实文件对象");
    CHECK(list_empty(&poll->poll_fds), "关闭文件对象同步清理 poll 登记");
    poll_destroy(poll);
    CHECK(close(pipe_fds[1]) == 0, "关闭宿主管道写端");
    return 0;
}

static int test_real_fd_registration_error(void) {
    struct fd *invalid = fd_create(&invalid_real_ops);
    CHECK(invalid != NULL, "创建无效真实文件对象");
    invalid->real_fd = -1;

    struct poll *poll = poll_create();
    CHECK(!IS_ERR(poll), "为无效真实文件创建 poll 实例");
    CHECK(poll_add_fd(poll, invalid, POLL_READ,
            (union poll_fd_info) {.num = 9}) == _EBADF,
            "真实后端逐项传播无效 fd 登记错误");
    CHECK(list_empty(&poll->poll_fds) &&
            list_empty(&invalid->poll_fds),
            "登记失败不留下半初始化关联");

    poll_destroy(poll);
    CHECK(fd_close(invalid) == 0, "释放无效真实文件对象");
    return 0;
}

int main(void) {
    struct task task;
    struct sighand sighand;
    init_current_task(&task, &sighand);

    int result = test_duplicate_poll_entries();
    if (result == 0)
        result = test_invalid_timeout();
    if (result == 0)
        result = test_notifications_preserve_deadline();
    if (result == 0)
        result = test_registration_semantics();
    if (result == 0)
        result = test_internal_wakeup_preserves_guest_edge_state();
    if (result == 0)
        result = test_wake_only_cleanup_leaves_safe_tombstone();
    if (result == 0)
        result = test_capacity_rejection_preserves_ready_event();
    if (result == 0)
        result = test_ready_add_wakes_waiter();
    if (result == 0)
        result = test_ready_mod_wakes_waiter();
    if (result == 0)
        result = test_listen_wait_snapshot_ignores_add();
    if (result == 0)
        result = test_listen_wait_snapshot_survives_delete();
    if (result == 0)
        result = test_listen_wait_snapshot_survives_cleanup();
    if (result == 0)
        result = test_destroy_wakes_active_waiter();
    if (result == 0)
        result = test_destroy_wakes_all_waiters();
    if (result == 0)
        result = test_destroy_races_with_fd_cleanup();
    if (result == 0)
        result = test_epoll_ctl_retains_both_fds();
    if (result == 0)
        result = test_epoll_wait_retains_epoll_fd();
    if (result == 0)
        result = test_real_fd_cleanup();
    if (result == 0)
        result = test_real_fd_registration_error();

    current = NULL;
    pthread_mutex_destroy(&task.waiting_cond_lock.m);
    pthread_mutex_destroy(&sighand.lock.m);
    return result;
}
