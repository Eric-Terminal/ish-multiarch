#include <stdio.h>
#include <string.h>

#include "fs/fd.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "kernel/resource.h"
#include "kernel/task.h"

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "fd 目标任务测试失败：%s（第 %d 行）\n", message, __LINE__); \
        return 1; \
    } \
} while (0)

static int closed_fds;

static int count_close(struct fd *fd) {
    (void) fd;
    closed_fds++;
    return 0;
}

static const struct fd_ops test_fd_ops = {
    .close = count_close,
};

static void init_task(struct task *task, struct tgroup *group, rlim_t_ nofile, int table_size) {
    memset(task, 0, sizeof(*task));
    memset(group, 0, sizeof(*group));
    lock_init(&group->lock);
    group->limits[RLIMIT_NOFILE_] = (struct rlimit_) {nofile, nofile};
    task->group = group;
    task->files = fdtable_new(table_size);
}

int main(void) {
    struct task current_task;
    struct task target_task;
    struct task small_task;
    struct tgroup current_group;
    struct tgroup target_group;
    struct tgroup small_group;
    init_task(&current_task, &current_group, 1, 1);
    init_task(&target_task, &target_group, 4, 1);
    init_task(&small_task, &small_group, 1, 1);
    CHECK(!IS_ERR(current_task.files), "当前任务 fd 表创建成功");
    CHECK(!IS_ERR(target_task.files), "目标任务 fd 表创建成功");
    CHECK(!IS_ERR(small_task.files), "小尺寸目标 fd 表创建成功");
    current = &current_task;

    struct fd *target_fds[4];
    for (fd_t f = 0; f < 4; f++) {
        target_fds[f] = fd_create(&test_fd_ops);
        CHECK(target_fds[f] != NULL, "目标任务测试 fd 创建成功");
        int flags = f == 1 ? O_CLOEXEC_ | O_NONBLOCK_ : 0;
        CHECK(f_install_task(&target_task, target_fds[f], flags) == f,
                "安装必须采用目标任务的 NOFILE 限制并按需扩容");
    }

    CHECK(target_task.files->size == 4, "目标 fd 表扩容到第四个描述符");
    CHECK(current_task.files->size == 1 && current_task.files->files[0] == NULL,
            "目标安装不得修改当前任务 fd 表");
    CHECK(f_get_task(&target_task, 3) == target_fds[3],
            "查询边界必须使用目标 fd 表尺寸");
    CHECK(f_get_task(&target_task, -1) == NULL && f_get_task(&target_task, 4) == NULL,
            "目标查询拒绝负数和越界描述符");
    current = &target_task;
    CHECK(f_get_task(&small_task, 3) == NULL,
            "当前表较大时仍按目标表尺寸拒绝越界描述符");
    current = &current_task;
    CHECK(bit_test(1, target_task.files->cloexec), "目标表记录 O_CLOEXEC");
    CHECK(fd_getflags(target_fds[1]) & O_NONBLOCK_, "目标 fd 记录 O_NONBLOCK");
    CHECK(f_get(0) == NULL, "兼容查询仍只访问当前任务");

    int before_close = closed_fds;
    struct fd *rejected_target_fd = fd_create(&test_fd_ops);
    CHECK(rejected_target_fd != NULL, "超限测试 fd 创建成功");
    CHECK(f_install_task(&target_task, rejected_target_fd, 0) == _EMFILE,
            "目标任务达到自身 NOFILE 后返回 EMFILE");
    CHECK(closed_fds == before_close + 1, "安装失败必须销毁被接管的 fd 引用");

    before_close = closed_fds;
    fdtable_do_cloexec(target_task.files);
    CHECK(target_task.files->files[1] == NULL && !bit_test(1, target_task.files->cloexec),
            "非当前表的 close-on-exec 清除 fd 与标志位");
    CHECK(closed_fds == before_close + 1, "close-on-exec 只释放带标志的引用");
    CHECK(f_close_task(&target_task, 1) == _EBADF, "close-on-exec 后重复关闭返回 EBADF");
    before_close = closed_fds;
    CHECK(f_close_task(&target_task, 3) == 0, "显式关闭必须作用于目标任务");
    CHECK(target_task.files->files[3] == NULL, "显式关闭清除目标表项");
    CHECK(closed_fds == before_close + 1, "显式关闭释放目标表持有的引用");

    struct fd *current_fd = fd_create(&test_fd_ops);
    CHECK(current_fd != NULL, "当前任务测试 fd 创建成功");
    CHECK(f_install(current_fd, O_CLOEXEC_) == 0, "兼容安装使用当前任务");
    CHECK(f_get(0) == current_fd && bit_test(0, current_task.files->cloexec),
            "兼容查询与标志保留原有行为");
    struct fd *rejected_current_fd = fd_create(&test_fd_ops);
    CHECK(rejected_current_fd != NULL, "当前任务超限测试 fd 创建成功");
    before_close = closed_fds;
    CHECK(f_install(rejected_current_fd, 0) == _EMFILE, "兼容安装遵守当前任务 NOFILE");
    CHECK(closed_fds == before_close + 1, "兼容安装失败销毁 fd 引用");
    CHECK(f_close(0) == 0, "兼容关闭使用当前任务");
    CHECK(f_get_task(&target_task, 0) == target_fds[0],
            "当前任务关闭不得影响目标任务");

    current = NULL;
    fdtable_release(current_task.files);
    fdtable_release(target_task.files);
    fdtable_release(small_task.files);
    CHECK(closed_fds == 7, "清理阶段恰好释放全部七个测试 fd");
    return 0;
}
