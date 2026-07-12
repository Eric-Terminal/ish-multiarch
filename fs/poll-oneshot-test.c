#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "fs/fd.h"
#include "fs/poll.h"
#include "fs/real.h"
#include "kernel/errno.h"
#include "kernel/task.h"

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "poll 单次触发测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        return 1; \
    } \
} while (0)

struct callback_result {
    int calls;
    int types;
    uint64_t info;
};

struct capacity_result {
    int calls;
    int delivered;
    int capacity;
    uint64_t info;
};

static int record_event(
        void *opaque, int types, union poll_fd_info info) {
    struct callback_result *result = opaque;
    result->calls++;
    result->types = types;
    result->info = info.num;
    return 1;
}

static int reject_event(
        void *opaque, int types, union poll_fd_info info) {
    struct callback_result *result = opaque;
    result->calls++;
    result->types = types;
    result->info = info.num;
    return 0;
}

static int record_with_capacity(
        void *opaque, int types, union poll_fd_info info) {
    struct capacity_result *result = opaque;
    use(types);
    result->calls++;
    if (result->delivered >= result->capacity)
        return 0;
    result->delivered++;
    result->info = info.num;
    return 1;
}

static int fake_ready(struct fd *fd) {
    return *(int *) fd->data;
}

static const struct fd_ops fake_ops = {
    .poll = fake_ready,
};

static void init_task(struct task *task, struct sighand *sighand) {
    memset(task, 0, sizeof(*task));
    memset(sighand, 0, sizeof(*sighand));
    atomic_init(&sighand->refcount, 1);
    lock_init(&sighand->lock);
    task->sighand = sighand;
}

static void init_current_task(struct task *task, struct sighand *sighand) {
    init_task(task, sighand);
    current = task;
}

struct wait_context {
    struct poll *poll;
    struct task task;
    struct sighand sighand;
    struct callback_result callback;
    int result;
};

static void *wait_for_event(void *opaque) {
    struct wait_context *context = opaque;
    current = &context->task;
    struct timespec timeout = {.tv_sec = 2};
    context->result = poll_wait(context->poll, record_event,
            &context->callback, &timeout);
    current = NULL;
    return NULL;
}

static bool wait_until_blocked(struct poll *poll) {
    for (unsigned attempt = 0; attempt < 100000; attempt++) {
        lock(&poll->lock);
        bool waiting = poll->waiters != 0;
        unlock(&poll->lock);
        if (waiting)
            return true;
        sched_yield();
    }
    return false;
}

static int test_duplicate_poll_entries(void) {
    int readiness = POLL_READ;
    struct fd *fd = fd_create(&fake_ops);
    CHECK(fd != NULL, "创建重复登记测试文件对象");
    fd->data = &readiness;

    struct poll *poll = poll_create();
    CHECK(!IS_ERR(poll), "创建重复登记 poll 实例");
    CHECK(poll_add_fd(poll, fd, POLL_READ,
            (union poll_fd_info) {.num = 1}) == 0,
            "普通 poll 接受首次登记");
    CHECK(poll_add_fd(poll, fd, POLL_READ,
            (union poll_fd_info) {.num = 2}) == 0,
            "普通 poll 保留重复文件描述符语义");

    struct timespec immediate = {0};
    struct callback_result result = {0};
    CHECK(poll_wait(poll, record_event, &result, &immediate) == 2 &&
            result.calls == 2, "重复登记分别投递事件");

    poll_destroy(poll);
    CHECK(fd_close(fd) == 0, "释放重复登记测试文件对象");
    return 0;
}

static int test_registration_semantics(void) {
    int readiness = POLL_READ;
    struct fd *fd = fd_create(&fake_ops);
    CHECK(fd != NULL, "创建测试文件对象");
    fd->data = &readiness;

    struct poll *poll = poll_create();
    CHECK(!IS_ERR(poll), "创建 poll 实例");
    union poll_fd_info first_info = {.num = 0x1234};
    CHECK(poll_add_fd_unique(poll, fd, POLL_READ | POLL_ONESHOT,
            first_info) == 0, "首次登记成功");
    CHECK(poll_has_fd(poll, fd), "登记后能够查询到文件对象");
    CHECK(poll_add_fd_unique(poll, fd, POLL_READ | POLL_ONESHOT,
            first_info) == _EEXIST, "重复 ADD 返回 EEXIST");

    struct timespec immediate = {0};
    struct callback_result rejected = {0};
    CHECK(poll_wait(poll, reject_event, &rejected, &immediate) == 0 &&
            rejected.calls == 1, "回调容量不足时不计入已投递事件");
    CHECK(poll_has_fd(poll, fd), "未投递事件不会禁用单次触发登记");

    struct callback_result first = {0};
    CHECK(poll_wait(poll, record_event, &first, &immediate) == 1,
            "首次就绪事件被投递");
    CHECK(first.calls == 1 && first.types == POLL_READ &&
            first.info == first_info.num, "首次事件保留类型与用户数据");
    CHECK(poll_has_fd(poll, fd), "单次触发后仍保留登记");

    struct callback_result disabled = {0};
    CHECK(poll_wait(poll, record_event, &disabled, &immediate) == 0 &&
            disabled.calls == 0, "未重置前不重复投递");

    union poll_fd_info second_info = {.num = 0x5678};
    CHECK(poll_mod_fd(poll, fd, POLL_READ | POLL_ONESHOT,
            second_info) == 0, "MOD 重置单次触发登记");
    struct callback_result second = {0};
    CHECK(poll_wait(poll, record_event, &second, &immediate) == 1,
            "重置后再次投递就绪事件");
    CHECK(second.calls == 1 && second.types == POLL_READ &&
            second.info == second_info.num, "重置同时更新用户数据");
    CHECK(poll_del_fd(poll, fd) == 0, "禁用状态仍可删除登记");
    CHECK(!poll_has_fd(poll, fd) && poll_del_fd(poll, fd) == _ENOENT,
            "删除后登记消失且重复删除返回 ENOENT");

    poll_destroy(poll);
    CHECK(fd_close(fd) == 0, "释放测试文件对象");
    return 0;
}

static int test_capacity_rejection_preserves_ready_event(void) {
    int readiness[2] = {POLL_READ, POLL_READ};
    struct fd *fds[2] = {
        fd_create(&fake_ops),
        fd_create(&fake_ops),
    };
    CHECK(fds[0] != NULL && fds[1] != NULL,
            "创建容量测试文件对象");
    fds[0]->data = &readiness[0];
    fds[1]->data = &readiness[1];

    struct poll *poll = poll_create();
    CHECK(!IS_ERR(poll), "创建容量测试 poll 实例");
    for (unsigned index = 0; index < 2; index++) {
        CHECK(poll_add_fd_unique(poll, fds[index],
                POLL_READ | POLL_ONESHOT,
                (union poll_fd_info) {.num = index + 1}) == 0,
                "登记容量测试就绪事件");
    }

    struct timespec immediate = {0};
    struct capacity_result first = {.capacity = 1};
    CHECK(poll_wait(poll, record_with_capacity, &first, &immediate) == 1 &&
            first.calls == 2 && first.delivered == 1,
            "首轮容量不足时只消费一个事件");

    struct capacity_result second = {.capacity = 1};
    CHECK(poll_wait(poll, record_with_capacity, &second, &immediate) == 1 &&
            second.calls == 1 && second.delivered == 1 &&
            second.info != first.info,
            "容量拒绝的事件在下一轮继续投递");

    struct capacity_result exhausted = {.capacity = 1};
    CHECK(poll_wait(poll, record_with_capacity,
            &exhausted, &immediate) == 0 && exhausted.calls == 0,
            "两个单次触发事件均已消费");

    poll_destroy(poll);
    CHECK(fd_close(fds[0]) == 0 && fd_close(fds[1]) == 0,
            "释放容量测试文件对象");
    return 0;
}

static int test_ready_add_wakes_waiter(void) {
    int readiness = POLL_READ;
    struct fd *fd = fd_create(&fake_ops);
    CHECK(fd != NULL, "创建 ADD 唤醒测试文件对象");
    fd->data = &readiness;

    struct poll *poll = poll_create();
    CHECK(!IS_ERR(poll), "创建 ADD 唤醒测试 poll 实例");
    struct wait_context context = {.poll = poll};
    init_task(&context.task, &context.sighand);
    pthread_t waiter;
    CHECK(pthread_create(&waiter, NULL, wait_for_event, &context) == 0,
            "启动 ADD 阻塞等待线程");

    bool blocked = wait_until_blocked(poll);
    int add_result = poll_add_fd_unique(poll, fd, POLL_READ,
            (union poll_fd_info) {.num = 0xadd});
    int join_result = pthread_join(waiter, NULL);
    CHECK(blocked, "ADD 前等待线程已进入阻塞状态");
    CHECK(add_result == 0, "向阻塞 poll 添加已就绪登记");
    CHECK(join_result == 0 && context.result == 1 &&
            context.callback.calls == 1 &&
            context.callback.types == POLL_READ &&
            context.callback.info == 0xadd,
            "已就绪 ADD 立即唤醒等待线程");

    pthread_mutex_destroy(&context.sighand.lock.m);
    poll_destroy(poll);
    CHECK(fd_close(fd) == 0, "释放 ADD 唤醒测试文件对象");
    return 0;
}

static int test_ready_mod_wakes_waiter(void) {
    int readiness = POLL_READ;
    struct fd *fd = fd_create(&fake_ops);
    CHECK(fd != NULL, "创建 MOD 唤醒测试文件对象");
    fd->data = &readiness;

    struct poll *poll = poll_create();
    CHECK(!IS_ERR(poll), "创建 MOD 唤醒测试 poll 实例");
    CHECK(poll_add_fd_unique(poll, fd, POLL_READ | POLL_ONESHOT,
            (union poll_fd_info) {.num = 1}) == 0,
            "登记 MOD 唤醒测试事件");
    struct timespec immediate = {0};
    struct callback_result initial = {0};
    CHECK(poll_wait(poll, record_event, &initial, &immediate) == 1,
            "先消费单次触发事件使登记禁用");

    struct wait_context context = {.poll = poll};
    init_task(&context.task, &context.sighand);
    pthread_t waiter;
    CHECK(pthread_create(&waiter, NULL, wait_for_event, &context) == 0,
            "启动 MOD 阻塞等待线程");

    bool blocked = wait_until_blocked(poll);
    int mod_result = poll_mod_fd(poll, fd,
            POLL_READ | POLL_ONESHOT,
            (union poll_fd_info) {.num = 2});
    int join_result = pthread_join(waiter, NULL);
    CHECK(blocked, "MOD 前等待线程已进入阻塞状态");
    CHECK(mod_result == 0, "重置阻塞 poll 中的已就绪登记");
    CHECK(join_result == 0 && context.result == 1 &&
            context.callback.calls == 1 &&
            context.callback.types == POLL_READ &&
            context.callback.info == 2,
            "已就绪 MOD 立即唤醒等待线程");

    pthread_mutex_destroy(&context.sighand.lock.m);
    poll_destroy(poll);
    CHECK(fd_close(fd) == 0, "释放 MOD 唤醒测试文件对象");
    return 0;
}

static int test_real_fd_cleanup(void) {
    int pipe_fds[2];
    CHECK(pipe(pipe_fds) == 0, "创建宿主管道");
    struct fd *read_end = fd_create(&realfs_fdops);
    CHECK(read_end != NULL, "创建真实文件对象");
    read_end->real_fd = pipe_fds[0];

    struct poll *poll = poll_create();
    CHECK(!IS_ERR(poll), "为真实文件创建 poll 实例");
    CHECK(poll_add_fd(poll, read_end, POLL_READ | POLL_ONESHOT,
            (union poll_fd_info) {.num = 7}) == 0,
            "登记真实管道读端");
    CHECK(write(pipe_fds[1], "x", 1) == 1, "使真实管道进入就绪状态");

    struct timespec immediate = {0};
    struct callback_result first = {0};
    CHECK(poll_wait(poll, record_event, &first, &immediate) == 1 &&
            first.calls == 1, "真实后端投递首次事件");
    struct callback_result disabled = {0};
    CHECK(poll_wait(poll, record_event, &disabled, &immediate) == 0 &&
            disabled.calls == 0, "真实后端在重置前保持禁用");
    CHECK(poll_mod_fd(poll, read_end, POLL_READ | POLL_ONESHOT,
            (union poll_fd_info) {.num = 8}) == 0,
            "真实后端可通过 MOD 重置");
    struct callback_result second = {0};
    CHECK(poll_wait(poll, record_event, &second, &immediate) == 1 &&
            second.calls == 1 && second.info == 8,
            "真实后端重置后再次投递并更新数据");

    CHECK(fd_close(read_end) == 0, "关闭禁用中的真实文件对象");
    CHECK(list_empty(&poll->poll_fds), "关闭文件对象同步清理 poll 登记");
    poll_destroy(poll);
    CHECK(close(pipe_fds[1]) == 0, "关闭宿主管道写端");
    return 0;
}

int main(void) {
    struct task task;
    struct sighand sighand;
    init_current_task(&task, &sighand);

    int result = test_duplicate_poll_entries();
    if (result == 0)
        result = test_registration_semantics();
    if (result == 0)
        result = test_capacity_rejection_preserves_ready_event();
    if (result == 0)
        result = test_ready_add_wakes_waiter();
    if (result == 0)
        result = test_ready_mod_wakes_waiter();
    if (result == 0)
        result = test_real_fd_cleanup();

    current = NULL;
    pthread_mutex_destroy(&sighand.lock.m);
    return result;
}
