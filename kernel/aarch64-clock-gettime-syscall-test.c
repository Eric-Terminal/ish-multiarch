#include <stdio.h>
#include <string.h>
#include <time.h>

#include "guest/aarch64/linux-signal-abi.h"
#include "guest/aarch64/linux-time-abi.h"
#include "guest/memory/address-space.h"
#include "kernel/aarch64-syscall-service.h"
#include "kernel/errno.h"
#include "kernel/task.h"
#include "kernel/time.h"

#define USER_BASE UINT64_C(0x00007abc12340000)
#define USER_MEMORY_SIZE 64
#define TIME_ADDRESS (USER_BASE + 3)

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, \
                "AArch64 clock_gettime 测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        return 1; \
    } \
} while (0)

struct user_probe {
    byte_t bytes[USER_MEMORY_SIZE];
    qword_t fail_write_at;
    unsigned reads;
    unsigned writes;
    qword_t last_write_address;
    dword_t last_write_size;
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
    probe->fail_write_at = UINT64_MAX;
}

static qword_t invoke(struct task *task, struct user_probe *probe,
        struct guest_linux_user_fault *fault,
        qword_t clock, qword_t address) {
    const struct guest_linux_syscall_context context = {
        .task_opaque = task,
        .user = {
            .opaque = probe,
            .read = read_user,
            .write = write_user,
        },
    };
    const struct guest_linux_syscall syscall = {
        .number = 113,
        .arguments = {
            clock,
            address,
            UINT64_C(0x1111222233334444),
            UINT64_C(0x5555666677778888),
        },
    };
    return ish_aarch64_linux_syscall_service.dispatch(
            &context, &syscall, fault);
}

static struct aarch64_linux_timespec load_time(
        const struct user_probe *probe) {
    struct aarch64_linux_timespec time;
    memcpy(&time, probe->bytes + (TIME_ADDRESS - USER_BASE), sizeof(time));
    return time;
}

static bool not_before(struct aarch64_linux_timespec wire,
        struct timespec host) {
    return wire.sec > host.tv_sec ||
            (wire.sec == host.tv_sec && wire.nsec >= host.tv_nsec);
}

static bool not_after(struct aarch64_linux_timespec wire,
        struct timespec host) {
    return wire.sec < host.tv_sec ||
            (wire.sec == host.tv_sec && wire.nsec <= host.tv_nsec);
}

static int test_wall_clocks(struct task *task) {
    static const struct {
        dword_t guest;
        clockid_t host;
    } clocks[] = {
        {CLOCK_REALTIME_, CLOCK_REALTIME},
        {CLOCK_MONOTONIC_, CLOCK_MONOTONIC},
        {CLOCK_MONOTONIC_RAW_, CLOCK_MONOTONIC_RAW},
        {CLOCK_REALTIME_COARSE_, CLOCK_REALTIME},
        {CLOCK_MONOTONIC_COARSE_, CLOCK_MONOTONIC},
        {CLOCK_REALTIME_ALARM_, CLOCK_REALTIME},
    };

    for (size_t index = 0; index < array_size(clocks); index++) {
        struct user_probe probe;
        struct guest_linux_user_fault fault;
        struct timespec before;
        struct timespec after;
        reset_probe(&probe);
        clock_gettime(clocks[index].host, &before);
        qword_t result = invoke(task, &probe, &fault,
                UINT64_C(0xabcdef0100000000) | clocks[index].guest,
                TIME_ADDRESS);
        clock_gettime(clocks[index].host, &after);
        struct aarch64_linux_timespec wire = load_time(&probe);
        CHECK(result == 0 && probe.reads == 0 && probe.writes == 1 &&
                probe.last_write_address == TIME_ADDRESS &&
                probe.last_write_size == sizeof(wire) &&
                wire.sec >= 0 && wire.nsec >= 0 &&
                wire.nsec < INT64_C(1000000000) &&
                not_before(wire, before) && not_after(wire, after) &&
                fault.kind == GUEST_MEMORY_FAULT_NONE,
                "常用 clock 应忽略参数高位并一次写回规范化时间");
    }
    return 0;
}

static int test_boottime_clocks(struct task *task) {
    struct user_probe probe;
    struct guest_linux_user_fault fault;
    reset_probe(&probe);
    qword_t result = invoke(
            task, &probe, &fault, CLOCK_BOOTTIME_, TIME_ADDRESS);
    struct aarch64_linux_timespec first = load_time(&probe);
    CHECK(result == 0 && probe.reads == 0 && probe.writes == 1 &&
            first.sec >= 0 && first.nsec >= 0 &&
            first.nsec < INT64_C(1000000000),
            "boottime 应返回包含休眠时段的规范化启动时间");

    reset_probe(&probe);
    result = invoke(
            task, &probe, &fault, CLOCK_BOOTTIME_ALARM_, TIME_ADDRESS);
    struct aarch64_linux_timespec second = load_time(&probe);
    CHECK(result == 0 && probe.reads == 0 && probe.writes == 1 &&
            (second.sec > first.sec ||
                    (second.sec == first.sec && second.nsec >= first.nsec)),
            "boottime alarm 应复用同一连续且不倒退的时间域");
    return 0;
}

static int test_cpu_clocks(struct task *task) {
    static const dword_t clocks[] = {
        CLOCK_PROCESS_CPUTIME_ID_,
        CLOCK_THREAD_CPUTIME_ID_,
    };
    for (size_t index = 0; index < array_size(clocks); index++) {
        struct user_probe probe;
        struct guest_linux_user_fault fault;
        reset_probe(&probe);
        qword_t result = invoke(
                task, &probe, &fault, clocks[index], TIME_ADDRESS);
        struct aarch64_linux_timespec wire = load_time(&probe);
        CHECK(result == 0 && probe.reads == 0 && probe.writes == 1 &&
                wire.sec >= 0 && wire.nsec >= 0 &&
                wire.nsec < INT64_C(1000000000),
                "进程与线程 CPU clock 应返回规范化累计时间");
    }
    return 0;
}

static int test_invalid_clock_precedes_pointer(struct task *task) {
    struct user_probe probe;
    struct guest_linux_user_fault fault;
    reset_probe(&probe);
    qword_t result = invoke(task, &probe, &fault,
            UINT64_C(0xabcdef010000007f), UINT64_MAX);
    CHECK(result == (qword_t) (sqword_t) _EINVAL &&
            probe.reads == 0 && probe.writes == 0 &&
            fault.address == 0 && fault.access == 0 && fault.kind == 0,
            "未知 clock 应在检查输出指针前返回 EINVAL");

    reset_probe(&probe);
    result = invoke(task, &probe, &fault, UINT64_MAX, UINT64_MAX);
    CHECK(result == (qword_t) (sqword_t) _EINVAL &&
            probe.reads == 0 && probe.writes == 0,
            "负 clockid 按低 32 位有符号值拒绝");
    return 0;
}

static int test_output_failures(struct task *task) {
    struct user_probe probe;
    struct guest_linux_user_fault fault;
    reset_probe(&probe);

    qword_t wrapping = UINT64_MAX - 7;
    qword_t result = invoke(
            task, &probe, &fault, CLOCK_REALTIME_, wrapping);
    CHECK(result == (qword_t) (sqword_t) _EFAULT &&
            probe.reads == 0 && probe.writes == 0 &&
            fault.address == wrapping &&
            fault.access == GUEST_MEMORY_WRITE &&
            fault.kind == GUEST_MEMORY_FAULT_ADDRESS_SIZE,
            "写回地址回绕时不进入用户内存回调");

    reset_probe(&probe);
    qword_t crossing_limit = AARCH64_LINUX_USER_ADDRESS_MAX - 7;
    result = invoke(task, &probe, &fault,
            CLOCK_MONOTONIC_, crossing_limit);
    CHECK(result == (qword_t) (sqword_t) _EFAULT &&
            probe.writes == 0 && fault.address == crossing_limit &&
            fault.access == GUEST_MEMORY_WRITE &&
            fault.kind == GUEST_MEMORY_FAULT_ADDRESS_SIZE,
            "写回跨越用户地址上限时提前失败");

    reset_probe(&probe);
    result = invoke(task, &probe, &fault, CLOCK_REALTIME_, 0);
    CHECK(result == (qword_t) (sqword_t) _EFAULT &&
            probe.writes == 1 && fault.address == 0 &&
            fault.access == GUEST_MEMORY_WRITE &&
            fault.kind == GUEST_MEMORY_FAULT_UNMAPPED,
            "空输出地址由用户内存回调报告精确映射故障");

    reset_probe(&probe);
    memset(probe.bytes, 0xa5, sizeof(probe.bytes));
    probe.fail_write_at = TIME_ADDRESS + 8;
    result = invoke(task, &probe, &fault,
            CLOCK_REALTIME_, TIME_ADDRESS);
    struct aarch64_linux_timespec partial = load_time(&probe);
    CHECK(result == (qword_t) (sqword_t) _EFAULT &&
            probe.writes == 1 && probe.last_write_size == 16 &&
            fault.address == TIME_ADDRESS + 8 &&
            fault.access == GUEST_MEMORY_WRITE &&
            fault.kind == GUEST_MEMORY_FAULT_UNMAPPED &&
            partial.sec > 0,
            "单次 16 字节写回传播纳秒字段处的部分故障");
    size_t nsec_offset = (size_t) (TIME_ADDRESS - USER_BASE) + 8;
    for (size_t index = 0; index < 8; index++) {
        CHECK(probe.bytes[nsec_offset + index] == 0xa5,
                "部分写故障不得改动故障位置之后的纳秒字段");
    }
    return 0;
}

int main(void) {
    struct task task = {0};
    current = &task;
    int result = test_wall_clocks(&task);
    if (result == 0)
        result = test_boottime_clocks(&task);
    if (result == 0)
        result = test_cpu_clocks(&task);
    if (result == 0)
        result = test_invalid_clock_precedes_pointer(&task);
    if (result == 0)
        result = test_output_failures(&task);
    current = NULL;
    return result;
}
