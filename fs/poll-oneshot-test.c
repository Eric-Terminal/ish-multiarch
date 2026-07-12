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

static int fake_ready(struct fd *fd) {
    return *(int *) fd->data;
}

static const struct fd_ops fake_ops = {
    .poll = fake_ready,
};

static void init_current_task(struct task *task, struct sighand *sighand) {
    memset(task, 0, sizeof(*task));
    memset(sighand, 0, sizeof(*sighand));
    atomic_init(&sighand->refcount, 1);
    lock_init(&sighand->lock);
    task->sighand = sighand;
    current = task;
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
        result = test_real_fd_cleanup();

    current = NULL;
    pthread_mutex_destroy(&sighand.lock.m);
    return result;
}
