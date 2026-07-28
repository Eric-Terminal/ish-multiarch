#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "fs/fd.h"
#include "fs/poll.h"
#include "guest/aarch64/linux-signal-abi.h"
#include "guest/aarch64/linux-time-abi.h"
#include "guest/linux/syscall-service.h"
#include "guest/memory/address-space.h"
#include "kernel/aarch64-syscall-service.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "kernel/resource.h"
#include "kernel/task.h"
#include "kernel/time.h"
#include "kernel/timerfd.h"

#define TIMERFD_CREATE_SYSCALL UINT64_C(85)
#define TIMERFD_SETTIME_SYSCALL UINT64_C(86)
#define TIMERFD_GETTIME_SYSCALL UINT64_C(87)
#define HIGH_ARGUMENT UINT64_C(0xa5a5a5a500000000)
#define USER_BASE UINT64_C(0x00007abc12340000)
#define USER_MEMORY_SIZE 512
#define NEW_ADDRESS (USER_BASE + 3)
#define OLD_ADDRESS (USER_BASE + 131)
#define QUERY_ADDRESS (USER_BASE + 259)
#define AARCH64_TFD_NONBLOCK UINT32_C(0x00000800)
#define AARCH64_TFD_CLOEXEC UINT32_C(0x00080000)
#define AARCH64_TFD_TIMER_ABSTIME UINT32_C(0x1)
#define LINUX_KTIME_MAX_SEC INT64_C(9223372036)
#define LINUX_KTIME_MAX_NSEC INT64_C(854775807)
#define IO_LOG_CAPACITY 8

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, \
                "AArch64 timerfd 系统调用测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        return false; \
    } \
} while (0)

_Static_assert(O_NONBLOCK_ == AARCH64_TFD_NONBLOCK &&
        O_CLOEXEC_ == AARCH64_TFD_CLOEXEC &&
        sizeof(struct aarch64_linux_itimerspec) == 32 &&
        _Alignof(struct aarch64_linux_itimerspec) == 8,
        "timerfd flag 与 wire 布局必须匹配 AArch64 Linux ABI");

struct timerfd_fixture {
    struct task task;
    struct tgroup group;
};

struct user_probe {
    byte_t bytes[USER_MEMORY_SIZE];
    qword_t fail_read_at;
    qword_t fail_write_at;
    bool allow_external;
    unsigned reads;
    unsigned writes;
    qword_t read_addresses[IO_LOG_CAPACITY];
    qword_t write_addresses[IO_LOG_CAPACITY];
    dword_t read_sizes[IO_LOG_CAPACITY];
    dword_t write_sizes[IO_LOG_CAPACITY];
};

struct read_worker {
    struct fd *fd;
    atomic_bool started;
    ssize_t result;
    uint64_t value;
};

static struct fd_ops ordinary_ops;

static qword_t encoded_error(int error) {
    return (qword_t) (sqword_t) error;
}

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
    unsigned operation = probe->reads++;
    if (operation < IO_LOG_CAPACITY) {
        probe->read_addresses[operation] = address;
        probe->read_sizes[operation] = size;
    }

    size_t offset;
    if (!probe_range(address, size, &offset)) {
        if (probe->allow_external) {
            memset(destination, 0, size);
            return true;
        }
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
    unsigned operation = probe->writes++;
    if (operation < IO_LOG_CAPACITY) {
        probe->write_addresses[operation] = address;
        probe->write_sizes[operation] = size;
    }

    size_t offset;
    if (!probe_range(address, size, &offset)) {
        if (probe->allow_external)
            return true;
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
    probe->allow_external = false;
}

static void reset_activity(struct user_probe *probe) {
    probe->reads = 0;
    probe->writes = 0;
    memset(probe->read_addresses, 0, sizeof(probe->read_addresses));
    memset(probe->write_addresses, 0, sizeof(probe->write_addresses));
    memset(probe->read_sizes, 0, sizeof(probe->read_sizes));
    memset(probe->write_sizes, 0, sizeof(probe->write_sizes));
    probe->fail_read_at = UINT64_MAX;
    probe->fail_write_at = UINT64_MAX;
    probe->allow_external = false;
}

static size_t probe_offset(qword_t address) {
    return (size_t) (address - USER_BASE);
}

static void store_wire(struct user_probe *probe, qword_t address,
        struct aarch64_linux_itimerspec wire) {
    memcpy(probe->bytes + probe_offset(address), &wire, sizeof(wire));
}

static struct aarch64_linux_itimerspec load_wire(
        const struct user_probe *probe, qword_t address) {
    struct aarch64_linux_itimerspec wire;
    memcpy(&wire, probe->bytes + probe_offset(address), sizeof(wire));
    return wire;
}

static bool init_fixture(
        struct timerfd_fixture *fixture, rlim_t_ descriptor_limit) {
    memset(fixture, 0, sizeof(*fixture));
    lock_init(&fixture->group.lock);
    fixture->group.limits[RLIMIT_NOFILE_] =
            (struct rlimit_) {descriptor_limit, descriptor_limit};
    fixture->task.group = &fixture->group;
    fixture->task.files = fdtable_new(1);
    if (IS_ERR(fixture->task.files))
        return false;
    current = &fixture->task;
    return true;
}

static void destroy_fixture(struct timerfd_fixture *fixture) {
    fdtable_release(fixture->task.files);
    if (current == &fixture->task)
        current = NULL;
}

static qword_t invoke(struct timerfd_fixture *fixture,
        struct user_probe *probe, struct guest_linux_user_fault *fault,
        qword_t number, qword_t argument0, qword_t argument1,
        qword_t argument2, qword_t argument3) {
    const struct guest_linux_syscall_context context = {
        .task_opaque = &fixture->task,
        .user = {
            .opaque = probe,
            .read = read_user,
            .write = write_user,
        },
    };
    const struct guest_linux_syscall syscall = {
        .number = number,
        .arguments = {
            argument0, argument1, argument2, argument3,
            UINT64_C(0x1122334455667788),
        },
    };
    current = &fixture->task;
    return ish_aarch64_linux_syscall_service.dispatch(
            &context, &syscall, fault);
}

static qword_t invoke_create(struct timerfd_fixture *fixture,
        struct user_probe *probe, struct guest_linux_user_fault *fault,
        qword_t clock, qword_t flags) {
    return invoke(fixture, probe, fault, TIMERFD_CREATE_SYSCALL,
            clock, flags, 0, 0);
}

static qword_t invoke_settime(struct timerfd_fixture *fixture,
        struct user_probe *probe, struct guest_linux_user_fault *fault,
        qword_t fd, qword_t flags, qword_t input, qword_t output) {
    return invoke(fixture, probe, fault, TIMERFD_SETTIME_SYSCALL,
            fd, flags, input, output);
}

static qword_t invoke_gettime(struct timerfd_fixture *fixture,
        struct user_probe *probe, struct guest_linux_user_fault *fault,
        qword_t fd, qword_t output) {
    return invoke(fixture, probe, fault, TIMERFD_GETTIME_SYSCALL,
            fd, output, 0, 0);
}

static bool two_reads_at(
        const struct user_probe *probe, qword_t address) {
    return probe->reads == 2 &&
            probe->read_addresses[0] == address &&
            probe->read_addresses[1] == address + 16 &&
            probe->read_sizes[0] == 16 &&
            probe->read_sizes[1] == 16;
}

static bool two_writes_at(
        const struct user_probe *probe, qword_t address) {
    return probe->writes == 2 &&
            probe->write_addresses[0] == address &&
            probe->write_addresses[1] == address + 16 &&
            probe->write_sizes[0] == 16 &&
            probe->write_sizes[1] == 16;
}

static bool test_creation_and_scalar_abi(void) {
    struct timerfd_fixture caller;
    struct timerfd_fixture target;
    CHECK(init_fixture(&caller, 4), "初始化调用任务");
    CHECK(init_fixture(&target, 4), "初始化目标任务");
    current = &caller.task;
    fd_t number = timerfd_create_task(
            &target.task, CLOCK_MONOTONIC_, O_NONBLOCK_);
    struct fd *created = f_get_task(&target.task, number);
    CHECK(number == 0 && created != NULL &&
            f_get_task(&caller.task, 0) == NULL,
            "task-aware 创建只向显式目标描述符表安装");
    CHECK(f_getfl_task(&target.task, number) ==
                    (O_RDWR_ | O_NONBLOCK_) &&
            f_getfd_task(&target.task, number) == 0,
            "创建的 timerfd 具有 O_RDWR 与独立非阻塞状态");
    CHECK(f_close_task(&target.task, number) == 0,
            "关闭 task-aware timerfd");
    destroy_fixture(&target);
    destroy_fixture(&caller);

    struct timerfd_fixture fixture;
    CHECK(init_fixture(&fixture, 8), "初始化 create 分派夹具");
    struct user_probe probe;
    struct guest_linux_user_fault fault;
    reset_probe(&probe);
    for (unsigned combination = 0; combination < 4; combination++) {
        dword_t flags =
                (combination & 1 ? AARCH64_TFD_NONBLOCK : 0) |
                (combination & 2 ? AARCH64_TFD_CLOEXEC : 0);
        dword_t clock = combination & 1 ?
                CLOCK_REALTIME_ : CLOCK_MONOTONIC_;
        qword_t result = invoke_create(&fixture, &probe, &fault,
                HIGH_ARGUMENT | clock,
                HIGH_ARGUMENT | flags);
        created = f_get_task(&fixture.task, 0);
        CHECK(result == 0 && created != NULL,
                "create 按低 32 位接受合法 flag 组合");
        CHECK(f_getfl_task(&fixture.task, 0) ==
                        (O_RDWR_ |
                                (flags & AARCH64_TFD_NONBLOCK)) &&
                f_getfd_task(&fixture.task, 0) ==
                        ((flags & AARCH64_TFD_CLOEXEC) ?
                                FD_CLOEXEC_ : 0),
                "CLOEXEC 与 NONBLOCK 分别进入正确状态域");
        CHECK(f_close_task(&fixture.task, 0) == 0,
                "关闭 flag 组合 timerfd");
    }
    CHECK(probe.reads == 0 && probe.writes == 0 &&
            fault.kind == GUEST_MEMORY_FAULT_NONE,
            "timerfd_create 不访问用户内存");
    CHECK(invoke_create(&fixture, &probe, &fault,
                    CLOCK_MONOTONIC_, UINT32_C(0x4)) ==
                    encoded_error(_EINVAL) &&
            invoke_create(&fixture, &probe, &fault,
                    CLOCK_BOOTTIME_, 0) == encoded_error(_EINVAL) &&
            invoke_create(&fixture, &probe, &fault,
                    UINT64_MAX, 0) == encoded_error(_EINVAL),
            "create 拒绝未知 flag 与当前未支持的 clock");
    destroy_fixture(&fixture);
    return true;
}

static bool test_copy_order_and_error_priority(void) {
    struct timerfd_fixture fixture;
    CHECK(init_fixture(&fixture, 8), "初始化 copy/error 夹具");
    struct user_probe probe;
    struct guest_linux_user_fault fault;
    reset_probe(&probe);
    CHECK(invoke_create(&fixture, &probe, &fault,
                    CLOCK_MONOTONIC_, AARCH64_TFD_NONBLOCK) == 0,
            "创建错误优先级用 timerfd");
    struct aarch64_linux_itimerspec valid = {
        .value = {.sec = 5, .nsec = 0},
    };
    store_wire(&probe, NEW_ADDRESS, valid);

    qword_t crossing = AARCH64_LINUX_USER_ADDRESS_MAX - 7;
    reset_activity(&probe);
    qword_t result = invoke_settime(&fixture, &probe, &fault,
            UINT64_MAX, UINT32_C(0x4), crossing, UINT64_MAX);
    CHECK(result == encoded_error(_EFAULT) &&
            probe.reads == 0 && probe.writes == 0 &&
            fault.address == crossing &&
            fault.access == GUEST_MEMORY_READ &&
            fault.kind == GUEST_MEMORY_FAULT_ADDRESS_SIZE,
            "新值地址尺寸故障优先于 flags 与 fd");

    qword_t second_crossing = AARCH64_LINUX_USER_ADDRESS_MAX - 23;
    reset_activity(&probe);
    probe.allow_external = true;
    result = invoke_settime(&fixture, &probe, &fault,
            UINT64_MAX, UINT32_C(0x4), second_crossing, UINT64_MAX);
    CHECK(result == encoded_error(_EFAULT) &&
            probe.reads == 1 && probe.read_sizes[0] == 16 &&
            probe.read_addresses[0] == second_crossing &&
            probe.writes == 0 &&
            fault.address == second_crossing + 16 &&
            fault.access == GUEST_MEMORY_READ &&
            fault.kind == GUEST_MEMORY_FAULT_ADDRESS_SIZE,
            "interval 位于边界内时才检查越界的 value 段");

    reset_activity(&probe);
    probe.fail_read_at = NEW_ADDRESS + 16;
    result = invoke_settime(&fixture, &probe, &fault,
            0, 0, NEW_ADDRESS, 0);
    CHECK(result == encoded_error(_EFAULT) &&
            two_reads_at(&probe, NEW_ADDRESS) &&
            probe.writes == 0 &&
            fault.address == NEW_ADDRESS + 16 &&
            fault.access == GUEST_MEMORY_READ &&
            fault.kind == GUEST_MEMORY_FAULT_UNMAPPED,
            "copyin 先读 interval，再传播 value 的精确故障");

    reset_activity(&probe);
    result = invoke_settime(&fixture, &probe, &fault,
            HIGH_ARGUMENT | UINT32_MAX, HIGH_ARGUMENT | UINT32_C(0x4),
            NEW_ADDRESS, crossing);
    CHECK(result == encoded_error(_EINVAL) &&
            two_reads_at(&probe, NEW_ADDRESS) && probe.writes == 0,
            "合法 copyin 后未知 flags 优先于坏 fd 与旧值指针");

    const struct aarch64_linux_itimerspec invalid_times[] = {
        {.interval = {.sec = -1}},
        {.interval = {.nsec = -1}},
        {.value = {.sec = -1}},
        {.value = {.nsec = -1}},
        {.interval = {.nsec = INT64_C(1000000000)}},
        {.value = {.nsec = INT64_C(1000000000)}},
    };
    for (size_t index = 0;
            index < array_size(invalid_times); index++) {
        store_wire(&probe, NEW_ADDRESS, invalid_times[index]);
        reset_activity(&probe);
        result = invoke_settime(&fixture, &probe, &fault,
                UINT32_MAX, 0, NEW_ADDRESS, crossing);
        CHECK(result == encoded_error(_EINVAL) &&
                two_reads_at(&probe, NEW_ADDRESS) &&
                probe.writes == 0,
                "负时间与十亿纳秒优先于坏 fd 和旧值指针");
    }

    struct aarch64_linux_itimerspec nanosecond_boundary = {
        .interval = {.nsec = INT64_C(999999999)},
    };
    store_wire(&probe, NEW_ADDRESS, nanosecond_boundary);
    reset_activity(&probe);
    result = invoke_settime(&fixture, &probe, &fault,
            UINT32_MAX, 0, NEW_ADDRESS, crossing);
    CHECK(result == encoded_error(_EBADF) &&
            two_reads_at(&probe, NEW_ADDRESS) && probe.writes == 0,
            "999999999 纳秒合法，因此继续到 fd 校验");

    store_wire(&probe, NEW_ADDRESS, valid);
    for (dword_t flags = 2; flags <= 3; flags++) {
        reset_activity(&probe);
        result = invoke_settime(&fixture, &probe, &fault,
                0, HIGH_ARGUMENT | flags, NEW_ADDRESS, crossing);
        CHECK(result == encoded_error(_EINVAL) &&
                two_reads_at(&probe, NEW_ADDRESS) &&
                probe.writes == 0,
                "本轮明确拒绝 CANCEL_ON_SET 的两个组合");
    }
    reset_activity(&probe);
    result = invoke_settime(&fixture, &probe, &fault,
            HIGH_ARGUMENT | UINT32_MAX, 0, NEW_ADDRESS, crossing);
    CHECK(result == encoded_error(_EBADF) &&
            two_reads_at(&probe, NEW_ADDRESS) && probe.writes == 0,
            "settime 按低 32 位解释负 fd，且 fd 错误优先于旧值指针");

    struct fd *ordinary = adhoc_fd_create(&ordinary_ops);
    CHECK(ordinary != NULL &&
            f_install_task(&fixture.task, ordinary, 0) == 1,
            "安装错误类型描述符");
    reset_activity(&probe);
    result = invoke_settime(&fixture, &probe, &fault,
            HIGH_ARGUMENT | 1, 0, NEW_ADDRESS, crossing);
    CHECK(result == encoded_error(_EINVAL) &&
            two_reads_at(&probe, NEW_ADDRESS) && probe.writes == 0,
            "错误 fd 类型优先于旧值输出故障");

    memset(probe.bytes + probe_offset(OLD_ADDRESS), 0xa5,
            sizeof(struct aarch64_linux_itimerspec));
    reset_activity(&probe);
    probe.fail_write_at = OLD_ADDRESS + 16;
    result = invoke_settime(&fixture, &probe, &fault,
            HIGH_ARGUMENT, 0, NEW_ADDRESS, OLD_ADDRESS);
    CHECK(result == encoded_error(_EFAULT) &&
            two_reads_at(&probe, NEW_ADDRESS) &&
            two_writes_at(&probe, OLD_ADDRESS) &&
            fault.address == OLD_ADDRESS + 16 &&
            fault.access == GUEST_MEMORY_WRITE,
            "旧值按 interval、value 分段写回并传播第二段故障");
    struct aarch64_linux_itimerspec partial =
            load_wire(&probe, OLD_ADDRESS);
    CHECK(partial.interval.sec == 0 && partial.interval.nsec == 0,
            "第二段旧值故障前已提交完整 interval");

    reset_activity(&probe);
    result = invoke_gettime(&fixture, &probe, &fault,
            HIGH_ARGUMENT, QUERY_ADDRESS);
    struct aarch64_linux_itimerspec current_value =
            load_wire(&probe, QUERY_ADDRESS);
    CHECK(result == 0 && two_writes_at(&probe, QUERY_ADDRESS) &&
            current_value.value.sec >= 0 &&
            (current_value.value.sec != 0 ||
                    current_value.value.nsec != 0),
            "旧值写回失败不回滚已经设置的新定时器");

    valid.value = (struct aarch64_linux_timespec) {
        .sec = 10, .nsec = 0,
    };
    store_wire(&probe, NEW_ADDRESS, valid);
    reset_activity(&probe);
    result = invoke_settime(&fixture, &probe, &fault,
            0, 0, NEW_ADDRESS, NEW_ADDRESS);
    CHECK(result == 0 && two_reads_at(&probe, NEW_ADDRESS) &&
            two_writes_at(&probe, NEW_ADDRESS),
            "new 与 old 指针别名时先完成 copyin 再写回");
    reset_activity(&probe);
    CHECK(invoke_gettime(&fixture, &probe, &fault,
                    0, QUERY_ADDRESS) == 0,
            "查询别名设置后的新状态");
    current_value = load_wire(&probe, QUERY_ADDRESS);
    CHECK(current_value.value.sec >= 8,
            "别名 old 写回不会覆盖已导入的新值");

    reset_activity(&probe);
    result = invoke_gettime(&fixture, &probe, &fault,
            UINT32_MAX, crossing);
    CHECK(result == encoded_error(_EBADF) && probe.writes == 0,
            "gettime 的坏 fd 优先于输出地址错误");
    reset_activity(&probe);
    result = invoke_gettime(&fixture, &probe, &fault, 1, crossing);
    CHECK(result == encoded_error(_EINVAL) && probe.writes == 0,
            "gettime 的 fd 类型错误优先于输出地址错误");
    reset_activity(&probe);
    result = invoke_gettime(&fixture, &probe, &fault, 0, crossing);
    CHECK(result == encoded_error(_EFAULT) && probe.writes == 0 &&
            fault.address == crossing &&
            fault.access == GUEST_MEMORY_WRITE &&
            fault.kind == GUEST_MEMORY_FAULT_ADDRESS_SIZE,
            "合法 fd 才检查完整 64 位输出指针边界");
    reset_activity(&probe);
    probe.allow_external = true;
    result = invoke_gettime(
            &fixture, &probe, &fault, 0, second_crossing);
    CHECK(result == encoded_error(_EFAULT) &&
            probe.writes == 1 && probe.write_sizes[0] == 16 &&
            probe.write_addresses[0] == second_crossing &&
            fault.address == second_crossing + 16 &&
            fault.access == GUEST_MEMORY_WRITE &&
            fault.kind == GUEST_MEMORY_FAULT_ADDRESS_SIZE,
            "gettime 先写边界内 interval，再拒绝越界 value");
    reset_activity(&probe);
    result = invoke_gettime(&fixture, &probe, &fault, 0, 0);
    CHECK(result == encoded_error(_EFAULT) && probe.writes == 1 &&
            fault.address == 0 &&
            fault.kind == GUEST_MEMORY_FAULT_UNMAPPED,
            "gettime 的 NULL 输出是映射故障而非可选参数");

    CHECK(f_close_task(&fixture.task, 1) == 0 &&
            f_close_task(&fixture.task, 0) == 0,
            "关闭错误优先级测试描述符");
    destroy_fixture(&fixture);
    return true;
}

static struct timespec add_milliseconds(
        struct timespec time, long milliseconds) {
    time.tv_nsec += milliseconds * 1000000L;
    time.tv_sec += time.tv_nsec / 1000000000L;
    time.tv_nsec %= 1000000000L;
    return time;
}

static bool before(struct timespec time, struct timespec limit) {
    return time.tv_sec < limit.tv_sec ||
            (time.tv_sec == limit.tv_sec &&
                    time.tv_nsec < limit.tv_nsec);
}

static bool wait_readable(struct fd *fd, long timeout_ms) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    struct timespec deadline = add_milliseconds(now, timeout_ms);
    const struct timespec pause = {.tv_nsec = 1000000};
    do {
        if ((fd->ops->poll(fd) & POLL_READ) != 0)
            return true;
        nanosleep(&pause, NULL);
        clock_gettime(CLOCK_MONOTONIC, &now);
    } while (before(now, deadline));
    return (fd->ops->poll(fd) & POLL_READ) != 0;
}

static void *read_timerfd(void *opaque) {
    struct read_worker *worker = opaque;
    current = NULL;
    atomic_store_explicit(
            &worker->started, true, memory_order_release);
    worker->result = file_read_fd(
            worker->fd, &worker->value, sizeof(worker->value));
    return NULL;
}

static bool test_timer_semantics(void) {
    struct timerfd_fixture fixture;
    CHECK(init_fixture(&fixture, 8), "初始化 timerfd 语义夹具");
    struct user_probe probe;
    struct guest_linux_user_fault fault;
    reset_probe(&probe);
    CHECK(invoke_create(&fixture, &probe, &fault,
                    HIGH_ARGUMENT | CLOCK_MONOTONIC_,
                    HIGH_ARGUMENT | AARCH64_TFD_NONBLOCK) == 0,
            "创建非阻塞语义 timerfd");
    struct fd *fd = f_get_task(&fixture.task, 0);
    CHECK(fd != NULL, "取得非阻塞 timerfd");

    byte_t short_buffer[sizeof(uint64_t) - 1] = {0};
    CHECK(file_read_task(&fixture.task, 0,
                    short_buffer, sizeof(short_buffer)) == _EINVAL,
            "不足八字节的读取返回 EINVAL");

    struct aarch64_linux_itimerspec wire = {
        .value = {.nsec = 20000000},
    };
    store_wire(&probe, NEW_ADDRESS, wire);
    reset_activity(&probe);
    CHECK(invoke_settime(&fixture, &probe, &fault,
                    0, 0, NEW_ADDRESS, OLD_ADDRESS) == 0 &&
            two_reads_at(&probe, NEW_ADDRESS) &&
            two_writes_at(&probe, OLD_ADDRESS),
            "设置相对 one-shot 并返回完整旧状态");
    struct aarch64_linux_itimerspec old =
            load_wire(&probe, OLD_ADDRESS);
    CHECK(old.interval.sec == 0 && old.interval.nsec == 0 &&
            old.value.sec == 0 && old.value.nsec == 0,
            "首次设置返回全零旧状态");
    CHECK(wait_readable(fd, 1000), "相对 one-shot 到期后变为可读");

    byte_t unaligned[sizeof(uint64_t) + 1] = {0};
    CHECK(file_read_task(&fixture.task, 0,
                    &unaligned[1], sizeof(uint64_t)) ==
                    (ssize_t) sizeof(uint64_t),
            "timerfd 接受未对齐的通用读取缓冲区");
    uint64_t expirations;
    memcpy(&expirations, &unaligned[1], sizeof(expirations));
    CHECK(expirations == 1 &&
            (fd->ops->poll(fd) & POLL_READ) == 0 &&
            file_read_task(&fixture.task, 0,
                    &expirations, sizeof(expirations)) == _EAGAIN,
            "one-shot 只产生一个计数且读取后恢复不可读");

    wire = (struct aarch64_linux_itimerspec) {
        .interval = {.nsec = 10000000},
        .value = {.nsec = 10000000},
    };
    store_wire(&probe, NEW_ADDRESS, wire);
    reset_activity(&probe);
    CHECK(invoke_settime(&fixture, &probe, &fault,
                    0, 0, NEW_ADDRESS, 0) == 0,
            "设置周期 timerfd");
    const struct timespec accumulate = {.tv_nsec = 120000000};
    nanosleep(&accumulate, NULL);
    CHECK(wait_readable(fd, 1000) &&
            file_read_task(&fixture.task, 0,
                    &expirations, sizeof(expirations)) ==
                    (ssize_t) sizeof(expirations) &&
            expirations >= 2,
            "周期 timerfd 累计多次到期而非折叠为一次");

    wire = (struct aarch64_linux_itimerspec) {
        .interval = {
            .sec = INT64_MAX,
            .nsec = INT64_C(999999999),
        },
    };
    store_wire(&probe, NEW_ADDRESS, wire);
    reset_activity(&probe);
    CHECK(invoke_settime(&fixture, &probe, &fault,
                    0, 0, NEW_ADDRESS, 0) == 0 &&
            invoke_gettime(&fixture, &probe, &fault,
                    0, QUERY_ADDRESS) == 0,
            "停用 timerfd 时保留并查询超长 interval");
    struct aarch64_linux_itimerspec queried =
            load_wire(&probe, QUERY_ADDRESS);
    CHECK(queried.value.sec == 0 && queried.value.nsec == 0 &&
            queried.interval.sec == LINUX_KTIME_MAX_SEC &&
            queried.interval.nsec == LINUX_KTIME_MAX_NSEC &&
            (fd->ops->poll(fd) & POLL_READ) == 0,
            "64 位时间由 core 饱和到 KTIME_MAX，零 value 保持停用");

    struct timespec absolute;
    clock_gettime(CLOCK_MONOTONIC, &absolute);
    absolute = add_milliseconds(absolute, 30);
    wire = (struct aarch64_linux_itimerspec) {
        .value = {
            .sec = absolute.tv_sec,
            .nsec = absolute.tv_nsec,
        },
    };
    store_wire(&probe, NEW_ADDRESS, wire);
    reset_activity(&probe);
    CHECK(invoke_settime(&fixture, &probe, &fault,
                    0, HIGH_ARGUMENT | AARCH64_TFD_TIMER_ABSTIME,
                    NEW_ADDRESS, 0) == 0 &&
            wait_readable(fd, 1000),
            "ABSTIME 按低 32 位设置绝对 monotonic 截止点");
    CHECK(file_read_task(&fixture.task, 0,
                    &expirations, sizeof(expirations)) ==
                    (ssize_t) sizeof(expirations) &&
            expirations == 1,
            "绝对 one-shot 产生单一到期计数");

    wire = (struct aarch64_linux_itimerspec) {
        .interval = {.nsec = 10000000},
        .value = {.nsec = 10000000},
    };
    store_wire(&probe, NEW_ADDRESS, wire);
    CHECK(invoke_settime(&fixture, &probe, &fault,
                    0, 0, NEW_ADDRESS, 0) == 0 &&
            wait_readable(fd, 1000),
            "制造重设前的待读取周期计数");
    wire = (struct aarch64_linux_itimerspec) {0};
    store_wire(&probe, NEW_ADDRESS, wire);
    CHECK(invoke_settime(&fixture, &probe, &fault,
                    0, 0, NEW_ADDRESS, 0) == 0 &&
            (fd->ops->poll(fd) & POLL_READ) == 0 &&
            file_read_task(&fixture.task, 0,
                    &expirations, sizeof(expirations)) == _EAGAIN,
            "重新停用定时器会清除尚未读取的 ticks");

    CHECK(invoke_create(&fixture, &probe, &fault,
                    CLOCK_MONOTONIC_, 0) == 1,
            "创建阻塞唤醒用 timerfd");
    struct fd *blocking = f_get_task(&fixture.task, 1);
    CHECK(blocking != NULL, "取得阻塞 timerfd");
    struct read_worker worker = {.fd = blocking};
    atomic_init(&worker.started, false);
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL,
                    read_timerfd, &worker) == 0,
            "创建阻塞读取线程");
    while (!atomic_load_explicit(
            &worker.started, memory_order_acquire))
        sched_yield();
    wire = (struct aarch64_linux_itimerspec) {
        .value = {.nsec = 20000000},
    };
    store_wire(&probe, NEW_ADDRESS, wire);
    current = &fixture.task;
    CHECK(invoke_settime(&fixture, &probe, &fault,
                    1, 0, NEW_ADDRESS, 0) == 0,
            "为已经阻塞的读取线程设置 one-shot");
    CHECK(pthread_join(thread, NULL) == 0 &&
            worker.result == (ssize_t) sizeof(uint64_t) &&
            worker.value == 1,
            "到期回调唤醒阻塞读取且返回完整计数");

    CHECK(f_close_task(&fixture.task, 1) == 0 &&
            f_close_task(&fixture.task, 0) == 0,
            "关闭 timerfd 语义测试描述符");
    destroy_fixture(&fixture);
    return true;
}

int main(void) {
    if (!test_creation_and_scalar_abi())
        return 1;
    if (!test_copy_order_and_error_priority())
        return 1;
    if (!test_timer_semantics())
        return 1;
    return 0;
}
