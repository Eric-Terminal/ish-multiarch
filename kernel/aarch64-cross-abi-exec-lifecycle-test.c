#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "debug.h"
#include "fs/fd.h"
#include "guest/aarch64/elf64.h"
#include "guest/aarch64/linux-process.h"
#include "guest/memory/address-space.h"
#include "kernel/aarch64-signal-service.h"
#include "kernel/aarch64-syscall-service.h"
#include "kernel/calls.h"
#include "kernel/futex.h"
#include "kernel/mm.h"
#include "kernel/signal.h"
#include "kernel/task.h"

#define TEST_CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "i386 到 AArch64 exec 生命周期测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        return false; \
    } \
} while (0)

#define IMAGE_SIZE 1024
#define IMAGE_FILE_SIZE UINT64_C(0x300)
#define IMAGE_ALIGNMENT UINT64_C(0x10000)
#define ENTRY_OFFSET UINT64_C(0x200)
#define LOAD_BIAS UINT64_C(0x0000400000000000)
#define STACK_TOP UINT64_C(0x00007fff00000000)
#define SIGNAL_TRAMPOLINE UINT64_C(0x00007ffe00000000)

#define TEST_PID 8
#define I386_BASE UINT32_C(0x00010000)
#define I386_ROBUST_HEAD I386_BASE
#define I386_CLEAR_TID (I386_ROBUST_HEAD + UINT32_C(4))
#define I386_ROBUST_ENTRY (I386_ROBUST_HEAD + UINT32_C(0x40))
#define I386_ROBUST_WORD (I386_ROBUST_ENTRY + UINT32_C(8))
#define I386_TIMEOUT (I386_ROBUST_HEAD + UINT32_C(0x100))
#define I386_READONLY_CLEAR_TID \
        (I386_BASE + PAGE_SIZE + UINT32_C(0x100))

#define I386_FUTEX_WAIT UINT32_C(0)
#define I386_FUTEX_REQUEUE UINT32_C(3)
#define I386_FUTEX_WAITERS UINT32_C(0x80000000)
#define I386_FUTEX_OWNER_DIED UINT32_C(0x40000000)

struct i386_robust_list_head_wire {
    dword_t next;
    sdword_t futex_offset;
    dword_t list_op_pending;
};

_Static_assert(sizeof(struct i386_robust_list_head_wire) == 12 &&
        offsetof(struct i386_robust_list_head_wire, next) == 0 &&
        offsetof(struct i386_robust_list_head_wire, futex_offset) == 4 &&
        offsetof(struct i386_robust_list_head_wire,
                list_op_pending) == 8,
        "i386 robust head 必须保持三个 32 位 wire 字");

struct exec_fixture {
    struct task task;
    struct task observer;
};

struct i386_waiter {
    struct task task;
    struct sighand *sighand;
    pthread_t thread;
    addr_t address;
    dword_t expected;
    int result;
};

static const struct fd_ops metadata_fd_ops = {0};

static void put_u16(byte_t *bytes, word_t value) {
    bytes[0] = (byte_t) value;
    bytes[1] = (byte_t) (value >> 8);
}

static void put_u32(byte_t *bytes, dword_t value) {
    for (byte_t index = 0; index < 4; index++)
        bytes[index] = (byte_t) (value >> (index * 8));
}

static void put_u64(byte_t *bytes, qword_t value) {
    for (byte_t index = 0; index < 8; index++)
        bytes[index] = (byte_t) (value >> (index * 8));
}

static void put_program_header(byte_t *bytes, dword_t type,
        dword_t flags, qword_t offset, qword_t address,
        qword_t file_size, qword_t memory_size, qword_t alignment) {
    put_u32(bytes, type);
    put_u32(bytes + 4, flags);
    put_u64(bytes + 8, offset);
    put_u64(bytes + 16, address);
    put_u64(bytes + 32, file_size);
    put_u64(bytes + 40, memory_size);
    put_u64(bytes + 48, alignment);
}

static void make_image(byte_t file[IMAGE_SIZE]) {
    memset(file, 0, IMAGE_SIZE);
    file[0] = 0x7f;
    file[1] = 'E';
    file[2] = 'L';
    file[3] = 'F';
    file[4] = 2;
    file[5] = 1;
    file[6] = 1;
    put_u16(file + 16, 3);
    put_u16(file + 18, 183);
    put_u32(file + 20, 1);
    put_u64(file + 24, ENTRY_OFFSET);
    put_u64(file + 32, AARCH64_ELF64_HEADER_SIZE);
    put_u16(file + 52, AARCH64_ELF64_HEADER_SIZE);
    put_u16(file + 54, AARCH64_ELF64_PROGRAM_HEADER_SIZE);
    put_u16(file + 56, 2);

    byte_t *headers = file + AARCH64_ELF64_HEADER_SIZE;
    qword_t header_bytes = 2 * AARCH64_ELF64_PROGRAM_HEADER_SIZE;
    put_program_header(headers, 6, 4,
            AARCH64_ELF64_HEADER_SIZE, AARCH64_ELF64_HEADER_SIZE,
            header_bytes, header_bytes, 8);
    put_program_header(headers + AARCH64_ELF64_PROGRAM_HEADER_SIZE,
            1, 5, 0, 0, IMAGE_FILE_SIZE,
            IMAGE_FILE_SIZE, IMAGE_ALIGNMENT);
    put_u32(file + ENTRY_OFFSET, UINT32_C(0xd503201f));
}

static struct aarch64_linux_process *make_candidate(
        struct task *task) {
    byte_t file[IMAGE_SIZE];
    make_image(file);
    const char *arguments[] = {"cross-abi-exec"};
    byte_t random[AARCH64_LINUX_PROCESS_RANDOM_SIZE] = {0};
    const struct aarch64_linux_process_config config = {
        .elf_data = file,
        .elf_size = sizeof(file),
        .load_bias = LOAD_BIAS,
        .stack_top = STACK_TOP,
        .stack_size = 2 * GUEST_MEMORY_PAGE_SIZE,
        .signal_trampoline_page = SIGNAL_TRAMPOLINE,
        .brk_limit = LOAD_BIAS + UINT64_C(0x100000),
        .executable = "/bin/cross-abi-exec",
        .arguments = arguments,
        .argument_count = array_size(arguments),
        .random = random,
        .tid = task->pid,
        .task_opaque = task,
        .syscalls = &ish_aarch64_linux_syscall_service,
        .signals = &ish_aarch64_linux_signal_service,
    };
    return aarch64_linux_process_create(&config, NULL);
}

static bool stage_candidate(struct task *task) {
    struct aarch64_linux_process *candidate = make_candidate(task);
    struct mm *metadata = mm_new();
    if (candidate == NULL || metadata == NULL) {
        aarch64_linux_process_destroy(candidate);
        if (metadata != NULL)
            mm_release(metadata);
        return false;
    }
    if (task_stage_aarch64_exec(task, candidate, metadata))
        return true;
    aarch64_linux_process_destroy(candidate);
    mm_release(metadata);
    return false;
}

static bool init_fixture(struct exec_fixture *fixture) {
    memset(fixture, 0, sizeof(*fixture));
    fixture->task.pid = fixture->task.tgid = TEST_PID;
    lock_init(&fixture->task.general_lock);
    struct mm *mm = mm_new();
    struct fd *metadata = fd_create(&metadata_fd_ops);
    if (mm == NULL || metadata == NULL) {
        if (mm != NULL)
            mm_release(mm);
        if (metadata != NULL)
            fd_close(metadata);
        return false;
    }
    mm->exefile = metadata;
    task_set_mm(&fixture->task, mm);
    current = &fixture->task;

    write_wrlock(&fixture->task.mem->lock);
    int error = pt_map_nothing(fixture->task.mem,
            PAGE(I386_BASE), 2, P_RWX | P_SHARED);
    write_wrunlock(&fixture->task.mem->lock);
    if (error != 0)
        return false;

    const struct timespec_ timeout = {.sec = 2};
    return user_write_task(&fixture->task,
            I386_TIMEOUT, &timeout, sizeof(timeout)) == 0;
}

static bool create_observer(struct exec_fixture *fixture) {
    struct mm *observer_mm = mm_copy(fixture->task.mm);
    if (observer_mm == NULL)
        return false;
    fixture->observer.pid = TEST_PID + 1;
    task_set_mm(&fixture->observer, observer_mm);
    return true;
}

static void destroy_fixture(struct exec_fixture *fixture) {
    task_discard_aarch64_exec(&fixture->task);
    task_release_aarch64_process(&fixture->task);
    if (fixture->task.mm != NULL)
        mm_release(fixture->task.mm);
    if (fixture->observer.mm != NULL)
        mm_release(fixture->observer.mm);
    fixture->task.mm = NULL;
    fixture->task.mem = NULL;
    fixture->observer.mm = NULL;
    fixture->observer.mem = NULL;
    pthread_mutex_destroy(&fixture->task.general_lock.m);
    current = NULL;
}

static bool write_u32(struct task *task,
        addr_t address, dword_t value) {
    return user_write_task(task,
            address, &value, sizeof(value)) == 0;
}

static bool read_u32(struct task *task,
        addr_t address, dword_t *value) {
    return user_read_task(task,
            address, value, sizeof(*value)) == 0;
}

static bool configure_robust_and_clear_tid(
        struct exec_fixture *fixture) {
    const struct i386_robust_list_head_wire head = {
        .next = I386_ROBUST_ENTRY,
        .futex_offset = 8,
    };
    const dword_t next = I386_ROBUST_HEAD;
    const dword_t robust = I386_FUTEX_WAITERS | TEST_PID;
    return user_write_task(&fixture->task, I386_ROBUST_HEAD,
                    &head, sizeof(head)) == 0 &&
            write_u32(&fixture->task, I386_ROBUST_ENTRY, next) &&
            write_u32(&fixture->task, I386_ROBUST_WORD, robust) &&
            sys_set_robust_list(I386_ROBUST_HEAD,
                    sizeof(struct i386_robust_list_head_wire)) == 0 &&
            sys_set_tid_address(I386_CLEAR_TID) == TEST_PID;
}

static bool new_image_has_fresh_registrations(
        struct exec_fixture *fixture) {
    return fixture->task.robust_list == 0 &&
            fixture->task.clear_tid == 0 &&
            fixture->task.aarch64_robust_list == 0 &&
            task_has_aarch64_process(&fixture->task) &&
            aarch64_linux_process_take_clear_child_tid(
                    fixture->task.aarch64_process) == 0;
}

static void *run_i386_waiter(void *opaque) {
    struct i386_waiter *waiter = opaque;
    current = &waiter->task;
    waiter->result = (sdword_t) sys_futex(
            waiter->address, I386_FUTEX_WAIT,
            waiter->expected, I386_TIMEOUT, 0, 0);
    STRACE(" = 0x%x\n", (dword_t) waiter->result);
    current = NULL;
    return NULL;
}

static bool start_waiter(struct i386_waiter *waiter,
        struct task *owner, pid_t_ pid,
        addr_t address, dword_t expected) {
    *waiter = (struct i386_waiter) {
        .address = address,
        .expected = expected,
    };
    waiter->task.pid = waiter->task.tgid = pid;
    task_set_mm(&waiter->task, owner->mm);
    mm_retain(waiter->task.mm);
    waiter->sighand = sighand_new();
    if (waiter->sighand == NULL) {
        mm_release(waiter->task.mm);
        waiter->task.mm = NULL;
        waiter->task.mem = NULL;
        return false;
    }
    waiter->task.sighand = waiter->sighand;
    lock_init(&waiter->task.waiting_cond_lock);
    if (pthread_create(&waiter->thread,
            NULL, run_i386_waiter, waiter) == 0)
        return true;
    pthread_mutex_destroy(&waiter->task.waiting_cond_lock.m);
    sighand_release(waiter->sighand);
    mm_release(waiter->task.mm);
    waiter->task.mm = NULL;
    waiter->task.mem = NULL;
    return false;
}

static bool wait_until_queued(struct task *owner, addr_t address) {
    current = owner;
    for (unsigned attempt = 0; attempt < 100000; attempt++) {
        dword_t result = sys_futex(address,
                I386_FUTEX_REQUEUE, 0, 1, address, 0);
        STRACE(" = 0x%x\n", result);
        if ((sdword_t) result < 0)
            return false;
        if (result == 1)
            return true;
        sched_yield();
    }
    return false;
}

static bool join_waiter(struct i386_waiter *waiter) {
    bool joined = pthread_join(waiter->thread, NULL) == 0;
    pthread_mutex_destroy(&waiter->task.waiting_cond_lock.m);
    sighand_release(waiter->sighand);
    mm_release(waiter->task.mm);
    waiter->task.mm = NULL;
    waiter->task.mem = NULL;
    return joined;
}

static bool test_single_mm_user(void) {
    struct exec_fixture fixture;
    TEST_CHECK(init_fixture(&fixture), "建立单 mm 用户夹具");
    TEST_CHECK(configure_robust_and_clear_tid(&fixture),
            "登记相互重叠的 i386 robust offset 与 clear-child-tid");
    TEST_CHECK(create_observer(&fixture) &&
            atomic_load_explicit(&fixture.task.mm->refcount,
                    memory_order_relaxed) == 1,
            "独立观察 mm 共享 backing 但不冒充旧 mm 用户");
    TEST_CHECK(stage_candidate(&fixture.task),
            "建立最小 AArch64 exec 候选");

    task_commit_aarch64_exec(&fixture.task);

    dword_t robust;
    dword_t clear_tid;
    TEST_CHECK(read_u32(&fixture.observer,
                    I386_ROBUST_WORD, &robust) &&
            read_u32(&fixture.observer,
                    I386_CLEAR_TID, &clear_tid) &&
            robust == (I386_FUTEX_WAITERS |
                    I386_FUTEX_OWNER_DIED) &&
            clear_tid == TEST_PID &&
            new_image_has_fresh_registrations(&fixture),
            "单用户提交修复 robust、消费注册但不写 clear-child-tid");
    destroy_fixture(&fixture);
    return true;
}

static bool test_robust_before_clear_tid(void) {
    struct exec_fixture fixture;
    TEST_CHECK(init_fixture(&fixture), "建立多 mm 用户顺序夹具");
    TEST_CHECK(configure_robust_and_clear_tid(&fixture) &&
            create_observer(&fixture),
            "准备可观察的 robust 与 clear-child-tid 共享状态");

    struct i386_waiter robust_waiter;
    struct i386_waiter clear_tid_waiter;
    TEST_CHECK(start_waiter(&robust_waiter,
                    &fixture.task, TEST_PID + 2,
                    I386_ROBUST_WORD,
                    I386_FUTEX_WAITERS | TEST_PID) &&
            start_waiter(&clear_tid_waiter,
                    &fixture.task, TEST_PID + 3,
                    I386_CLEAR_TID, TEST_PID),
            "创建共享旧 mm 的 robust 与 clear-child-tid 等待者");
    TEST_CHECK(wait_until_queued(
                    &fixture.task, I386_ROBUST_WORD) &&
            wait_until_queued(&fixture.task, I386_CLEAR_TID),
            "确认两个等待者都已进入生产 futex 队列");
    TEST_CHECK(stage_candidate(&fixture.task),
            "建立顺序场景的 AArch64 候选");

    task_commit_aarch64_exec(&fixture.task);

    TEST_CHECK(join_waiter(&robust_waiter) &&
            join_waiter(&clear_tid_waiter) &&
            robust_waiter.result == 0 &&
            clear_tid_waiter.result == 0,
            "成功提交依次唤醒 robust 与 clear-child-tid 等待者");
    dword_t robust;
    dword_t clear_tid;
    TEST_CHECK(read_u32(&fixture.observer,
                    I386_ROBUST_WORD, &robust) &&
            read_u32(&fixture.observer,
                    I386_CLEAR_TID, &clear_tid) &&
            robust == (I386_FUTEX_WAITERS |
                    I386_FUTEX_OWNER_DIED) &&
            clear_tid == 0 &&
            new_image_has_fresh_registrations(&fixture),
            "ctid 清零前读取重叠 offset，随后发布零注册的新映像");
    destroy_fixture(&fixture);
    return true;
}

static bool test_clear_tid_write_fault_still_wakes(void) {
    struct exec_fixture fixture;
    TEST_CHECK(init_fixture(&fixture), "建立只读 clear-child-tid 夹具");
    const dword_t marker = UINT32_C(0x6a09e667);
    TEST_CHECK(write_u32(&fixture.task,
                    I386_READONLY_CLEAR_TID, marker) &&
            sys_set_tid_address(I386_READONLY_CLEAR_TID) == TEST_PID &&
            create_observer(&fixture),
            "登记带独立共享 backing 观察者的 clear-child-tid");

    struct i386_waiter waiter;
    TEST_CHECK(start_waiter(&waiter,
                    &fixture.task, TEST_PID + 4,
                    I386_READONLY_CLEAR_TID, marker) &&
            wait_until_queued(
                    &fixture.task, I386_READONLY_CLEAR_TID),
            "在降权前排入 clear-child-tid 等待者");
    write_wrlock(&fixture.task.mem->lock);
    int protection_error = pt_set_flags(fixture.task.mem,
            PAGE(I386_READONLY_CLEAR_TID), 1, P_READ | P_SHARED);
    write_wrunlock(&fixture.task.mem->lock);
    TEST_CHECK(protection_error == 0 &&
            stage_candidate(&fixture.task),
            "把旧映像 clear-child-tid 页降为只读并建立候选");

    task_commit_aarch64_exec(&fixture.task);

    TEST_CHECK(join_waiter(&waiter) && waiter.result == 0,
            "clear-child-tid 清零故障后仍执行生产 wake");
    dword_t observed;
    TEST_CHECK(read_u32(&fixture.observer,
                    I386_READONLY_CLEAR_TID, &observed) &&
            observed == marker &&
            new_image_has_fresh_registrations(&fixture),
            "只读 backing 未被修改且旧注册已消费");
    destroy_fixture(&fixture);
    return true;
}

static bool test_candidate_discard_has_no_side_effect(void) {
    struct exec_fixture fixture;
    TEST_CHECK(init_fixture(&fixture), "建立候选回滚夹具");
    TEST_CHECK(configure_robust_and_clear_tid(&fixture) &&
            stage_candidate(&fixture.task),
            "登记旧 i386 状态并建立待回滚候选");
    TEST_CHECK(aarch64_linux_process_take_clear_child_tid(
                    fixture.task.exec_transition.process) == 0,
            "新 AArch64 候选从空 clear-child-tid 注册开始");

    task_discard_aarch64_exec(&fixture.task);

    dword_t robust;
    dword_t clear_tid;
    TEST_CHECK(!task_has_aarch64_process(&fixture.task) &&
            !task_has_aarch64_exec_candidate(&fixture.task) &&
            fixture.task.robust_list == I386_ROBUST_HEAD &&
            fixture.task.clear_tid == I386_CLEAR_TID &&
            read_u32(&fixture.task, I386_ROBUST_WORD, &robust) &&
            read_u32(&fixture.task, I386_CLEAR_TID, &clear_tid) &&
            robust == (I386_FUTEX_WAITERS | TEST_PID) &&
            clear_tid == TEST_PID,
            "回滚不消费旧注册、不修复 robust 且不清零 ctid");
    fixture.task.robust_list = 0;
    fixture.task.clear_tid = 0;
    destroy_fixture(&fixture);
    return true;
}

typedef bool (*scenario_function)(void);

static bool run_isolated(
        const char *name, scenario_function scenario) {
    pid_t child = fork();
    if (child < 0) {
        fprintf(stderr, "创建 %s 隔离进程失败\n", name);
        return false;
    }
    if (child == 0) {
        alarm(8);
        bool passed = scenario();
        if (passed && futex_test_live_count() != 0) {
            fprintf(stderr, "%s 遗留 futex 对象\n", name);
            passed = false;
        }
        exit(passed ? EXIT_SUCCESS : EXIT_FAILURE);
    }

    int status = 0;
    pid_t waited = waitpid(child, &status, 0);
    if (waited != child ||
            !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "%s 场景失败", name);
        if (waited == child && WIFSIGNALED(status))
            fprintf(stderr, "（信号 %d）", WTERMSIG(status));
        fputc('\n', stderr);
        return false;
    }
    return true;
}

int main(void) {
    static const struct {
        const char *name;
        scenario_function run;
    } scenarios[] = {
        {"单旧 mm 用户", test_single_mm_user},
        {"robust 先于 clear-child-tid", test_robust_before_clear_tid},
        {"clear-child-tid 写故障仍唤醒",
                test_clear_tid_write_fault_still_wakes},
        {"候选回滚无副作用",
                test_candidate_discard_has_no_side_effect},
    };
    for (size_t index = 0; index < array_size(scenarios); index++) {
        if (!run_isolated(scenarios[index].name,
                scenarios[index].run))
            return 1;
    }
    return 0;
}
