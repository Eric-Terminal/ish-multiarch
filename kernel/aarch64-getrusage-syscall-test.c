#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "fs/fd.h"
#include "guest/aarch64/linux-resource-abi.h"
#include "guest/aarch64/linux-signal-abi.h"
#include "guest/memory/address-space.h"
#include "kernel/aarch64-syscall-service.h"
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/memory.h"
#include "kernel/mm.h"
#include "kernel/resource.h"
#include "kernel/task.h"

#define AARCH64_GETRUSAGE_SYSCALL UINT64_C(165)
#define HIGH_ARGUMENT UINT64_C(0xa5a5a5a500000000)
#define USER_BASE UINT64_C(0x00007abc12340000)
#define USER_MEMORY_SIZE 512
#define RUSAGE_ADDRESS (USER_BASE + 3)
#define I386_PAGE UINT32_C(0x10000000)
#define I386_OUTPUT (I386_PAGE + UINT32_C(0x100))
#define I386_UNMAPPED (I386_PAGE + UINT32_C(0x1100))

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, \
                "AArch64 getrusage 系统调用测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        return false; \
    } \
} while (0)

_Static_assert(sizeof(struct aarch64_linux_rusage) == 144 &&
        _Alignof(struct aarch64_linux_rusage) == 8,
        "测试必须使用 AArch64 Linux 的 144 字节 LP64 rusage wire");
_Static_assert(sizeof(struct rusage_) == 72,
        "i386 rusage wire 必须保持 72 字节");

struct syscall_fixture {
    struct task task;
    struct tgroup group;
};

struct user_probe {
    byte_t bytes[USER_MEMORY_SIZE];
    qword_t base;
    qword_t fail_write_at;
    unsigned reads;
    unsigned writes;
    qword_t last_write_address;
    dword_t last_write_size;
};

static qword_t encoded_error(int error) {
    return (qword_t) (sqword_t) error;
}

static bool range_contains(
        qword_t address, dword_t size, qword_t target) {
    return target >= address && target - address < size;
}

static bool probe_range(const struct user_probe *probe,
        qword_t address, dword_t size, size_t *offset) {
    if (address < probe->base)
        return false;
    qword_t relative = address - probe->base;
    if (relative > USER_MEMORY_SIZE ||
            size > USER_MEMORY_SIZE - relative)
        return false;
    *offset = (size_t) relative;
    return true;
}

static bool read_user(void *opaque, qword_t address,
        void *destination, dword_t size,
        struct guest_linux_user_fault *fault) {
    (void) address;
    (void) destination;
    (void) size;
    (void) fault;
    struct user_probe *probe = opaque;
    probe->reads++;
    return false;
}

static bool write_user(void *opaque, qword_t address,
        const void *source, dword_t size,
        struct guest_linux_user_fault *fault) {
    struct user_probe *probe = opaque;
    probe->writes++;
    probe->last_write_address = address;
    probe->last_write_size = size;

    size_t offset;
    if (!probe_range(probe, address, size, &offset)) {
        *fault = (struct guest_linux_user_fault) {
            .address = address,
            .access = GUEST_MEMORY_WRITE,
            .kind = GUEST_MEMORY_FAULT_UNMAPPED,
        };
        return false;
    }
    if (probe->fail_write_at != UINT64_MAX &&
            range_contains(address, size, probe->fail_write_at)) {
        dword_t prefix =
                (dword_t) (probe->fail_write_at - address);
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
    probe->base = USER_BASE;
    probe->fail_write_at = UINT64_MAX;
}

static struct aarch64_linux_rusage load_wire(
        const struct user_probe *probe) {
    struct aarch64_linux_rusage wire;
    memcpy(&wire, probe->bytes + (RUSAGE_ADDRESS - USER_BASE),
            sizeof(wire));
    return wire;
}

static qword_t invoke_getrusage(
        struct syscall_fixture *fixture, struct user_probe *probe,
        struct guest_linux_user_fault *fault,
        qword_t who, qword_t address) {
    const struct guest_linux_syscall_context context = {
        .task_opaque = &fixture->task,
        .user = {
            .opaque = probe,
            .read = read_user,
            .write = write_user,
        },
    };
    const struct guest_linux_syscall syscall = {
        .number = AARCH64_GETRUSAGE_SYSCALL,
        .arguments = {
            who,
            address,
            UINT64_C(0x1111222233334444),
            UINT64_C(0x5555666677778888),
        },
    };
    return ish_aarch64_linux_syscall_service.dispatch(
            &context, &syscall, fault);
}

static void init_fixture(struct syscall_fixture *fixture) {
    memset(fixture, 0, sizeof(*fixture));
    list_init(&fixture->group.threads);
    list_init(&fixture->group.session);
    list_init(&fixture->group.pgroup);
    lock_init(&fixture->group.lock);
    fixture->group.leader = &fixture->task;
    fixture->task.group = &fixture->group;
    current = &fixture->task;
}

static void destroy_fixture(struct syscall_fixture *fixture) {
    current = NULL;
    lock_destroy(&fixture->group.lock);
}

static bool test_children_wire_and_low_word(void) {
    struct syscall_fixture fixture;
    init_fixture(&fixture);
    fixture.group.children_rusage = (struct rusage_) {
        .utime = {UINT32_C(0x7fffffff), UINT32_C(123456)},
        .stime = {3, UINT32_C(654321)},
        .maxrss = UINT32_C(0x80000001),
        .ixrss = 2,
        .idrss = 3,
        .isrss = 4,
        .minflt = 5,
        .majflt = 6,
        .nswap = 7,
        .inblock = 8,
        .oublock = 9,
        .msgsnd = 10,
        .msgrcv = 11,
        .nsignals = 12,
        .nvcsw = 13,
        .nivcsw = UINT32_C(0xfffffffe),
    };

    struct user_probe probe;
    struct guest_linux_user_fault fault;
    reset_probe(&probe);
    memset(probe.bytes, 0xa5, sizeof(probe.bytes));
    qword_t result = invoke_getrusage(&fixture, &probe, &fault,
            HIGH_ARGUMENT | UINT32_MAX, RUSAGE_ADDRESS);
    struct aarch64_linux_rusage wire = load_wire(&probe);
    CHECK(result == 0 && probe.reads == 0 && probe.writes == 1 &&
            probe.last_write_address == RUSAGE_ADDRESS &&
            probe.last_write_size == sizeof(wire) &&
            fault.kind == GUEST_MEMORY_FAULT_NONE,
            "CHILDREN 忽略 who 高位并一次写回未对齐 LP64 wire");
    CHECK(wire.utime.sec == INT32_MAX &&
            wire.utime.usec == 123456 &&
            wire.stime.sec == 3 && wire.stime.usec == 654321 &&
            wire.maxrss == (sqword_t) INT32_MIN + 1 &&
            wire.ixrss == 2 && wire.idrss == 3 &&
            wire.isrss == 4 && wire.minflt == 5 &&
            wire.majflt == 6 && wire.nswap == 7 &&
            wire.inblock == 8 && wire.oublock == 9 &&
            wire.msgsnd == 10 && wire.msgrcv == 11 &&
            wire.nsignals == 12 && wire.nvcsw == 13 &&
            wire.nivcsw == -2,
            "CHILDREN 将内部 i386 字段逐项符号扩展为 AArch64 long");
    CHECK(probe.bytes[0] == 0xa5 &&
            probe.bytes[1] == 0xa5 && probe.bytes[2] == 0xa5 &&
            probe.bytes[3 + sizeof(wire)] == 0xa5,
            "144 字节写回不触碰前后 canary");
    destroy_fixture(&fixture);
    return true;
}

static int64_t timeval_microseconds(
        struct aarch64_linux_timeval value) {
    return value.sec * INT64_C(1000000) + value.usec;
}

static bool test_self_and_thread_accounting(void) {
    struct syscall_fixture fixture;
    init_fixture(&fixture);
    fixture.group.rusage = (struct rusage_) {
        .utime = {7, 800000},
        .stime = {11, 900000},
    };

    struct user_probe probe;
    struct guest_linux_user_fault fault;
    reset_probe(&probe);
    CHECK(invoke_getrusage(&fixture, &probe, &fault,
                    HIGH_ARGUMENT | RUSAGE_THREAD_,
                    RUSAGE_ADDRESS) == 0,
            "THREAD 返回当前执行线程资源时间");
    struct aarch64_linux_rusage thread = load_wire(&probe);
    CHECK(thread.utime.sec >= 0 && thread.utime.usec >= 0 &&
            thread.utime.usec < 1000000 &&
            thread.stime.sec >= 0 && thread.stime.usec >= 0 &&
            thread.stime.usec < 1000000,
            "THREAD 时间字段保持规范 timeval");

    reset_probe(&probe);
    CHECK(invoke_getrusage(&fixture, &probe, &fault,
                    HIGH_ARGUMENT | RUSAGE_SELF_,
                    RUSAGE_ADDRESS) == 0,
            "SELF 返回线程组累计资源时间");
    struct aarch64_linux_rusage self = load_wire(&probe);
    CHECK(timeval_microseconds(self.utime) >=
                    timeval_microseconds(thread.utime) +
                    INT64_C(7800000) &&
            timeval_microseconds(self.stime) >=
                    timeval_microseconds(thread.stime) +
                    INT64_C(11900000),
            "SELF 在当前线程时间上累计已经退出的同组线程");
    reset_probe(&probe);
    CHECK(invoke_getrusage(&fixture, &probe, &fault,
                    HIGH_ARGUMENT | RUSAGE_THREAD_,
                    RUSAGE_ADDRESS) == 0,
            "第二次 THREAD 采样夹住 SELF 的 host 采样时刻");
    struct aarch64_linux_rusage thread_after = load_wire(&probe);
    int64_t self_utime =
            timeval_microseconds(self.utime) - INT64_C(7800000);
    int64_t self_stime =
            timeval_microseconds(self.stime) - INT64_C(11900000);
    CHECK(self_utime >= timeval_microseconds(thread.utime) &&
            self_utime <=
                    timeval_microseconds(thread_after.utime) &&
            self_stime >= timeval_microseconds(thread.stime) &&
            self_stime <=
                    timeval_microseconds(thread_after.stime),
            "SELF 精确加入已退出线程时间且不污染 THREAD");
    destroy_fixture(&fixture);
    return true;
}

static bool test_error_order_and_output_faults(void) {
    struct syscall_fixture fixture;
    init_fixture(&fixture);
    fixture.group.children_rusage.utime =
            (struct timeval_) {17, 123456};

    struct user_probe probe;
    struct guest_linux_user_fault fault;
    reset_probe(&probe);
    CHECK(invoke_getrusage(&fixture, &probe, &fault,
                    HIGH_ARGUMENT | UINT32_C(2), UINT64_MAX) ==
                    encoded_error(_EINVAL) &&
            probe.reads == 0 && probe.writes == 0 &&
            fault.kind == GUEST_MEMORY_FAULT_NONE,
            "非法 who 的 EINVAL 优先于坏输出地址");

    reset_probe(&probe);
    CHECK(invoke_getrusage(&fixture, &probe, &fault,
                    HIGH_ARGUMENT | (UINT32_MAX - UINT32_C(1)),
                    RUSAGE_ADDRESS) == encoded_error(_EINVAL) &&
            probe.writes == 0,
            "低 32 位按有符号值拒绝 RUSAGE_BOTH");

    qword_t wrapping = UINT64_MAX - UINT64_C(100);
    reset_probe(&probe);
    CHECK(invoke_getrusage(&fixture, &probe, &fault,
                    UINT32_MAX, wrapping) == encoded_error(_EFAULT) &&
            probe.writes == 0 && fault.address == wrapping &&
            fault.access == GUEST_MEMORY_WRITE &&
            fault.kind == GUEST_MEMORY_FAULT_ADDRESS_SIZE,
            "输出地址回绕在用户回调前报告结构性 EFAULT");

    qword_t last_valid = AARCH64_LINUX_USER_ADDRESS_MAX -
            sizeof(struct aarch64_linux_rusage) + UINT64_C(1);
    reset_probe(&probe);
    probe.base = last_valid;
    CHECK(invoke_getrusage(&fixture, &probe, &fault,
                    UINT32_MAX, last_valid) == 0 &&
            probe.writes == 1 &&
            probe.last_write_address == last_valid &&
            probe.last_write_size ==
                    sizeof(struct aarch64_linux_rusage) &&
            fault.kind == GUEST_MEMORY_FAULT_NONE,
            "最后一个合法 144 字节区间允许完整写回");

    qword_t crossing = last_valid + UINT64_C(1);
    reset_probe(&probe);
    CHECK(invoke_getrusage(&fixture, &probe, &fault,
                    UINT32_MAX, crossing) == encoded_error(_EFAULT) &&
            probe.writes == 0 && fault.address == crossing &&
            fault.access == GUEST_MEMORY_WRITE &&
            fault.kind == GUEST_MEMORY_FAULT_ADDRESS_SIZE,
            "144 字节输出跨越 48 位上限时不调用用户写回");

    reset_probe(&probe);
    CHECK(invoke_getrusage(&fixture, &probe, &fault,
                    UINT32_MAX, 0) == encoded_error(_EFAULT) &&
            probe.writes == 1 && fault.address == 0 &&
            fault.access == GUEST_MEMORY_WRITE &&
            fault.kind == GUEST_MEMORY_FAULT_UNMAPPED,
            "空输出地址由用户内存回调报告映射故障");

    reset_probe(&probe);
    memset(probe.bytes, 0xa5, sizeof(probe.bytes));
    probe.fail_write_at = RUSAGE_ADDRESS + 72;
    CHECK(invoke_getrusage(&fixture, &probe, &fault,
                    UINT32_MAX, RUSAGE_ADDRESS) ==
                    encoded_error(_EFAULT) &&
            probe.writes == 1 &&
            probe.last_write_size ==
                    sizeof(struct aarch64_linux_rusage) &&
            fault.address == RUSAGE_ADDRESS + 72 &&
            fault.access == GUEST_MEMORY_WRITE &&
            fault.kind == GUEST_MEMORY_FAULT_UNMAPPED,
            "单次 144 字节写回传播中途部分故障");
    size_t offset =
            (size_t) (RUSAGE_ADDRESS - USER_BASE) + 72;
    for (size_t index = offset; index < offset + 72; index++) {
        CHECK(probe.bytes[index] == 0xa5,
                "部分写故障不修改故障位置之后的字节");
    }
    destroy_fixture(&fixture);
    return true;
}

static bool map_i386_page(struct task *task) {
    struct mm *mm = mm_new();
    if (mm == NULL)
        return false;
    write_wrlock(&mm->mem.lock);
    int error = pt_map_nothing(
            &mm->mem, PAGE(I386_PAGE), 1, P_RWX);
    write_wrunlock(&mm->mem.lock);
    if (error < 0) {
        mm_release(mm);
        return false;
    }
    task_set_mm(task, mm);
    return true;
}

static bool test_i386_shared_core(void) {
    struct syscall_fixture fixture;
    init_fixture(&fixture);
    CHECK(map_i386_page(&fixture.task),
            "为 i386 getrusage 建立真实页表");
    fixture.group.children_rusage = (struct rusage_) {
        .utime = {23, 345678},
        .stime = {29, 456789},
        .maxrss = 31,
        .nivcsw = 37,
    };
    qword_t canary = UINT64_C(0x1122334455667788);
    CHECK(user_write_task(&fixture.task,
                    I386_OUTPUT + sizeof(struct rusage_),
                    &canary, sizeof(canary)) == 0,
            "在 i386 rusage wire 之后写入尾 canary");

    CHECK(sys_getrusage(RUSAGE_CHILDREN_, I386_OUTPUT) == 0,
            "i386 CHILDREN 复用共享资源快照核心");
    struct rusage_ observed;
    CHECK(user_read_task(&fixture.task, I386_OUTPUT,
                    &observed, sizeof(observed)) == 0 &&
            memcmp(&observed, &fixture.group.children_rusage,
                    sizeof(observed)) == 0,
            "i386 仍写回独立的 72 字节兼容 wire");

    CHECK(sys_getrusage(RUSAGE_THREAD_, I386_OUTPUT) == 0 &&
            user_read_task(&fixture.task, I386_OUTPUT,
                    &observed, sizeof(observed)) == 0 &&
            observed.utime.usec < 1000000 &&
            observed.stime.usec < 1000000,
            "i386 THREAD 通过共享核心返回规范时间");
    qword_t observed_canary = 0;
    CHECK(user_read_task(&fixture.task,
                    I386_OUTPUT + sizeof(struct rusage_),
                    &observed_canary, sizeof(observed_canary)) == 0 &&
            observed_canary == canary,
            "i386 两条路径都只写 72 字节且不触碰尾 canary");
    CHECK(sys_getrusage(2, I386_UNMAPPED) == (dword_t) _EINVAL,
            "i386 同样让非法 who 优先于坏输出地址");

    mm_release(fixture.task.mm);
    fixture.task.mm = NULL;
    destroy_fixture(&fixture);
    return true;
}

int main(void) {
    if (!test_children_wire_and_low_word() ||
            !test_self_and_thread_accounting() ||
            !test_error_order_and_output_faults() ||
            !test_i386_shared_core())
        return 1;
    puts("AArch64 getrusage 与共享资源快照测试通过");
    return 0;
}
