#include <assert.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "guest/aarch64/linux-signal-abi.h"
#include "kernel/aarch64-signal-service.h"
#include "kernel/signal-delivery.h"
#include "kernel/signal.h"
#include "kernel/task.h"

#define HANDLER_ONE UINT64_C(0x0000abcd12345678)
#define HANDLER_TWO UINT64_C(0x0000dcba87654321)

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "AArch64 信号服务测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        return 1; \
    } \
} while (0)

_Static_assert(sizeof(guest_addr_t) == 4,
        "生产信号服务测试必须位于 i386 内核类型域");

struct signal_fixture {
    struct task task;
    struct tgroup group;
    struct sighand sighand;
};

struct installer_probe {
    struct task *task;
    enum guest_linux_signal_install_status responses[3];
    unsigned response_count;
    unsigned calls;
    bool poll_active;
    bool synchronous;
    bool lock_held;
    struct guest_linux_signal_delivery deliveries[3];
};

struct ptrace_controller {
    struct task *task;
    bool observed_stop;
};

static void init_fixture(struct signal_fixture *fixture) {
    *fixture = (struct signal_fixture) {0};

    list_init(&fixture->group.threads);
    list_init(&fixture->group.session);
    list_init(&fixture->group.pgroup);
    lock_init(&fixture->group.lock);
    cond_init(&fixture->group.child_exit);
    cond_init(&fixture->group.stopped_cond);

    atomic_init(&fixture->sighand.refcount, 1);
    lock_init(&fixture->sighand.lock);

    list_init(&fixture->task.group_links);
    list_init(&fixture->task.children);
    list_init(&fixture->task.siblings);
    list_init(&fixture->task.queue);
    cond_init(&fixture->task.pause);
    lock_init(&fixture->task.ptrace.lock);
    cond_init(&fixture->task.ptrace.cond);
    lock_init(&fixture->task.general_lock);
    lock_init(&fixture->task.waiting_cond_lock);

    fixture->task.pid = fixture->task.tgid = 1234;
    fixture->task.group = &fixture->group;
    fixture->task.sighand = &fixture->sighand;
    fixture->group.leader = &fixture->task;
    list_add_tail(&fixture->group.threads,
            &fixture->task.group_links);
    current = &fixture->task;
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

static void destroy_fixture(struct signal_fixture *fixture) {
    clear_pending(&fixture->task);
    cond_destroy(&fixture->task.ptrace.cond);
    pthread_mutex_destroy(&fixture->task.ptrace.lock.m);
    cond_destroy(&fixture->task.pause);
    pthread_mutex_destroy(&fixture->task.waiting_cond_lock.m);
    pthread_mutex_destroy(&fixture->task.general_lock.m);
    pthread_mutex_destroy(&fixture->sighand.lock.m);
    cond_destroy(&fixture->group.stopped_cond);
    cond_destroy(&fixture->group.child_exit);
    pthread_mutex_destroy(&fixture->group.lock.m);
    current = NULL;
}

static void queue_info(struct task *task, struct siginfo_ info) {
    assert(info.sig >= 1 && info.sig <= NUM_SIGS);
    assert(!sigset_has(task->pending, info.sig));
    struct sigqueue *queued = malloc(sizeof(*queued));
    assert(queued != NULL);
    *queued = (struct sigqueue) {.info = info};
    list_add_tail(&task->queue, &queued->queue);
    sigset_add(&task->pending, info.sig);
}

static void queue_signal(struct task *task, int signal) {
    queue_info(task, (struct siginfo_) {
        .sig = signal,
        .code = SI_USER_,
    });
}

static void init_probe(struct installer_probe *probe, struct task *task) {
    *probe = (struct installer_probe) {
        .task = task,
        .synchronous = true,
        .lock_held = true,
    };
}

static enum guest_linux_signal_install_status capture_delivery(
        void *opaque,
        const struct guest_linux_signal_delivery *delivery) {
    struct installer_probe *probe = opaque;
    unsigned call = probe->calls++;
    probe->synchronous = probe->synchronous &&
            probe->poll_active && current == probe->task;

    int lock_result = trylock(&probe->task->sighand->lock);
    probe->lock_held = probe->lock_held && lock_result != 0;
    if (lock_result == 0)
        unlock(&probe->task->sighand->lock);

    if (call < array_size(probe->deliveries))
        probe->deliveries[call] = *delivery;
    if (call < probe->response_count)
        return probe->responses[call];
    return GUEST_LINUX_SIGNAL_INSTALL_COMPLETE;
}

static struct guest_linux_signal_poll_result poll_signal(
        struct signal_fixture *fixture,
        struct installer_probe *probe) {
    const struct guest_linux_signal_context context = {
        .runtime_opaque = ish_aarch64_linux_signal_service.runtime_opaque,
        .task_opaque = &fixture->task,
    };
    current = &fixture->task;
    probe->poll_active = true;
    struct guest_linux_signal_poll_result result =
            ish_aarch64_linux_signal_service.poll(
                    &context, capture_delivery, probe);
    probe->poll_active = false;
    return result;
}

static struct guest_linux_signal_context signal_context(
        struct signal_fixture *fixture) {
    return (struct guest_linux_signal_context) {
        .runtime_opaque = ish_aarch64_linux_signal_service.runtime_opaque,
        .task_opaque = &fixture->task,
    };
}

static void restore_signal(
        struct signal_fixture *fixture,
        const struct guest_linux_signal_restore_request *request) {
    const struct guest_linux_signal_context context =
            signal_context(fixture);
    current = &fixture->task;
    ish_aarch64_linux_signal_service.restore(&context, request);
}

static void report_bad_frame(
        struct signal_fixture *fixture, qword_t frame_address) {
    const struct guest_linux_signal_context context =
            signal_context(fixture);
    current = &fixture->task;
    ish_aarch64_linux_signal_service.bad_frame(
            &context, frame_address);
}

static bool sighand_is_unlocked(struct sighand *sighand) {
    int result = trylock(&sighand->lock);
    if (result != 0)
        return false;
    unlock(&sighand->lock);
    return true;
}

static void *resume_ptrace_stop(void *opaque) {
    struct ptrace_controller *controller = opaque;
    while (true) {
        lock(&controller->task->ptrace.lock);
        if (controller->task->ptrace.stopped) {
            controller->observed_stop = true;
            unlock(&controller->task->ptrace.lock);

            // 停顿期间修改共享动作，确认 poll 不保留跨解锁的 action 指针。
            lock(&controller->task->sighand->lock);
            controller->task->sighand->action[SIGUSR1_].handler = SIG_IGN_;
            unlock(&controller->task->sighand->lock);

            lock(&controller->task->ptrace.lock);
            controller->task->ptrace.traced = false;
            controller->task->ptrace.stopped = false;
            notify(&controller->task->ptrace.cond);
            unlock(&controller->task->ptrace.lock);
            return NULL;
        }
        unlock(&controller->task->ptrace.lock);
        sched_yield();
    }
}

static int test_idle_and_blocked_pending(void) {
    struct signal_fixture fixture;
    struct installer_probe probe;
    init_fixture(&fixture);

    fixture.task.blocked = sig_mask(SIGUSR1_) | sig_mask(SIGUSR2_);
    fixture.task.saved_mask = sig_mask(SIGUSR1_);
    fixture.task.has_saved_mask = true;
    init_probe(&probe, &fixture.task);
    struct guest_linux_signal_poll_result result =
            poll_signal(&fixture, &probe);
    CHECK(result.status == GUEST_LINUX_SIGNAL_POLL_IDLE &&
            result.signal == 0 && probe.calls == 0 &&
            fixture.task.blocked == sig_mask(SIGUSR1_) &&
            !fixture.task.has_saved_mask &&
            sighand_is_unlocked(&fixture.sighand),
            "IDLE 恢复 saved_mask 且不调用 installer");

    queue_signal(&fixture.task, SIGUSR1_);
    init_probe(&probe, &fixture.task);
    result = poll_signal(&fixture, &probe);
    CHECK(result.status == GUEST_LINUX_SIGNAL_POLL_IDLE &&
            result.signal == 0 && probe.calls == 0 &&
            fixture.task.pending == sig_mask(SIGUSR1_) &&
            list_size(&fixture.task.queue) == 1,
            "被阻塞的 pending 信号保留位与队列节点");

    destroy_fixture(&fixture);
    return 0;
}

static int test_ignore_then_handler_in_same_poll(void) {
    struct signal_fixture fixture;
    struct installer_probe probe;
    init_fixture(&fixture);
    fixture.sighand.action[SIGUSR1_].handler = SIG_IGN_;
    fixture.sighand.action[SIGUSR2_].handler = HANDLER_ONE;
    queue_signal(&fixture.task, SIGUSR1_);
    queue_signal(&fixture.task, SIGUSR2_);

    init_probe(&probe, &fixture.task);
    struct guest_linux_signal_poll_result result =
            poll_signal(&fixture, &probe);
    CHECK(result.status == GUEST_LINUX_SIGNAL_POLL_HANDLER &&
            result.signal == SIGUSR2_ && probe.calls == 1 &&
            probe.synchronous && probe.lock_held &&
            fixture.task.pending == 0 &&
            list_empty(&fixture.task.queue),
            "同一 poll 消费忽略信号后继续找到 handler");

    destroy_fixture(&fixture);
    return 0;
}

static int test_one_handler_per_poll(void) {
    struct signal_fixture fixture;
    struct installer_probe probe;
    init_fixture(&fixture);
    fixture.sighand.action[SIGUSR1_].handler = HANDLER_ONE;
    fixture.sighand.action[SIGUSR2_].handler = HANDLER_TWO;
    queue_signal(&fixture.task, SIGUSR1_);
    queue_signal(&fixture.task, SIGUSR2_);

    init_probe(&probe, &fixture.task);
    struct guest_linux_signal_poll_result first =
            poll_signal(&fixture, &probe);
    CHECK(first.status == GUEST_LINUX_SIGNAL_POLL_HANDLER &&
            first.signal == SIGUSR1_ && probe.calls == 1 &&
            fixture.task.blocked == sig_mask(SIGUSR1_) &&
            fixture.task.pending == sig_mask(SIGUSR2_) &&
            list_size(&fixture.task.queue) == 1,
            "首次 poll 只提交一个 handler");

    init_probe(&probe, &fixture.task);
    struct guest_linux_signal_poll_result second =
            poll_signal(&fixture, &probe);
    CHECK(second.status == GUEST_LINUX_SIGNAL_POLL_HANDLER &&
            second.signal == SIGUSR2_ && probe.calls == 1 &&
            fixture.task.blocked ==
                    (sig_mask(SIGUSR1_) | sig_mask(SIGUSR2_)) &&
            fixture.task.pending == 0 &&
            list_empty(&fixture.task.queue),
            "后续 poll 再提交下一个 handler");

    destroy_fixture(&fixture);
    return 0;
}

static dword_t exported_payload_kind(dword_t internal_kind) {
    switch (internal_kind) {
        case SIGNAL_INFO_PAYLOAD_KILL:
            return GUEST_LINUX_SIGNAL_PAYLOAD_KILL;
        case SIGNAL_INFO_PAYLOAD_TIMER:
            return GUEST_LINUX_SIGNAL_PAYLOAD_TIMER;
        case SIGNAL_INFO_PAYLOAD_CHILD:
            return GUEST_LINUX_SIGNAL_PAYLOAD_CHILD;
        case SIGNAL_INFO_PAYLOAD_FAULT:
            return GUEST_LINUX_SIGNAL_PAYLOAD_FAULT;
        case SIGNAL_INFO_PAYLOAD_SIGSYS:
            return GUEST_LINUX_SIGNAL_PAYLOAD_SIGSYS;
        case SIGNAL_INFO_PAYLOAD_NONE:
        default:
            return GUEST_LINUX_SIGNAL_PAYLOAD_NONE;
    }
}

static bool signal_info_matches(
        const struct guest_linux_signal_info *actual,
        const struct siginfo_ *expected) {
    if (actual->signal != expected->sig ||
            actual->error != expected->sig_errno ||
            actual->code != expected->code ||
            actual->payload_kind !=
                    exported_payload_kind(expected->payload_kind))
        return false;

    switch (expected->payload_kind) {
        case SIGNAL_INFO_PAYLOAD_KILL:
            return actual->kill.pid == expected->kill.pid &&
                    actual->kill.uid == expected->kill.uid;
        case SIGNAL_INFO_PAYLOAD_TIMER:
            return actual->timer.timer == expected->timer.timer &&
                    actual->timer.overrun == expected->timer.overrun &&
                    actual->timer.value == expected->timer.value &&
                    actual->timer.private_value == expected->timer._private;
        case SIGNAL_INFO_PAYLOAD_CHILD:
            return actual->child.pid == expected->child.pid &&
                    actual->child.uid == expected->child.uid &&
                    actual->child.status == expected->child.status &&
                    actual->child.utime == expected->child.utime &&
                    actual->child.stime == expected->child.stime;
        case SIGNAL_INFO_PAYLOAD_FAULT:
            return actual->fault.address == expected->fault.addr;
        case SIGNAL_INFO_PAYLOAD_SIGSYS:
            return actual->sigsys.address == expected->sigsys.addr &&
                    actual->sigsys.syscall == expected->sigsys.syscall &&
                    actual->sigsys.architecture == expected->sigsys.arch;
        case SIGNAL_INFO_PAYLOAD_NONE:
            return true;
        default:
            return false;
    }
}

static bool delivery_matches(
        const struct guest_linux_signal_delivery *delivery,
        const struct siginfo_ *info,
        const struct signal_action *action,
        sigset_t_ blocked,
        const struct signal_altstack *altstack) {
    return signal_info_matches(&delivery->info, info) &&
            delivery->action.handler == action->handler &&
            delivery->action.flags == action->flags &&
            delivery->action.restorer == action->restorer &&
            delivery->action.mask == action->mask &&
            delivery->blocked_mask == blocked &&
            delivery->altstack.base == altstack->stack &&
            delivery->altstack.size == altstack->size &&
            delivery->altstack.flags == altstack->flags &&
            delivery->altstack.reserved == 0;
}

static int test_complete_delivery_dto(void) {
    const struct siginfo_ cases[] = {
        {
            .sig = SIGTRAP_, .sig_errno = -1, .code = SI_KERNEL_,
            .payload_kind = SIGNAL_INFO_PAYLOAD_NONE,
        },
        {
            .sig = SIGUSR1_, .sig_errno = -2, .code = SI_USER_,
            .payload_kind = SIGNAL_INFO_PAYLOAD_KILL,
            .kill = {.pid = -1234, .uid = UINT32_C(0x89abcdef)},
        },
        {
            .sig = SIGALRM_, .sig_errno = -3, .code = SI_TIMER_,
            .payload_kind = SIGNAL_INFO_PAYLOAD_TIMER,
            .timer = {
                .timer = -9, .overrun = 7,
                .value = UINT64_C(0xabcdef0123456789),
                ._private = -5,
            },
        },
        {
            .sig = SIGCHLD_, .sig_errno = -4, .code = SI_KERNEL_,
            .payload_kind = SIGNAL_INFO_PAYLOAD_CHILD,
            .child = {
                .pid = 2468, .uid = UINT32_C(0xfedcba98), .status = -6,
                .utime = INT64_C(0x1122334412345678),
                .stime = -INT64_C(0x1234567890),
            },
        },
        {
            .sig = SIGSEGV_, .sig_errno = -5, .code = SEGV_ACCERR_,
            .payload_kind = SIGNAL_INFO_PAYLOAD_FAULT,
            .fault.addr = UINT64_C(0x12345678abcdef01),
        },
        {
            .sig = SIGSYS_, .sig_errno = -6, .code = SI_KERNEL_,
            .payload_kind = SIGNAL_INFO_PAYLOAD_SIGSYS,
            .sigsys = {
                .addr = UINT64_C(0x2345678912345678),
                .syscall = 439, .arch = UINT32_C(0xc00000b7),
            },
        },
    };
    const struct signal_action action = {
        .handler = UINT64_C(0x1122334455667788),
        .flags = UINT64_C(0x100000000) | SA_SIGINFO_ | SA_ONSTACK_,
        .restorer = UINT64_C(0x8877665544332211),
        .mask = sig_mask(SIGUSR2_) | sig_mask(NUM_SIGS),
    };
    const sigset_t_ blocked = sig_mask(SIGPIPE_) | sig_mask(SIGXCPU_);
    const struct signal_altstack altstack = {
        .stack = UINT64_C(0x0000700012345000),
        .size = UINT64_C(0x0000000123456000),
        .flags = SS_ONSTACK_,
    };

    for (size_t index = 0; index < array_size(cases); index++) {
        struct signal_fixture fixture;
        struct installer_probe probe;
        init_fixture(&fixture);
        fixture.task.blocked = blocked;
        fixture.task.altstack = altstack;
        fixture.sighand.action[cases[index].sig] = action;
        queue_info(&fixture.task, cases[index]);

        init_probe(&probe, &fixture.task);
        struct guest_linux_signal_poll_result result =
                poll_signal(&fixture, &probe);
        CHECK(result.status == GUEST_LINUX_SIGNAL_POLL_HANDLER &&
                result.signal == cases[index].sig &&
                probe.calls == 1 && probe.synchronous &&
                probe.lock_held &&
                delivery_matches(&probe.deliveries[0],
                        &cases[index], &action, blocked, &altstack),
                "info 全 payload 及 action/blocked/altstack DTO 完整导出");
        destroy_fixture(&fixture);
    }
    return 0;
}

static int test_successful_commit_flags(void) {
    struct signal_fixture fixture;
    struct installer_probe probe;
    init_fixture(&fixture);
    fixture.task.blocked = sig_mask(SIGALRM_);
    fixture.sighand.action[SIGUSR1_] = (struct signal_action) {
        .handler = HANDLER_ONE,
        .flags = SA_NODEFER_ | SA_RESETHAND_,
        .restorer = UINT64_C(0x123456789abcdef0),
        .mask = sig_mask(SIGUSR2_) |
                sig_mask(SIGKILL_) | sig_mask(SIGSTOP_),
    };
    queue_signal(&fixture.task, SIGUSR1_);

    init_probe(&probe, &fixture.task);
    struct guest_linux_signal_poll_result result =
            poll_signal(&fixture, &probe);
    const struct signal_action reset = {0};
    CHECK(result.status == GUEST_LINUX_SIGNAL_POLL_HANDLER &&
            result.signal == SIGUSR1_ && probe.calls == 1 &&
            fixture.task.blocked ==
                    (sig_mask(SIGALRM_) | sig_mask(SIGUSR2_)) &&
            !sigset_has(fixture.task.blocked, SIGUSR1_) &&
            memcmp(&fixture.sighand.action[SIGUSR1_],
                    &reset, sizeof(reset)) == 0 &&
            fixture.task.pending == 0 &&
            list_empty(&fixture.task.queue),
            "COMPLETE 后提交 SA_NODEFER 掩码与 SA_RESETHAND 动作");

    destroy_fixture(&fixture);
    return 0;
}

static int test_frame_fault_forces_sigsegv_handler(void) {
    struct signal_fixture fixture;
    struct installer_probe probe;
    init_fixture(&fixture);
    const struct signal_action original_action = {
        .handler = HANDLER_ONE,
        .flags = SA_RESETHAND_,
        .mask = sig_mask(SIGALRM_),
    };
    fixture.sighand.action[SIGUSR1_] = original_action;
    fixture.sighand.action[SIGSEGV_] = (struct signal_action) {
        .handler = HANDLER_TWO,
        .flags = SA_NODEFER_,
        .mask = sig_mask(SIGUSR2_),
    };
    queue_signal(&fixture.task, SIGUSR1_);

    init_probe(&probe, &fixture.task);
    probe.responses[0] = GUEST_LINUX_SIGNAL_INSTALL_FRAME_FAULT;
    probe.responses[1] = GUEST_LINUX_SIGNAL_INSTALL_COMPLETE;
    probe.response_count = 2;
    struct guest_linux_signal_poll_result result =
            poll_signal(&fixture, &probe);
    CHECK(result.status == GUEST_LINUX_SIGNAL_POLL_HANDLER &&
            result.signal == SIGSEGV_ && probe.calls == 2 &&
            probe.synchronous && probe.lock_held &&
            probe.deliveries[0].info.signal == SIGUSR1_ &&
            probe.deliveries[1].info.signal == SIGSEGV_ &&
            probe.deliveries[1].info.code == SI_KERNEL_ &&
            probe.deliveries[1].info.payload_kind ==
                    GUEST_LINUX_SIGNAL_PAYLOAD_NONE &&
            memcmp(&fixture.sighand.action[SIGUSR1_],
                    &original_action, sizeof(original_action)) == 0 &&
            fixture.task.blocked == sig_mask(SIGUSR2_) &&
            !sigset_has(fixture.task.blocked, SIGSEGV_) &&
            fixture.task.pending == 0 &&
            list_empty(&fixture.task.queue),
            "FRAME_FAULT 不提交原动作并在同轮安装强制 SIGSEGV");

    destroy_fixture(&fixture);
    return 0;
}

static int test_second_frame_fault_terminates(void) {
    struct signal_fixture fixture;
    struct installer_probe probe;
    init_fixture(&fixture);
    fixture.sighand.action[SIGUSR1_].handler = HANDLER_ONE;
    fixture.sighand.action[SIGSEGV_].handler = HANDLER_TWO;
    queue_signal(&fixture.task, SIGUSR1_);

    init_probe(&probe, &fixture.task);
    probe.responses[0] = GUEST_LINUX_SIGNAL_INSTALL_FRAME_FAULT;
    probe.responses[1] = GUEST_LINUX_SIGNAL_INSTALL_FRAME_FAULT;
    probe.response_count = 2;
    struct guest_linux_signal_poll_result result =
            poll_signal(&fixture, &probe);
    CHECK(result.status == GUEST_LINUX_SIGNAL_POLL_TERMINATE &&
            result.signal == SIGSEGV_ && probe.calls == 2 &&
            probe.synchronous && probe.lock_held &&
            probe.deliveries[0].info.signal == SIGUSR1_ &&
            probe.deliveries[1].info.signal == SIGSEGV_ &&
            fixture.sighand.action[SIGSEGV_].handler == SIG_DFL_ &&
            fixture.task.pending == 0 &&
            list_empty(&fixture.task.queue) &&
            sighand_is_unlocked(&fixture.sighand),
            "强制 SIGSEGV 再次 FRAME_FAULT 时终止且不留 pending");

    destroy_fixture(&fixture);
    return 0;
}

static int test_default_stop_and_terminate(void) {
    struct signal_fixture fixture;
    struct installer_probe probe;
    init_fixture(&fixture);
    queue_signal(&fixture.task, SIGTSTP_);
    queue_signal(&fixture.task, SIGTERM_);

    init_probe(&probe, &fixture.task);
    struct guest_linux_signal_poll_result stop =
            poll_signal(&fixture, &probe);
    CHECK(stop.status == GUEST_LINUX_SIGNAL_POLL_STOP &&
            stop.signal == SIGTSTP_ && probe.calls == 0 &&
            fixture.group.stopped &&
            fixture.group.stop_code == (SIGTSTP_ << 8 | 0x7f) &&
            fixture.task.pending == sig_mask(SIGTERM_) &&
            list_size(&fixture.task.queue) == 1 &&
            sighand_is_unlocked(&fixture.sighand),
            "默认停止动作返回 STOP 且单轮不消费后继信号");

    init_probe(&probe, &fixture.task);
    struct guest_linux_signal_poll_result terminate =
            poll_signal(&fixture, &probe);
    CHECK(terminate.status == GUEST_LINUX_SIGNAL_POLL_TERMINATE &&
            terminate.signal == SIGTERM_ && probe.calls == 0 &&
            fixture.task.pending == 0 &&
            list_empty(&fixture.task.queue) &&
            sighand_is_unlocked(&fixture.sighand),
            "默认致命动作返回 TERMINATE 且不构造 handler DTO");

    destroy_fixture(&fixture);

    init_fixture(&fixture);
    fixture.sighand.action[SIGUSR1_].handler = HANDLER_ONE;
    queue_signal(&fixture.task, SIGUSR1_);
    queue_signal(&fixture.task, SIGKILL_);
    init_probe(&probe, &fixture.task);
    struct guest_linux_signal_poll_result killed =
            poll_signal(&fixture, &probe);
    CHECK(killed.status == GUEST_LINUX_SIGNAL_POLL_TERMINATE &&
            killed.signal == SIGKILL_ && probe.calls == 0 &&
            fixture.task.pending == sig_mask(SIGUSR1_) &&
            list_size(&fixture.task.queue) == 1,
            "pending SIGKILL 抢占普通 handler 并直接返回 TERMINATE");

    destroy_fixture(&fixture);
    return 0;
}

static int test_ptrace_stop_restarts_selection(void) {
    struct signal_fixture fixture;
    struct installer_probe probe;
    init_fixture(&fixture);
    fixture.task.ptrace.traced = true;
    fixture.sighand.action[SIGUSR1_].handler = HANDLER_ONE;
    fixture.sighand.action[SIGUSR2_].handler = HANDLER_TWO;
    queue_signal(&fixture.task, SIGUSR1_);
    queue_signal(&fixture.task, SIGUSR2_);

    struct ptrace_controller controller = {
        .task = &fixture.task,
    };
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL,
            resume_ptrace_stop, &controller) == 0,
            "建立 ptrace 恢复线程");

    init_probe(&probe, &fixture.task);
    struct guest_linux_signal_poll_result result =
            poll_signal(&fixture, &probe);
    CHECK(pthread_join(thread, NULL) == 0,
            "等待 ptrace 恢复线程结束");
    CHECK(controller.observed_stop &&
            fixture.task.ptrace.info.sig == SIGUSR1_ &&
            result.status == GUEST_LINUX_SIGNAL_POLL_HANDLER &&
            result.signal == SIGUSR2_ && probe.calls == 1 &&
            fixture.task.pending == 0 &&
            list_empty(&fixture.task.queue) &&
            sighand_is_unlocked(&fixture.sighand),
            "ptrace 停顿消费原信号并在恢复后重新选择队列");

    destroy_fixture(&fixture);
    return 0;
}

static int test_restore_valid_and_disabled_altstack(void) {
    struct signal_fixture fixture;
    init_fixture(&fixture);
    fixture.task.blocked = sig_mask(SIGALRM_);
    fixture.task.altstack = (struct signal_altstack) {
        .stack = UINT64_C(0x0000700010000000),
        .size = UINT64_C(8192),
    };

    const qword_t restored_base = UINT64_C(0x0000700020000000);
    struct guest_linux_signal_restore_request request = {
        .stack_pointer = UINT64_C(0x0000600012345000),
        .blocked_mask = sig_mask(SIGUSR1_) |
                sig_mask(SIGKILL_) | sig_mask(SIGSTOP_),
        .altstack = {
            .base = restored_base,
            .size = AARCH64_LINUX_MINSIGSTKSZ + 1024,
            .reserved = UINT32_C(0xa5a5a5a5),
        },
    };
    restore_signal(&fixture, &request);
    CHECK(fixture.task.blocked == sig_mask(SIGUSR1_) &&
            fixture.task.altstack.stack == restored_base &&
            fixture.task.altstack.size ==
                    AARCH64_LINUX_MINSIGSTKSZ + 1024 &&
            fixture.task.altstack.flags == 0,
            "恢复掩码时清除不可阻塞位并安装有效替代栈");

    request.blocked_mask = sig_mask(SIGUSR2_);
    request.altstack = (struct guest_linux_signal_stack) {
        .base = UINT64_C(0xffffffffffffffff),
        .size = UINT64_C(0xeeeeeeeeeeeeeeee),
        .flags = SS_DISABLE_,
        .reserved = UINT32_C(0xcccccccc),
    };
    restore_signal(&fixture, &request);
    CHECK(fixture.task.blocked == sig_mask(SIGUSR2_) &&
            fixture.task.altstack.stack == 0 &&
            fixture.task.altstack.size == 0 &&
            fixture.task.altstack.flags == SS_DISABLE_,
            "恢复禁用替代栈并忽略 DTO 保留字段");

    destroy_fixture(&fixture);
    return 0;
}

static int test_restore_rejects_invalid_altstack_only(void) {
    const struct signal_altstack old_stack = {
        .stack = UINT64_C(0x0000700012345000),
        .size = UINT64_C(8192),
        .flags = 0,
    };
    const qword_t new_base = UINT64_C(0x0000700023456000);
    const qword_t outside_sp = UINT64_C(0x0000600012345000);
    const struct {
        struct guest_linux_signal_stack altstack;
        qword_t stack_pointer;
        const char *message;
    } cases[] = {
        {
            .altstack = {
                .base = new_base,
                .size = UINT64_C(8192),
                .flags = UINT32_C(4),
            },
            .stack_pointer = outside_sp,
            .message = "无效 flags 保留原替代栈但提交掩码",
        },
        {
            .altstack = {
                .base = new_base,
                .size = AARCH64_LINUX_MINSIGSTKSZ - 1,
            },
            .stack_pointer = outside_sp,
            .message = "过小替代栈保留原配置但提交掩码",
        },
        {
            .altstack = {
                .base = AARCH64_LINUX_USER_ADDRESS_MAX - 4096,
                .size = UINT64_C(8192),
            },
            .stack_pointer = outside_sp,
            .message = "越过用户地址上界时保留原配置但提交掩码",
        },
        {
            .altstack = {
                .base = new_base,
                .size = UINT64_C(8192),
            },
            .stack_pointer = old_stack.stack + 128,
            .message = "仍在旧替代栈上时保留原配置但提交掩码",
        },
    };

    for (size_t index = 0; index < array_size(cases); index++) {
        struct signal_fixture fixture;
        init_fixture(&fixture);
        fixture.task.blocked = sig_mask(SIGALRM_);
        fixture.task.altstack = old_stack;
        const struct guest_linux_signal_restore_request request = {
            .stack_pointer = cases[index].stack_pointer,
            .blocked_mask = sig_mask(SIGUSR2_) |
                    sig_mask(SIGKILL_) | sig_mask(SIGSTOP_),
            .altstack = cases[index].altstack,
        };
        restore_signal(&fixture, &request);
        CHECK(fixture.task.blocked == sig_mask(SIGUSR2_) &&
                memcmp(&fixture.task.altstack,
                        &old_stack, sizeof(old_stack)) == 0,
                cases[index].message);
        destroy_fixture(&fixture);
    }
    return 0;
}

static bool queued_bad_frame_matches(
        const struct sigqueue *queued, qword_t frame_address) {
    return queued->info.sig == SIGSEGV_ &&
            queued->info.sig_errno == 0 &&
            queued->info.code == SEGV_MAPERR_ &&
            queued->info.payload_kind == SIGNAL_INFO_PAYLOAD_FAULT &&
            queued->info.fault.addr == frame_address;
}

static int test_bad_frame_info_priority_and_handler(void) {
    struct signal_fixture fixture;
    init_fixture(&fixture);
    const struct signal_action action = {
        .handler = HANDLER_TWO,
        .flags = SA_SIGINFO_ | SA_NODEFER_,
        .mask = sig_mask(SIGUSR2_),
    };
    fixture.sighand.action[SIGSEGV_] = action;
    queue_signal(&fixture.task, SIGUSR1_);

    const qword_t first_address = UINT64_C(0x00007abc12345670);
    report_bad_frame(&fixture, first_address);
    struct sigqueue *first = list_first_entry(
            &fixture.task.queue, struct sigqueue, queue);
    CHECK(fixture.task.pending ==
                    (sig_mask(SIGSEGV_) | sig_mask(SIGUSR1_)) &&
            list_size(&fixture.task.queue) == 2 &&
            queued_bad_frame_matches(first, first_address) &&
            memcmp(&fixture.sighand.action[SIGSEGV_],
                    &action, sizeof(action)) == 0 &&
            sighand_is_unlocked(&fixture.sighand),
            "坏帧携带精确信息并抢占已有异步信号且保留自定义 handler");

    const qword_t second_address = UINT64_C(0x00007abc87654320);
    report_bad_frame(&fixture, second_address);
    first = list_first_entry(
            &fixture.task.queue, struct sigqueue, queue);
    CHECK(list_size(&fixture.task.queue) == 2 &&
            queued_bad_frame_matches(first, second_address),
            "重复坏帧合并 SIGSEGV 时更新精确故障地址");

    destroy_fixture(&fixture);
    return 0;
}

static int test_bad_frame_resets_blocked_or_ignored_handler(void) {
    struct signal_fixture fixture;
    init_fixture(&fixture);
    fixture.task.blocked = sig_mask(SIGSEGV_) | sig_mask(SIGUSR2_);
    fixture.sighand.action[SIGSEGV_] = (struct signal_action) {
        .handler = HANDLER_TWO,
        .flags = SA_SIGINFO_,
        .mask = sig_mask(SIGALRM_),
    };
    report_bad_frame(&fixture, UINT64_C(0x00007abc11112220));
    struct sigqueue *first = list_first_entry(
            &fixture.task.queue, struct sigqueue, queue);
    CHECK(fixture.task.blocked == sig_mask(SIGUSR2_) &&
            fixture.sighand.action[SIGSEGV_].handler == SIG_DFL_ &&
            fixture.sighand.action[SIGSEGV_].flags == 0 &&
            queued_bad_frame_matches(first,
                    UINT64_C(0x00007abc11112220)),
            "被阻塞的 SIGSEGV 重置默认动作并解除阻塞");
    destroy_fixture(&fixture);

    init_fixture(&fixture);
    fixture.sighand.action[SIGSEGV_] = (struct signal_action) {
        .handler = SIG_IGN_,
        .flags = SA_NODEFER_,
        .mask = sig_mask(SIGALRM_),
    };
    report_bad_frame(&fixture, UINT64_C(0x00007abc33334440));
    first = list_first_entry(
            &fixture.task.queue, struct sigqueue, queue);
    CHECK(fixture.sighand.action[SIGSEGV_].handler == SIG_DFL_ &&
            fixture.sighand.action[SIGSEGV_].flags == 0 &&
            queued_bad_frame_matches(first,
                    UINT64_C(0x00007abc33334440)),
            "被忽略的 SIGSEGV 重置默认动作并强制排队");
    destroy_fixture(&fixture);
    return 0;
}

static void force_sync_info(struct signal_fixture *fixture,
        int signal, const struct siginfo_ *info) {
    lock(&fixture->sighand.lock);
    signal_force_sync_info_locked(
            &fixture->sighand, signal, info);
    unlock(&fixture->sighand.lock);
}

static int test_forced_sync_info_priority_and_handler(void) {
    struct signal_fixture fixture;
    struct installer_probe probe;
    init_fixture(&fixture);
    const struct signal_action action = {
        .handler = HANDLER_ONE,
        .flags = SA_SIGINFO_,
        .mask = sig_mask(SIGUSR2_),
    };
    fixture.sighand.action[SIGILL_] = action;
    queue_signal(&fixture.task, SIGUSR1_);

    struct siginfo_ info = {
        .sig = SIGILL_,
        .code = ILL_ILLOPC_,
        .payload_kind = SIGNAL_INFO_PAYLOAD_FAULT,
        .fault.addr = UINT64_C(0x0000400012345000),
    };
    force_sync_info(&fixture, SIGILL_, &info);
    struct sigqueue *first = list_first_entry(
            &fixture.task.queue, struct sigqueue, queue);
    CHECK(first->info.sig == SIGILL_ &&
            first->info.code == ILL_ILLOPC_ &&
            first->info.payload_kind == SIGNAL_INFO_PAYLOAD_FAULT &&
            first->info.fault.addr == info.fault.addr &&
            fixture.task.pending ==
                    (sig_mask(SIGILL_) | sig_mask(SIGUSR1_)) &&
            memcmp(&fixture.sighand.action[SIGILL_],
                    &action, sizeof(action)) == 0,
            "未阻塞同步异常抢占异步队列并保留自定义 handler");

    info.fault.addr = UINT64_C(0x0000400087654000);
    force_sync_info(&fixture, SIGILL_, &info);
    first = list_first_entry(
            &fixture.task.queue, struct sigqueue, queue);
    CHECK(list_size(&fixture.task.queue) == 2 &&
            first->info.sig == SIGILL_ &&
            first->info.fault.addr == info.fault.addr,
            "重复同步异常合并队列节点并更新精确故障地址");

    init_probe(&probe, &fixture.task);
    struct guest_linux_signal_poll_result result =
            poll_signal(&fixture, &probe);
    CHECK(result.status == GUEST_LINUX_SIGNAL_POLL_HANDLER &&
            result.signal == SIGILL_ && probe.calls == 1 &&
            probe.deliveries[0].info.code == ILL_ILLOPC_ &&
            probe.deliveries[0].info.fault.address == info.fault.addr &&
            fixture.task.pending == sig_mask(SIGUSR1_),
            "强制同步异常在下一安全点携带精确信息进入 handler");
    destroy_fixture(&fixture);
    return 0;
}

static int test_forced_sync_resets_blocked_or_ignored_action(void) {
    struct signal_fixture fixture;
    struct installer_probe probe;
    init_fixture(&fixture);
    fixture.task.blocked = sig_mask(SIGILL_) | sig_mask(SIGUSR2_);
    fixture.sighand.action[SIGILL_] = (struct signal_action) {
        .handler = HANDLER_ONE,
        .flags = SA_SIGINFO_,
        .mask = sig_mask(SIGALRM_),
    };
    const struct siginfo_ ill = {
        .sig = SIGILL_,
        .code = ILL_ILLOPC_,
        .payload_kind = SIGNAL_INFO_PAYLOAD_FAULT,
        .fault.addr = UINT64_C(0x0000400011110000),
    };
    force_sync_info(&fixture, SIGILL_, &ill);
    init_probe(&probe, &fixture.task);
    struct guest_linux_signal_poll_result result =
            poll_signal(&fixture, &probe);
    CHECK(result.status == GUEST_LINUX_SIGNAL_POLL_TERMINATE &&
            result.signal == SIGILL_ && probe.calls == 0 &&
            fixture.task.blocked == sig_mask(SIGUSR2_) &&
            fixture.sighand.action[SIGILL_].handler == SIG_DFL_,
            "被阻塞的同步异常解除阻塞并恢复默认致命动作");
    destroy_fixture(&fixture);

    init_fixture(&fixture);
    fixture.sighand.action[SIGBUS_] = (struct signal_action) {
        .handler = SIG_IGN_,
        .flags = SA_NODEFER_,
        .mask = sig_mask(SIGALRM_),
    };
    const struct siginfo_ bus = {
        .sig = SIGBUS_,
        .code = BUS_ADRALN_,
        .payload_kind = SIGNAL_INFO_PAYLOAD_FAULT,
        .fault.addr = UINT64_C(0x0000400022220001),
    };
    force_sync_info(&fixture, SIGBUS_, &bus);
    init_probe(&probe, &fixture.task);
    result = poll_signal(&fixture, &probe);
    CHECK(result.status == GUEST_LINUX_SIGNAL_POLL_TERMINATE &&
            result.signal == SIGBUS_ && probe.calls == 0 &&
            fixture.sighand.action[SIGBUS_].handler == SIG_DFL_,
            "被忽略的同步异常恢复默认动作并强制投递");
    destroy_fixture(&fixture);
    return 0;
}

int main(void) {
    if (test_idle_and_blocked_pending() != 0)
        return 1;
    if (test_ignore_then_handler_in_same_poll() != 0)
        return 1;
    if (test_one_handler_per_poll() != 0)
        return 1;
    if (test_complete_delivery_dto() != 0)
        return 1;
    if (test_successful_commit_flags() != 0)
        return 1;
    if (test_frame_fault_forces_sigsegv_handler() != 0)
        return 1;
    if (test_second_frame_fault_terminates() != 0)
        return 1;
    if (test_default_stop_and_terminate() != 0)
        return 1;
    if (test_ptrace_stop_restarts_selection() != 0)
        return 1;
    if (test_restore_valid_and_disabled_altstack() != 0)
        return 1;
    if (test_restore_rejects_invalid_altstack_only() != 0)
        return 1;
    if (test_bad_frame_info_priority_and_handler() != 0)
        return 1;
    if (test_bad_frame_resets_blocked_or_ignored_handler() != 0)
        return 1;
    if (test_forced_sync_info_priority_and_handler() != 0)
        return 1;
    if (test_forced_sync_resets_blocked_or_ignored_action() != 0)
        return 1;
    return 0;
}
