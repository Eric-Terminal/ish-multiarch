#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "debug.h"
#include "fs/fd.h"
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/futex.h"
#include "kernel/mm.h"
#include "kernel/signal.h"
#include "kernel/task.h"

#define TEST_FUTEX_WAIT 0
#define TEST_FUTEX_WAKE 1
#define TEST_FUTEX_REQUEUE 3

#define TEST_FUTEX_WAITERS UINT32_C(0x80000000)
#define TEST_FUTEX_OWNER_DIED UINT32_C(0x40000000)
#define TEST_FUTEX_TID_MASK UINT32_C(0x3fffffff)
#define TEST_ROBUST_PI UINT32_C(0x1)
#define TEST_ROBUST_LIMIT UINT32_C(2048)

#define TEST_BASE UINT32_C(0x10000000)
#define TEST_FAULT_PAGE UINT32_C(0x20000000)
#define TEST_LIMIT_BASE UINT32_C(0x30000000)
#define TEST_CTID_BASE UINT32_C(0x40000000)
#define TEST_LAST_PAGE UINT32_C(0xfffff000)

#define TEST_CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "i386 robust futex 生命周期测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        return false; \
    } \
} while (0)

// i386 guest wire 布局不随 host pointer、long 或 arm64_32 ABI 改变。
struct i386_robust_list_head_wire {
    dword_t next;
    sdword_t futex_offset;
    dword_t list_op_pending;
};

_Static_assert(sizeof(struct i386_robust_list_head_wire) == 12 &&
        _Alignof(struct i386_robust_list_head_wire) == 4 &&
        __builtin_offsetof(struct i386_robust_list_head_wire, next) == 0 &&
        __builtin_offsetof(
                struct i386_robust_list_head_wire, futex_offset) == 4 &&
        __builtin_offsetof(
                struct i386_robust_list_head_wire, list_op_pending) == 8,
        "i386 robust_list_head 必须保持 ILP32 wire 布局");

struct test_fixture {
    struct task task;
};

static const struct fd_ops test_executable_fd_ops = {0};

struct futex_waiter {
    pthread_t thread;
    struct task task;
    struct sighand *sighand;
    addr_t address;
    dword_t expected;
    atomic_uint stage;
    dword_t result;
    bool started;
};

static bool fixture_init(struct test_fixture *fixture) {
    memset(fixture, 0, sizeof(*fixture));
    fixture->task.pid = fixture->task.tgid = 1234;
    struct mm *mm = mm_new();
    if (mm == NULL)
        return false;
    task_set_mm(&fixture->task, mm);
    current = &fixture->task;
    return true;
}

static void fixture_destroy(struct test_fixture *fixture) {
    current = NULL;
    mm_release(fixture->task.mm);
    fixture->task.mm = NULL;
    fixture->task.mem = NULL;
}

static bool map_pages(struct test_fixture *fixture,
        addr_t address, pages_t count, unsigned flags) {
    write_wrlock(&fixture->task.mem->lock);
    int result = pt_map_nothing(
            fixture->task.mem, PAGE(address), count, flags);
    write_wrunlock(&fixture->task.mem->lock);
    return result == 0;
}

static bool protect_pages(struct test_fixture *fixture,
        addr_t address, pages_t count, unsigned flags) {
    write_wrlock(&fixture->task.mem->lock);
    int result = pt_set_flags(
            fixture->task.mem, PAGE(address), count, flags);
    write_wrunlock(&fixture->task.mem->lock);
    return result == 0;
}

static bool write_bytes(struct test_fixture *fixture,
        addr_t address, const void *bytes, size_t size) {
    return user_write_task(
            &fixture->task, address, bytes, size) == 0;
}

static bool write_u32(struct test_fixture *fixture,
        addr_t address, dword_t value) {
    return write_bytes(fixture, address, &value, sizeof(value));
}

static bool read_u32(struct test_fixture *fixture,
        addr_t address, dword_t *value) {
    return user_read_task(
            &fixture->task, address, value, sizeof(*value)) == 0;
}

static bool write_head(struct test_fixture *fixture,
        addr_t address, dword_t next, sdword_t offset,
        dword_t pending) {
    const struct i386_robust_list_head_wire head = {
        .next = next,
        .futex_offset = offset,
        .list_op_pending = pending,
    };
    return write_bytes(fixture, address, &head, sizeof(head));
}

static bool register_robust_head(
        struct test_fixture *fixture, addr_t address) {
    current = &fixture->task;
    return sys_set_robust_list(
            address, sizeof(struct i386_robust_list_head_wire)) == 0;
}

static dword_t call_futex(struct test_fixture *fixture,
        addr_t address, dword_t operation, dword_t value,
        addr_t value2, addr_t second_address) {
    current = &fixture->task;
    dword_t result = sys_futex(address, operation, value,
            value2, second_address, 0);
    // 直接调用 syscall helper 时补齐 dispatcher 负责的 trace 收尾。
    STRACE(" = 0x%x\n", result);
    return result;
}

static void *waiter_main(void *opaque) {
    struct futex_waiter *waiter = opaque;
    current = &waiter->task;
    atomic_store_explicit(
            &waiter->stage, 1, memory_order_release);
    waiter->result = sys_futex(
            waiter->address, TEST_FUTEX_WAIT,
            waiter->expected, 0, 0, 0);
    STRACE(" = 0x%x\n", waiter->result);
    atomic_store_explicit(
            &waiter->stage, 2, memory_order_release);
    current = NULL;
    return NULL;
}

static bool waiter_start(struct test_fixture *fixture,
        struct futex_waiter *waiter, pid_t_ pid,
        addr_t address, dword_t expected) {
    memset(waiter, 0, sizeof(*waiter));
    waiter->task.pid = waiter->task.tgid = pid;
    waiter->address = address;
    waiter->expected = expected;
    atomic_init(&waiter->stage, 0);

    mm_retain(fixture->task.mm);
    task_set_mm(&waiter->task, fixture->task.mm);
    waiter->sighand = sighand_new();
    if (waiter->sighand == NULL) {
        mm_release(waiter->task.mm);
        return false;
    }
    waiter->task.sighand = waiter->sighand;
    lock_init(&waiter->task.waiting_cond_lock);

    if (pthread_create(
            &waiter->thread, NULL, waiter_main, waiter) != 0) {
        pthread_mutex_destroy(&waiter->task.waiting_cond_lock.m);
        sighand_release(waiter->sighand);
        mm_release(waiter->task.mm);
        return false;
    }
    waiter->started = true;
    // 先让 worker 进入 futex 调用，避免探测循环在 host 调度前淹没日志缓冲。
    for (unsigned attempt = 0; attempt < 200000; attempt++) {
        if (atomic_load_explicit(
                    &waiter->stage, memory_order_acquire) != 0)
            return true;
        sched_yield();
    }
    return false;
}

static unsigned waiter_stage(const struct futex_waiter *waiter) {
    return atomic_load_explicit(
            &waiter->stage, memory_order_acquire);
}

static bool wait_until_queued(struct test_fixture *fixture,
        addr_t address, dword_t expected_count) {
    for (unsigned attempt = 0; attempt < 200000; attempt++) {
        dword_t count = call_futex(fixture,
                address, TEST_FUTEX_REQUEUE,
                0, expected_count, address);
        if (count == expected_count)
            return true;
        sched_yield();
    }
    return false;
}

static bool wait_for_returned_count(
        struct futex_waiter *waiters[], size_t count,
        size_t expected) {
    for (unsigned attempt = 0; attempt < 200000; attempt++) {
        size_t returned = 0;
        for (size_t index = 0; index < count; index++)
            returned += waiter_stage(waiters[index]) == 2;
        if (returned == expected)
            return true;
        if (returned > expected)
            return false;
        sched_yield();
    }
    return false;
}

static bool waiter_join_and_destroy(struct futex_waiter *waiter) {
    if (!waiter->started ||
            pthread_join(waiter->thread, NULL) != 0)
        return false;
    bool passed = waiter->result == 0 && waiter_stage(waiter) == 2;
    pthread_mutex_destroy(&waiter->task.waiting_cond_lock.m);
    sighand_release(waiter->sighand);
    mm_release(waiter->task.mm);
    waiter->started = false;
    return passed;
}

static bool test_listed_pending_pi_and_mismatch(void) {
    struct test_fixture fixture;
    TEST_CHECK(fixture_init(&fixture), "建立主链清理夹具");
    TEST_CHECK(map_pages(&fixture, TEST_BASE, 2, P_RWX),
            "映射主链测试页");

    const addr_t head = TEST_BASE + UINT32_C(0x40);
    const addr_t listed = TEST_BASE + UINT32_C(0x100);
    const addr_t pi = TEST_BASE + UINT32_C(0x140);
    const addr_t mismatch = TEST_BASE + UINT32_C(0x180);
    const addr_t pending = TEST_BASE + UINT32_C(0x1c0);
    const sdword_t offset = 8;
    const dword_t owner = (dword_t) fixture.task.pid;
    const dword_t owned_waiters = owner | TEST_FUTEX_WAITERS;
    const dword_t other_owner =
            ((owner + 7) & TEST_FUTEX_TID_MASK) |
            TEST_FUTEX_WAITERS;
    const dword_t pending_owner_zero = TEST_FUTEX_WAITERS;

    TEST_CHECK(write_head(
                    &fixture, head, listed, offset, pending) &&
            write_u32(&fixture, listed, pi | TEST_ROBUST_PI) &&
            write_u32(&fixture, pi, mismatch) &&
            write_u32(&fixture, mismatch, head) &&
            write_u32(&fixture, listed + offset, owned_waiters) &&
            write_u32(&fixture, pi + offset, owned_waiters) &&
            write_u32(&fixture, mismatch + offset, other_owner) &&
            write_u32(&fixture, pending + offset, pending_owner_zero),
            "准备普通、PI、owner mismatch 与 pending owner=0 节点");

    struct futex_waiter waiters[5];
    TEST_CHECK(waiter_start(&fixture, &waiters[0], 2000,
                    listed + offset, owned_waiters) &&
            wait_until_queued(&fixture, listed + offset, 1) &&
            waiter_start(&fixture, &waiters[1], 2001,
                    listed + offset, owned_waiters) &&
            wait_until_queued(&fixture, listed + offset, 2) &&
            waiter_start(&fixture, &waiters[2], 2002,
                    pi + offset, owned_waiters) &&
            wait_until_queued(&fixture, pi + offset, 1) &&
            waiter_start(&fixture, &waiters[3], 2003,
                    mismatch + offset, other_owner) &&
            wait_until_queued(&fixture, mismatch + offset, 1) &&
            waiter_start(&fixture, &waiters[4], 2004,
                    pending + offset, pending_owner_zero) &&
            wait_until_queued(&fixture, pending + offset, 1),
            "启动并确认五个 robust futex 等待者入队");

    TEST_CHECK(register_robust_head(&fixture, head),
            "注册主链 robust 头");
    futex_cleanup_task_i386(&fixture.task);

    dword_t listed_after;
    dword_t pi_after;
    dword_t mismatch_after;
    dword_t pending_after;
    TEST_CHECK(read_u32(&fixture, listed + offset, &listed_after) &&
            read_u32(&fixture, pi + offset, &pi_after) &&
            read_u32(&fixture, mismatch + offset, &mismatch_after) &&
            read_u32(&fixture, pending + offset, &pending_after) &&
            listed_after ==
                    (TEST_FUTEX_WAITERS | TEST_FUTEX_OWNER_DIED) &&
            pi_after ==
                    (TEST_FUTEX_WAITERS | TEST_FUTEX_OWNER_DIED) &&
            mismatch_after == other_owner &&
            pending_after == pending_owner_zero &&
            fixture.task.robust_list == 0,
            "按 owner、PI 与 pending 规则更新并消费 robust 注册");

    struct futex_waiter *listed_waiters[] = {
        &waiters[0], &waiters[1],
    };
    struct futex_waiter *pending_waiters[] = {&waiters[4]};
    TEST_CHECK(wait_for_returned_count(
                    listed_waiters, array_size(listed_waiters), 1) &&
            wait_for_returned_count(
                    pending_waiters, array_size(pending_waiters), 1) &&
            waiter_stage(&waiters[2]) == 1 &&
            waiter_stage(&waiters[3]) == 1,
            "listed 仅唤醒一个，pending owner=0 唤醒且 PI/mismatch 不误唤醒");

    TEST_CHECK(call_futex(&fixture, listed + offset,
                    TEST_FUTEX_WAKE, 1, 0, 0) == 1 &&
            call_futex(&fixture, pi + offset,
                    TEST_FUTEX_WAKE, 1, 0, 0) == 1 &&
            call_futex(&fixture, mismatch + offset,
                    TEST_FUTEX_WAKE, 1, 0, 0) == 1,
            "手动回收未被 robust 规则唤醒的等待者");
    for (size_t index = 0; index < array_size(waiters); index++)
        TEST_CHECK(waiter_join_and_destroy(&waiters[index]),
                "等待主链场景等待者正常退出");

    TEST_CHECK(atomic_load_explicit(
                    &fixture.task.mm->refcount,
                    memory_order_relaxed) == 1,
            "主链等待者释放全部 mm 用户引用");
    fixture_destroy(&fixture);
    return true;
}

static bool test_pending_owned_negative_offset_and_wrap(void) {
    struct test_fixture fixture;
    TEST_CHECK(fixture_init(&fixture), "建立负 offset 清理夹具");
    TEST_CHECK(map_pages(&fixture, TEST_BASE, 2, P_RWX),
            "映射负 offset 测试页");

    const addr_t head = TEST_BASE + UINT32_C(0x400);
    const addr_t listed = TEST_BASE + UINT32_C(0x508);
    const addr_t pending = TEST_BASE + UINT32_C(0x608);
    const sdword_t offset = -8;
    const dword_t owner = (dword_t) fixture.task.pid;
    const dword_t pending_value = owner | TEST_FUTEX_WAITERS;
    TEST_CHECK(write_head(
                    &fixture, head, listed, offset, pending) &&
            write_u32(&fixture, listed, head) &&
            write_u32(&fixture, listed + offset, owner) &&
            write_u32(&fixture, pending + offset, pending_value),
            "准备负 offset 的 listed 与 owner 匹配 pending");

    struct futex_waiter waiter;
    TEST_CHECK(waiter_start(&fixture, &waiter, 2100,
                    pending + offset, pending_value) &&
            wait_until_queued(&fixture, pending + offset, 1) &&
            register_robust_head(&fixture, head),
            "排入 pending 等待者并注册负 offset robust 头");
    futex_cleanup_task_i386(&fixture.task);

    dword_t listed_after;
    dword_t pending_after;
    struct futex_waiter *pending_waiters[] = {&waiter};
    TEST_CHECK(read_u32(&fixture, listed + offset, &listed_after) &&
            read_u32(&fixture, pending + offset, &pending_after) &&
            listed_after == TEST_FUTEX_OWNER_DIED &&
            pending_after ==
                    (TEST_FUTEX_WAITERS | TEST_FUTEX_OWNER_DIED) &&
            wait_for_returned_count(
                    pending_waiters, array_size(pending_waiters), 1) &&
            waiter_join_and_destroy(&waiter),
            "负 offset 正确定位且 owner 匹配 pending 更新并唤醒");

    TEST_CHECK(map_pages(&fixture, 0, 1, P_RWX) &&
            map_pages(&fixture, TEST_LAST_PAGE, 1, P_RWX),
            "映射 32 位地址空间首尾页");
    const addr_t wrapping_entry = UINT32_C(4);
    const addr_t wrapping_futex = UINT32_C(0xfffffffc);
    const addr_t wrapping_head = TEST_BASE + UINT32_C(0x700);
    TEST_CHECK(write_head(&fixture, wrapping_head,
                    wrapping_entry, -8, 0) &&
            write_u32(&fixture, wrapping_entry, wrapping_head) &&
            write_u32(&fixture, wrapping_futex, owner) &&
            register_robust_head(&fixture, wrapping_head),
            "准备 compat_uptr_t 模加法回绕节点");
    futex_cleanup_task_i386(&fixture.task);
    dword_t wrapping_after;
    TEST_CHECK(read_u32(&fixture, wrapping_futex, &wrapping_after) &&
            wrapping_after == TEST_FUTEX_OWNER_DIED,
            "32 位 entry 加负 offset 按 compat 地址模回绕");

    fixture_destroy(&fixture);
    return true;
}

static bool test_bad_head_and_next_fault(void) {
    struct test_fixture fixture;
    TEST_CHECK(fixture_init(&fixture), "建立 robust 故障夹具");
    TEST_CHECK(map_pages(&fixture, TEST_BASE, 2, P_RWX) &&
            map_pages(&fixture, TEST_FAULT_PAGE, 1, P_RWX),
            "映射 robust 故障测试页");

    const addr_t invalid_head = TEST_FAULT_PAGE + 2 * PAGE_SIZE;
    TEST_CHECK(register_robust_head(&fixture, invalid_head),
            "允许登记尚未探测的坏 robust 头");
    futex_cleanup_task_i386(&fixture.task);
    TEST_CHECK(fixture.task.robust_list == 0,
            "坏 robust 头静默终止且注册只消费一次");

    const addr_t head = TEST_BASE + UINT32_C(0x800);
    const addr_t fault_entry = TEST_FAULT_PAGE + PAGE_SIZE - 2;
    const addr_t fault_futex = TEST_FAULT_PAGE + PAGE_SIZE - 8;
    const addr_t pending = TEST_BASE + UINT32_C(0x906);
    const addr_t pending_futex = TEST_BASE + UINT32_C(0x900);
    const sdword_t offset = -6;
    const dword_t owner = (dword_t) fixture.task.pid;
    const byte_t next_prefix[2] = {0x34, 0x12};
    TEST_CHECK(write_head(&fixture, head,
                    fault_entry, offset, pending) &&
            write_bytes(&fixture, fault_entry,
                    next_prefix, sizeof(next_prefix)) &&
            write_u32(&fixture, fault_futex, owner) &&
            write_u32(&fixture, pending_futex, owner) &&
            register_robust_head(&fixture, head),
            "准备 next 跨入未映射页的当前节点");
    futex_cleanup_task_i386(&fixture.task);

    dword_t fault_after;
    dword_t pending_after;
    TEST_CHECK(read_u32(&fixture, fault_futex, &fault_after) &&
            read_u32(&fixture, pending_futex, &pending_after) &&
            fault_after == TEST_FUTEX_OWNER_DIED &&
            pending_after == owner &&
            fixture.task.robust_list == 0,
            "next 故障前仍修复当前节点，随后停止且跳过 pending");

    fixture_destroy(&fixture);
    return true;
}

static bool test_traversal_limit(void) {
    struct test_fixture fixture;
    TEST_CHECK(fixture_init(&fixture), "建立 robust 遍历上限夹具");
    TEST_CHECK(map_pages(&fixture, TEST_LIMIT_BASE, 5, P_RWX),
            "映射 2049 个 robust 节点");

    const addr_t head = TEST_LIMIT_BASE + UINT32_C(0x20);
    const addr_t first = TEST_LIMIT_BASE + UINT32_C(0x100);
    const addr_t stride = 8;
    const sdword_t offset = 4;
    const dword_t owner = (dword_t) fixture.task.pid;
    const addr_t pending = first +
            (TEST_ROBUST_LIMIT + 2) * stride;
    TEST_CHECK(write_head(&fixture, head, first, offset, pending),
            "写入 robust 遍历上限测试头");
    for (dword_t index = 0;
            index <= TEST_ROBUST_LIMIT; index++) {
        addr_t entry = first + index * stride;
        addr_t next = index == TEST_ROBUST_LIMIT ?
                head : entry + stride;
        TEST_CHECK(write_u32(&fixture, entry, next) &&
                write_u32(&fixture, entry + offset, owner),
                "构造超出 Linux robust 上限的主链");
    }
    TEST_CHECK(write_u32(&fixture, pending + offset, owner) &&
            register_robust_head(&fixture, head),
            "准备遍历上限后的独立 pending 节点");
    futex_cleanup_task_i386(&fixture.task);

    const addr_t last_processed = first +
            (TEST_ROBUST_LIMIT - 1) * stride;
    const addr_t capped = first + TEST_ROBUST_LIMIT * stride;
    dword_t first_after;
    dword_t last_after;
    dword_t capped_after;
    dword_t pending_after;
    TEST_CHECK(read_u32(&fixture, first + offset, &first_after) &&
            read_u32(&fixture, last_processed + offset, &last_after) &&
            read_u32(&fixture, capped + offset, &capped_after) &&
            read_u32(&fixture, pending + offset, &pending_after) &&
            first_after == TEST_FUTEX_OWNER_DIED &&
            last_after == TEST_FUTEX_OWNER_DIED &&
            capped_after == owner &&
            pending_after == TEST_FUTEX_OWNER_DIED &&
            fixture.task.robust_list == 0,
            "最多处理 2048 个主链节点并仍单独处理 pending");

    fixture_destroy(&fixture);
    return true;
}

static bool test_private_cow_isolation(void) {
    struct test_fixture fixture;
    TEST_CHECK(fixture_init(&fixture),
            "建立私有 COW robust 清理夹具");
    TEST_CHECK(map_pages(&fixture, TEST_BASE, 1, P_RWX),
            "映射私有 robust 测试页");

    const addr_t head = TEST_BASE + UINT32_C(0x40);
    const addr_t entry = TEST_BASE + UINT32_C(0x100);
    const sdword_t offset = 8;
    const dword_t owner = (dword_t) fixture.task.pid;
    const dword_t original = owner | TEST_FUTEX_WAITERS;
    TEST_CHECK(write_head(&fixture, head, entry, offset, 0) &&
            write_u32(&fixture, entry, head) &&
            write_u32(&fixture, entry + offset, original),
            "准备复制前的私有 robust 节点");

    struct task observer = {0};
    fixture.task.mm->exefile = fd_create(
            &test_executable_fd_ops);
    TEST_CHECK(fixture.task.mm->exefile != NULL,
            "建立 COW 地址空间的可执行文件引用");
    struct mm *observer_mm = mm_copy(fixture.task.mm);
    TEST_CHECK(observer_mm != NULL,
            "复制 COW 观察地址空间");
    task_set_mm(&observer, observer_mm);

    TEST_CHECK(register_robust_head(&fixture, head),
            "注册 COW 页上的 robust 头");
    futex_cleanup_task_i386(&fixture.task);

    dword_t active_after;
    dword_t observer_after;
    TEST_CHECK(read_u32(&fixture, entry + offset, &active_after) &&
            user_read_task(&observer, entry + offset,
                    &observer_after, sizeof(observer_after)) == 0 &&
            active_after ==
                    (TEST_FUTEX_WAITERS | TEST_FUTEX_OWNER_DIED) &&
            observer_after == original &&
            fixture.task.robust_list == 0,
            "robust 比较交换只写当前 COW 地址空间");

    mm_release(observer.mm);
    observer.mm = NULL;
    observer.mem = NULL;
    fixture_destroy(&fixture);
    return true;
}

static bool test_unaligned_host_mapping_fault(void) {
    struct test_fixture fixture;
    TEST_CHECK(fixture_init(&fixture),
            "建立 host 未对齐映射夹具");

    void *backing = mmap(NULL, PAGE_SIZE + 1,
            PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    TEST_CHECK(backing != MAP_FAILED,
            "分配带偏移的 host 后备");
    write_wrlock(&fixture.task.mem->lock);
    int map_result = pt_map(fixture.task.mem,
            PAGE(TEST_BASE), 1, backing, 1, P_RWX);
    write_wrunlock(&fixture.task.mem->lock);
    TEST_CHECK(map_result == 0,
            "建立 guest 对齐但 host 未对齐的映射");

    const addr_t head = TEST_BASE + UINT32_C(0x40);
    const addr_t entry = TEST_BASE + UINT32_C(0x100);
    const sdword_t offset = 8;
    const dword_t owner = (dword_t) fixture.task.pid;
    TEST_CHECK(write_head(&fixture, head, entry, offset, 0) &&
            write_u32(&fixture, entry, head) &&
            write_u32(&fixture, entry + offset, owner),
            "准备 host 未对齐的 robust futex 字");

    dword_t observed = UINT32_C(0x6a09e667);
    TEST_CHECK(mem_compare_exchange_u32(fixture.task.mem,
                    entry + offset, owner,
                    TEST_FUTEX_OWNER_DIED, &observed) ==
                    MEM_COMPARE_EXCHANGE_FAULT &&
            observed == UINT32_C(0x6a09e667),
            "未对齐 host 原子比较交换按故障返回且不改 observed");

    TEST_CHECK(register_robust_head(&fixture, head),
            "注册 host 未对齐映射上的 robust 头");
    futex_cleanup_task_i386(&fixture.task);
    dword_t after;
    TEST_CHECK(read_u32(&fixture, entry + offset, &after) &&
            after == owner && fixture.task.robust_list == 0,
            "robust 清理拒绝未对齐 host 原子访问并消费注册");

    fixture_destroy(&fixture);
    return true;
}

static bool test_clear_child_tid_mm_users_and_write_fault(void) {
    struct test_fixture fixture;
    TEST_CHECK(fixture_init(&fixture),
            "建立 i386 clear-child-tid 夹具");
    TEST_CHECK(map_pages(&fixture, TEST_CTID_BASE, 1, P_RWX),
            "映射 clear-child-tid 测试页");

    const addr_t clear_tid = TEST_CTID_BASE + UINT32_C(0x100);
    const dword_t marker = UINT32_C(0x6a09e667);
    TEST_CHECK(write_u32(&fixture, clear_tid, marker) &&
            atomic_load_explicit(&fixture.task.mm->refcount,
                    memory_order_relaxed) == 1,
            "准备仅由当前 task 使用的 clear-child-tid 字");
    fixture.task.clear_tid = clear_tid;
    futex_cleanup_task_i386(&fixture.task);
    dword_t after;
    TEST_CHECK(read_u32(&fixture, clear_tid, &after) &&
            after == marker && fixture.task.clear_tid == 0,
            "mm_users==1 时只消费 ctid 注册而不访问用户字");

    TEST_CHECK(protect_pages(&fixture,
                    TEST_CTID_BASE, 1, P_READ),
            "把 clear-child-tid 页降为只读");
    struct futex_waiter waiter;
    TEST_CHECK(waiter_start(&fixture, &waiter, 2200,
                    clear_tid, marker) &&
            wait_until_queued(&fixture, clear_tid, 1) &&
            atomic_load_explicit(&fixture.task.mm->refcount,
                    memory_order_relaxed) > 1,
            "以额外 mm 用户在只读 ctid 字上排入等待者");
    fixture.task.clear_tid = clear_tid;
    futex_cleanup_task_i386(&fixture.task);

    struct futex_waiter *waiters[] = {&waiter};
    TEST_CHECK(wait_for_returned_count(
                    waiters, array_size(waiters), 1) &&
            waiter_join_and_destroy(&waiter) &&
            read_u32(&fixture, clear_tid, &after) &&
            after == marker && fixture.task.clear_tid == 0 &&
            atomic_load_explicit(&fixture.task.mm->refcount,
                    memory_order_relaxed) == 1,
            "只读写零失败后仍 WAKE(1)、消费注册并保持用户字");

    fixture_destroy(&fixture);
    return true;
}

static bool test_robust_registration_abi(void) {
    struct test_fixture fixture;
    TEST_CHECK(fixture_init(&fixture),
            "建立 i386 robust 注册 ABI 夹具");
    TEST_CHECK(map_pages(&fixture, TEST_BASE, 1, P_RWX),
            "映射 get_robust_list 写回页");

    const addr_t registered = TEST_BASE + UINT32_C(0x800);
    const addr_t head_output = TEST_BASE + UINT32_C(0x100);
    const addr_t length_output = TEST_BASE + UINT32_C(0x104);
    const dword_t marker = UINT32_C(0x6a09e667);
    const dword_t robust_head_size =
            (dword_t) sizeof(struct i386_robust_list_head_wire);
    current = &fixture.task;
    TEST_CHECK(sys_set_robust_list(registered,
                    robust_head_size) == 0 &&
            sys_set_robust_list(registered,
                    robust_head_size - 1) ==
                    _EINVAL &&
            fixture.task.robust_list == registered,
            "set_robust_list 只接受精确 12 字节且失败不改注册");

    TEST_CHECK(write_u32(&fixture, head_output, marker) &&
            write_u32(&fixture, length_output, marker) &&
            sys_get_robust_list(
                    0, head_output, length_output) == 0,
            "pid 0 按 Linux 语义查询当前任务");
    dword_t observed_head;
    dword_t observed_length;
    TEST_CHECK(read_u32(&fixture, head_output, &observed_head) &&
            read_u32(&fixture, length_output, &observed_length) &&
            observed_head == registered &&
            observed_length == robust_head_size,
            "get_robust_list 写回固定宽度头指针与长度");

    TEST_CHECK(write_u32(&fixture, length_output, marker) &&
            sys_get_robust_list(0,
                    TEST_FAULT_PAGE, length_output) == _EFAULT &&
            read_u32(&fixture, length_output, &observed_length) &&
            observed_length == robust_head_size,
            "head 写回故障前已经提交长度");
    TEST_CHECK(write_u32(&fixture, head_output, marker) &&
            sys_get_robust_list(0,
                    head_output, TEST_FAULT_PAGE) == _EFAULT &&
            read_u32(&fixture, head_output, &observed_head) &&
            observed_head == marker,
            "长度写回故障时不得提前修改 head 输出");

    fixture.task.robust_list = 0;
    fixture_destroy(&fixture);
    return true;
}

typedef bool (*scenario_function)(void);

static bool run_isolated(
        const char *name, scenario_function scenario) {
    pid_t child = fork();
    if (child < 0) {
        fprintf(stderr, "创建 %s 隔离进程失败：%s\n",
                name, strerror(errno));
        return false;
    }
    if (child == 0) {
        alarm(30);
        bool passed = scenario();
        if (passed && futex_test_live_count() != 0) {
            fprintf(stderr, "%s 遗留 futex 对象\n", name);
            passed = false;
        }
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
        {"i386 robust 主链、PI、pending 与 owner mismatch",
                test_listed_pending_pi_and_mismatch},
        {"i386 robust pending owner 与负 offset",
                test_pending_owned_negative_offset_and_wrap},
        {"i386 robust 坏头与 next 故障",
                test_bad_head_and_next_fault},
        {"i386 robust 2048 遍历上限",
                test_traversal_limit},
        {"i386 robust 私有 COW 隔离",
                test_private_cow_isolation},
        {"i386 robust host 未对齐映射故障",
                test_unaligned_host_mapping_fault},
        {"i386 clear-child-tid mm 用户与写故障",
                test_clear_child_tid_mm_users_and_write_fault},
        {"i386 robust 注册 ABI 与部分写顺序",
                test_robust_registration_abi},
    };
    for (size_t index = 0; index < array_size(scenarios); index++) {
        if (!run_isolated(
                scenarios[index].name, scenarios[index].function))
            return 1;
    }
    return 0;
}
