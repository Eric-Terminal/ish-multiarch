#include <stdio.h>
#include <string.h>
#include <time.h>

#include "guest/aarch64/linux-time-abi.h"
#include "guest/memory/address-space.h"
#include "kernel/aarch64-syscall-service.h"
#include "kernel/errno.h"
#include "kernel/signal.h"
#include "kernel/task.h"
#include "kernel/time.h"

#define USER_BASE UINT64_C(0x00007abc12340000)
#define INPUT (USER_BASE + 3)
#define OUTPUT (USER_BASE + 67)
#define BAD (USER_BASE + 128)
#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "AArch64 itimer 测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        return 1; \
    } \
} while (0)

struct user_probe {
    byte_t bytes[128];
    unsigned reads;
    unsigned writes;
};

static bool copy_user(struct user_probe *probe, qword_t address,
        void *bytes, dword_t size, enum guest_memory_access access,
        struct guest_linux_user_fault *fault) {
    if (address < USER_BASE || address - USER_BASE > sizeof(probe->bytes) ||
            size > sizeof(probe->bytes) - (address - USER_BASE)) {
        *fault = (struct guest_linux_user_fault) {
            .address = address, .access = access,
            .kind = GUEST_MEMORY_FAULT_UNMAPPED,
        };
        return false;
    }
    if (access == GUEST_MEMORY_READ)
        memcpy(bytes, probe->bytes + address - USER_BASE, size);
    else
        memcpy(probe->bytes + address - USER_BASE, bytes, size);
    return true;
}

static bool read_user(void *opaque, qword_t address, void *bytes,
        dword_t size, struct guest_linux_user_fault *fault) {
    struct user_probe *probe = opaque;
    probe->reads++;
    return copy_user(probe, address, bytes, size, GUEST_MEMORY_READ, fault);
}

static bool write_user(void *opaque, qword_t address, const void *bytes,
        dword_t size, struct guest_linux_user_fault *fault) {
    struct user_probe *probe = opaque;
    probe->writes++;
    return copy_user(probe, address, (void *) bytes, size,
            GUEST_MEMORY_WRITE, fault);
}

static sqword_t invoke(struct task *task, struct user_probe *probe,
        struct guest_linux_user_fault *fault, qword_t number,
        qword_t which, qword_t value, qword_t old_value) {
    const struct guest_linux_syscall_context context = {
        .task_opaque = task,
        .user = {.opaque = probe, .read = read_user, .write = write_user},
    };
    const struct guest_linux_syscall syscall = {
        .number = number, .arguments = {which, value, old_value},
    };
    return (sqword_t) ish_aarch64_linux_syscall_service.dispatch(
            &context, &syscall, fault);
}

static int check_itimers(struct task *task) {
    struct user_probe probe = {0};
    struct guest_linux_user_fault fault;
    struct aarch64_linux_itimerval value = {0}, result;

    CHECK(invoke(task, &probe, &fault, 102, 0, OUTPUT, 0) == 0 &&
            task->group->itimer == NULL && probe.reads == 0,
            "getitimer 首次查询不创建定时器，也不读取输出缓冲");
    memcpy(&result, probe.bytes + OUTPUT - USER_BASE, sizeof(result));
    CHECK(memcmp(&result, &value, sizeof(value)) == 0,
            "未设置的定时器按 LP64 布局写回全零");

    CHECK(invoke(task, &probe, &fault, 103, 99, BAD, 0) == _EFAULT &&
            fault.address == BAD && fault.access == GUEST_MEMORY_READ,
            "setitimer 先读取 new_value 再校验 which");
    unsigned writes = probe.writes;
    CHECK(invoke(task, &probe, &fault, 102, 99, BAD, 0) == _EINVAL &&
            probe.writes == writes,
            "getitimer 先校验 which 再写 curr_value");
    CHECK(invoke(task, &probe, &fault, 103, 0, UINT64_MAX - 7, 0) == _EFAULT &&
            fault.kind == GUEST_MEMORY_FAULT_ADDRESS_SIZE,
            "跨地址上限的 itimerval 不得环绕成有效地址");

    const struct aarch64_linux_itimerval invalid[] = {
        {.value.sec = -1}, {.value.usec = -1}, {.value.usec = 1000000},
        {.interval.sec = -1}, {.interval.usec = -1}, {.interval.usec = 1000000},
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        memcpy(probe.bytes + INPUT - USER_BASE, &invalid[i], sizeof(value));
        CHECK(invoke(task, &probe, &fault, 103, 0, INPUT, 0) == _EINVAL &&
                task->group->itimer == NULL,
                "负秒数与非规范微秒在停用时也必须拒绝且无副作用");
    }

    value = (struct aarch64_linux_itimerval) {
        .interval = {3, 456789}, .value = {INT64_C(4294967296), 123456},
    };
    memcpy(probe.bytes + INPUT - USER_BASE, &value, sizeof(value));
    CHECK(invoke(task, &probe, &fault, 103,
            UINT64_C(0xffffffff00000000), INPUT, OUTPUT) == 0,
            "which 忽略高位；非对齐高地址与超过 32 位的秒数可用");
    memcpy(&result, probe.bytes + OUTPUT - USER_BASE, sizeof(result));
    CHECK(result.value.sec == 0 && result.interval.sec == 0,
            "首次设置写回零旧值");
    CHECK(invoke(task, &probe, &fault, 102, 0, OUTPUT, 0) == 0,
            "查询运行中的定时器");
    memcpy(&result, probe.bytes + OUTPUT - USER_BASE, sizeof(result));
    CHECK(result.value.sec >= value.value.sec - 1 &&
            result.value.sec <= value.value.sec &&
            result.interval.sec == 3 && result.interval.usec == 456789,
            "查询保留完整秒数和周期微秒，不重设定时器");

    unsigned reads = probe.reads;
    CHECK(invoke(task, &probe, &fault, 103, 0, 0, OUTPUT) == 0 &&
            probe.reads == reads,
            "NULL new_value 停用定时器且不访问零地址");
    memcpy(&result, probe.bytes + OUTPUT - USER_BASE, sizeof(result));
    CHECK(result.interval.sec == 3 && result.value.sec >= value.value.sec - 1,
            "停用仍写回原有的活动时间和周期");

    value = (struct aarch64_linux_itimerval) {.interval = {5, 1}};
    memcpy(probe.bytes + INPUT - USER_BASE, &value, sizeof(value));
    CHECK(invoke(task, &probe, &fault, 103, 0, INPUT, 0) == 0 &&
            invoke(task, &probe, &fault, 102, 0, OUTPUT, 0) == 0,
            "零 value 可以携带非零 interval 停用");
    memcpy(&result, probe.bytes + OUTPUT - USER_BASE, sizeof(result));
    CHECK(result.value.sec == 0 && result.value.usec == 0 &&
            result.interval.sec == 0 && result.interval.usec == 0,
            "停用后查询的剩余时间与周期全部清零");

    value = (struct aarch64_linux_itimerval) {.value.sec = INT64_MAX};
    memcpy(probe.bytes + INPUT - USER_BASE, &value, sizeof(value));
    CHECK(invoke(task, &probe, &fault, 103, 0, INPUT, BAD) == _EFAULT &&
            fault.access == GUEST_MEMORY_WRITE &&
            invoke(task, &probe, &fault, 102, 0, OUTPUT, 0) == 0,
            "旧值 copyout 失败不撤销新定时器");
    memcpy(&result, probe.bytes + OUTPUT - USER_BASE, sizeof(result));
    CHECK(result.value.sec >= INT64_C(9223372035) &&
            result.value.sec <= INT64_C(9223372036),
            "极大正时长按 Linux KTIME_MAX 饱和");

    // 阻塞 SIGALRM 后观察 pending，可验证真实到期而不打断宿主测试线程。
    sigset_add(&task->blocked, SIGALRM_);
    value = (struct aarch64_linux_itimerval) {.value.usec = 10000};
    memcpy(probe.bytes + INPUT - USER_BASE, &value, sizeof(value));
    CHECK(invoke(task, &probe, &fault, 103, 0, INPUT, 0) == 0,
            "微秒定时器可以启动");
    bool delivered = false;
    for (unsigned i = 0; i < 2000 && !delivered; i++) {
        struct timespec delay = {.tv_nsec = 1000000};
        nanosleep(&delay, NULL);
        lock(&task->sighand->lock);
        delivered = sigset_has(task->group->shared_pending, SIGALRM_);
        unlock(&task->sighand->lock);
    }
    CHECK(delivered, "ITIMER_REAL 到期向线程组投递 SIGALRM");
    return 0;
}

int main(void) {
    struct task task = {0};
    struct tgroup group = {0};
    struct sighand sighand = {.refcount = 1};
    task.group = &group;
    task.sighand = &sighand;
    group.leader = &task;
    lock_init(&group.lock);
    lock_init(&sighand.lock);
    lock_init(&task.waiting_cond_lock);
    cond_init(&task.pause);
    list_init(&task.queue);
    list_init(&task.group_links);
    list_init(&group.threads);
    list_add_tail(&group.threads, &task.group_links);
    current = &task;
    task_thread_store(&task, pthread_self());

    int result = check_itimers(&task);
    tgroup_timers_destroy(&group);
    signal_flush_pending(&task);
    signal_flush_group_pending(&group);
    current = NULL;
    cond_destroy(&task.pause);
    pthread_mutex_destroy(&task.waiting_cond_lock.m);
    pthread_mutex_destroy(&sighand.lock.m);
    pthread_mutex_destroy(&group.lock.m);
    return result;
}
