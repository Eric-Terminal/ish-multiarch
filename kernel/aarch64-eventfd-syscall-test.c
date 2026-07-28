#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "fs/fd.h"
#include "fs/poll.h"
#include "guest/linux/syscall-service.h"
#include "guest/memory/address-space.h"
#include "kernel/aarch64-syscall-service.h"
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/eventfd.h"
#include "kernel/fs.h"
#include "kernel/resource.h"
#include "kernel/task.h"

#define EVENTFD2_SYSCALL UINT64_C(19)
#define HIGH_ARGUMENT UINT64_C(0xa5a5a5a500000000)
#define AARCH64_EFD_NONBLOCK UINT32_C(0x00000800)
#define AARCH64_EFD_CLOEXEC UINT32_C(0x00080000)
#define CONCURRENT_READERS 8

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, \
                "AArch64 eventfd2 系统调用测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        return false; \
    } \
} while (0)

_Static_assert(EFD_SEMAPHORE_ == UINT32_C(0x1) &&
        O_NONBLOCK_ == AARCH64_EFD_NONBLOCK &&
        O_CLOEXEC_ == AARCH64_EFD_CLOEXEC,
        "eventfd2 共享 flag 必须匹配 AArch64 Linux 数值");

struct eventfd_fixture {
    struct task task;
    struct tgroup group;
};

struct user_access_probe {
    unsigned reads;
    unsigned writes;
};

struct read_worker {
    struct fd *fd;
    atomic_bool started;
    ssize_t result;
    uint64_t value;
};

static qword_t encoded_error(int error) {
    return (qword_t) (sqword_t) error;
}

static bool reject_user_read(void *opaque, qword_t address,
        void *destination, dword_t size,
        struct guest_linux_user_fault *fault) {
    (void) address;
    (void) destination;
    (void) size;
    (void) fault;
    struct user_access_probe *probe = opaque;
    probe->reads++;
    return false;
}

static bool reject_user_write(void *opaque, qword_t address,
        const void *source, dword_t size,
        struct guest_linux_user_fault *fault) {
    (void) address;
    (void) source;
    (void) size;
    (void) fault;
    struct user_access_probe *probe = opaque;
    probe->writes++;
    return false;
}

static bool init_fixture(
        struct eventfd_fixture *fixture, rlim_t_ descriptor_limit) {
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

static void destroy_fixture(struct eventfd_fixture *fixture) {
    fdtable_release(fixture->task.files);
    if (current == &fixture->task)
        current = NULL;
}

static qword_t invoke_eventfd2(
        struct eventfd_fixture *fixture,
        struct user_access_probe *probe,
        struct guest_linux_user_fault *fault,
        qword_t initial_value, qword_t flags) {
    const struct guest_linux_syscall_context context = {
        .task_opaque = &fixture->task,
        .user = {
            .opaque = probe,
            .read = reject_user_read,
            .write = reject_user_write,
        },
    };
    const struct guest_linux_syscall syscall = {
        .number = EVENTFD2_SYSCALL,
        .arguments = {
            initial_value,
            flags,
            UINT64_C(0x1122334455667788),
        },
    };
    current = &fixture->task;
    return ish_aarch64_linux_syscall_service.dispatch(
            &context, &syscall, fault);
}

static bool test_task_aware_creation(void) {
    struct eventfd_fixture caller;
    struct eventfd_fixture target;
    CHECK(init_fixture(&caller, 4), "初始化调用任务");
    CHECK(init_fixture(&target, 4), "初始化目标任务");
    current = &caller.task;

    fd_t number = eventfd_create_task(
            &target.task, 9, EFD_SEMAPHORE_ | O_NONBLOCK_);
    struct fd *created = f_get_task(&target.task, number);
    CHECK(number == 0 && created != NULL &&
            f_get_task(&caller.task, 0) == NULL &&
            created->eventfd.val == 9 &&
            created->eventfd.semaphore,
            "task-aware 创建只向显式目标表安装 eventfd");
    CHECK(f_getfl_task(&target.task, number) ==
                    (O_RDWR_ | O_NONBLOCK_) &&
            f_getfd_task(&target.task, number) == 0,
            "task-aware 创建保留读写与非阻塞状态");

    CHECK(f_close_task(&target.task, number) == 0,
            "关闭显式目标任务中的 eventfd");
    destroy_fixture(&target);
    destroy_fixture(&caller);
    return true;
}

static bool test_aarch64_flags_and_semaphore(void) {
    struct eventfd_fixture fixture;
    CHECK(init_fixture(&fixture, 8), "初始化 AArch64 flag 夹具");
    struct user_access_probe access = {0};
    struct guest_linux_user_fault fault;

    qword_t result = invoke_eventfd2(&fixture, &access, &fault,
            HIGH_ARGUMENT, HIGH_ARGUMENT | UINT32_C(0x4));
    CHECK(result == encoded_error(_EINVAL) &&
            f_get_task(&fixture.task, 0) == NULL,
            "未知低位 flag 返回 EINVAL 且不安装描述符");

    for (unsigned combination = 0; combination < 8; combination++) {
        dword_t flags =
                (combination & 1 ? EFD_SEMAPHORE_ : 0) |
                (combination & 2 ? AARCH64_EFD_NONBLOCK : 0) |
                (combination & 4 ? AARCH64_EFD_CLOEXEC : 0);
        dword_t initial_value = combination + 1;
        result = invoke_eventfd2(&fixture, &access, &fault,
                HIGH_ARGUMENT | initial_value,
                HIGH_ARGUMENT | flags);
        struct fd *created = f_get_task(&fixture.task, 0);
        CHECK(result == 0 && created != NULL &&
                created->eventfd.val == initial_value &&
                created->eventfd.semaphore ==
                        ((flags & EFD_SEMAPHORE_) != 0),
                "eventfd2 按低 32 位解释参数并接受合法 flag 组合");
        CHECK(f_getfl_task(&fixture.task, 0) ==
                        (O_RDWR_ |
                                (flags & AARCH64_EFD_NONBLOCK)) &&
                f_getfd_task(&fixture.task, 0) ==
                        ((flags & AARCH64_EFD_CLOEXEC) ?
                                FD_CLOEXEC_ : 0),
                "NONBLOCK、CLOEXEC 与 O_RDWR 分别进入正确状态域");
        CHECK(f_close_task(&fixture.task, 0) == 0,
                "逐项关闭 flag 组合描述符");
    }
    CHECK(access.reads == 0 && access.writes == 0 &&
            fault.kind == GUEST_MEMORY_FAULT_NONE,
            "eventfd2 不访问用户内存并清空故障结果");

    result = invoke_eventfd2(&fixture, &access, &fault,
            HIGH_ARGUMENT | 3,
            HIGH_ARGUMENT | EFD_SEMAPHORE_ |
                    AARCH64_EFD_NONBLOCK);
    CHECK(result == 0, "创建非阻塞信号量 eventfd");
    uint64_t value = 0;
    for (unsigned index = 0; index < 3; index++) {
        CHECK(file_read_task(
                        &fixture.task, 0, &value, sizeof(value)) ==
                        sizeof(value) &&
                value == 1,
                "EFD_SEMAPHORE 每次读取恰好消费一个计数");
    }
    CHECK(file_read_task(&fixture.task, 0, &value, sizeof(value)) ==
                    _EAGAIN,
            "信号量计数耗尽后非阻塞读取返回 EAGAIN");

    value = 4;
    CHECK(file_write_task(
                    &fixture.task, 0, &value, sizeof(value)) ==
                    sizeof(value),
            "向信号量 eventfd 增加多个计数");
    struct fd *semaphore = f_get_task(&fixture.task, 0);
    CHECK(semaphore != NULL &&
            (semaphore->ops->poll(semaphore) &
                    (POLL_READ | POLL_WRITE)) ==
                    (POLL_READ | POLL_WRITE),
            "非空且未接近上限的 eventfd 同时可读可写");
    CHECK(file_read_task(
                    &fixture.task, 0, &value, sizeof(value)) ==
                    sizeof(value) &&
            value == 1 && semaphore->eventfd.val == 3,
            "信号量读取保留其余计数与可读状态");
    CHECK(f_close_task(&fixture.task, 0) == 0,
            "关闭信号量 eventfd");

    fd_t ordinary = sys_eventfd2(5, 0);
    value = 7;
    CHECK(ordinary == 0 &&
            file_write_task(&fixture.task, ordinary,
                    &value, sizeof(value)) == sizeof(value) &&
            file_read_task(&fixture.task, ordinary,
                    &value, sizeof(value)) == sizeof(value) &&
            value == 12,
            "i386 包装继续创建整值累加并一次清零的普通 eventfd");
    CHECK(f_close_task(&fixture.task, ordinary) == 0,
            "关闭 i386 包装创建的 eventfd");

    destroy_fixture(&fixture);
    return true;
}

static bool test_overflow_lengths_and_poll(void) {
    struct eventfd_fixture fixture;
    CHECK(init_fixture(&fixture, 4), "初始化 eventfd 边界夹具");
    fd_t number = eventfd_create_task(&fixture.task, 0, O_NONBLOCK_);
    struct fd *event = f_get_task(&fixture.task, number);
    CHECK(number == 0 && event != NULL, "创建边界测试 eventfd");

    byte_t short_buffer[sizeof(uint64_t) - 1] = {0};
    CHECK(file_read_task(&fixture.task, number,
                    short_buffer, sizeof(short_buffer)) == _EINVAL &&
            file_write_task(&fixture.task, number,
                    short_buffer, sizeof(short_buffer)) == _EINVAL,
            "不足八字节的读写均返回 EINVAL");

    uint64_t value = UINT64_MAX;
    CHECK(file_write_task(&fixture.task, number,
                    &value, sizeof(value)) == _EINVAL &&
            event->eventfd.val == 0,
            "拒绝 UINT64_MAX 且不改变计数");

    lock(&event->lock);
    event->eventfd.val = UINT64_MAX - 2;
    unlock(&event->lock);
    value = 1;
    CHECK(file_write_task(&fixture.task, number,
                    &value, sizeof(value)) == sizeof(value) &&
            event->eventfd.val == UINT64_MAX - 1,
            "允许计数恰好增长到 Linux 上限");
    CHECK(file_write_task(&fixture.task, number,
                    &value, sizeof(value)) == _EAGAIN &&
            event->eventfd.val == UINT64_MAX - 1,
            "非阻塞写入不得越过计数上限");
    int readiness = event->ops->poll(event);
    CHECK((readiness & POLL_READ) != 0 &&
            (readiness & POLL_WRITE) == 0,
            "达到上限时保持可读并撤销可写状态");

    value = 0;
    CHECK(file_read_task(&fixture.task, number,
                    &value, sizeof(value)) == sizeof(value) &&
            value == UINT64_MAX - 1 &&
            event->eventfd.val == 0,
            "普通 eventfd 一次读取全部计数并清零");
    readiness = event->ops->poll(event);
    CHECK((readiness & POLL_READ) == 0 &&
            (readiness & POLL_WRITE) != 0,
            "清零后撤销可读并恢复可写状态");

    value = 0;
    CHECK(file_write_task(&fixture.task, number,
                    &value, sizeof(value)) == sizeof(value) &&
            event->eventfd.val == 0 &&
            (event->ops->poll(event) & POLL_READ) == 0,
            "写入零成功但不会制造可读计数");

    byte_t unaligned_write[sizeof(uint64_t) + 1] = {0};
    byte_t unaligned_read[sizeof(uint64_t) + 1] = {0};
    value = 17;
    memcpy(&unaligned_write[1], &value, sizeof(value));
    CHECK(file_write_task(&fixture.task, number,
                    &unaligned_write[1], sizeof(value)) ==
                    sizeof(value),
            "eventfd 接受未对齐的通用写入缓冲区");
    CHECK(file_read_task(&fixture.task, number,
                    &unaligned_read[1], sizeof(value)) ==
                    sizeof(value),
            "eventfd 接受未对齐的通用读取缓冲区");
    value = 0;
    memcpy(&value, &unaligned_read[1], sizeof(value));
    CHECK(value == 17,
            "未对齐缓冲区往返保持完整的八字节计数");

    CHECK(f_close_task(&fixture.task, number) == 0,
            "关闭边界测试 eventfd");
    destroy_fixture(&fixture);
    return true;
}

static void *read_eventfd(void *opaque) {
    struct read_worker *worker = opaque;
    current = NULL;
    atomic_store_explicit(
            &worker->started, true, memory_order_release);
    worker->result = file_read_fd(
            worker->fd, &worker->value, sizeof(worker->value));
    return NULL;
}

static bool test_concurrent_reads_and_wakeup(void) {
    struct eventfd_fixture fixture;
    CHECK(init_fixture(&fixture, 4), "初始化 eventfd 并发夹具");
    fd_t number = eventfd_create_task(
            &fixture.task, CONCURRENT_READERS, EFD_SEMAPHORE_);
    struct fd *event = f_get_task(&fixture.task, number);
    CHECK(number == 0 && event != NULL, "创建并发信号量 eventfd");

    struct read_worker workers[CONCURRENT_READERS] = {0};
    pthread_t threads[CONCURRENT_READERS];
    for (unsigned index = 0; index < CONCURRENT_READERS; index++) {
        workers[index].fd = event;
        atomic_init(&workers[index].started, false);
        CHECK(pthread_create(&threads[index], NULL,
                        read_eventfd, &workers[index]) == 0,
                "创建并发信号量读取线程");
    }
    for (unsigned index = 0; index < CONCURRENT_READERS; index++) {
        CHECK(pthread_join(threads[index], NULL) == 0 &&
                workers[index].result ==
                        (ssize_t) sizeof(uint64_t) &&
                workers[index].value == 1,
                "并发信号量读取各自原子消费一个计数");
    }
    CHECK(event->eventfd.val == 0,
            "并发信号量读取不会丢失或重复计数");
    CHECK(f_close_task(&fixture.task, number) == 0,
            "关闭并发信号量 eventfd");

    number = eventfd_create_task(&fixture.task, 0, 0);
    event = f_get_task(&fixture.task, number);
    CHECK(number == 0 && event != NULL, "创建阻塞唤醒 eventfd");
    struct read_worker waiter = {.fd = event};
    atomic_init(&waiter.started, false);
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, read_eventfd, &waiter) == 0,
            "创建阻塞读取线程");
    while (!atomic_load_explicit(
            &waiter.started, memory_order_acquire))
        sched_yield();
    uint64_t value = 9;
    CHECK(file_write_task(&fixture.task, number,
                    &value, sizeof(value)) == sizeof(value),
            "并发写入唤醒阻塞读取");
    CHECK(pthread_join(thread, NULL) == 0 &&
            waiter.result == (ssize_t) sizeof(uint64_t) &&
            waiter.value == 9 && event->eventfd.val == 0,
            "阻塞读取观察完整计数且不发生丢失唤醒");
    CHECK(f_close_task(&fixture.task, number) == 0,
            "关闭阻塞唤醒 eventfd");

    destroy_fixture(&fixture);
    return true;
}

int main(void) {
    if (!test_task_aware_creation())
        return 1;
    if (!test_aarch64_flags_and_semaphore())
        return 1;
    if (!test_overflow_lengths_and_poll())
        return 1;
    if (!test_concurrent_reads_and_wakeup())
        return 1;
    return 0;
}
