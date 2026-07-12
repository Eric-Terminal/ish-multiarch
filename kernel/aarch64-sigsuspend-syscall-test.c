#include <assert.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "guest/aarch64/linux-signal-abi.h"
#include "guest/memory/address-space.h"
#include "kernel/aarch64-signal-service.h"
#include "kernel/aarch64-syscall-service.h"
#include "kernel/errno.h"
#include "kernel/signal.h"
#include "kernel/task.h"

#define USER_BASE UINT64_C(0x00007abc12340000)
#define USER_MEMORY_SIZE 64

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, \
                "AArch64 sigsuspend 系统调用测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        return 1; \
    } \
} while (0)

struct user_probe {
    byte_t bytes[USER_MEMORY_SIZE];
    qword_t fail_read_at;
    unsigned reads;
    qword_t last_address;
    dword_t last_size;
};

struct signal_fixture {
    struct task task;
    struct tgroup group;
    struct sighand sighand;
};

struct signal_sender {
    struct task *task;
    atomic_bool cancel;
    atomic_bool sent;
};

struct delivery_capture {
    unsigned calls;
    struct guest_linux_signal_delivery delivery;
};

static bool range_contains(
        qword_t address, dword_t size, qword_t target) {
    return target >= address && target - address < size;
}

static bool read_user(void *opaque, qword_t address,
        void *destination, dword_t size,
        struct guest_linux_user_fault *fault) {
    struct user_probe *probe = opaque;
    probe->reads++;
    probe->last_address = address;
    probe->last_size = size;

    if (address < USER_BASE ||
            address - USER_BASE > sizeof(probe->bytes) ||
            size > sizeof(probe->bytes) - (address - USER_BASE)) {
        *fault = (struct guest_linux_user_fault) {
            .address = address,
            .access = GUEST_MEMORY_READ,
            .kind = GUEST_MEMORY_FAULT_UNMAPPED,
        };
        return false;
    }

    size_t offset = (size_t) (address - USER_BASE);
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

static void reset_probe(struct user_probe *probe) {
    memset(probe, 0, sizeof(*probe));
    probe->fail_read_at = UINT64_MAX;
}

static void store_sigset(
        struct user_probe *probe, qword_t address, sigset_t_ set) {
    assert(address >= USER_BASE &&
            address - USER_BASE <= sizeof(probe->bytes) - sizeof(set));
    size_t offset = (size_t) (address - USER_BASE);
    memcpy(probe->bytes + offset, &set, sizeof(set));
}

static qword_t invoke(struct signal_fixture *fixture,
        struct user_probe *probe, struct guest_linux_user_fault *fault,
        qword_t address, qword_t size) {
    const struct guest_linux_syscall_context context = {
        .task_opaque = &fixture->task,
        .user = {
            .opaque = probe,
            .read = read_user,
        },
    };
    const struct guest_linux_syscall syscall = {
        .number = 133,
        .arguments = {address, size},
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

static enum guest_linux_signal_install_status capture_delivery(
        void *opaque,
        const struct guest_linux_signal_delivery *delivery) {
    struct delivery_capture *capture = opaque;
    capture->calls++;
    capture->delivery = *delivery;
    return GUEST_LINUX_SIGNAL_INSTALL_COMPLETE;
}

static struct guest_linux_signal_poll_result poll_signal(
        struct signal_fixture *fixture,
        struct delivery_capture *capture) {
    const struct guest_linux_signal_context context = {
        .task_opaque = &fixture->task,
    };
    return ish_aarch64_linux_signal_service.poll(
            &context, capture_delivery, capture);
}

static void *send_when_waiting(void *opaque) {
    struct signal_sender *sender = opaque;
    while (!atomic_load_explicit(
            &sender->cancel, memory_order_acquire)) {
        lock(&sender->task->waiting_cond_lock);
        bool waiting = sender->task->waiting_cond == &sender->task->pause;
        unlock(&sender->task->waiting_cond_lock);
        if (waiting) {
            queue_signal(sender->task, SIGUSR2_);
            atomic_store_explicit(
                    &sender->sent, true, memory_order_release);
            return NULL;
        }
        sched_yield();
    }
    return NULL;
}

static void clear_pending(struct task *task) {
    signal_flush_pending(task);
}

static void init_fixture(struct signal_fixture *fixture) {
    *fixture = (struct signal_fixture) {0};
    list_init(&fixture->task.queue);
    list_init(&fixture->group.threads);
    fixture->task.sighand = &fixture->sighand;
    fixture->task.group = &fixture->group;
    fixture->group.leader = &fixture->task;
    list_add_tail(&fixture->group.threads,
            &fixture->task.group_links);

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

static int test_argument_failures(struct signal_fixture *fixture) {
    const qword_t address = USER_BASE + 3;
    struct user_probe probe;
    struct guest_linux_user_fault fault;
    reset_probe(&probe);
    fixture->task.blocked = sig_mask(SIGUSR1_);

    qword_t result = invoke(fixture, &probe, &fault,
            UINT64_MAX, UINT64_C(0x100000008));
    CHECK(result == (qword_t) (sqword_t) _EINVAL &&
            probe.reads == 0 && fault.address == 0 &&
            fixture->task.blocked == sig_mask(SIGUSR1_) &&
            !fixture->task.has_saved_mask,
            "完整 64 位 size 错误优先且不读取掩码");

    qword_t wrapping = UINT64_MAX - sizeof(sigset_t_) + 2;
    result = invoke(fixture, &probe, &fault,
            wrapping, sizeof(sigset_t_));
    CHECK(result == (qword_t) (sqword_t) _EFAULT &&
            probe.reads == 0 && fault.address == wrapping &&
            fault.access == GUEST_MEMORY_READ &&
            fault.kind == GUEST_MEMORY_FAULT_ADDRESS_SIZE &&
            fixture->task.blocked == sig_mask(SIGUSR1_) &&
            !fixture->task.has_saved_mask,
            "64 位地址回绕在回调和状态变更前失败");

    qword_t crossing_user_limit =
            AARCH64_LINUX_USER_ADDRESS_MAX - 3;
    result = invoke(fixture, &probe, &fault,
            crossing_user_limit, sizeof(sigset_t_));
    CHECK(result == (qword_t) (sqword_t) _EFAULT &&
            probe.reads == 0 &&
            fault.address == crossing_user_limit &&
            fault.access == GUEST_MEMORY_READ &&
            fault.kind == GUEST_MEMORY_FAULT_ADDRESS_SIZE &&
            fixture->task.blocked == sig_mask(SIGUSR1_) &&
            !fixture->task.has_saved_mask,
            "跨越 AArch64 用户地址上限时不进入内存回调");

    reset_probe(&probe);
    store_sigset(&probe, address, sig_mask(SIGUSR2_));
    probe.fail_read_at = address + 4;
    result = invoke(fixture, &probe, &fault,
            address, sizeof(sigset_t_));
    CHECK(result == (qword_t) (sqword_t) _EFAULT &&
            probe.reads == 1 && probe.last_address == address &&
            probe.last_size == sizeof(sigset_t_) &&
            fault.address == address + 4 &&
            fault.access == GUEST_MEMORY_READ &&
            fault.kind == GUEST_MEMORY_FAULT_UNMAPPED &&
            fixture->task.blocked == sig_mask(SIGUSR1_) &&
            !fixture->task.has_saved_mask,
            "高位未对齐掩码单次读取并传播部分故障");
    return 0;
}

static int test_already_pending(struct signal_fixture *fixture) {
    const qword_t address = USER_BASE + 3;
    const sigset_t_ high_signal_bit = sig_mask(NUM_SIGS);
    const sigset_t_ original =
            sig_mask(SIGCHLD_) | sig_mask(SIGUSR1_);
    const sigset_t_ requested = high_signal_bit |
            sig_mask(SIGKILL_) | sig_mask(SIGSTOP_);
    struct user_probe probe;
    struct guest_linux_user_fault fault;
    reset_probe(&probe);
    store_sigset(&probe, address, requested);

    fixture->task.blocked = original;
    fixture->sighand.action[SIGCHLD_] = (struct signal_action) {
        .handler = UINT64_C(0x0000400012345678),
    };
    queue_signal(&fixture->task, SIGCHLD_);
    qword_t result = invoke(fixture, &probe, &fault,
            address, sizeof(sigset_t_));
    CHECK(result == (qword_t) (sqword_t) _EINTR &&
            probe.reads == 1 && probe.last_address == address &&
            probe.last_size == sizeof(sigset_t_) &&
            fixture->task.saved_mask == original &&
            fixture->task.has_saved_mask &&
            fixture->task.blocked == high_signal_bit &&
            sigset_has(fixture->task.pending, SIGCHLD_) &&
            fixture->task.waiting_cond == NULL,
            "已 pending 信号立即中断并保留待投递恢复状态");

    struct delivery_capture capture = {0};
    struct guest_linux_signal_poll_result polled =
            poll_signal(fixture, &capture);
    CHECK(polled.status == GUEST_LINUX_SIGNAL_POLL_HANDLER &&
            polled.signal == SIGCHLD_ && capture.calls == 1 &&
            capture.delivery.info.signal == SIGCHLD_ &&
            capture.delivery.blocked_mask == original &&
            !fixture->task.has_saved_mask &&
            fixture->task.pending == 0 &&
            list_empty(&fixture->task.queue),
            "信号轮询恢复旧掩码并把它写入 handler 帧状态");
    fixture->task.blocked = 0;
    return 0;
}

static int test_signal_after_wait_registration(
        struct signal_fixture *fixture) {
    const qword_t address = USER_BASE + 5;
    const sigset_t_ high_signal_bit = sig_mask(NUM_SIGS);
    const sigset_t_ original = sig_mask(SIGUSR2_);
    struct user_probe probe;
    struct guest_linux_user_fault fault;
    reset_probe(&probe);
    store_sigset(&probe, address, high_signal_bit |
            sig_mask(SIGUSR1_) |
            sig_mask(SIGKILL_) | sig_mask(SIGSTOP_));
    fixture->task.blocked = original;
    fixture->sighand.action[SIGUSR2_] = (struct signal_action) {
        .handler = UINT64_C(0x0000400087654321),
    };
    queue_signal(&fixture->task, SIGUSR1_);

    struct signal_sender sender = {.task = &fixture->task};
    pthread_t sender_thread;
    CHECK(pthread_create(&sender_thread,
            NULL, send_when_waiting, &sender) == 0,
            "创建等待期信号发送线程");
    qword_t result = invoke(fixture, &probe, &fault,
            address, sizeof(sigset_t_));
    atomic_store_explicit(&sender.cancel, true, memory_order_release);
    CHECK(pthread_join(sender_thread, NULL) == 0,
            "回收等待期信号发送线程");
    CHECK(atomic_load_explicit(&sender.sent, memory_order_acquire) &&
            result == (qword_t) (sqword_t) _EINTR &&
            fixture->task.saved_mask == original &&
            fixture->task.has_saved_mask &&
            fixture->task.blocked ==
                    (high_signal_bit | sig_mask(SIGUSR1_)) &&
            sigset_has(fixture->task.pending, SIGUSR1_) &&
            sigset_has(fixture->task.pending, SIGUSR2_) &&
            fixture->task.waiting_cond == NULL &&
            fixture->task.waiting_lock == NULL,
            "登记条件变量后到达的信号可靠唤醒等待者");

    struct delivery_capture capture = {0};
    struct guest_linux_signal_poll_result polled =
            poll_signal(fixture, &capture);
    CHECK(polled.status == GUEST_LINUX_SIGNAL_POLL_HANDLER &&
            polled.signal == SIGUSR2_ && capture.calls == 1 &&
            capture.delivery.blocked_mask == original &&
            !fixture->task.has_saved_mask &&
            fixture->task.pending == sig_mask(SIGUSR1_) &&
            list_size(&fixture->task.queue) == 1,
            "异步唤醒只消费临时掩码允许的信号");
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
    int result = test_argument_failures(&fixture);
    if (result == 0)
        result = test_already_pending(&fixture);
    if (result == 0)
        result = test_signal_after_wait_registration(&fixture);
    destroy_fixture(&fixture);

    alarm(0);
    CHECK(sigaction(SIGUSR1, &previous, NULL) == 0,
            "恢复 host 唤醒信号动作");
    return result;
}
