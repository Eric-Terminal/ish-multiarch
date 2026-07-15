#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
#define TEST_FUTEX_PRIVATE UINT32_C(128)

#define TEST_FUTEX_WAITERS UINT32_C(0x80000000)
#define TEST_FUTEX_OWNER_DIED UINT32_C(0x40000000)

#define TEST_BASE UINT32_C(0x52000000)
#define TEST_UNMAPPED UINT32_C(0x62000000)

#define TEST_CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "i386 共享 futex 测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        return false; \
    } \
} while (0)

_Static_assert(sizeof(((struct mem *) 0)->identity) == 8 &&
        sizeof(((struct data *) 0)->identity) == 8 &&
        sizeof(((struct data *) 0)->file_backing_offset) == 8,
        "i386 futex 身份与后备偏移必须保持 64 位");

struct i386_robust_list_head_wire {
    dword_t next;
    sdword_t futex_offset;
    dword_t list_op_pending;
};

_Static_assert(sizeof(struct i386_robust_list_head_wire) == 12,
        "i386 robust_list_head 必须保持 32 位 wire 布局");

struct test_fixture {
    struct task task;
};

struct futex_waiter {
    pthread_t thread;
    struct task task;
    struct sighand *sighand;
    addr_t address;
    dword_t operation;
    dword_t expected;
    atomic_uint stage;
    dword_t result;
    bool started;
};

static const struct fd_ops test_executable_fd_ops = {0};

static bool fixture_init(struct test_fixture *fixture, pid_t_ pid) {
    memset(fixture, 0, sizeof(*fixture));
    fixture->task.pid = fixture->task.tgid = pid;
    struct mm *memory = mm_new();
    if (memory == NULL)
        return false;
    memory->exefile = fd_create(&test_executable_fd_ops);
    if (memory->exefile == NULL) {
        mm_release(memory);
        return false;
    }
    task_set_mm(&fixture->task, memory);
    current = &fixture->task;
    return true;
}

static bool fixture_copy(struct test_fixture *destination,
        const struct test_fixture *source, pid_t_ pid) {
    memset(destination, 0, sizeof(*destination));
    destination->task.pid = destination->task.tgid = pid;
    struct mm *memory = mm_copy(source->task.mm);
    if (memory == NULL)
        return false;
    task_set_mm(&destination->task, memory);
    return true;
}

static void fixture_destroy(struct test_fixture *fixture) {
    if (current == &fixture->task)
        current = NULL;
    mm_release(fixture->task.mm);
    fixture->task.mm = NULL;
    fixture->task.mem = NULL;
}

static bool map_pages(struct task *task, addr_t address,
        pages_t count, unsigned flags) {
    write_wrlock(&task->mem->lock);
    int result = pt_map_nothing(
            task->mem, PAGE(address), count, flags);
    write_wrunlock(&task->mem->lock);
    return result == 0;
}

static bool replace_page(struct task *task,
        addr_t address, unsigned flags) {
    write_wrlock(&task->mem->lock);
    int result = pt_unmap_always(task->mem, PAGE(address), 1);
    if (result == 0)
        result = pt_map_nothing(
                task->mem, PAGE(address), 1, flags);
    write_wrunlock(&task->mem->lock);
    return result == 0;
}

static bool unmap_page(struct task *task, addr_t address) {
    write_wrlock(&task->mem->lock);
    int result = pt_unmap_always(task->mem, PAGE(address), 1);
    write_wrunlock(&task->mem->lock);
    return result == 0;
}

static bool write_u32(
        struct task *task, addr_t address, dword_t value) {
    return user_write_task(
            task, address, &value, sizeof(value)) == 0;
}

static bool read_u32(
        struct task *task, addr_t address, dword_t *value) {
    return user_read_task(
            task, address, value, sizeof(*value)) == 0;
}

static bool write_robust_head(struct task *task,
        addr_t address, dword_t next, sdword_t offset) {
    const struct i386_robust_list_head_wire head = {
        .next = next,
        .futex_offset = offset,
    };
    return user_write_task(
            task, address, &head, sizeof(head)) == 0;
}

static sdword_t call_futex(struct task *task,
        addr_t address, dword_t operation, dword_t value,
        addr_t value2, addr_t second_address) {
    current = task;
    sdword_t result = (sdword_t) sys_futex(address, operation, value,
            value2, second_address, 0);
    STRACE(" = 0x%x\n", (dword_t) result);
    return result;
}

static void *waiter_main(void *opaque) {
    struct futex_waiter *waiter = opaque;
    current = &waiter->task;
    atomic_store_explicit(
            &waiter->stage, 1, memory_order_release);
    waiter->result = sys_futex(waiter->address,
            waiter->operation, waiter->expected, 0, 0, 0);
    STRACE(" = 0x%x\n", waiter->result);
    atomic_store_explicit(
            &waiter->stage, 2, memory_order_release);
    current = NULL;
    return NULL;
}

static bool waiter_start(struct futex_waiter *waiter,
        struct mm *memory, pid_t_ pid, addr_t address,
        dword_t operation, dword_t expected) {
    memset(waiter, 0, sizeof(*waiter));
    waiter->task.pid = waiter->task.tgid = pid;
    waiter->address = address;
    waiter->operation = operation;
    waiter->expected = expected;
    atomic_init(&waiter->stage, 0);

    mm_retain(memory);
    task_set_mm(&waiter->task, memory);
    waiter->sighand = sighand_new();
    if (waiter->sighand == NULL) {
        mm_release(memory);
        return false;
    }
    waiter->task.sighand = waiter->sighand;
    lock_init(&waiter->task.waiting_cond_lock);
    if (pthread_create(
            &waiter->thread, NULL, waiter_main, waiter) != 0) {
        pthread_mutex_destroy(&waiter->task.waiting_cond_lock.m);
        sighand_release(waiter->sighand);
        mm_release(memory);
        return false;
    }
    waiter->started = true;
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

static bool wait_until_queued(struct task *probe,
        addr_t address, dword_t wait_operation,
        dword_t expected_count) {
    dword_t private_flag = wait_operation & TEST_FUTEX_PRIVATE;
    for (unsigned attempt = 0; attempt < 200000; attempt++) {
        sdword_t count = call_futex(probe, address,
                TEST_FUTEX_REQUEUE | private_flag,
                0, expected_count, address);
        if (count == (sdword_t) expected_count)
            return true;
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

static bool test_shared_wait_wake_and_isolation(void) {
    struct test_fixture parent;
    struct test_fixture child;
    TEST_CHECK(fixture_init(&parent, 3000), "建立父地址空间");
    TEST_CHECK(map_pages(&parent.task, TEST_BASE, 3,
                    P_RWX | P_SHARED),
            "建立三页匿名共享映射");
    const addr_t first = TEST_BASE + UINT32_C(0x40);
    const addr_t second = TEST_BASE + PAGE_SIZE + UINT32_C(0x40);
    const addr_t generation =
            TEST_BASE + 2 * PAGE_SIZE + UINT32_C(0x40);
    const dword_t value = UINT32_C(7);
    TEST_CHECK(write_u32(&parent.task, first, value) &&
            write_u32(&parent.task, second, value) &&
            write_u32(&parent.task, generation, value),
            "写入共享 futex 初值");
    TEST_CHECK(fixture_copy(&child, &parent, 3001),
            "复制独立 mm 并共享匿名后备");

    struct futex_waiter shared_waiter;
    TEST_CHECK(waiter_start(&shared_waiter, child.task.mm,
                    3010, first, TEST_FUTEX_WAIT, value) &&
            wait_until_queued(
                    &parent.task, first, TEST_FUTEX_WAIT, 1) &&
            call_futex(&parent.task, first,
                    TEST_FUTEX_WAKE, 1, 0, 0) == 1 &&
            waiter_join_and_destroy(&shared_waiter),
            "不同 mm 通过同一匿名共享后备互相唤醒");

    struct futex_waiter private_waiter;
    TEST_CHECK(waiter_start(&private_waiter, child.task.mm,
                    3011, first,
                    TEST_FUTEX_WAIT | TEST_FUTEX_PRIVATE, value) &&
            wait_until_queued(&child.task, first,
                    TEST_FUTEX_WAIT | TEST_FUTEX_PRIVATE, 1) &&
            call_futex(&parent.task, first,
                    TEST_FUTEX_WAKE | TEST_FUTEX_PRIVATE,
                    1, 0, 0) == 0 &&
            waiter_stage(&private_waiter) == 1 &&
            call_futex(&child.task, first,
                    TEST_FUTEX_WAKE | TEST_FUTEX_PRIVATE,
                    1, 0, 0) == 1 &&
            waiter_join_and_destroy(&private_waiter),
            "PRIVATE 标志始终按 mm 隔离共享映射");

    struct futex_waiter offset_waiter;
    TEST_CHECK(waiter_start(&offset_waiter, child.task.mm,
                    3012, second, TEST_FUTEX_WAIT, value) &&
            wait_until_queued(
                    &parent.task, second, TEST_FUTEX_WAIT, 1) &&
            call_futex(&parent.task, first,
                    TEST_FUTEX_WAKE, 1, 0, 0) == 0 &&
            call_futex(&parent.task, second,
                    TEST_FUTEX_WAKE, 1, 0, 0) == 1 &&
            waiter_join_and_destroy(&offset_waiter),
            "同一后备的不同页偏移不发生键碰撞");

    struct futex_waiter generation_waiter;
    TEST_CHECK(waiter_start(&generation_waiter, child.task.mm,
                    3013, generation, TEST_FUTEX_WAIT, value) &&
            wait_until_queued(&parent.task,
                    generation, TEST_FUTEX_WAIT, 1) &&
            replace_page(&parent.task, generation,
                    P_RWX | P_SHARED) &&
            write_u32(&parent.task, generation, value) &&
            call_futex(&parent.task, generation,
                    TEST_FUTEX_WAKE, 1, 0, 0) == 0 &&
            waiter_stage(&generation_waiter) == 1 &&
            call_futex(&child.task, generation,
                    TEST_FUTEX_WAKE, 1, 0, 0) == 1 &&
            waiter_join_and_destroy(&generation_waiter),
            "同一 VA 的新后备不会误唤醒旧映射等待者");

    fixture_destroy(&child);
    fixture_destroy(&parent);
    return true;
}

static bool test_shared_requeue_and_errors(void) {
    struct test_fixture parent;
    struct test_fixture child;
    TEST_CHECK(fixture_init(&parent, 3100), "建立 REQUEUE 父地址空间");
    TEST_CHECK(map_pages(&parent.task, TEST_BASE, 2,
                    P_RWX | P_SHARED),
            "映射 REQUEUE 共享页");
    const addr_t source = TEST_BASE + UINT32_C(0x80);
    const addr_t target = TEST_BASE + PAGE_SIZE + UINT32_C(0x80);
    const dword_t value = UINT32_C(11);
    TEST_CHECK(write_u32(&parent.task, source, value) &&
            write_u32(&parent.task, target, value) &&
            fixture_copy(&child, &parent, 3101),
            "准备 REQUEUE 独立 mm");

    struct futex_waiter waiter;
    TEST_CHECK(waiter_start(&waiter, child.task.mm,
                    3110, source, TEST_FUTEX_WAIT, value) &&
            wait_until_queued(
                    &parent.task, source, TEST_FUTEX_WAIT, 1) &&
            call_futex(&parent.task, source,
                    TEST_FUTEX_REQUEUE, 0, 1, target) == 1 &&
            call_futex(&parent.task, source,
                    TEST_FUTEX_WAKE, 1, 0, 0) == 0 &&
            call_futex(&parent.task, target,
                    TEST_FUTEX_WAKE, 1, 0, 0) == 1 &&
            waiter_join_and_destroy(&waiter),
            "跨 mm REQUEUE 固定源键与目标键后迁移等待者");

    TEST_CHECK(waiter_start(&waiter, child.task.mm,
                    3111, source, TEST_FUTEX_WAIT, value) &&
            wait_until_queued(
                    &parent.task, source, TEST_FUTEX_WAIT, 1) &&
            unmap_page(&parent.task, target) &&
            call_futex(&parent.task, source,
                    TEST_FUTEX_REQUEUE, 0, 1, target) == _EFAULT &&
            waiter_stage(&waiter) == 1 &&
            call_futex(&parent.task, source,
                    TEST_FUTEX_WAKE, 1, 0, 0) == 1 &&
            waiter_join_and_destroy(&waiter),
            "目标解析故障发生在 REQUEUE 任何队列副作用之前");

    TEST_CHECK(map_pages(&parent.task, target, 1,
                    P_RWX | P_SHARED) &&
            write_u32(&parent.task, target, value) &&
            waiter_start(&waiter, child.task.mm,
                    3112, source, TEST_FUTEX_WAIT, value) &&
            wait_until_queued(
                    &parent.task, source, TEST_FUTEX_WAIT, 1),
            "为 REQUEUE OOM 建立新目标后备");
    futex_test_fail_allocation_at(0);
    sdword_t oom_result = call_futex(&parent.task, source,
            TEST_FUTEX_REQUEUE, 0, 1, target);
    futex_test_fail_allocation_at(SIZE_MAX);
    TEST_CHECK(oom_result == _ENOMEM &&
            waiter_stage(&waiter) == 1 &&
            call_futex(&parent.task, source,
                    TEST_FUTEX_WAKE, 1, 0, 0) == 1 &&
            waiter_join_and_destroy(&waiter),
            "目标对象 OOM 时 REQUEUE 保持零副作用");

    TEST_CHECK(call_futex(&parent.task, TEST_UNMAPPED,
                    TEST_FUTEX_WAKE | TEST_FUTEX_PRIVATE,
                    1, 0, 0) == 0 &&
            call_futex(&parent.task, TEST_UNMAPPED,
                    TEST_FUTEX_REQUEUE | TEST_FUTEX_PRIVATE,
                    0, 1, TEST_UNMAPPED + PAGE_SIZE) == 0 &&
            call_futex(&parent.task, TEST_UNMAPPED,
                    TEST_FUTEX_WAKE, 1, 0, 0) == _EFAULT &&
            call_futex(&parent.task, TEST_UNMAPPED,
                    TEST_FUTEX_REQUEUE, UINT32_MAX,
                    1, TEST_UNMAPPED + PAGE_SIZE) == _EINVAL &&
            call_futex(&parent.task, TEST_UNMAPPED,
                    TEST_FUTEX_REQUEUE, 0,
                    UINT32_MAX, TEST_UNMAPPED + PAGE_SIZE) == _EINVAL,
            "PRIVATE 空地址与共享解析、负 REQUEUE 计数遵循 Linux 顺序");

    fixture_destroy(&child);
    fixture_destroy(&parent);
    return true;
}

static bool test_mapping_metadata_and_mremap(void) {
    struct test_fixture parent;
    TEST_CHECK(fixture_init(&parent, 3200), "建立映射元数据夹具");
    TEST_CHECK(map_pages(&parent.task, TEST_BASE, 1,
                    P_RWX | P_SHARED),
            "映射待保护与扩展的匿名共享页");

    current = &parent.task;
    TEST_CHECK(sys_mprotect(TEST_BASE, PAGE_SIZE, P_READ) == 0,
            "降低共享页保护");
    read_wrlock(&parent.task.mem->lock);
    unsigned protected_flags =
            mem_pt(parent.task.mem, PAGE(TEST_BASE))->flags;
    read_wrunlock(&parent.task.mem->lock);
    TEST_CHECK((protected_flags & (P_SHARED | P_ANONYMOUS)) ==
                    (P_SHARED | P_ANONYMOUS) &&
            (protected_flags & P_RWX) == P_READ,
            "mprotect 只替换 RWX 且保留共享映射元数据");
    TEST_CHECK(sys_mprotect(TEST_BASE, PAGE_SIZE, P_RWX) == 0,
            "恢复共享页写权限");

    TEST_CHECK(sys_mremap(TEST_BASE, 1,
                    PAGE_SIZE + 1, 0) == _EFAULT,
            "没有单一 shmem 后备时拒绝伪造共享扩展段");
    TEST_CHECK(replace_page(&parent.task, TEST_BASE, P_RWX),
            "改建私有匿名映射以验证普通扩展");
    TEST_CHECK(sys_mremap(TEST_BASE, 1,
                    PAGE_SIZE + 1, 0) == (int_t) TEST_BASE,
            "非整页长度按向上取整扩展私有映射");

    read_wrlock(&parent.task.mem->lock);
    struct pt_entry *extension = mem_pt(
            parent.task.mem, PAGE(TEST_BASE) + 1);
    bool extension_matches = extension != NULL &&
            (extension->flags & P_ANONYMOUS) != 0 &&
            (extension->flags & P_SHARED) == 0;
    read_wrunlock(&parent.task.mem->lock);
    TEST_CHECK(extension_matches,
            "mremap 扩展段保持私有匿名映射元数据");

    const addr_t extended_word =
            TEST_BASE + PAGE_SIZE + UINT32_C(0x100);
    const dword_t value = UINT32_C(19);
    dword_t observed;
    TEST_CHECK(write_u32(&parent.task, extended_word, value) &&
            read_u32(&parent.task, extended_word, &observed) &&
            observed == value,
            "私有扩展段可以正常读写");

    TEST_CHECK(sys_mremap(TEST_BASE, 0, PAGE_SIZE, 0) == _EINVAL &&
            sys_mremap(TEST_BASE, PAGE_SIZE, 0, 0) == _EINVAL &&
            sys_mremap(TEST_BASE, PAGE_SIZE + 1, 1, 0) ==
                    (int_t) TEST_BASE,
            "拒绝零长度并按向上取整缩小映射");
    read_wrlock(&parent.task.mem->lock);
    bool extension_removed = mem_pt(
            parent.task.mem, PAGE(TEST_BASE) + 1) == NULL;
    struct data *hint_original =
            mem_pt(parent.task.mem, PAGE(TEST_BASE))->data;
    read_wrunlock(&parent.task.mem->lock);
    TEST_CHECK(extension_removed, "缩小映射移除完整尾页");

    addr_t hinted = sys_mmap2(TEST_BASE, PAGE_SIZE, P_RWX,
            MMAP_PRIVATE | MMAP_ANONYMOUS, -1, 0);
    read_wrlock(&parent.task.mem->lock);
    bool hint_preserved =
            mem_pt(parent.task.mem, PAGE(TEST_BASE))->data == hint_original;
    bool hint_mapped = hinted != TEST_BASE &&
            mem_pt(parent.task.mem, PAGE(hinted)) != NULL;
    read_wrunlock(&parent.task.mem->lock);
    TEST_CHECK(hint_mapped && hint_preserved,
            "冲突的非 FIXED hint 改找空洞且不覆盖原映射");

    fixture_destroy(&parent);
    return true;
}

static bool test_cross_mm_robust_and_clear_tid(void) {
    struct test_fixture parent;
    struct test_fixture child;
    TEST_CHECK(fixture_init(&parent, 3300), "建立退出清理父地址空间");
    TEST_CHECK(map_pages(&parent.task, TEST_BASE, 2,
                    P_RWX | P_SHARED),
            "映射 robust 与 clear-child-tid 共享页");
    TEST_CHECK(fixture_copy(&child, &parent, 3301),
            "复制退出任务独立 mm");

    const addr_t head = TEST_BASE + UINT32_C(0x40);
    const addr_t entry = TEST_BASE + UINT32_C(0x100);
    const sdword_t offset = 8;
    const addr_t robust_word = entry + offset;
    const dword_t owned =
            (dword_t) child.task.pid | TEST_FUTEX_WAITERS;
    TEST_CHECK(write_robust_head(
                    &parent.task, head, entry, offset) &&
            write_u32(&parent.task, entry, head) &&
            write_u32(&parent.task, robust_word, owned),
            "准备共享 robust 节点");

    struct futex_waiter waiter;
    TEST_CHECK(waiter_start(&waiter, parent.task.mm,
                    3310, robust_word, TEST_FUTEX_WAIT, owned) &&
            wait_until_queued(
                    &child.task, robust_word, TEST_FUTEX_WAIT, 1),
            "让另一 mm 的 robust 等待者入队");
    child.task.robust_list = head;
    current = &child.task;
    futex_cleanup_task_i386(&child.task);
    dword_t robust_after;
    TEST_CHECK(waiter_join_and_destroy(&waiter) &&
            read_u32(&parent.task, robust_word, &robust_after) &&
            robust_after ==
                    (TEST_FUTEX_WAITERS | TEST_FUTEX_OWNER_DIED),
            "robust CAS 使用实际共享映射快照唤醒另一 mm");

    const addr_t clear_tid =
            TEST_BASE + PAGE_SIZE + UINT32_C(0x100);
    const dword_t tid = (dword_t) child.task.pid;
    TEST_CHECK(write_u32(&parent.task, clear_tid, tid) &&
            waiter_start(&waiter, parent.task.mm,
                    3311, clear_tid, TEST_FUTEX_WAIT, tid) &&
            wait_until_queued(
                    &child.task, clear_tid, TEST_FUTEX_WAIT, 1),
            "让另一 mm 的 clear-child-tid 等待者入队");
    child.task.clear_tid = clear_tid;
    // Linux 只在退出 mm 仍有其他用户时执行 clear-child-tid。
    mm_retain(child.task.mm);
    current = &child.task;
    futex_cleanup_task_i386(&child.task);
    mm_release(child.task.mm);
    dword_t clear_after;
    TEST_CHECK(waiter_join_and_destroy(&waiter) &&
            read_u32(&parent.task, clear_tid, &clear_after) &&
            clear_after == 0 && child.task.clear_tid == 0,
            "clear-child-tid 清零后按共享键重新解析并跨 mm 唤醒");

    fixture_destroy(&child);
    fixture_destroy(&parent);
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
        {"i386 共享 WAIT/WAKE、PRIVATE 与代际隔离",
                test_shared_wait_wake_and_isolation},
        {"i386 共享 REQUEUE、故障顺序与 OOM 原子性",
                test_shared_requeue_and_errors},
        {"i386 映射元数据与 mremap 边界",
                test_mapping_metadata_and_mremap},
        {"i386 跨 mm robust 与 clear-child-tid",
                test_cross_mm_robust_and_clear_tid},
    };
    for (size_t index = 0; index < array_size(scenarios); index++) {
        if (!run_isolated(
                scenarios[index].name, scenarios[index].function))
            return 1;
    }
    return 0;
}
