#include <assert.h>
#include <limits.h>
#include <stdint.h>

#include "guest/aarch64/linux-signal-abi.h"
#include "guest/aarch64/linux-time-abi.h"
#include "guest/memory/address-space.h"
#include "kernel/aarch64-time-service.h"
#include "kernel/errno.h"
#include "kernel/task.h"
#include "util/timer.h"

#define AARCH64_TIME_USER_ADDRESS_LIMIT \
    (AARCH64_LINUX_USER_ADDRESS_MAX + UINT64_C(1))
#define AARCH64_LINUX_KTIME_MAX_SEC INT64_C(9223372036)
#define AARCH64_LINUX_KTIME_MAX_NSEC INT64_C(854775807)

_Static_assert(AARCH64_LINUX_KTIME_MAX_SEC * INT64_C(1000000000) +
        AARCH64_LINUX_KTIME_MAX_NSEC == INT64_MAX,
        "AArch64 nanosleep 的截止上限必须匹配 Linux KTIME_MAX");

static qword_t time_result(sqword_t result) {
    return (qword_t) result;
}

static bool time_user_range_fits(qword_t address, qword_t size) {
    return address <= AARCH64_TIME_USER_ADDRESS_LIMIT &&
            size <= AARCH64_TIME_USER_ADDRESS_LIMIT - address;
}

static qword_t time_user_range_error(
        struct guest_linux_user_fault *fault, qword_t address,
        enum guest_memory_access access) {
    *fault = (struct guest_linux_user_fault) {
        .address = address,
        .access = (dword_t) access,
        .kind = GUEST_MEMORY_FAULT_ADDRESS_SIZE,
    };
    return time_result(_EFAULT);
}

static qword_t read_guest_timespec(
        const struct guest_linux_syscall_context *context,
        qword_t address, struct aarch64_linux_timespec *time,
        struct guest_linux_user_fault *fault) {
    if (!time_user_range_fits(address, sizeof(*time)))
        return time_user_range_error(
                fault, address, GUEST_MEMORY_READ);
    assert(context->user.read != NULL);
    if (!context->user.read(context->user.opaque,
            address, time, sizeof(*time), fault))
        return time_result(_EFAULT);
    return 0;
}

static qword_t write_guest_timespec(
        const struct guest_linux_syscall_context *context,
        qword_t address, struct timer_time time,
        struct guest_linux_user_fault *fault) {
    if (!time_user_range_fits(
            address, sizeof(struct aarch64_linux_timespec)))
        return time_user_range_error(
                fault, address, GUEST_MEMORY_WRITE);
    const struct aarch64_linux_timespec wire = {
        .sec = time.sec,
        .nsec = time.nsec,
    };
    assert(context->user.write != NULL);
    if (!context->user.write(context->user.opaque,
            address, &wire, sizeof(wire), fault))
        return time_result(_EFAULT);
    return 0;
}

static struct timer_time monotonic_now(void) {
    return timer_time_from_timespec(timespec_now(CLOCK_MONOTONIC));
}

static bool time_after(
        struct timer_time time, struct timer_time other) {
    return time.sec > other.sec ||
            (time.sec == other.sec && time.nsec > other.nsec);
}

static struct timer_time make_deadline(
        struct timer_time started, struct timer_time duration) {
    const struct timer_time maximum = {
        AARCH64_LINUX_KTIME_MAX_SEC,
        AARCH64_LINUX_KTIME_MAX_NSEC,
    };
    // Linux 先把相对时长转换为 ktime，再把绝对截止点饱和到 KTIME_MAX。
    if (duration.sec >= AARCH64_LINUX_KTIME_MAX_SEC)
        return maximum;
    struct timer_time available = timer_time_subtract(maximum, started);
    return !timer_time_positive(available) || time_after(duration, available) ?
            maximum : timer_time_add(started, duration);
}

static struct timer_time deadline_remaining(struct timer_time deadline) {
    struct timer_time remaining = timer_time_subtract(
            deadline, monotonic_now());
    return timer_time_positive(remaining) ? remaining :
            (struct timer_time) {0};
}

static struct timespec host_wait_slice(struct timer_time remaining) {
    int64_t seconds = remaining.sec;
    long nanoseconds = (long) remaining.nsec;
    // watchOS arm64_32 的 time_t 只有 32 位，长等待必须分段。
    if (seconds > INT32_MAX) {
        seconds = INT32_MAX;
        nanoseconds = 0;
    }
    return (struct timespec) {
        .tv_sec = (time_t) seconds,
        .tv_nsec = nanoseconds,
    };
}

qword_t aarch64_linux_dispatch_nanosleep(
        const struct guest_linux_syscall_context *context,
        const struct guest_linux_syscall *syscall,
        struct task *task, struct guest_linux_user_fault *fault) {
    assert(task != NULL && task == current && task->sighand != NULL);
    struct aarch64_linux_timespec request;
    qword_t copied = read_guest_timespec(
            context, syscall->arguments[0], &request, fault);
    if ((sqword_t) copied < 0)
        return copied;
    if (request.sec < 0 || request.nsec < 0 ||
            request.nsec >= INT64_C(1000000000))
        return time_result(_EINVAL);

    struct timer_time duration = {request.sec, request.nsec};
    if (timer_time_is_zero(duration))
        return 0;
    struct timer_time deadline = make_deadline(monotonic_now(), duration);

    bool interrupted = false;
    struct sighand *sighand = task->sighand;
    lock(&sighand->lock);
    while (true) {
        struct timer_time remaining = deadline_remaining(deadline);
        if (!timer_time_positive(remaining))
            break;
        struct timespec wait = host_wait_slice(remaining);
        if (wait_for(&task->pause, &sighand->lock, &wait) == _EINTR) {
            interrupted = true;
            break;
        }
    }
    unlock(&sighand->lock);
    if (!interrupted)
        return 0;

    struct timer_time remaining = deadline_remaining(deadline);
    // 到期与信号竞争时由已经完成的睡眠胜出，不写 rem。
    if (!timer_time_positive(remaining))
        return 0;

    qword_t rem_address = syscall->arguments[1];
    if (rem_address != 0) {
        qword_t written = write_guest_timespec(context,
                rem_address, remaining, fault);
        if ((sqword_t) written < 0)
            return written;
    }
    return time_result(_EINTR);
}
