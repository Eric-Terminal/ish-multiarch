#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "emu/interrupt.h"
#include "fs/fd.h"
#include "fs/inode.h"
#include "guest/linux/futex-abi.h"
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/futex.h"
#include "kernel/mm.h"
#include "kernel/signal.h"
#include "kernel/task.h"
#include "kernel/time.h"

#define TEST_FUTEX_WAIT 0
#define TEST_FUTEX_WAKE 1
#define TEST_FUTEX_REQUEUE 3
#define TEST_FUTEX_PRIVATE UINT32_C(128)

#define TEST_BASE UINT32_C(0x57000000)
#define VECTOR_ADDRESS TEST_BASE
#define WORD_ADDRESS (TEST_BASE + PAGE_SIZE)
#define TIMEOUT_ADDRESS (TEST_BASE + 2 * PAGE_SIZE)
#define COPY_PAGE (TEST_BASE + 3 * PAGE_SIZE)
#define PROT_NONE_PAGE (COPY_PAGE + 2 * PAGE_SIZE)
#define SPECIAL_PAGE (COPY_PAGE + 3 * PAGE_SIZE)
#define FILE_COW_PAGE (COPY_PAGE + 4 * PAGE_SIZE)
#define FILE_OOM_PAGE (COPY_PAGE + 5 * PAGE_SIZE)
#define TEST_UNMAPPED UINT32_C(0x67000000)

#define TEST_CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "i386 futex_waitv 测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        return false; \
    } \
} while (0)

struct test_fixture {
    struct task task;
    struct tgroup group;
    struct sighand *sighand;
};

struct waitv_call {
    pthread_t thread;
    struct task *task;
    addr_t vector;
    dword_t count;
    addr_t timeout;
    sdword_t clock;
    atomic_uint stage;
    sdword_t result;
    bool started;
};

static const struct fd_ops test_executable_fd_ops = {0};

static int close_test_file(struct fd *file) {
    free(file->inode);
    file->inode = NULL;
    return 0;
}

static const struct fd_ops test_file_fd_ops = {
    .close = close_test_file,
};

static bool fixture_attach_mm(struct test_fixture *fixture,
        struct mm *memory, pid_t_ pid) {
    memset(fixture, 0, sizeof(*fixture));
    fixture->task.pid = fixture->task.tgid = pid;
    fixture->sighand = sighand_new();
    if (fixture->sighand == NULL)
        return false;
    fixture->task.sighand = fixture->sighand;
    fixture->task.group = &fixture->group;
    list_init(&fixture->task.group_links);
    list_init(&fixture->group.threads);
    fixture->group.leader = &fixture->task;
    list_add_tail(&fixture->group.threads,
            &fixture->task.group_links);
    lock_init(&fixture->group.lock);
    cond_init(&fixture->group.stopped_cond);
    fixture->task.waiting_poll_notify_fd = -1;
    lock_init(&fixture->task.waiting_cond_lock);
    task_set_mm(&fixture->task, memory);
    return true;
}

static bool fixture_init(struct test_fixture *fixture, pid_t_ pid) {
    struct mm *memory = mm_new();
    if (memory == NULL)
        return false;
    memory->exefile = fd_create(&test_executable_fd_ops);
    if (memory->exefile == NULL) {
        mm_release(memory);
        return false;
    }
    if (!fixture_attach_mm(fixture, memory, pid)) {
        mm_release(memory);
        return false;
    }
    current = &fixture->task;
    task_thread_store(&fixture->task, pthread_self());
    return true;
}

static bool fixture_share(struct test_fixture *fixture,
        struct mm *memory, pid_t_ pid) {
    mm_retain(memory);
    if (fixture_attach_mm(fixture, memory, pid))
        return true;
    mm_release(memory);
    return false;
}

static bool fixture_copy(struct test_fixture *fixture,
        struct mm *source, pid_t_ pid) {
    struct mm *memory = mm_copy(source);
    if (memory == NULL)
        return false;
    if (fixture_attach_mm(fixture, memory, pid))
        return true;
    mm_release(memory);
    return false;
}

static void fixture_destroy(struct test_fixture *fixture) {
    if (current == &fixture->task)
        current = NULL;
    fixture->task.pending = 0;
    mm_release(fixture->task.mm);
    sighand_release(fixture->sighand);
    pthread_mutex_destroy(&fixture->task.waiting_cond_lock.m);
    cond_destroy(&fixture->group.stopped_cond);
    pthread_mutex_destroy(&fixture->group.lock.m);
    fixture->task.mm = NULL;
    fixture->task.mem = NULL;
    fixture->task.group = NULL;
    fixture->task.sighand = NULL;
}

static bool map_pages(struct task *task, addr_t address,
        pages_t count, unsigned flags) {
    write_wrlock(&task->mem->lock);
    int result = pt_map_nothing(task->mem, PAGE(address), count, flags);
    write_wrunlock(&task->mem->lock);
    return result == 0;
}

static bool map_private_file_page(
        struct task *task, addr_t address) {
    void *memory = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED)
        return false;
    struct fd *file = fd_create(&test_file_fd_ops);
    if (file == NULL) {
        (void) munmap(memory, PAGE_SIZE);
        return false;
    }
    file->inode = calloc(1, sizeof(*file->inode));
    if (file->inode == NULL) {
        fd_close(file);
        (void) munmap(memory, PAGE_SIZE);
        return false;
    }
    file->inode->futex_sequence = UINT64_C(0x1f83d9abfb41bd6b);
    file->type = S_IFREG;

    write_wrlock(&task->mem->lock);
    int result = pt_map(task->mem, PAGE(address), 1,
            memory, 0, P_RWX | P_COW | P_FILE_BACKED);
    if (result == 0)
        mem_pt(task->mem, PAGE(address))->data->fd = file;
    write_wrunlock(&task->mem->lock);
    if (result < 0)
        fd_close(file);
    return result == 0;
}

static bool write_guest(struct task *task, addr_t address,
        const void *value, size_t size) {
    return user_write_task(task, address, value, size) == 0;
}

static bool write_word(struct task *task, addr_t address, dword_t value) {
    return write_guest(task, address, &value, sizeof(value));
}

static bool write_waiter(struct task *task, addr_t vector, size_t index,
        qword_t value, qword_t address, dword_t flags, dword_t reserved) {
    const struct guest_linux_futex_waitv waiter = {
        .value = value,
        .address = address,
        .flags = flags,
        .reserved = reserved,
    };
    return write_guest(task,
            vector + (addr_t) (index * sizeof(waiter)),
            &waiter, sizeof(waiter));
}

static bool write_timeout(struct task *task, addr_t address,
        sqword_t seconds, sqword_t nanoseconds) {
    const struct guest_linux_kernel_timespec timeout = {
        .sec = seconds,
        .nsec = nanoseconds,
    };
    return write_guest(task, address, &timeout, sizeof(timeout));
}

static bool write_deadline_after(struct task *task, addr_t address,
        clockid_t clock, int64_t nanoseconds) {
    struct timespec now;
    if (clock_gettime(clock, &now) != 0)
        return false;
    int64_t nsec = now.tv_nsec + nanoseconds;
    sqword_t sec = now.tv_sec + nsec / INT64_C(1000000000);
    nsec %= INT64_C(1000000000);
    return write_timeout(task, address, sec, nsec);
}

static sdword_t call_waitv(struct task *task, addr_t vector,
        dword_t count, dword_t flags, addr_t timeout, sdword_t clock) {
    struct task *saved = current;
    current = task;
    sdword_t result = (sdword_t) sys_futex_waitv(
            vector, count, flags, timeout, clock);
    current = saved;
    return result;
}

static sdword_t call_futex(struct task *task, addr_t address,
        dword_t operation, dword_t value,
        addr_t value2, addr_t second_address) {
    struct task *saved = current;
    current = task;
    sdword_t result = (sdword_t) sys_futex(address, operation, value,
            value2, second_address, 0);
    current = saved;
    return result;
}

static void *waitv_main(void *opaque) {
    struct waitv_call *call = opaque;
    current = call->task;
    task_thread_store(call->task, pthread_self());
    atomic_store_explicit(&call->stage, 1, memory_order_release);
    call->result = (sdword_t) sys_futex_waitv(call->vector,
            call->count, 0, call->timeout, call->clock);
    atomic_store_explicit(&call->stage, 2, memory_order_release);
    current = NULL;
    return NULL;
}

static bool start_waitv(struct waitv_call *call, struct task *task,
        addr_t vector, dword_t count, addr_t timeout, sdword_t clock) {
    *call = (struct waitv_call) {
        .task = task,
        .vector = vector,
        .count = count,
        .timeout = timeout,
        .clock = clock,
    };
    atomic_init(&call->stage, 0);
    if (pthread_create(&call->thread, NULL, waitv_main, call) != 0)
        return false;
    call->started = true;
    for (unsigned attempt = 0; attempt < 200000; attempt++) {
        if (atomic_load_explicit(
                    &call->stage, memory_order_acquire) != 0)
            return true;
        sched_yield();
    }
    // pthread 已成功创建；即使调度器暂未运行，也必须由调用方负责 join。
    return true;
}

static bool join_waitv(struct waitv_call *call, sdword_t expected) {
    if (!call->started || pthread_join(call->thread, NULL) != 0)
        return false;
    call->started = false;
    return atomic_load_explicit(
                    &call->stage, memory_order_acquire) == 2 &&
            call->result == expected;
}

static bool wait_until_queued(struct task *observer,
        addr_t address, bool private_mapping, dword_t count) {
    dword_t private_flag = private_mapping ? TEST_FUTEX_PRIVATE : 0;
    for (unsigned attempt = 0; attempt < 200000; attempt++) {
        if (call_futex(observer, address,
                    TEST_FUTEX_REQUEUE | private_flag,
                    0, count, address) == (sdword_t) count)
            return true;
        sched_yield();
    }
    return false;
}

static bool wait_until_interruptible(struct task *task) {
    for (unsigned attempt = 0; attempt < 200000; attempt++) {
        lock(&task->waiting_cond_lock);
        bool registered = task->waiting_cond != NULL &&
                task->waiting_lock != NULL;
        unlock(&task->waiting_cond_lock);
        if (registered)
            return true;
        sched_yield();
    }
    return false;
}

// 复现正式信号路径的锁顺序，测试 bit-only pending 对 condition 的唤醒。
static bool interrupt_waiter(struct task *task, int signal) {
    lock(&task->sighand->lock);
    sigset_add(&task->pending, signal);
    unlock(&task->sighand->lock);

    for (unsigned attempt = 0; attempt < 200000; attempt++) {
        lock(&task->waiting_cond_lock);
        cond_t *condition = task->waiting_cond;
        lock_t *wait_lock = task->waiting_lock;
        if (condition == NULL || wait_lock == NULL) {
            unlock(&task->waiting_cond_lock);
            sched_yield();
            continue;
        }
        if (trylock(wait_lock) == 0) {
            notify(condition);
            unlock(wait_lock);
            unlock(&task->waiting_cond_lock);
            return true;
        }
        unlock(&task->waiting_cond_lock);
        sched_yield();
    }
    return false;
}

static bool test_abi_and_copy_order(void) {
    struct test_fixture fixture;
    TEST_CHECK(fixture_init(&fixture, 4100), "建立 ABI 测试夹具");
    TEST_CHECK(map_pages(&fixture.task, TEST_BASE, 4, P_RWX | P_SHARED),
            "映射 ABI、超时与跨页复制区域");
    const dword_t size_flag = GUEST_LINUX_FUTEX2_SIZE_U32;
    TEST_CHECK(write_word(&fixture.task, WORD_ADDRESS, 7) &&
            write_waiter(&fixture.task, VECTOR_ADDRESS, 0,
                    7, WORD_ADDRESS, size_flag, 0),
            "写入一项标准 waitv 线结构");

    TEST_CHECK(sizeof(struct guest_linux_futex_waitv) == 24 &&
            _Alignof(struct guest_linux_futex_waitv) == 8 &&
            sizeof(struct guest_linux_kernel_timespec) == 16,
            "i386 使用固定 24 字节 waiter 与 16 字节 time64");
    fixture.task.cpu.eax = 449;
    fixture.task.cpu.ebx = VECTOR_ADDRESS;
    fixture.task.cpu.ecx = 1;
    fixture.task.cpu.edx = 1;
    fixture.task.cpu.esi = 0;
    fixture.task.cpu.edi = 0;
    handle_interrupt(INT_SYSCALL);
    TEST_CHECK((sdword_t) fixture.task.cpu.eax == _EINVAL,
            "INT 0x80 表项 449 接入 futex_waitv 并保留参数顺序");
    TEST_CHECK(call_waitv(&fixture.task, 0, 1, 1,
                    TEST_UNMAPPED, 99) == _EINVAL &&
            call_waitv(&fixture.task, 0, 1, 0, 0, 0) == _EINVAL &&
            call_waitv(&fixture.task, VECTOR_ADDRESS, 0, 0, 0, 0) ==
                    _EINVAL &&
            call_waitv(&fixture.task, VECTOR_ADDRESS, 129, 0, 0, 0) ==
                    _EINVAL,
            "系统调用标志、空指针与数量边界先于用户访问校验");
    TEST_CHECK(call_waitv(&fixture.task, TEST_UNMAPPED, 1, 0,
                    TEST_UNMAPPED, 99) == _EINVAL &&
            call_waitv(&fixture.task, TEST_UNMAPPED, 1, 0,
                    TEST_UNMAPPED, CLOCK_MONOTONIC_) == _EFAULT,
            "非空 timeout 先校验 clock，再读取 timeout，最后读取数组");
    TEST_CHECK(write_timeout(&fixture.task, TIMEOUT_ADDRESS, -1, 0) &&
            call_waitv(&fixture.task, TEST_UNMAPPED, 1, 0,
                    TIMEOUT_ADDRESS, CLOCK_MONOTONIC_) == _EINVAL,
            "非法 time64 在 waiter 数组故障前返回 EINVAL");

    const addr_t crossing = COPY_PAGE + PAGE_SIZE - 32;
    TEST_CHECK(write_waiter(&fixture.task, crossing, 0,
                    7, WORD_ADDRESS, size_flag, 1) &&
            call_waitv(&fixture.task, crossing, 2, 0, 0, 0) == _EINVAL,
            "首项语义错误先于后一项跨页读取故障");
    TEST_CHECK(write_waiter(&fixture.task, crossing, 0,
                    7, WORD_ADDRESS, size_flag, 0) &&
            call_waitv(&fixture.task, crossing, 2, 0, 0, 0) == _EFAULT,
            "前项有效时逐项复制精确报告后一项 EFAULT");

    TEST_CHECK(write_waiter(&fixture.task, VECTOR_ADDRESS, 0,
                    UINT64_C(1) << 32, WORD_ADDRESS, size_flag, 0) &&
            call_waitv(&fixture.task, VECTOR_ADDRESS, 1, 0, 0, 0) ==
                    _EINVAL &&
            write_waiter(&fixture.task, VECTOR_ADDRESS, 0,
                    7, WORD_ADDRESS, 0, 0) &&
            call_waitv(&fixture.task, VECTOR_ADDRESS, 1, 0, 0, 0) ==
                    _EINVAL &&
            write_waiter(&fixture.task, VECTOR_ADDRESS, 0,
                    7, WORD_ADDRESS, size_flag | UINT32_C(4), 0) &&
            call_waitv(&fixture.task, VECTOR_ADDRESS, 1, 0, 0, 0) ==
                    _EINVAL,
            "拒绝超宽值、缺失 U32 尺寸与未实现逐项标志");
    TEST_CHECK(write_waiter(&fixture.task, VECTOR_ADDRESS, 0,
                    7, UINT64_C(1) << 32, size_flag, 0) &&
            call_waitv(&fixture.task, VECTOR_ADDRESS, 1, 0, 0, 0) ==
                    _EFAULT &&
            write_waiter(&fixture.task, VECTOR_ADDRESS, 0,
                    7, WORD_ADDRESS + 1, size_flag, 0) &&
            call_waitv(&fixture.task, VECTOR_ADDRESS, 1, 0, 0, 0) ==
                    _EINVAL,
            "i386 不截断 64 位地址字段并拒绝未对齐 futex 字");

    futex_test_fail_allocation_at(0);
    sdword_t allocation_result = call_waitv(&fixture.task,
            TEST_UNMAPPED, 1, 0, 0, 0);
    futex_test_fail_allocation_at(SIZE_MAX);
    TEST_CHECK(allocation_result == _ENOMEM,
            "数组分配 OOM 发生在首项用户复制之前");

    fixture_destroy(&fixture);
    return true;
}

static bool test_wait_wake_vectors(void) {
    struct test_fixture parent;
    struct test_fixture same_mm;
    struct test_fixture child;
    struct test_fixture child_peer;
    TEST_CHECK(fixture_init(&parent, 4200), "建立向量等待父夹具");
    TEST_CHECK(map_pages(&parent.task, TEST_BASE, 3,
                    P_RWX | P_SHARED) &&
            map_pages(&parent.task, COPY_PAGE, 1, P_RWX),
            "映射共享向量与私有匿名键域控制页");
    TEST_CHECK(fixture_share(&same_mm, parent.task.mm, 4201) &&
            fixture_copy(&child, parent.task.mm, 4202) &&
            fixture_share(&child_peer, child.task.mm, 4203),
            "建立同 mm 线程与独立 mm 的共享后备观察者");
    const dword_t size_flag = GUEST_LINUX_FUTEX2_SIZE_U32;
    const addr_t first = WORD_ADDRESS;
    const addr_t second = WORD_ADDRESS + 4;
    const addr_t third = WORD_ADDRESS + 8;
    const addr_t requeue_target = WORD_ADDRESS + 12;
    const addr_t domain = COPY_PAGE;
    TEST_CHECK(write_word(&parent.task, first, 11) &&
            write_word(&parent.task, second, 12) &&
            write_word(&parent.task, third, 13) &&
            write_word(&parent.task, requeue_target, 14) &&
            write_word(&parent.task, domain, 19),
            "准备三项共享值与私有匿名键域控制值");

    TEST_CHECK(write_waiter(&parent.task, VECTOR_ADDRESS, 0,
                    11, first, size_flag, 0) &&
            write_deadline_after(&parent.task, TIMEOUT_ADDRESS,
                    CLOCK_MONOTONIC, INT64_C(2000000000)),
            "准备单项共享等待");
    struct waitv_call call;
    bool started = start_waitv(&call, &child.task, VECTOR_ADDRESS,
            1, TIMEOUT_ADDRESS, CLOCK_MONOTONIC_);
    bool queued = started && wait_until_queued(&parent.task, first, false, 1);
    sdword_t wake = queued ? call_futex(&parent.task, first,
            TEST_FUTEX_WAKE, 1, 0, 0) : -1;
    sdword_t isolated;
    bool joined = started && join_waitv(&call, 0);
    TEST_CHECK(queued && wake == 1 && joined,
            "一项 SHARED waitv 可由独立 mm 的同一后备唤醒");

    TEST_CHECK(write_waiter(&parent.task, VECTOR_ADDRESS, 0,
                    19, domain, size_flag | TEST_FUTEX_PRIVATE, 0) &&
            write_deadline_after(&parent.task, TIMEOUT_ADDRESS,
                    CLOCK_MONOTONIC, INT64_C(2000000000)),
            "准备私有匿名页上的 PRIVATE waitv 键域隔离");
    started = start_waitv(&call, &same_mm.task, VECTOR_ADDRESS,
            1, TIMEOUT_ADDRESS, CLOCK_MONOTONIC_);
    queued = started && wait_until_queued(
            &parent.task, domain, true, 1);
    isolated = queued ? call_futex(&parent.task, domain,
            TEST_FUTEX_WAKE, 1, 0, 0) : -1;
    wake = queued ? call_futex(&parent.task, domain,
            TEST_FUTEX_WAKE | TEST_FUTEX_PRIVATE, 1, 0, 0) : -1;
    joined = started && join_waitv(&call, 0);
    TEST_CHECK(queued && isolated == 0 && wake == 1 && joined,
            "mm-shared 键不命中同 VA 的 PRIVATE waitv 队列");

    TEST_CHECK(write_waiter(&parent.task, VECTOR_ADDRESS, 0,
                    19, domain, size_flag, 0) &&
            write_deadline_after(&parent.task, TIMEOUT_ADDRESS,
                    CLOCK_MONOTONIC, INT64_C(2000000000)),
            "准备私有匿名页上的 mm-shared waitv 键域隔离");
    started = start_waitv(&call, &same_mm.task, VECTOR_ADDRESS,
            1, TIMEOUT_ADDRESS, CLOCK_MONOTONIC_);
    queued = started && wait_until_queued(
            &parent.task, domain, false, 1);
    isolated = queued ? call_futex(&parent.task, domain,
            TEST_FUTEX_WAKE | TEST_FUTEX_PRIVATE, 1, 0, 0) : -1;
    wake = queued ? call_futex(&parent.task, domain,
            TEST_FUTEX_WAKE, 1, 0, 0) : -1;
    joined = started && join_waitv(&call, 0);
    TEST_CHECK(queued && isolated == 0 && wake == 1 && joined,
            "PRIVATE 键不命中同 VA 的 mm-shared waitv 队列");

    TEST_CHECK(write_waiter(&parent.task, VECTOR_ADDRESS, 0,
                    11, first, size_flag | TEST_FUTEX_PRIVATE, 0) &&
            write_deadline_after(&parent.task, TIMEOUT_ADDRESS,
                    CLOCK_MONOTONIC, INT64_C(2000000000)),
            "准备跨 mm PRIVATE 隔离等待");
    started = start_waitv(&call, &child.task, VECTOR_ADDRESS,
            1, TIMEOUT_ADDRESS, CLOCK_MONOTONIC_);
    queued = started && wait_until_queued(&child_peer.task, first, true, 1);
    isolated = queued ? call_futex(&parent.task, first,
            TEST_FUTEX_WAKE | TEST_FUTEX_PRIVATE, 1, 0, 0) : -1;
    wake = queued ? call_futex(&child_peer.task, first,
            TEST_FUTEX_WAKE | TEST_FUTEX_PRIVATE, 1, 0, 0) : -1;
    joined = started && join_waitv(&call, 0);
    TEST_CHECK(queued && isolated == 0 && wake == 1 && joined,
            "PRIVATE 项按 mm identity 隔离且同 mm 线程可唤醒");

    TEST_CHECK(write_waiter(&parent.task, VECTOR_ADDRESS, 0,
                    11, first, size_flag | TEST_FUTEX_PRIVATE, 0) &&
            write_waiter(&parent.task, VECTOR_ADDRESS, 1,
                    12, second, size_flag | TEST_FUTEX_PRIVATE, 0) &&
            write_waiter(&parent.task, VECTOR_ADDRESS, 2,
                    13, third, size_flag | TEST_FUTEX_PRIVATE, 0) &&
            write_deadline_after(&parent.task, TIMEOUT_ADDRESS,
                    CLOCK_MONOTONIC, INT64_C(2000000000)),
            "准备任意索引唤醒向量");
    started = start_waitv(&call, &same_mm.task, VECTOR_ADDRESS,
            3, TIMEOUT_ADDRESS, CLOCK_MONOTONIC_);
    queued = started && wait_until_queued(&parent.task, second, true, 1);
    wake = queued ? call_futex(&parent.task, second,
            TEST_FUTEX_WAKE | TEST_FUTEX_PRIVATE, 1, 0, 0) : -1;
    joined = started && join_waitv(&call, 1);
    TEST_CHECK(queued && wake == 1 && joined &&
            call_futex(&parent.task, first,
                    TEST_FUTEX_WAKE | TEST_FUTEX_PRIVATE,
                    1, 0, 0) == 0,
            "唤醒任意元素返回对应索引并原子摘除其余节点");

    TEST_CHECK(write_deadline_after(&parent.task, TIMEOUT_ADDRESS,
                    CLOCK_MONOTONIC, INT64_C(2000000000)),
            "准备 classic REQUEUE 后的向量等待");
    started = start_waitv(&call, &same_mm.task, VECTOR_ADDRESS,
            3, TIMEOUT_ADDRESS, CLOCK_MONOTONIC_);
    queued = started && wait_until_queued(&parent.task, second, true, 1);
    sdword_t requeued = queued ? call_futex(&parent.task, second,
            TEST_FUTEX_REQUEUE | TEST_FUTEX_PRIVATE,
            0, 1, requeue_target) : -1;
    sdword_t source_wake = requeued == 1 ? call_futex(
            &parent.task, second,
            TEST_FUTEX_WAKE | TEST_FUTEX_PRIVATE, 1, 0, 0) : -1;
    wake = requeued == 1 ? call_futex(&parent.task, requeue_target,
            TEST_FUTEX_WAKE | TEST_FUTEX_PRIVATE, 1, 0, 0) : -1;
    joined = started && join_waitv(&call, 1);
    TEST_CHECK(queued && requeued == 1 && source_wake == 0 &&
            wake == 1 && joined,
            "classic REQUEUE 转移后只从目标键唤醒并保留原向量索引");

    TEST_CHECK(write_waiter(&parent.task, VECTOR_ADDRESS, 0,
                    11, first, size_flag | TEST_FUTEX_PRIVATE, 0) &&
            write_waiter(&parent.task, VECTOR_ADDRESS, 1,
                    11, first, size_flag | TEST_FUTEX_PRIVATE, 0) &&
            write_deadline_after(&parent.task, TIMEOUT_ADDRESS,
                    CLOCK_MONOTONIC, INT64_C(2000000000)),
            "准备重复地址向量");
    started = start_waitv(&call, &same_mm.task, VECTOR_ADDRESS,
            2, TIMEOUT_ADDRESS, CLOCK_MONOTONIC_);
    queued = started && wait_until_queued(&parent.task, first, true, 2);
    wake = queued ? call_futex(&parent.task, first,
            TEST_FUTEX_WAKE | TEST_FUTEX_PRIVATE, 1, 0, 0) : -1;
    joined = started && join_waitv(&call, 0);
    TEST_CHECK(queued && wake == 1 && joined &&
            call_futex(&parent.task, first,
                    TEST_FUTEX_WAKE | TEST_FUTEX_PRIVATE,
                    2, 0, 0) == 0,
            "重复地址保留独立队列项且首次唤醒后清理同组余项");

    for (size_t index = 0; index < GUEST_LINUX_FUTEX_WAITV_MAX; index++) {
        addr_t word = WORD_ADDRESS + (addr_t) (index * sizeof(dword_t));
        if (!write_word(&parent.task, word, (dword_t) index + 100) ||
                !write_waiter(&parent.task, VECTOR_ADDRESS, index,
                        (dword_t) index + 100, word,
                        size_flag | TEST_FUTEX_PRIVATE, 0)) {
            fixture_destroy(&child_peer);
            fixture_destroy(&child);
            fixture_destroy(&same_mm);
            fixture_destroy(&parent);
            return false;
        }
    }
    TEST_CHECK(write_deadline_after(&parent.task, TIMEOUT_ADDRESS,
                    CLOCK_MONOTONIC, INT64_C(2000000000)),
            "准备 128 项向量超时");
    addr_t last = WORD_ADDRESS +
            (GUEST_LINUX_FUTEX_WAITV_MAX - 1) * sizeof(dword_t);
    started = start_waitv(&call, &same_mm.task, VECTOR_ADDRESS,
            GUEST_LINUX_FUTEX_WAITV_MAX,
            TIMEOUT_ADDRESS, CLOCK_MONOTONIC_);
    queued = started && wait_until_queued(&parent.task, last, true, 1);
    bool all_objects = queued && futex_test_live_count() ==
            GUEST_LINUX_FUTEX_WAITV_MAX;
    wake = queued ? call_futex(&parent.task, last,
            TEST_FUTEX_WAKE | TEST_FUTEX_PRIVATE, 1, 0, 0) : -1;
    joined = started && join_waitv(
            &call, GUEST_LINUX_FUTEX_WAITV_MAX - 1);
    TEST_CHECK(queued && all_objects && wake == 1 && joined &&
            futex_test_live_count() == 0,
            "128 项上限一次登记并返回末项索引且完整释放对象");

    fixture_destroy(&child_peer);
    fixture_destroy(&child);
    fixture_destroy(&same_mm);
    fixture_destroy(&parent);
    return true;
}

static bool test_absolute_timeouts_and_signal(void) {
    struct test_fixture fixture;
    struct test_fixture waiter;
    TEST_CHECK(fixture_init(&fixture, 4300), "建立超时测试夹具");
    TEST_CHECK(map_pages(&fixture.task, TEST_BASE, 3, P_RWX) &&
            fixture_share(&waiter, fixture.task.mm, 4301),
            "映射超时内存并建立等待线程任务");
    const dword_t flags = GUEST_LINUX_FUTEX2_SIZE_U32 |
            TEST_FUTEX_PRIVATE;
    TEST_CHECK(write_word(&fixture.task, WORD_ADDRESS, 23) &&
            write_waiter(&fixture.task, VECTOR_ADDRESS, 0,
                    23, WORD_ADDRESS, flags, 0),
            "准备超时向量");

    TEST_CHECK(write_timeout(&fixture.task, TIMEOUT_ADDRESS, 0, 0) &&
            call_waitv(&fixture.task, VECTOR_ADDRESS, 1, 0,
                    TIMEOUT_ADDRESS, CLOCK_MONOTONIC_) == _ETIMEDOUT &&
            call_waitv(&fixture.task, VECTOR_ADDRESS, 1, 0,
                    TIMEOUT_ADDRESS, CLOCK_REALTIME_) == _ETIMEDOUT,
            "MONOTONIC 与 REALTIME 的已过期绝对期限立即超时");
    TEST_CHECK(write_deadline_after(&fixture.task, TIMEOUT_ADDRESS,
                    CLOCK_MONOTONIC, INT64_C(20000000)) &&
            call_waitv(&fixture.task, VECTOR_ADDRESS, 1, 0,
                    TIMEOUT_ADDRESS, CLOCK_MONOTONIC_) == _ETIMEDOUT,
            "MONOTONIC 使用绝对而非相对 time64");
    TEST_CHECK(write_deadline_after(&fixture.task, TIMEOUT_ADDRESS,
                    CLOCK_REALTIME, INT64_C(20000000)) &&
            call_waitv(&fixture.task, VECTOR_ADDRESS, 1, 0,
                    TIMEOUT_ADDRESS, CLOCK_REALTIME_) == _ETIMEDOUT,
            "REALTIME 使用绝对期限并重看墙上时钟");

    TEST_CHECK(write_waiter(&fixture.task, VECTOR_ADDRESS, 0,
                    24, WORD_ADDRESS, flags, 0) &&
            call_waitv(&fixture.task, VECTOR_ADDRESS, 1, 0,
                    0, INT32_MIN) == _EAGAIN,
            "空 timeout 忽略 clockid 并继续比较 futex 值");
    TEST_CHECK(write_timeout(&fixture.task, TIMEOUT_ADDRESS, 0, -1) &&
            call_waitv(&fixture.task, VECTOR_ADDRESS, 1, 0,
                    TIMEOUT_ADDRESS, CLOCK_MONOTONIC_) == _EINVAL &&
            write_timeout(&fixture.task, TIMEOUT_ADDRESS,
                    0, INT64_C(1000000000)) &&
            call_waitv(&fixture.task, VECTOR_ADDRESS, 1, 0,
                    TIMEOUT_ADDRESS, CLOCK_REALTIME_) == _EINVAL,
            "拒绝负纳秒与十亿纳秒的非规范 time64");

    TEST_CHECK(write_waiter(&fixture.task, VECTOR_ADDRESS, 0,
                    23, WORD_ADDRESS, flags, 0) &&
            write_deadline_after(&fixture.task, TIMEOUT_ADDRESS,
                    CLOCK_MONOTONIC, INT64_C(2000000000)),
            "准备可中断 waitv");
    struct waitv_call call;
    bool started = start_waitv(&call, &waiter.task, VECTOR_ADDRESS,
            1, TIMEOUT_ADDRESS, CLOCK_MONOTONIC_);
    bool registered = started && wait_until_interruptible(&waiter.task);
    bool interrupted = registered && interrupt_waiter(
            &waiter.task, SIGUSR1_);
    bool joined = started && join_waitv(&call, _EINTR);
    lock(&waiter.task.sighand->lock);
    waiter.task.pending = 0;
    unlock(&waiter.task.sighand->lock);
    TEST_CHECK(registered && interrupted && joined &&
            futex_test_live_count() == 0,
            "登记后的未阻塞 guest 信号可靠返回 EINTR 并清空队列");

    fixture_destroy(&waiter);
    fixture_destroy(&fixture);
    return true;
}

static bool test_prepare_order_and_oom(void) {
    struct test_fixture fixture;
    TEST_CHECK(fixture_init(&fixture, 4400), "建立准备顺序测试夹具");
    TEST_CHECK(map_pages(&fixture.task, TEST_BASE, 3, P_RWX),
            "映射准备顺序测试内存");
    TEST_CHECK(map_pages(&fixture.task, COPY_PAGE, 1, P_READ) &&
            map_pages(&fixture.task, COPY_PAGE + PAGE_SIZE, 1,
                    P_READ | P_SHARED) &&
            map_pages(&fixture.task, PROT_NONE_PAGE, 1, 0) &&
            map_pages(&fixture.task, SPECIAL_PAGE, 1, P_RWX | P_SPECIAL) &&
            map_private_file_page(&fixture.task, FILE_COW_PAGE) &&
            map_private_file_page(&fixture.task, FILE_OOM_PAGE),
            "映射只读匿名页、PROT_NONE、特殊页与私有文件页");
    const dword_t size = GUEST_LINUX_FUTEX2_SIZE_U32;
    const dword_t private_flags = size | TEST_FUTEX_PRIVATE;
    const addr_t first = WORD_ADDRESS;
    const addr_t second = WORD_ADDRESS + 4;
    const addr_t third = WORD_ADDRESS + 8;
    TEST_CHECK(write_word(&fixture.task, first, 31) &&
            write_word(&fixture.task, second, 32) &&
            write_word(&fixture.task, third, 33),
            "准备故障顺序 futex 值");

    TEST_CHECK(write_waiter(&fixture.task, VECTOR_ADDRESS, 0,
                    30, first, private_flags, 0) &&
            write_waiter(&fixture.task, VECTOR_ADDRESS, 1,
                    0, TEST_UNMAPPED, private_flags, 0) &&
            call_waitv(&fixture.task, VECTOR_ADDRESS, 2,
                    0, 0, 0) == _EAGAIN,
            "前项 mismatch 先于后项 PRIVATE 值读取故障");
    TEST_CHECK(write_waiter(&fixture.task, VECTOR_ADDRESS, 1,
                    0, TEST_UNMAPPED, size, 0) &&
            call_waitv(&fixture.task, VECTOR_ADDRESS, 2,
                    0, 0, 0) == _EFAULT,
            "后项 SHARED 稳定键故障先于前项值 mismatch");
    TEST_CHECK(write_waiter(&fixture.task, VECTOR_ADDRESS, 0,
                    31, first, private_flags, 0) &&
            write_waiter(&fixture.task, VECTOR_ADDRESS, 1,
                    0, TEST_UNMAPPED, private_flags, 0) &&
            call_waitv(&fixture.task, VECTOR_ADDRESS, 2,
                    0, 0, 0) == _EFAULT,
            "前项匹配后精确报告后项 PRIVATE 值故障");
    TEST_CHECK(write_waiter(&fixture.task, VECTOR_ADDRESS, 0,
                    30, first, private_flags, 0) &&
            write_waiter(&fixture.task, VECTOR_ADDRESS, 1,
                    0, COPY_PAGE, size, 0) &&
            call_waitv(&fixture.task, VECTOR_ADDRESS, 2,
                    0, 0, 0) == _EFAULT,
            "只读匿名私有页的 SHARED 键故障先于前项值 mismatch");
    TEST_CHECK(write_waiter(&fixture.task, VECTOR_ADDRESS, 1,
                    0, COPY_PAGE, private_flags, 0) &&
            call_waitv(&fixture.task, VECTOR_ADDRESS, 2,
                    0, 0, 0) == _EAGAIN &&
            write_waiter(&fixture.task, VECTOR_ADDRESS, 0,
                    31, first, private_flags, 0) &&
            write_waiter(&fixture.task, VECTOR_ADDRESS, 1,
                    1, COPY_PAGE, private_flags, 0) &&
            call_waitv(&fixture.task, VECTOR_ADDRESS, 2,
                    0, 0, 0) == _EAGAIN,
            "PRIVATE 仍可读取只读匿名私有页并保留两阶段顺序");
    TEST_CHECK(write_waiter(&fixture.task, VECTOR_ADDRESS, 1,
                    1, COPY_PAGE + PAGE_SIZE, size, 0) &&
            call_waitv(&fixture.task, VECTOR_ADDRESS, 2,
                    0, 0, 0) == _EAGAIN,
            "只读匿名共享页仍可形成 SHARED 稳定键");
    TEST_CHECK(write_waiter(&fixture.task, VECTOR_ADDRESS, 0,
                    0, PROT_NONE_PAGE, size, 0) &&
            call_waitv(&fixture.task, VECTOR_ADDRESS, 1,
                    0, 0, 0) == _EFAULT &&
            write_waiter(&fixture.task, VECTOR_ADDRESS, 0,
                    0, PROT_NONE_PAGE, private_flags, 0) &&
            call_waitv(&fixture.task, VECTOR_ADDRESS, 1,
                    0, 0, 0) == _EFAULT,
            "SHARED 与 PRIVATE waitv 都拒绝读取 PROT_NONE 页");
    TEST_CHECK(write_waiter(&fixture.task, VECTOR_ADDRESS, 0,
                    0, SPECIAL_PAGE, size, 0) &&
            call_waitv(&fixture.task, VECTOR_ADDRESS, 1,
                    0, 0, 0) == _EFAULT &&
            write_waiter(&fixture.task, VECTOR_ADDRESS, 0,
                    1, SPECIAL_PAGE, private_flags, 0) &&
            call_waitv(&fixture.task, VECTOR_ADDRESS, 1,
                    0, 0, 0) == _EAGAIN,
            "SHARED waitv 拒绝特殊页且 PRIVATE 仍可读取其值");
    TEST_CHECK(write_waiter(&fixture.task, VECTOR_ADDRESS, 0,
                    1, FILE_OOM_PAGE, size, 0),
            "准备私有文件页 waitv ENOMEM 向量");
    mem_test_fail_private_cow_at(0);
    sdword_t file_oom_result = call_waitv(
            &fixture.task, VECTOR_ADDRESS, 1, 0, 0, 0);
    mem_test_fail_private_cow_at(SIZE_MAX);
    TEST_CHECK(file_oom_result == _ENOMEM,
            "waitv 原样传播共享键写固定的 ENOMEM");
    read_wrlock(&fixture.task.mem->lock);
    struct pt_entry *file_oom_entry =
            mem_pt(fixture.task.mem, PAGE(FILE_OOM_PAGE));
    bool file_oom_origin = file_oom_entry != NULL &&
            (file_oom_entry->flags & (P_FILE_BACKED | P_COW)) ==
                    (P_FILE_BACKED | P_COW);
    read_wrunlock(&fixture.task.mem->lock);
    TEST_CHECK(file_oom_origin,
            "waitv 分配失败不改变文件页后备状态");
    TEST_CHECK(write_waiter(&fixture.task, VECTOR_ADDRESS, 0,
                    1, FILE_COW_PAGE, size, 0) &&
            call_waitv(&fixture.task, VECTOR_ADDRESS, 1,
                    0, 0, 0) == _EAGAIN &&
            sys_mprotect(FILE_COW_PAGE, PAGE_SIZE, P_READ) == 0 &&
            call_waitv(&fixture.task, VECTOR_ADDRESS, 1,
                    0, 0, 0) == _EFAULT &&
            write_waiter(&fixture.task, VECTOR_ADDRESS, 0,
                    1, FILE_COW_PAGE, private_flags, 0) &&
            call_waitv(&fixture.task, VECTOR_ADDRESS, 1,
                    0, 0, 0) == _EAGAIN,
            "waitv 共享取键触发私有文件页 COW 并保留 PRIVATE 读取");
    read_wrlock(&fixture.task.mem->lock);
    struct pt_entry *file_cow_entry =
            mem_pt(fixture.task.mem, PAGE(FILE_COW_PAGE));
    bool file_cow_origin = file_cow_entry != NULL &&
            (file_cow_entry->flags & (P_FILE_BACKED | P_COW)) == 0 &&
            file_cow_entry->data->fd != NULL;
    read_wrunlock(&fixture.task.mem->lock);
    TEST_CHECK(file_cow_origin,
            "waitv 写固定只清物理文件来源并保留 VMA 文件元数据");
    TEST_CHECK(write_waiter(&fixture.task, VECTOR_ADDRESS, 0,
                    31, first, private_flags, 0) &&
            write_waiter(&fixture.task, VECTOR_ADDRESS, 1,
                    32, second, private_flags, 0) &&
            write_waiter(&fixture.task, VECTOR_ADDRESS, 2,
                    33, third, private_flags, 0),
            "准备三个唯一键的 OOM 向量");

    unsigned baseline = futex_test_live_count();
    bool oom_passed = baseline == 0;
    for (size_t allocation = 0; allocation <= 3; allocation++) {
        futex_test_fail_allocation_at(allocation);
        sdword_t result = call_waitv(&fixture.task,
                VECTOR_ADDRESS, 3, 0, 0, 0);
        futex_test_fail_allocation_at(SIZE_MAX);
        bool empty = call_futex(&fixture.task, first,
                TEST_FUTEX_REQUEUE | TEST_FUTEX_PRIVATE,
                0, 3, first) == 0;
        if (result != _ENOMEM || !empty ||
                futex_test_live_count() != baseline)
            oom_passed = false;
    }
    futex_test_fail_allocation_at(SIZE_MAX);
    TEST_CHECK(oom_passed,
            "数组及三个目标对象的每个 OOM 点均无队列和 live_count 副作用");

    fixture_destroy(&fixture);
    return true;
}

typedef bool (*scenario_function)(void);

static bool run_isolated(const char *name, scenario_function scenario) {
    pid_t child = fork();
    if (child < 0) {
        fprintf(stderr, "创建 %s 隔离进程失败：%s\n",
                name, strerror(errno));
        return false;
    }
    if (child == 0) {
        alarm(20);
        bool passed = scenario();
        if (passed && futex_test_live_count() != 0) {
            fprintf(stderr, "%s 遗留 futex 对象\n", name);
            passed = false;
        }
        futex_test_fail_allocation_at(SIZE_MAX);
        mem_test_fail_private_cow_at(SIZE_MAX);
        alarm(0);
        exit(passed ? EXIT_SUCCESS : EXIT_FAILURE);
    }

    int status;
    pid_t waited;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited != child) {
        fprintf(stderr, "等待 %s 隔离进程失败：%s\n",
                name, strerror(errno));
        return false;
    }
    if (WIFSIGNALED(status)) {
        fprintf(stderr, "%s 被 host 信号 %d 终止\n",
                name, WTERMSIG(status));
        return false;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "%s 返回状态 %d\n", name,
                WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        return false;
    }
    return true;
}

int main(void) {
    const struct {
        const char *name;
        scenario_function function;
    } scenarios[] = {
        {"i386 futex_waitv ABI 与逐项复制顺序",
                test_abi_and_copy_order},
        {"i386 futex_waitv 单项、多项与稳定键",
                test_wait_wake_vectors},
        {"i386 futex_waitv 绝对超时与信号",
                test_absolute_timeouts_and_signal},
        {"i386 futex_waitv 准备顺序与 OOM 原子性",
                test_prepare_order_and_oom},
    };
    for (size_t index = 0; index < array_size(scenarios); index++) {
        if (!run_isolated(
                scenarios[index].name, scenarios[index].function))
            return 1;
    }
    return 0;
}
