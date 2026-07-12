#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "fs/fd.h"
#include "fs/poll.h"
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/mm.h"
#include "kernel/resource.h"
#include "kernel/task.h"
#include "kernel/time.h"

#define USER_PAGE UINT32_C(0x00100000)
#define READFDS_ADDRESS (USER_PAGE + 16)
#define WRITEFDS_ADDRESS (USER_PAGE + 24)
#define TIMEOUT_ADDRESS (USER_PAGE + 32)
#define UNMAPPED_ADDRESS UINT32_C(0x00200000)

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "select 生命周期测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        return 1; \
    } \
} while (0)

struct select_fixture {
    struct task task;
    struct tgroup group;
    struct sighand sighand;
};

struct readiness_gate {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    bool entered;
    bool released;
};

struct fd_probe {
    struct readiness_gate *gate;
    int poll_events;
    atomic_uint close_calls;
};

struct select_call {
    struct task *task;
    bool use_poll;
    fd_t nfds;
    addr_t readfds_address;
    addr_t timeout_address;
    int poll_timeout;
    sdword_t result;
};

static void host_wakeup_handler(int signal) {
    (void) signal;
}

static int map_user_page(struct task *task) {
    write_wrlock(&task->mem->lock);
    int error = pt_map_nothing(
            task->mem, PAGE(USER_PAGE), 1, P_RWX);
    write_wrunlock(&task->mem->lock);
    return error;
}

static bool fixture_init(struct select_fixture *fixture) {
    memset(fixture, 0, sizeof(*fixture));
    lock_init(&fixture->group.lock);
    fixture->group.limits[RLIMIT_NOFILE_] =
            (struct rlimit_) {4, 4};
    fixture->task.group = &fixture->group;
    fixture->task.files = fdtable_new(1);
    if (IS_ERR(fixture->task.files)) {
        pthread_mutex_destroy(&fixture->group.lock.m);
        return false;
    }

    atomic_init(&fixture->sighand.refcount, 1);
    lock_init(&fixture->sighand.lock);
    fixture->task.sighand = &fixture->sighand;
    list_init(&fixture->task.queue);
    lock_init(&fixture->task.waiting_cond_lock);
    fixture->task.waiting_poll_notify_fd = -1;
    struct mm *mm = mm_new();
    if (mm == NULL) {
        fdtable_release(fixture->task.files);
        pthread_mutex_destroy(&fixture->task.waiting_cond_lock.m);
        pthread_mutex_destroy(&fixture->sighand.lock.m);
        pthread_mutex_destroy(&fixture->group.lock.m);
        return false;
    }
    task_set_mm(&fixture->task, mm);
    if (map_user_page(&fixture->task) != 0) {
        fdtable_release(fixture->task.files);
        mm_release(fixture->task.mm);
        pthread_mutex_destroy(&fixture->task.waiting_cond_lock.m);
        pthread_mutex_destroy(&fixture->sighand.lock.m);
        pthread_mutex_destroy(&fixture->group.lock.m);
        return false;
    }
    current = &fixture->task;
    return true;
}

static void fixture_destroy(struct select_fixture *fixture) {
    current = NULL;
    fdtable_release(fixture->task.files);
    mm_release(fixture->task.mm);
    pthread_mutex_destroy(&fixture->task.waiting_cond_lock.m);
    pthread_mutex_destroy(&fixture->sighand.lock.m);
    pthread_mutex_destroy(&fixture->group.lock.m);
}

static void readiness_gate_init(struct readiness_gate *gate) {
    memset(gate, 0, sizeof(*gate));
    pthread_mutex_init(&gate->lock, NULL);
    pthread_cond_init(&gate->cond, NULL);
}

static void readiness_gate_release(struct readiness_gate *gate) {
    pthread_mutex_lock(&gate->lock);
    gate->released = true;
    pthread_cond_broadcast(&gate->cond);
    pthread_mutex_unlock(&gate->lock);
}

static void readiness_gate_destroy(struct readiness_gate *gate) {
    pthread_cond_destroy(&gate->cond);
    pthread_mutex_destroy(&gate->lock);
}

static bool readiness_gate_wait_until_entered(
        struct readiness_gate *gate) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 2;

    pthread_mutex_lock(&gate->lock);
    int error = 0;
    while (!gate->entered && error == 0)
        error = pthread_cond_timedwait(
                &gate->cond, &gate->lock, &deadline);
    bool entered = gate->entered;
    pthread_mutex_unlock(&gate->lock);
    return entered;
}

static int probe_poll(struct fd *fd) {
    struct fd_probe *probe = fd->data;
    if (probe->gate == NULL)
        return probe->poll_events;

    pthread_mutex_lock(&probe->gate->lock);
    probe->gate->entered = true;
    pthread_cond_broadcast(&probe->gate->cond);
    while (!probe->gate->released)
        pthread_cond_wait(&probe->gate->cond, &probe->gate->lock);
    pthread_mutex_unlock(&probe->gate->lock);
    return POLL_READ;
}

static struct fd *make_fd(struct fd_probe *probe);

static int test_ready_set_count(struct select_fixture *fixture) {
    struct fd_probe probe = {.poll_events = POLL_ERR};
    atomic_init(&probe.close_calls, 0);
    struct fd *fd = make_fd(&probe);
    CHECK(fd != NULL && f_install_task(&fixture->task, fd, 0) == 0,
            "安装就绪集合计数测试文件对象");

    byte_t selected = 1;
    byte_t unselected = 0;
    struct timeval_ immediate = {0};
    CHECK(user_put(READFDS_ADDRESS, selected) == 0 &&
            user_put(WRITEFDS_ADDRESS, unselected) == 0 &&
            user_put(TIMEOUT_ADDRESS, immediate) == 0,
            "准备仅请求读位的双指针 select 参数");
    CHECK((sdword_t) sys_select(1, READFDS_ADDRESS,
                    WRITEFDS_ADDRESS, 0, TIMEOUT_ADDRESS) == 1,
            "POLLERR 只计入实际请求的读位");
    byte_t ready_read;
    byte_t ready_write;
    CHECK(user_get(READFDS_ADDRESS, ready_read) == 0 &&
            user_get(WRITEFDS_ADDRESS, ready_write) == 0 &&
            ready_read == 1 && ready_write == 0,
            "未请求的写位保持未就绪");

    CHECK(user_put(READFDS_ADDRESS, unselected) == 0 &&
            user_put(WRITEFDS_ADDRESS, selected) == 0,
            "准备仅请求写位的双指针 select 参数");
    CHECK((sdword_t) sys_select(1, READFDS_ADDRESS,
                    WRITEFDS_ADDRESS, 0, TIMEOUT_ADDRESS) == 1,
            "POLLERR 只计入实际请求的写位");
    CHECK(user_get(READFDS_ADDRESS, ready_read) == 0 &&
            user_get(WRITEFDS_ADDRESS, ready_write) == 0 &&
            ready_read == 0 && ready_write == 1,
            "未请求的读位保持未就绪");

    probe.poll_events = POLL_READ | POLL_WRITE;
    CHECK(user_put(READFDS_ADDRESS, selected) == 0 &&
            user_put(WRITEFDS_ADDRESS, selected) == 0,
            "准备同一 fd 的双集合 select 参数");
    CHECK((sdword_t) sys_select(1, READFDS_ADDRESS,
                    WRITEFDS_ADDRESS, 0, TIMEOUT_ADDRESS) == 2,
            "同一 fd 的读写命中分别计入 select 返回值");
    CHECK(f_close_task(&fixture->task, 0) == 0 &&
            atomic_load_explicit(&probe.close_calls,
                    memory_order_relaxed) == 1,
            "释放就绪集合计数测试文件对象");
    return 0;
}

static int probe_close(struct fd *fd) {
    struct fd_probe *probe = fd->data;
    atomic_fetch_add_explicit(
            &probe->close_calls, 1, memory_order_relaxed);
    return 0;
}

static const struct fd_ops probe_ops = {
    .poll = probe_poll,
    .close = probe_close,
};

static struct fd *make_fd(struct fd_probe *probe) {
    struct fd *fd = fd_create(&probe_ops);
    if (fd != NULL)
        fd->data = probe;
    return fd;
}

static void *run_select(void *opaque) {
    struct select_call *call = opaque;
    current = call->task;
    task_thread_store(call->task, pthread_self());
    if (call->use_poll)
        call->result = (sdword_t) sys_poll(
                0, 0, call->poll_timeout);
    else
        call->result = (sdword_t) sys_select(
                call->nfds, call->readfds_address,
                0, 0, call->timeout_address);
    current = NULL;
    return NULL;
}

static bool wait_for_poll_registration(
        struct task *task, int *notify_fd) {
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += 2;

    while (true) {
        lock(&task->waiting_cond_lock);
        bool active = task->waiting_poll_active;
        int fd = task->waiting_poll_notify_fd;
        unlock(&task->waiting_cond_lock);
        if (active) {
            if (notify_fd != NULL)
                *notify_fd = fd;
            return fd >= 0;
        }

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > deadline.tv_sec ||
                (now.tv_sec == deadline.tv_sec &&
                        now.tv_nsec >= deadline.tv_nsec))
            return false;
        sched_yield();
    }
}

static bool poll_registration_cleared(struct task *task) {
    lock(&task->waiting_cond_lock);
    bool cleared = !task->waiting_poll_active &&
            task->waiting_poll_notify_fd == -1;
    unlock(&task->waiting_cond_lock);
    return cleared;
}

static int64_t elapsed_nanoseconds(
        struct timespec started, struct timespec finished) {
    return (int64_t) (finished.tv_sec - started.tv_sec) *
            INT64_C(1000000000) + finished.tv_nsec - started.tv_nsec;
}

static void queue_guest_signal_from_sender(struct task *task, int signal) {
    struct task *saved = current;
    current = NULL;
    deliver_signal(task, signal, (struct siginfo_) {
        .code = SI_USER_,
        .payload_kind = SIGNAL_INFO_PAYLOAD_KILL,
    });
    current = saved;
}

static int test_atomic_signal_wakeup(struct select_fixture *fixture) {
    const struct timeval_ long_timeout = {.sec = 2};
    CHECK(user_put(TIMEOUT_ADDRESS, long_timeout) == 0,
            "准备原子信号唤醒超时");

    for (unsigned use_poll = 0; use_poll < 2; use_poll++) {
        fixture->task.blocked = 0;
        signal_flush_pending(&fixture->task);
        struct select_call call = {
            .task = &fixture->task,
            .use_poll = use_poll != 0,
            .nfds = 0,
            .timeout_address = TIMEOUT_ADDRESS,
            .poll_timeout = 2000,
        };
        pthread_t thread;
        CHECK(pthread_create(&thread, NULL, run_select, &call) == 0,
                "启动独占 poll 信号等待线程");
        int notify_fd = -1;
        bool active = wait_for_poll_registration(
                &fixture->task, &notify_fd);
        if (active)
            queue_guest_signal_from_sender(
                    &fixture->task, SIGUSR1_);
        int join_result = pthread_join(thread, NULL);

        CHECK(active && notify_fd >= 0 && join_result == 0 &&
                call.result == _EINTR &&
                sigset_has(fixture->task.pending, SIGUSR1_) &&
                poll_registration_cleared(&fixture->task),
                use_poll ?
                        "poll 在发布 active 后不丢 guest 信号唤醒" :
                        "select 在发布 active 后不丢 guest 信号唤醒");
        signal_flush_pending(&fixture->task);
    }
    return 0;
}

static int test_blocked_and_host_signals(
        struct select_fixture *fixture) {
    const struct timeval_ short_timeout = {.usec = 50000};
    CHECK(user_put(TIMEOUT_ADDRESS, short_timeout) == 0,
            "准备阻塞信号短超时");
    fixture->task.blocked = sig_mask(SIGUSR1_);
    deliver_signal(&fixture->task, SIGUSR1_, (struct siginfo_) {
        .code = SI_USER_,
        .payload_kind = SIGNAL_INFO_PAYLOAD_KILL,
    });

    struct select_call call = {
        .task = &fixture->task,
        .nfds = 0,
        .timeout_address = TIMEOUT_ADDRESS,
    };
    struct timespec started;
    struct timespec finished;
    clock_gettime(CLOCK_MONOTONIC, &started);
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, run_select, &call) == 0,
            "启动阻塞 guest 信号等待线程");
    bool active = wait_for_poll_registration(&fixture->task, NULL);
    unsigned host_interrupts = 0;
    const struct timespec pulse = {.tv_nsec = 1000000};
    while (active && host_interrupts < 200) {
        pthread_kill(task_thread_load(&fixture->task), SIGUSR1);
        host_interrupts++;
        nanosleep(&pulse, NULL);
        lock(&fixture->task.waiting_cond_lock);
        active = fixture->task.waiting_poll_active;
        unlock(&fixture->task.waiting_cond_lock);
    }
    int join_result = pthread_join(thread, NULL);
    clock_gettime(CLOCK_MONOTONIC, &finished);

    CHECK(join_result == 0 && call.result == 0 &&
            host_interrupts != 0 &&
            elapsed_nanoseconds(started, finished) >= INT64_C(40000000) &&
            sigset_has(fixture->task.pending, SIGUSR1_) &&
            poll_registration_cleared(&fixture->task),
            "被 guest 掩码阻塞的 pending 与直接 host SIGUSR1 不产生虚假 EINTR");
    signal_flush_pending(&fixture->task);
    fixture->task.blocked = 0;
    return 0;
}

static int test_ready_precedes_pending(
        struct select_fixture *fixture) {
    struct fd_probe probe = {.poll_events = POLL_READ};
    atomic_init(&probe.close_calls, 0);
    struct fd *fd = make_fd(&probe);
    CHECK(fd != NULL && f_install_task(&fixture->task, fd, 0) == 0,
            "安装 ready 优先测试文件对象");
    byte_t selected = 1;
    const struct timeval_ long_timeout = {.sec = 2};
    CHECK(user_put(READFDS_ADDRESS, selected) == 0 &&
            user_put(TIMEOUT_ADDRESS, long_timeout) == 0,
            "准备 ready 与 pending 并存参数");
    deliver_signal(&fixture->task, SIGUSR1_, (struct siginfo_) {
        .code = SI_USER_,
        .payload_kind = SIGNAL_INFO_PAYLOAD_KILL,
    });

    sdword_t result = (sdword_t) sys_select(
            1, READFDS_ADDRESS, 0, 0, TIMEOUT_ADDRESS);
    byte_t ready = 0;
    CHECK(user_get(READFDS_ADDRESS, ready) == 0 &&
            result == 1 && ready == 1 &&
            sigset_has(fixture->task.pending, SIGUSR1_) &&
            poll_registration_cleared(&fixture->task),
            "fd ready 与 pending 并存时先返回 ready 位");
    signal_flush_pending(&fixture->task);
    CHECK(f_close_task(&fixture->task, 0) == 0 &&
            atomic_load_explicit(&probe.close_calls,
                    memory_order_relaxed) == 1,
            "释放 ready 优先测试文件对象");
    return 0;
}

static int test_argument_bounds(void) {
    struct timeval_ immediate = {0};
    CHECK(user_put(TIMEOUT_ADDRESS, immediate) == 0,
            "写入立即超时参数");
    CHECK((sdword_t) sys_select(-1, 0, 0, 0, 0) == _EINVAL,
            "拒绝负数 nfds");
    CHECK((sdword_t) sys_select(5, 0, 0, 0, 0) == _EINVAL,
            "拒绝超过 NOFILE 限制的 nfds");
    CHECK((sdword_t) sys_select(0, UNMAPPED_ADDRESS,
            UNMAPPED_ADDRESS, UNMAPPED_ADDRESS,
            TIMEOUT_ADDRESS) == 0,
            "nfds 为零时不访问 fd_set 指针");
    return 0;
}

static int test_error_cleanup_balance(struct select_fixture *fixture) {
    struct fd_probe probe = {0};
    atomic_init(&probe.close_calls, 0);
    struct fd *fd = make_fd(&probe);
    CHECK(fd != NULL && f_install_task(&fixture->task, fd, 0) == 0,
            "安装错误清理测试文件对象");

    byte_t selected = 3;
    CHECK(user_put(READFDS_ADDRESS, selected) == 0,
            "选择有效与无效描述符");
    CHECK((sdword_t) sys_select(
            2, READFDS_ADDRESS, 0, 0, TIMEOUT_ADDRESS) == _EBADF,
            "构建中遇到无效描述符返回 EBADF");
    CHECK(atomic_load_explicit(
            &fd->refcount, memory_order_relaxed) == 1,
            "错误退出释放此前取得的独立引用");
    CHECK(f_close_task(&fixture->task, 0) == 0 &&
            atomic_load_explicit(&probe.close_calls,
                    memory_order_relaxed) == 1,
            "错误退出未泄漏文件对象");
    return 0;
}

static int test_close_and_reuse(struct select_fixture *fixture) {
    struct readiness_gate gate;
    readiness_gate_init(&gate);
    struct fd_probe original_probe = {.gate = &gate};
    struct fd_probe replacement_probe = {0};
    atomic_init(&original_probe.close_calls, 0);
    atomic_init(&replacement_probe.close_calls, 0);
    struct fd *original = make_fd(&original_probe);
    struct fd *replacement = make_fd(&replacement_probe);
    CHECK(original != NULL && replacement != NULL &&
            f_install_task(&fixture->task, original, 0) == 0,
            "安装 close/reuse 竞态测试文件对象");

    byte_t selected = 1;
    struct timeval_ immediate = {0};
    CHECK(user_put(READFDS_ADDRESS, selected) == 0 &&
            user_put(TIMEOUT_ADDRESS, immediate) == 0,
            "准备 close/reuse select 参数");
    struct select_call call = {
        .task = &fixture->task,
        .nfds = 1,
        .readfds_address = READFDS_ADDRESS,
        .timeout_address = TIMEOUT_ADDRESS,
    };
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, run_select, &call) == 0,
            "启动 select 等待线程");

    bool entered = readiness_gate_wait_until_entered(&gate);
    bool retained = entered && atomic_load_explicit(
            &original->refcount, memory_order_acquire) == 2;
    int close_result = _EBADF;
    fd_t replacement_number = _EBADF;
    unsigned retained_after_close = 0;
    if (retained) {
        close_result = f_close_task(&fixture->task, 0);
        retained_after_close = atomic_load_explicit(
                &original->refcount, memory_order_acquire);
        replacement_number = f_install_task(
                &fixture->task, replacement, 0);
        replacement = NULL;
    }
    readiness_gate_release(&gate);
    int join_result = pthread_join(thread, NULL);

    byte_t result_set = 0;
    int read_result = user_get(READFDS_ADDRESS, result_set);
    int replacement_close_result = _EBADF;
    if (!retained) {
        f_close_task(&fixture->task, 0);
        fd_close(replacement);
    } else if (replacement_number >= 0) {
        replacement_close_result = f_close_task(
                &fixture->task, replacement_number);
    }
    unsigned original_close_calls = atomic_load_explicit(
            &original_probe.close_calls, memory_order_relaxed);
    unsigned replacement_close_calls = atomic_load_explicit(
            &replacement_probe.close_calls, memory_order_relaxed);
    readiness_gate_destroy(&gate);

    CHECK(entered && retained,
            "select 在就绪回调期间持有独立 fd 引用");
    CHECK(close_result == 0 && retained_after_close == 1 &&
            replacement_number == 0,
            "并发 close 后原对象存活且槽位可安全复用");
    CHECK(join_result == 0 && call.result == 1 &&
            read_result == 0 && bit_test(0, &result_set),
            "close/reuse 期间 select 仍按原对象完成投递");
    CHECK(original_close_calls == 1,
            "select 返回后恰好释放一次原文件对象");
    CHECK(replacement_close_result == 0 && replacement_close_calls == 1,
            "测试结束时释放复用槽位中的替换对象");
    return 0;
}

int main(void) {
    struct sigaction host_action = {0};
    struct sigaction old_host_action;
    host_action.sa_handler = host_wakeup_handler;
    sigemptyset(&host_action.sa_mask);
    CHECK(sigaction(SIGUSR1, &host_action, &old_host_action) == 0,
            "安装 host 唤醒信号处理器");

    struct select_fixture fixture;
    CHECK(fixture_init(&fixture), "初始化 select 生命周期夹具");

    int result = test_argument_bounds();
    if (result == 0)
        result = test_ready_set_count(&fixture);
    if (result == 0)
        result = test_error_cleanup_balance(&fixture);
    if (result == 0)
        result = test_close_and_reuse(&fixture);
    if (result == 0)
        result = test_atomic_signal_wakeup(&fixture);
    if (result == 0)
        result = test_blocked_and_host_signals(&fixture);
    if (result == 0)
        result = test_ready_precedes_pending(&fixture);

    fixture_destroy(&fixture);
    sigaction(SIGUSR1, &old_host_action, NULL);
    return result;
}
