#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#include "kernel/errno.h"
#include "kernel/signal-delivery.h"
#include "kernel/signal.h"
#include "kernel/task.h"

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "排队信号测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        return 1; \
    } \
} while (0)

struct local_target {
    struct task task;
    struct tgroup group;
    struct sighand sighand;
};

static void init_local_target(struct local_target *target,
        uid_t_ uid, rlim_t_ limit) {
    *target = (struct local_target) {0};
    list_init(&target->task.queue);
    lock_init(&target->sighand.lock);
    target->task.sighand = &target->sighand;
    target->task.group = &target->group;
    target->task.uid = uid;
    lock_init(&target->group.lock);
    target->group.leader = &target->task;
    target->group.limits[RLIMIT_SIGPENDING_] = (struct rlimit_) {
        .cur = limit,
        .max = limit,
    };
}

static struct siginfo_ queue_info(qword_t value) {
    return (struct siginfo_) {
        .code = SI_QUEUE_,
        .payload_kind = SIGNAL_INFO_PAYLOAD_QUEUE,
        .queue = {
            .pid = 1234,
            .uid = 5678,
            .value = value,
        },
    };
}

static int enqueue_local(struct local_target *target, int signal,
        qword_t value, enum signal_queue_policy policy) {
    lock(&target->sighand.lock);
    int result = signal_enqueue_locked(&target->task, signal,
            queue_info(value), policy, target->task.uid,
            target->group.limits[RLIMIT_SIGPENDING_].cur);
    unlock(&target->sighand.lock);
    return result;
}

static void flush_local(struct local_target *target) {
    lock(&target->sighand.lock);
    signal_flush_pending(&target->task);
    unlock(&target->sighand.lock);
}

static int test_shared_uid_limit_and_cleanup(void) {
    struct local_target first, second, isolated;
    init_local_target(&first, 1001, 2);
    init_local_target(&second, 1001, 2);
    init_local_target(&isolated, 1002, 2);

    CHECK(enqueue_local(&first, SIGRTMIN_, 1,
                    SIGNAL_QUEUE_EXPLICIT) == SIGNAL_ENQUEUE_QUEUED &&
            enqueue_local(&second, SIGRTMIN_, 2,
                    SIGNAL_QUEUE_EXPLICIT) == SIGNAL_ENQUEUE_QUEUED &&
            enqueue_local(&second, SIGRTMIN_, 3,
                    SIGNAL_QUEUE_EXPLICIT) == _EAGAIN,
            "同一目标 real UID 的不同进程共享软限额");
    CHECK(enqueue_local(&isolated, SIGRTMIN_, 4,
                    SIGNAL_QUEUE_EXPLICIT) == SIGNAL_ENQUEUE_QUEUED,
            "不同 real UID 使用独立 pending 配额");

    lock(&first.sighand.lock);
    struct sigqueue *queued = list_first_entry(
            &first.task.queue, struct sigqueue, queue);
    signal_dequeue_locked(&first.task, queued);
    unlock(&first.sighand.lock);
    CHECK(enqueue_local(&second, SIGRTMIN_, 5,
                    SIGNAL_QUEUE_EXPLICIT) == SIGNAL_ENQUEUE_QUEUED,
            "正常 dequeue 立即归还共享 UID 配额");

    lock(&second.sighand.lock);
    signal_discard_pending_locked(&second.task, SIGRTMIN_);
    unlock(&second.sighand.lock);
    CHECK(enqueue_local(&first, SIGRTMIN_, 6,
                    SIGNAL_QUEUE_EXPLICIT) == SIGNAL_ENQUEUE_QUEUED &&
            enqueue_local(&first, SIGRTMIN_, 7,
                    SIGNAL_QUEUE_EXPLICIT) == SIGNAL_ENQUEUE_QUEUED,
            "忽略动作清空同号队列时逐项归还配额");

    second.group.limits[RLIMIT_SIGPENDING_].cur = 1;
    CHECK(enqueue_local(&second, SIGRTMIN_, 8,
                    SIGNAL_QUEUE_EXPLICIT) == _EAGAIN,
            "降低软限额后已有占用不被豁免");
    signal_flush_pending(&first.task);
    CHECK(enqueue_local(&second, SIGRTMIN_, 9,
                    SIGNAL_QUEUE_EXPLICIT) == SIGNAL_ENQUEUE_QUEUED,
            "任务退出使用的 flush 路径归还全部配额");

    flush_local(&first);
    flush_local(&second);
    flush_local(&isolated);
    return 0;
}

static int test_allocation_failure_and_bit_only(void) {
    struct local_target target;
    init_local_target(&target, 2001, 1);

    target.group.limits[RLIMIT_SIGPENDING_].cur = 0;
    CHECK(enqueue_local(&target, SIGKILL_, 0,
                    SIGNAL_QUEUE_EXPLICIT) == SIGNAL_ENQUEUE_BIT_ONLY &&
            target.task.pending == sig_mask(SIGKILL_) &&
            list_empty(&target.task.queue),
            "显式 SIGKILL 不受零软限额约束且不分配信息节点");
    flush_local(&target);

    target.group.limits[RLIMIT_SIGPENDING_].cur = 1;
    signal_queue_test_fail_allocation_at(0);
    CHECK(enqueue_local(&target, SIGKILL_, 0,
                    SIGNAL_QUEUE_EXPLICIT) == SIGNAL_ENQUEUE_BIT_ONLY &&
            target.task.pending == sig_mask(SIGKILL_) &&
            list_empty(&target.task.queue),
            "显式 SIGKILL 在节点内存故障下仍可靠进入 pending");
    flush_local(&target);
    CHECK(enqueue_local(&target, SIGRTMIN_, 0,
                    SIGNAL_QUEUE_EXPLICIT) == _EAGAIN &&
            target.task.pending == 0 && list_empty(&target.task.queue),
            "SIGKILL 快速路径不消耗后续节点分配故障");

    signal_queue_test_fail_allocation_at(0);
    CHECK(enqueue_local(&target, SIGRTMIN_, 1,
                    SIGNAL_QUEUE_EXPLICIT) == _EAGAIN &&
            target.task.pending == 0 && list_empty(&target.task.queue),
            "显式发送节点分配失败时返回 EAGAIN 且不留计数");
    signal_queue_test_fail_allocation_at(SIZE_MAX);
    CHECK(enqueue_local(&target, SIGRTMIN_, 2,
                    SIGNAL_QUEUE_EXPLICIT) == SIGNAL_ENQUEUE_QUEUED &&
            enqueue_local(&target, SIGRTMIN_, 3,
                    SIGNAL_QUEUE_EXPLICIT) == _EAGAIN,
            "节点分配失败恢复后完整软限额仍可使用一次");
    flush_local(&target);

    target.task.uid = 2002;
    signal_queue_test_fail_allocation_at(1);
    CHECK(enqueue_local(&target, SIGRTMIN_, 4,
                    SIGNAL_QUEUE_EXPLICIT) == _EAGAIN &&
            target.task.pending == 0 && list_empty(&target.task.queue),
            "UID 计数对象分配失败时释放未入队节点");
    signal_queue_test_fail_allocation_at(SIZE_MAX);
    CHECK(enqueue_local(&target, SIGRTMIN_, 5,
                    SIGNAL_QUEUE_EXPLICIT) == SIGNAL_ENQUEUE_QUEUED,
            "计数对象分配失败不遗留幽灵占用");
    flush_local(&target);

    target.task.uid = 2003;
    target.group.limits[RLIMIT_SIGPENDING_].cur = 0;
    for (int index = 0; index < 4096; index++)
        CHECK(enqueue_local(&target, SIGRTMIN_, (qword_t) index,
                        SIGNAL_QUEUE_LEGACY) == SIGNAL_ENQUEUE_BIT_ONLY,
                "legacy 实时信号满额时保持成功 fallback");
    CHECK(target.task.pending == sig_mask(SIGRTMIN_) &&
            list_empty(&target.task.queue),
            "重复 legacy fallback 只保留 pending 位而不增长队列");
    struct siginfo_ fallback;
    lock(&target.sighand.lock);
    bool taken = signal_take_unblocked_locked(
            &target.task, 0, false, &fallback);
    unlock(&target.sighand.lock);
    CHECK(taken && fallback.sig == SIGRTMIN_ &&
            fallback.code == SI_USER_ && fallback.kill.pid == 0 &&
            fallback.kill.uid == 0 && target.task.pending == 0,
            "bit-only fallback 可消费并生成 Linux SI_USER 空信息");

    target.task.uid = 2004;
    signal_queue_test_fail_allocation_at(0);
    struct siginfo_ first_fault = {
        .sig = SIGSEGV_,
        .code = SEGV_MAPERR_,
        .payload_kind = SIGNAL_INFO_PAYLOAD_FAULT,
        .fault.addr = UINT64_C(0x1111222233334444),
    };
    current = &target.task;
    lock(&target.sighand.lock);
    signal_force_sync_info_locked(
            &target.sighand, SIGSEGV_, &first_fault);
    unlock(&target.sighand.lock);
    CHECK(target.task.pending == sig_mask(SIGSEGV_) &&
            list_empty(&target.task.queue),
            "强制标准信号 OOM 时保留 bit-only pending");

    signal_queue_test_fail_allocation_at(SIZE_MAX);
    struct siginfo_ recovered_fault = first_fault;
    recovered_fault.fault.addr = UINT64_C(0xaaaabbbbccccdddd);
    lock(&target.sighand.lock);
    signal_force_sync_info_locked(
            &target.sighand, SIGSEGV_, &recovered_fault);
    struct siginfo_ recovered;
    taken = signal_take_unblocked_locked(
            &target.task, 0, false, &recovered);
    unlock(&target.sighand.lock);
    current = NULL;
    CHECK(taken &&
            recovered.payload_kind == SIGNAL_INFO_PAYLOAD_FAULT &&
            recovered.fault.addr == recovered_fault.fault.addr &&
            target.task.pending == 0 && list_empty(&target.task.queue),
            "OOM 恢复后 FORCE 为 bit-only 标准信号补建精确信息节点");
    return 0;
}

static int test_fifo_and_standard_coalescing(void) {
    struct local_target target;
    init_local_target(&target, 3001, 8);
    CHECK(enqueue_local(&target, SIGTSTP_, 1,
                    SIGNAL_QUEUE_EXPLICIT) == SIGNAL_ENQUEUE_QUEUED &&
            enqueue_local(&target, SIGTERM_, 2,
                    SIGNAL_QUEUE_EXPLICIT) == SIGNAL_ENQUEUE_QUEUED,
            "排队两个不同普通信号");
    struct siginfo_ info;
    lock(&target.sighand.lock);
    bool first = signal_take_unblocked_locked(
            &target.task, 0, false, &info);
    int first_signal = info.sig;
    bool second = signal_take_unblocked_locked(
            &target.task, 0, false, &info);
    unlock(&target.sighand.lock);
    CHECK(first && second && first_signal == SIGTSTP_ &&
            info.sig == SIGTERM_,
            "普通异步队列继续保持原有 FIFO 顺序");

    CHECK(enqueue_local(&target, SIGUSR1_, 3,
                    SIGNAL_QUEUE_EXPLICIT) == SIGNAL_ENQUEUE_QUEUED &&
            enqueue_local(&target, SIGUSR1_, 4,
                    SIGNAL_QUEUE_EXPLICIT) == SIGNAL_ENQUEUE_COALESCED &&
            list_size(&target.task.queue) == 1,
            "显式普通信号仍只保留一个队列节点");
    flush_local(&target);

    CHECK(enqueue_local(&target, SIGRTMIN_ + 1, 5,
                    SIGNAL_QUEUE_EXPLICIT) == SIGNAL_ENQUEUE_QUEUED &&
            enqueue_local(&target, SIGRTMIN_ + 1, 6,
                    SIGNAL_QUEUE_EXPLICIT) == SIGNAL_ENQUEUE_QUEUED,
            "同号实时信号全部入队");
    lock(&target.sighand.lock);
    first = signal_take_unblocked_locked(&target.task, 0, false, &info);
    qword_t first_value = info.queue.value;
    second = signal_take_unblocked_locked(&target.task, 0, false, &info);
    unlock(&target.sighand.lock);
    CHECK(first && second && first_value == 5 && info.queue.value == 6,
            "同号实时信号按入队 FIFO 保留 sigval");
    return 0;
}

#define CONCURRENT_TARGETS 4
#define CONCURRENT_SENDERS 8
#define CONCURRENT_ATTEMPTS 64
#define CONCURRENT_LIMIT 73

struct concurrent_sender {
    struct local_target *targets;
    int index;
    atomic_int *successes;
};

static void *enqueue_concurrently(void *opaque) {
    struct concurrent_sender *sender = opaque;
    struct local_target *target =
            &sender->targets[sender->index % CONCURRENT_TARGETS];
    for (int attempt = 0; attempt < CONCURRENT_ATTEMPTS; attempt++) {
        int result = enqueue_local(target, SIGRTMIN_ + 2,
                (qword_t) sender->index << 32 | (dword_t) attempt,
                SIGNAL_QUEUE_EXPLICIT);
        if (result == SIGNAL_ENQUEUE_QUEUED)
            atomic_fetch_add_explicit(
                    sender->successes, 1, memory_order_relaxed);
        else
            assert(result == _EAGAIN);
    }
    return NULL;
}

static int test_concurrent_shared_limit(void) {
    struct local_target targets[CONCURRENT_TARGETS];
    for (int index = 0; index < CONCURRENT_TARGETS; index++)
        init_local_target(&targets[index], 4001, CONCURRENT_LIMIT);

    atomic_int successes;
    atomic_init(&successes, 0);
    pthread_t threads[CONCURRENT_SENDERS];
    struct concurrent_sender senders[CONCURRENT_SENDERS];
    for (int index = 0; index < CONCURRENT_SENDERS; index++) {
        senders[index] = (struct concurrent_sender) {
            .targets = targets,
            .index = index,
            .successes = &successes,
        };
        CHECK(pthread_create(&threads[index], NULL,
                        enqueue_concurrently, &senders[index]) == 0,
                "创建并发显式发送线程");
    }
    for (int index = 0; index < CONCURRENT_SENDERS; index++)
        CHECK(pthread_join(threads[index], NULL) == 0,
                "回收并发显式发送线程");

    unsigned long queued = 0;
    for (int index = 0; index < CONCURRENT_TARGETS; index++)
        queued += list_size(&targets[index].task.queue);
    CHECK(atomic_load_explicit(&successes, memory_order_relaxed) ==
                    CONCURRENT_LIMIT && queued == CONCURRENT_LIMIT,
            "并发共享 UID 限额恰好允许 limit 个节点且不超卖");
    for (int index = 0; index < CONCURRENT_TARGETS; index++)
        flush_local(&targets[index]);
    CHECK(enqueue_local(&targets[0], SIGRTMIN_ + 2, 1,
                    SIGNAL_QUEUE_EXPLICIT) == SIGNAL_ENQUEUE_QUEUED,
            "并发队列清空后共享计数归零");
    flush_local(&targets[0]);
    return 0;
}

struct published_group {
    struct task *leader;
    struct task *sibling;
    struct tgroup group;
    struct sighand sighand;
};

static bool init_published_group(struct published_group *published,
        uid_t_ uid, rlim_t_ limit) {
    *published = (struct published_group) {0};
    published->leader = task_create_(NULL);
    if (published->leader == NULL)
        return false;

    list_init(&published->group.threads);
    list_init(&published->group.session);
    list_init(&published->group.pgroup);
    lock_init(&published->group.lock);
    cond_init(&published->group.stopped_cond);
    published->group.leader = published->leader;
    published->group.sid = published->leader->pid;
    published->group.pgid = published->leader->pid;
    published->group.limits[RLIMIT_SIGPENDING_] = (struct rlimit_) {
        .cur = limit,
        .max = limit,
    };
    lock_init(&published->sighand.lock);
    published->sighand.refcount = 1;
    published->leader->group = &published->group;
    published->leader->sighand = &published->sighand;
    published->leader->tgid = published->leader->pid;
    published->leader->uid = published->leader->suid = uid;
    published->leader->blocked = sig_mask(SIGRTMIN_);
    task_publish(published->leader);

    published->sibling = task_create_(published->leader);
    if (published->sibling == NULL)
        return false;
    published->sibling->group = &published->group;
    published->sibling->sighand = &published->sighand;
    published->sibling->tgid = published->leader->pid;
    published->sibling->uid = published->sibling->suid = uid;
    published->sibling->blocked = sig_mask(SIGRTMIN_);
    task_publish(published->sibling);
    return true;
}

static void destroy_published_group(struct published_group *published) {
    signal_flush_pending(published->leader);
    signal_flush_pending(published->sibling);
    cond_destroy(&published->sibling->pause);
    cond_destroy(&published->sibling->ptrace.cond);
    cond_destroy(&published->leader->pause);
    cond_destroy(&published->leader->ptrace.cond);
    pthread_mutex_destroy(&published->sibling->general_lock.m);
    pthread_mutex_destroy(&published->sibling->waiting_cond_lock.m);
    pthread_mutex_destroy(&published->sibling->ptrace.lock.m);
    pthread_mutex_destroy(&published->leader->general_lock.m);
    pthread_mutex_destroy(&published->leader->waiting_cond_lock.m);
    pthread_mutex_destroy(&published->leader->ptrace.lock.m);
    lock(&pids_lock);
    lock(&published->group.lock);
    list_remove(&published->sibling->group_links);
    task_destroy(published->sibling);
    list_remove(&published->leader->group_links);
    list_remove(&published->group.session);
    list_remove(&published->group.pgroup);
    task_destroy(published->leader);
    unlock(&published->group.lock);
    unlock(&pids_lock);
    cond_destroy(&published->group.stopped_cond);
    pthread_mutex_destroy(&published->sighand.lock.m);
    pthread_mutex_destroy(&published->group.lock.m);
}

static int test_process_and_thread_targeting(void) {
    struct published_group target;
    CHECK(init_published_group(&target, 5001, 8),
            "创建已发布目标线程组");
    struct task sender = {
        .uid = 5001,
        .euid = 5001,
    };
    current = &sender;
    struct siginfo_ info = queue_info(UINT64_C(0x123456789abcdef0));

    CHECK(task_rt_sigqueueinfo(target.leader->pid,
                    SIGRTMIN_, &info) == 0 &&
            list_size(&target.leader->queue) == 1 &&
            list_empty(&target.sibling->queue),
            "process-directed 信号优先投递到存活 leader");
    signal_flush_pending(target.leader);

    target.leader->blocked |= sig_mask(SIGCONT_);
    target.group.limits[RLIMIT_SIGPENDING_].cur = 0;
    lock(&target.group.lock);
    target.group.stopped = true;
    target.group.stop_code = SIGSTOP_;
    unlock(&target.group.lock);
    int resume_error = task_rt_sigqueueinfo(
            target.leader->pid, SIGCONT_, &info);
    lock(&target.group.lock);
    bool group_resumed = !target.group.stopped &&
            target.group.stop_code == 0;
    unlock(&target.group.lock);
    CHECK(resume_error == _EAGAIN && group_resumed &&
            !sigset_has(target.leader->pending, SIGCONT_) &&
            list_empty(&target.leader->queue),
            "显式 SIGCONT 排队失败仍先恢复已停止线程组");
    target.leader->blocked &= ~sig_mask(SIGCONT_);
    target.group.limits[RLIMIT_SIGPENDING_].cur = 8;

    target.leader->exiting = true;
    CHECK(task_rt_sigqueueinfo(target.leader->pid,
                    SIGRTMIN_, &info) == 0 &&
            list_empty(&target.leader->queue) &&
            list_size(&target.sibling->queue) == 1,
            "leader 先退出时 process-directed 信号选择存活 sibling");
    signal_flush_pending(target.sibling);
    CHECK(task_rt_sigqueueinfo(target.leader->pid, 0, &info) == 0 &&
            list_empty(&target.sibling->queue),
            "signal 0 对仍存在的退出 leader 只做权限与存在性检查");

    target.sibling->exiting = true;
    CHECK(task_rt_sigqueueinfo(target.leader->pid,
                    SIGRTMIN_, &info) == 0 &&
            list_empty(&target.leader->queue) &&
            list_empty(&target.sibling->queue),
            "完全退出但未 reap 的线程组与 Linux 一样静默接受信号");
    CHECK(task_rt_tgsigqueueinfo(target.leader->pid,
                    target.sibling->pid, SIGRTMIN_, &info) == _ESRCH,
            "thread-directed 信号拒绝已经 exiting 的精确线程");
    target.sibling->exiting = false;

    CHECK(task_rt_tgsigqueueinfo(target.leader->pid,
                    target.sibling->pid, SIGRTMIN_, &info) == 0 &&
            list_size(&target.sibling->queue) == 1,
            "rt_tgsigqueueinfo 只向匹配 tgid/tid 的线程排队");
    signal_flush_pending(target.sibling);
    CHECK(task_rt_tgsigqueueinfo(0, target.sibling->pid,
                    SIGRTMIN_, &info) == _EINVAL &&
            task_rt_tgsigqueueinfo(target.leader->pid, 0,
                    SIGRTMIN_, &info) == _EINVAL &&
            task_rt_tgsigqueueinfo(target.leader->pid + 1,
                    target.sibling->pid, SIGRTMIN_, &info) == _ESRCH,
            "thread-directed 调用区分非法 ID 与线程组不匹配");
    CHECK(task_rt_sigqueueinfo(-1, SIGRTMIN_, &info) == _ESRCH &&
            task_rt_sigqueueinfo(target.leader->pid,
                    NUM_SIGS + 1, &info) == _EINVAL,
            "process-directed 调用拒绝非正 PID 与非法信号号");

    struct siginfo_ wrong_code = info;
    wrong_code.code = SI_TIMER_;
    CHECK(task_rt_sigqueueinfo(target.leader->pid,
                    SIGRTMIN_, &wrong_code) == _EPERM,
            "未建模 union 的其他负 si_code 明确返回 EPERM");
    sender.uid = sender.euid = 6001;
    CHECK(task_rt_sigqueueinfo(target.leader->pid,
                    SIGRTMIN_, &info) == _EPERM,
            "发送权限按发送者与目标 real/saved UID 检查");

    target.leader->exiting = false;
    target.sibling->exiting = false;
    destroy_published_group(&target);
    current = NULL;
    return 0;
}

int main(void) {
    struct sigaction ignored = {.sa_handler = SIG_IGN};
    sigemptyset(&ignored.sa_mask);
    sigaction(SIGUSR1, &ignored, NULL);
    signal_queue_test_fail_allocation_at(SIZE_MAX);

    if (test_shared_uid_limit_and_cleanup() != 0 ||
            test_allocation_failure_and_bit_only() != 0 ||
            test_fifo_and_standard_coalescing() != 0 ||
            test_concurrent_shared_limit() != 0 ||
            test_process_and_thread_targeting() != 0)
        return 1;
    return 0;
}
