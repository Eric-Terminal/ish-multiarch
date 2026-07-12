#include <assert.h>
#include <limits.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "guest/aarch64/linux-signal-abi.h"
#include "guest/aarch64/linux-time-abi.h"
#include "guest/memory/address-space.h"
#include "kernel/aarch64-syscall-service.h"
#include "kernel/errno.h"
#include "kernel/signal.h"
#include "kernel/task.h"

#define USER_BASE UINT64_C(0x00007abc12340000)
#define USER_MEMORY_SIZE 192
#define REQUEST_ADDRESS (USER_BASE + 3)
#define REMAINING_ADDRESS (USER_BASE + 99)

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, \
                "AArch64 nanosleep 系统调用测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        return 1; \
    } \
} while (0)

struct user_probe {
    byte_t bytes[USER_MEMORY_SIZE];
    qword_t fail_read_at;
    qword_t fail_write_at;
    unsigned reads;
    unsigned writes;
    qword_t last_read_address;
    qword_t last_write_address;
    dword_t last_read_size;
    dword_t last_write_size;
};

struct signal_fixture {
    struct task task;
    struct tgroup group;
    struct sighand sighand;
};

struct signal_sender {
    struct task *task;
    int signal;
    atomic_bool cancel;
    atomic_bool sent;
};

static bool range_contains(
        qword_t address, dword_t size, qword_t target) {
    return target >= address && target - address < size;
}

static bool probe_range(
        qword_t address, dword_t size, size_t *offset) {
    if (address < USER_BASE ||
            address - USER_BASE > USER_MEMORY_SIZE ||
            size > USER_MEMORY_SIZE - (address - USER_BASE))
        return false;
    *offset = (size_t) (address - USER_BASE);
    return true;
}

static bool read_user(void *opaque, qword_t address,
        void *destination, dword_t size,
        struct guest_linux_user_fault *fault) {
    struct user_probe *probe = opaque;
    probe->reads++;
    probe->last_read_address = address;
    probe->last_read_size = size;

    size_t offset;
    if (!probe_range(address, size, &offset)) {
        *fault = (struct guest_linux_user_fault) {
            .address = address,
            .access = GUEST_MEMORY_READ,
            .kind = GUEST_MEMORY_FAULT_UNMAPPED,
        };
        return false;
    }

    if (probe->fail_read_at != UINT64_MAX &&
            range_contains(address, size, probe->fail_read_at)) {
        dword_t prefix = (dword_t) (probe->fail_read_at - address);
        memcpy(destination, probe->bytes + offset, prefix);
        *fault = (struct guest_linux_user_fault) {
            .address = probe->fail_read_at,
            .access = GUEST_MEMORY_READ,
            .kind = GUEST_MEMORY_FAULT_UNMAPPED,
        };
        return false;
    }

    memcpy(destination, probe->bytes + offset, size);
    return true;
}

static bool write_user(void *opaque, qword_t address,
        const void *source, dword_t size,
        struct guest_linux_user_fault *fault) {
    struct user_probe *probe = opaque;
    probe->writes++;
    probe->last_write_address = address;
    probe->last_write_size = size;

    size_t offset;
    if (!probe_range(address, size, &offset)) {
        *fault = (struct guest_linux_user_fault) {
            .address = address,
            .access = GUEST_MEMORY_WRITE,
            .kind = GUEST_MEMORY_FAULT_UNMAPPED,
        };
        return false;
    }

    if (probe->fail_write_at != UINT64_MAX &&
            range_contains(address, size, probe->fail_write_at)) {
        dword_t prefix = (dword_t) (probe->fail_write_at - address);
        memcpy(probe->bytes + offset, source, prefix);
        *fault = (struct guest_linux_user_fault) {
            .address = probe->fail_write_at,
            .access = GUEST_MEMORY_WRITE,
            .kind = GUEST_MEMORY_FAULT_UNMAPPED,
        };
        return false;
    }

    memcpy(probe->bytes + offset, source, size);
    return true;
}

static void reset_probe(struct user_probe *probe) {
    memset(probe, 0, sizeof(*probe));
    probe->fail_read_at = UINT64_MAX;
    probe->fail_write_at = UINT64_MAX;
}

static size_t probe_offset(qword_t address) {
    assert(address >= USER_BASE &&
            address - USER_BASE <= USER_MEMORY_SIZE -
                    sizeof(struct aarch64_linux_timespec));
    return (size_t) (address - USER_BASE);
}

static void store_timespec(struct user_probe *probe, qword_t address,
        sqword_t sec, sqword_t nsec) {
    const struct aarch64_linux_timespec wire = {
        .sec = sec,
        .nsec = nsec,
    };
    memcpy(probe->bytes + probe_offset(address), &wire, sizeof(wire));
}

static struct aarch64_linux_timespec load_timespec(
        const struct user_probe *probe, qword_t address) {
    struct aarch64_linux_timespec wire;
    memcpy(&wire, probe->bytes + probe_offset(address), sizeof(wire));
    return wire;
}

static void fill_timespec(
        struct user_probe *probe, qword_t address, byte_t value) {
    memset(probe->bytes + probe_offset(address),
            value, sizeof(struct aarch64_linux_timespec));
}

static bool timespec_bytes_equal(const struct user_probe *probe,
        qword_t address, byte_t value) {
    size_t offset = probe_offset(address);
    for (size_t index = 0;
            index < sizeof(struct aarch64_linux_timespec); index++) {
        if (probe->bytes[offset + index] != value)
            return false;
    }
    return true;
}

static qword_t invoke(struct signal_fixture *fixture,
        struct user_probe *probe, struct guest_linux_user_fault *fault,
        qword_t request_address, qword_t remaining_address) {
    const struct guest_linux_syscall_context context = {
        .task_opaque = &fixture->task,
        .user = {
            .opaque = probe,
            .read = read_user,
            .write = write_user,
        },
    };
    const struct guest_linux_syscall syscall = {
        .number = 101,
        .arguments = {request_address, remaining_address},
    };
    return ish_aarch64_linux_syscall_service.dispatch(
            &context, &syscall, fault);
}

static void queue_signal(struct task *task, int signal) {
    send_signal(task, signal, (struct siginfo_) {
        .code = SI_USER_,
        .payload_kind = SIGNAL_INFO_PAYLOAD_KILL,
        .kill = {.pid = 1234, .uid = 5678},
    });
}

static void clear_pending(struct task *task) {
    struct sigqueue *queued, *temporary;
    list_for_each_entry_safe(&task->queue,
            queued, temporary, queue) {
        list_remove(&queued->queue);
        free(queued);
    }
    task->pending = 0;
}

static void *send_when_waiting(void *opaque) {
    struct signal_sender *sender = opaque;
    while (!atomic_load_explicit(
            &sender->cancel, memory_order_acquire)) {
        lock(&sender->task->waiting_cond_lock);
        bool waiting = sender->task->waiting_cond == &sender->task->pause;
        unlock(&sender->task->waiting_cond_lock);
        if (waiting) {
            queue_signal(sender->task, sender->signal);
            atomic_store_explicit(
                    &sender->sent, true, memory_order_release);
            return NULL;
        }
        sched_yield();
    }
    return NULL;
}

static void *notify_when_waiting(void *opaque) {
    struct signal_sender *sender = opaque;
    while (!atomic_load_explicit(
            &sender->cancel, memory_order_acquire)) {
        lock(&sender->task->waiting_cond_lock);
        bool waiting = sender->task->waiting_cond == &sender->task->pause;
        unlock(&sender->task->waiting_cond_lock);
        if (waiting) {
            // 取得同一状态锁后再通知，确保通知发生在条件等待已经登记之后。
            lock(&sender->task->sighand->lock);
            notify(&sender->task->pause);
            unlock(&sender->task->sighand->lock);
            atomic_store_explicit(
                    &sender->sent, true, memory_order_release);
            return NULL;
        }
        sched_yield();
    }
    return NULL;
}

static void init_fixture(struct signal_fixture *fixture) {
    *fixture = (struct signal_fixture) {0};
    list_init(&fixture->task.queue);
    list_init(&fixture->task.group_links);
    list_init(&fixture->group.threads);
    fixture->task.sighand = &fixture->sighand;
    fixture->task.group = &fixture->group;
    fixture->group.leader = &fixture->task;
    list_add_tail(&fixture->group.threads,
            &fixture->task.group_links);

    atomic_init(&fixture->sighand.refcount, 1);
    lock_init(&fixture->sighand.lock);
    lock_init(&fixture->group.lock);
    cond_init(&fixture->group.stopped_cond);
    lock_init(&fixture->task.waiting_cond_lock);
    cond_init(&fixture->task.pause);
    lock_init(&fixture->task.ptrace.lock);
    cond_init(&fixture->task.ptrace.cond);
    task_thread_store(&fixture->task, pthread_self());
    current = &fixture->task;
}

static void destroy_fixture(struct signal_fixture *fixture) {
    clear_pending(&fixture->task);
    current = NULL;
    cond_destroy(&fixture->task.ptrace.cond);
    pthread_mutex_destroy(&fixture->task.ptrace.lock.m);
    cond_destroy(&fixture->task.pause);
    pthread_mutex_destroy(&fixture->task.waiting_cond_lock.m);
    cond_destroy(&fixture->group.stopped_cond);
    pthread_mutex_destroy(&fixture->group.lock.m);
    pthread_mutex_destroy(&fixture->sighand.lock.m);
}

static int test_request_read_failures(
        struct signal_fixture *fixture) {
    struct user_probe probe;
    struct guest_linux_user_fault fault;
    reset_probe(&probe);

    const qword_t wrapping = UINT64_MAX - 7;
    qword_t result = invoke(fixture, &probe, &fault, wrapping, 0);
    CHECK(result == (qword_t) (sqword_t) _EFAULT &&
            probe.reads == 0 && probe.writes == 0 &&
            fault.address == wrapping &&
            fault.access == GUEST_MEMORY_READ &&
            fault.kind == GUEST_MEMORY_FAULT_ADDRESS_SIZE,
            "请求地址回绕时不进入用户内存回调");

    const qword_t crossing_limit = AARCH64_LINUX_USER_ADDRESS_MAX - 7;
    result = invoke(fixture, &probe, &fault, crossing_limit, 0);
    CHECK(result == (qword_t) (sqword_t) _EFAULT &&
            probe.reads == 0 && probe.writes == 0 &&
            fault.address == crossing_limit &&
            fault.access == GUEST_MEMORY_READ &&
            fault.kind == GUEST_MEMORY_FAULT_ADDRESS_SIZE,
            "请求跨越 AArch64 用户地址上限时提前失败");

    reset_probe(&probe);
    result = invoke(fixture, &probe, &fault, 0, 0);
    CHECK(result == (qword_t) (sqword_t) _EFAULT &&
            probe.reads == 1 && probe.writes == 0 &&
            fault.address == 0 &&
            fault.access == GUEST_MEMORY_READ &&
            fault.kind == GUEST_MEMORY_FAULT_UNMAPPED,
            "空请求地址由用户内存回调报告精确映射故障");

    reset_probe(&probe);
    store_timespec(&probe, REQUEST_ADDRESS, 0, 1);
    probe.fail_read_at = REQUEST_ADDRESS + 8;
    result = invoke(fixture, &probe, &fault, REQUEST_ADDRESS, 0);
    CHECK(result == (qword_t) (sqword_t) _EFAULT &&
            probe.reads == 1 && probe.writes == 0 &&
            probe.last_read_address == REQUEST_ADDRESS &&
            probe.last_read_size == 16 &&
            fault.address == REQUEST_ADDRESS + 8 &&
            fault.access == GUEST_MEMORY_READ &&
            fault.kind == GUEST_MEMORY_FAULT_UNMAPPED,
            "两个有符号 64 位字段必须由一次 16 字节读取完整取得");
    return 0;
}

static int test_invalid_requests(struct signal_fixture *fixture) {
    struct user_probe probe;
    struct guest_linux_user_fault fault;

    reset_probe(&probe);
    store_timespec(&probe, REQUEST_ADDRESS, INT64_MIN, 0);
    qword_t result = invoke(
            fixture, &probe, &fault, REQUEST_ADDRESS, REMAINING_ADDRESS);
    CHECK(result == (qword_t) (sqword_t) _EINVAL &&
            probe.reads == 1 && probe.last_read_size == 16 &&
            probe.writes == 0 && fault.address == 0,
            "负秒数按 signed64 解释并返回 EINVAL");

    reset_probe(&probe);
    store_timespec(&probe, REQUEST_ADDRESS, 0, -1);
    result = invoke(
            fixture, &probe, &fault, REQUEST_ADDRESS, REMAINING_ADDRESS);
    CHECK(result == (qword_t) (sqword_t) _EINVAL &&
            probe.reads == 1 && probe.last_read_size == 16 &&
            probe.writes == 0 && fault.address == 0,
            "负纳秒数按 signed64 解释并返回 EINVAL");

    reset_probe(&probe);
    store_timespec(&probe, REQUEST_ADDRESS,
            0, INT64_C(1000000000));
    result = invoke(
            fixture, &probe, &fault, REQUEST_ADDRESS, REMAINING_ADDRESS);
    CHECK(result == (qword_t) (sqword_t) _EINVAL &&
            probe.reads == 1 && probe.last_read_size == 16 &&
            probe.writes == 0 && fault.address == 0,
            "十亿纳秒不属于规范化 timespec");
    return 0;
}

static int64_t elapsed_nanoseconds(
        struct timespec started, struct timespec finished) {
    return (int64_t) (finished.tv_sec - started.tv_sec) *
            INT64_C(1000000000) + finished.tv_nsec - started.tv_nsec;
}

static int test_success_never_writes_remaining(
        struct signal_fixture *fixture) {
    struct user_probe probe;
    struct guest_linux_user_fault fault;
    fixture->sighand.action[SIGUSR1_] = (struct signal_action) {
        .handler = UINT64_C(0x0000400012345678),
    };

    reset_probe(&probe);
    store_timespec(&probe, REQUEST_ADDRESS, 0, 0);
    fill_timespec(&probe, REMAINING_ADDRESS, 0xa5);
    queue_signal(&fixture->task, SIGUSR1_);
    qword_t result = invoke(
            fixture, &probe, &fault, REQUEST_ADDRESS, REMAINING_ADDRESS);
    CHECK(result == 0 && probe.reads == 1 && probe.writes == 0 &&
            timespec_bytes_equal(&probe, REMAINING_ADDRESS, 0xa5) &&
            sigset_has(fixture->task.pending, SIGUSR1_),
            "零时长在 pending 信号前先成功且不访问 rem");
    clear_pending(&fixture->task);

    reset_probe(&probe);
    store_timespec(&probe, REQUEST_ADDRESS, 0, 5000000);
    fill_timespec(&probe, REMAINING_ADDRESS, 0x5a);
    struct timespec started;
    struct timespec finished;
    clock_gettime(CLOCK_MONOTONIC, &started);
    result = invoke(
            fixture, &probe, &fault, REQUEST_ADDRESS, UINT64_MAX);
    clock_gettime(CLOCK_MONOTONIC, &finished);
    CHECK(result == 0 && probe.reads == 1 &&
            probe.last_read_address == REQUEST_ADDRESS &&
            probe.last_read_size == 16 && probe.writes == 0 &&
            timespec_bytes_equal(&probe, REMAINING_ADDRESS, 0x5a) &&
            elapsed_nanoseconds(started, finished) >= 1000000 &&
            fixture->task.waiting_cond == NULL &&
            fixture->task.waiting_lock == NULL,
            "短请求等待到期成功且成功路径忽略非法 rem");
    return 0;
}

static bool timespec_not_after(
        struct aarch64_linux_timespec time,
        struct aarch64_linux_timespec limit) {
    return time.sec < limit.sec ||
            (time.sec == limit.sec && time.nsec <= limit.nsec);
}

static int test_pending_signal_writes_remaining(
        struct signal_fixture *fixture) {
    const struct aarch64_linux_timespec request = {
        .sec = INT64_MAX,
        .nsec = 0,
    };
    const struct aarch64_linux_timespec ktime_max = {
        .sec = INT64_C(9223372036),
        .nsec = INT64_C(854775807),
    };
    struct user_probe probe;
    struct guest_linux_user_fault fault;
    reset_probe(&probe);
    store_timespec(&probe, REQUEST_ADDRESS, request.sec, request.nsec);
    fill_timespec(&probe, REMAINING_ADDRESS, 0xa5);
    queue_signal(&fixture->task, SIGUSR1_);

    qword_t result = invoke(
            fixture, &probe, &fault, REQUEST_ADDRESS, REMAINING_ADDRESS);
    struct aarch64_linux_timespec remaining =
            load_timespec(&probe, REMAINING_ADDRESS);
    CHECK(result == (qword_t) (sqword_t) _EINTR &&
            probe.reads == 1 && probe.writes == 1 &&
            probe.last_write_address == REMAINING_ADDRESS &&
            probe.last_write_size == 16 &&
            remaining.sec > INT32_MAX && remaining.nsec >= 0 &&
            remaining.nsec < INT64_C(1000000000) &&
            timespec_not_after(remaining, ktime_max) &&
            sigset_has(fixture->task.pending, SIGUSR1_) &&
            fixture->task.waiting_cond == NULL &&
            fixture->task.waiting_lock == NULL,
            "INT64_MAX 秒请求饱和到 KTIME_MAX 并写回未窄化余量");
    clear_pending(&fixture->task);
    return 0;
}

static int test_remaining_write_failures(
        struct signal_fixture *fixture) {
    struct user_probe probe;
    struct guest_linux_user_fault fault;
    reset_probe(&probe);
    store_timespec(&probe, REQUEST_ADDRESS, 60, 0);
    queue_signal(&fixture->task, SIGUSR1_);

    const qword_t wrapping = UINT64_MAX - 7;
    qword_t result = invoke(
            fixture, &probe, &fault, REQUEST_ADDRESS, wrapping);
    CHECK(result == (qword_t) (sqword_t) _EFAULT &&
            probe.reads == 1 && probe.writes == 0 &&
            fault.address == wrapping &&
            fault.access == GUEST_MEMORY_WRITE &&
            fault.kind == GUEST_MEMORY_FAULT_ADDRESS_SIZE,
            "EINTR 写回 rem 地址回绕时不进入写回调");
    clear_pending(&fixture->task);

    reset_probe(&probe);
    store_timespec(&probe, REQUEST_ADDRESS, 60, 0);
    queue_signal(&fixture->task, SIGUSR1_);
    const qword_t crossing_limit = AARCH64_LINUX_USER_ADDRESS_MAX - 7;
    result = invoke(
            fixture, &probe, &fault, REQUEST_ADDRESS, crossing_limit);
    CHECK(result == (qword_t) (sqword_t) _EFAULT &&
            probe.reads == 1 && probe.writes == 0 &&
            fault.address == crossing_limit &&
            fault.access == GUEST_MEMORY_WRITE &&
            fault.kind == GUEST_MEMORY_FAULT_ADDRESS_SIZE,
            "EINTR 写回 rem 跨越用户地址上限时提前失败");
    clear_pending(&fixture->task);

    reset_probe(&probe);
    store_timespec(&probe, REQUEST_ADDRESS, 60, 0);
    fill_timespec(&probe, REMAINING_ADDRESS, 0xa5);
    probe.fail_write_at = REMAINING_ADDRESS + 8;
    queue_signal(&fixture->task, SIGUSR1_);
    result = invoke(fixture, &probe, &fault,
            REQUEST_ADDRESS, REMAINING_ADDRESS);
    struct aarch64_linux_timespec partial =
            load_timespec(&probe, REMAINING_ADDRESS);
    CHECK(result == (qword_t) (sqword_t) _EFAULT &&
            probe.reads == 1 && probe.writes == 1 &&
            probe.last_write_address == REMAINING_ADDRESS &&
            probe.last_write_size == 16 &&
            fault.address == REMAINING_ADDRESS + 8 &&
            fault.access == GUEST_MEMORY_WRITE &&
            fault.kind == GUEST_MEMORY_FAULT_UNMAPPED &&
            partial.sec > 0 && partial.sec <= 60,
            "rem 单次 16 字节写回传播第二字段的部分写故障");
    size_t nsec_offset = probe_offset(REMAINING_ADDRESS) + 8;
    for (size_t index = 0; index < 8; index++) {
        CHECK(probe.bytes[nsec_offset + index] == 0xa5,
                "部分写故障不得改动故障位置之后的纳秒字段");
    }
    clear_pending(&fixture->task);
    return 0;
}

static int test_remaining_destinations(
        struct signal_fixture *fixture) {
    struct user_probe probe;
    struct guest_linux_user_fault fault;

    reset_probe(&probe);
    store_timespec(&probe, REQUEST_ADDRESS, 60, 0);
    queue_signal(&fixture->task, SIGUSR1_);
    qword_t result = invoke(fixture, &probe, &fault, REQUEST_ADDRESS, 0);
    CHECK(result == (qword_t) (sqword_t) _EINTR &&
            probe.reads == 1 && probe.writes == 0 &&
            sigset_has(fixture->task.pending, SIGUSR1_),
            "EINTR 接受空 rem 且保留待投递信号");
    clear_pending(&fixture->task);

    reset_probe(&probe);
    store_timespec(&probe, REQUEST_ADDRESS, 60, 0);
    queue_signal(&fixture->task, SIGUSR1_);
    result = invoke(fixture, &probe, &fault,
            REQUEST_ADDRESS, REQUEST_ADDRESS);
    struct aarch64_linux_timespec remaining =
            load_timespec(&probe, REQUEST_ADDRESS);
    CHECK(result == (qword_t) (sqword_t) _EINTR &&
            probe.reads == 1 && probe.writes == 1 &&
            probe.last_read_address == REQUEST_ADDRESS &&
            probe.last_write_address == REQUEST_ADDRESS &&
            remaining.sec > 0 && remaining.sec <= 60 &&
            remaining.nsec >= 0 &&
            remaining.nsec < INT64_C(1000000000) &&
            sigset_has(fixture->task.pending, SIGUSR1_),
            "req 与 rem 同址时先完整读取再覆盖规范化余量");
    clear_pending(&fixture->task);
    return 0;
}

static int test_spurious_notification_keeps_waiting(
        struct signal_fixture *fixture) {
    struct user_probe probe;
    struct guest_linux_user_fault fault;
    reset_probe(&probe);
    store_timespec(&probe, REQUEST_ADDRESS, 0, 50000000);

    struct signal_sender notifier = {.task = &fixture->task};
    pthread_t notifier_thread;
    CHECK(pthread_create(&notifier_thread,
            NULL, notify_when_waiting, &notifier) == 0,
            "创建伪唤醒线程");
    struct timespec started;
    struct timespec finished;
    clock_gettime(CLOCK_MONOTONIC, &started);
    qword_t result = invoke(fixture, &probe, &fault, REQUEST_ADDRESS, 0);
    clock_gettime(CLOCK_MONOTONIC, &finished);
    atomic_store_explicit(&notifier.cancel, true, memory_order_release);
    CHECK(pthread_join(notifier_thread, NULL) == 0,
            "回收伪唤醒线程");

    CHECK(atomic_load_explicit(&notifier.sent, memory_order_acquire) &&
            result == 0 && probe.reads == 1 && probe.writes == 0 &&
            elapsed_nanoseconds(started, finished) >= 30000000 &&
            fixture->task.waiting_cond == NULL &&
            fixture->task.waiting_lock == NULL,
            "无 pending 信号的条件通知不得缩短睡眠");
    return 0;
}

static int test_signal_after_wait_registration(
        struct signal_fixture *fixture) {
    const struct aarch64_linux_timespec request = {
        .sec = (sqword_t) INT32_MAX + 2,
        .nsec = 0,
    };
    struct user_probe probe;
    struct guest_linux_user_fault fault;
    reset_probe(&probe);
    store_timespec(&probe, REQUEST_ADDRESS, request.sec, request.nsec);
    fill_timespec(&probe, REMAINING_ADDRESS, 0x5a);
    fixture->sighand.action[SIGUSR2_] = (struct signal_action) {
        .handler = UINT64_C(0x0000400087654321),
        .flags = AARCH64_LINUX_SA_RESTART,
    };

    struct signal_sender sender = {
        .task = &fixture->task,
        .signal = SIGUSR2_,
    };
    pthread_t sender_thread;
    CHECK(pthread_create(&sender_thread,
            NULL, send_when_waiting, &sender) == 0,
            "创建等待登记观察线程");
    qword_t result = invoke(
            fixture, &probe, &fault, REQUEST_ADDRESS, REMAINING_ADDRESS);
    atomic_store_explicit(&sender.cancel, true, memory_order_release);
    CHECK(pthread_join(sender_thread, NULL) == 0,
            "回收等待登记观察线程");

    struct aarch64_linux_timespec remaining =
            load_timespec(&probe, REMAINING_ADDRESS);
    CHECK(atomic_load_explicit(&sender.sent, memory_order_acquire) &&
            result == (qword_t) (sqword_t) _EINTR &&
            probe.reads == 1 && probe.writes == 1 &&
            remaining.sec > INT32_MAX &&
            timespec_not_after(remaining, request) &&
            sigset_has(fixture->task.pending, SIGUSR2_) &&
            fixture->task.waiting_cond == NULL &&
            fixture->task.waiting_lock == NULL,
            "SA_RESTART 信号在等待登记后可靠唤醒并返回 EINTR 余量");
    clear_pending(&fixture->task);
    return 0;
}

static int test_blocked_pending_does_not_interrupt(
        struct signal_fixture *fixture) {
    struct user_probe probe;
    struct guest_linux_user_fault fault;
    reset_probe(&probe);
    store_timespec(&probe, REQUEST_ADDRESS, 0, 5000000);
    fill_timespec(&probe, REMAINING_ADDRESS, 0xa5);
    fixture->task.blocked = sig_mask(SIGUSR1_);
    queue_signal(&fixture->task, SIGUSR1_);

    struct timespec started;
    struct timespec finished;
    clock_gettime(CLOCK_MONOTONIC, &started);
    qword_t result = invoke(
            fixture, &probe, &fault, REQUEST_ADDRESS, REMAINING_ADDRESS);
    clock_gettime(CLOCK_MONOTONIC, &finished);
    CHECK(result == 0 && probe.reads == 1 && probe.writes == 0 &&
            timespec_bytes_equal(&probe, REMAINING_ADDRESS, 0xa5) &&
            elapsed_nanoseconds(started, finished) >= 1000000 &&
            sigset_has(fixture->task.pending, SIGUSR1_) &&
            fixture->task.waiting_cond == NULL &&
            fixture->task.waiting_lock == NULL,
            "已阻塞的 pending 信号不打断短睡眠");
    fixture->task.blocked = 0;
    clear_pending(&fixture->task);
    return 0;
}

int main(void) {
    struct sigaction ignore = {.sa_handler = SIG_IGN};
    struct sigaction previous;
    sigemptyset(&ignore.sa_mask);
    CHECK(sigaction(SIGUSR1, &ignore, &previous) == 0,
            "忽略测试使用的 host 唤醒信号");
    alarm(15);

    struct signal_fixture fixture;
    init_fixture(&fixture);
    int result = test_request_read_failures(&fixture);
    if (result == 0)
        result = test_invalid_requests(&fixture);
    if (result == 0)
        result = test_success_never_writes_remaining(&fixture);
    if (result == 0)
        result = test_pending_signal_writes_remaining(&fixture);
    if (result == 0)
        result = test_remaining_write_failures(&fixture);
    if (result == 0)
        result = test_remaining_destinations(&fixture);
    if (result == 0)
        result = test_spurious_notification_keeps_waiting(&fixture);
    if (result == 0)
        result = test_signal_after_wait_registration(&fixture);
    if (result == 0)
        result = test_blocked_pending_does_not_interrupt(&fixture);
    destroy_fixture(&fixture);

    alarm(0);
    CHECK(sigaction(SIGUSR1, &previous, NULL) == 0,
            "恢复 host 唤醒信号动作");
    return result;
}
