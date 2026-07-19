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
    int retained_close_count = closed_fds;
    struct fd *retained = f_get_task_retain(&target_task, 2);
    CHECK(retained == target_fds[2], "保留查询返回目标 fd 的独立引用");
    CHECK(fd_close(retained) == 0 && closed_fds == retained_close_count,
            "释放独立引用不得关闭表中仍持有的 fd");
    CHECK(f_get_task_retain(&target_task, 99) == NULL,
            "保留查询拒绝越界描述符");
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

    before_close = closed_fds;
    CHECK(f_dupfd_task(&target_task, 0, 1, 0) == 1,
            "F_DUPFD 从指定下界选择最低空槽");
    CHECK(f_get_task(&target_task, 1) == target_fds[0] &&
            f_getfd_task(&target_task, 1) == 0,
            "普通复制共享文件对象并清除描述符 CLOEXEC");
    CHECK(f_setfd_task(&target_task, 1, FD_CLOEXEC_) == 0 &&
            f_getfd_task(&target_task, 1) == FD_CLOEXEC_ &&
            f_getfd_task(&target_task, 0) == 0,
            "描述符 flags 只属于各自表项");
    CHECK(f_dup3_task(&target_task, 0, 3, O_CLOEXEC_) == 3 &&
            f_get_task(&target_task, 3) == target_fds[0] &&
            f_getfd_task(&target_task, 3) == FD_CLOEXEC_,
            "dup3 精确安装目标并设置 CLOEXEC");
    CHECK(f_dup3_task(&target_task, 0, 0, 0) == _EINVAL,
            "dup3 拒绝相同源与目标");
    CHECK(f_dup3_task(&target_task, 99, 3, O_NONBLOCK_) == _EINVAL,
            "dup3 的非法 flags 优先于无效源描述符");
    CHECK(f_dup3_task(&target_task, 99, 4, 0) == _EBADF,
            "dup3 的越界目标返回 EBADF");
    CHECK(f_dupfd_task(&target_task, 99, -1, 0) == _EBADF,
            "F_DUPFD 先验证源描述符");
    CHECK(f_dupfd_task(&target_task, 0, -1, 0) == _EINVAL &&
            f_dupfd_task(&target_task, 0, 4, 0) == _EINVAL,
            "F_DUPFD 拒绝负下界与 NOFILE 边界");
    CHECK(f_dupfd_task(&target_task, 0, 0, 0) == _EMFILE,
            "描述符表没有空槽时复制返回 EMFILE");
    CHECK(f_setfd_task(&target_task, 2, FD_CLOEXEC_) == 0,
            "替换测试先标记目标描述符");
    CHECK(f_dup3_task(&target_task, 0, 2, 0) == 2 &&
            f_get_task(&target_task, 2) == target_fds[0] &&
            f_getfd_task(&target_task, 2) == 0 &&
            closed_fds == before_close + 1,
            "dup3 原子替换目标、清除 CLOEXEC 并只关闭旧对象一次");
    CHECK(f_dup2_task(&target_task, 0, 0) == 0,
            "dup2 对有效同号描述符保持原项");
    CHECK(f_setfl_task(&target_task, 0,
            O_APPEND_ | O_NONBLOCK_) == 0 &&
            (f_getfl_task(&target_task, 1) &
                    (O_APPEND_ | O_NONBLOCK_)) ==
                    (O_APPEND_ | O_NONBLOCK_),
            "复制项共享 open-file-description 状态 flags");
    CHECK(f_getfd_task(&target_task, 99) == _EBADF &&
            f_setfd_task(&target_task, 99, 0) == _EBADF &&
            f_getfl_task(&target_task, 99) == _EBADF,
            "标志操作统一拒绝无效描述符");
    CHECK(f_close_task(&target_task, 3) == 0 &&
            f_close_task(&target_task, 1) == 0 &&
            closed_fds == before_close + 1,
            "清理复制项不会提前关闭仍由源表项持有的对象");

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

    struct task pair_task;
    struct tgroup pair_group;
    init_task(&pair_task, &pair_group, 2, 1);
    CHECK(!IS_ERR(pair_task.files), "双端原子安装表创建成功");
    struct fd *pair[2] = {
        fd_create(&test_fd_ops),
        fd_create(&test_fd_ops),
    };
    CHECK(pair[0] != NULL && pair[1] != NULL,
            "双端原子安装对象创建成功");
    fd_t pair_numbers[2] = {-1, -1};
    qword_t pair_generations[2] = {0, 0};
    CHECK(f_install_pair_task_tracked(&pair_task, pair,
                    O_CLOEXEC_, pair_numbers, pair_generations) == 0 &&
            pair_numbers[0] == 0 && pair_numbers[1] == 1 &&
            pair_generations[0] == 1 && pair_generations[1] == 1 &&
            f_get_task(&pair_task, 0) == pair[0] &&
            f_get_task(&pair_task, 1) == pair[1] &&
            f_getfd_task(&pair_task, 0) == FD_CLOEXEC_ &&
            f_getfd_task(&pair_task, 1) == FD_CLOEXEC_,
            "双端在同一操作内取得互异最低槽位并发布标志");
    CHECK(f_close_task(&pair_task, 1) == 0 &&
            f_close_task(&pair_task, 0) == 0,
            "双端原子安装成功项可独立关闭");
    fdtable_release(pair_task.files);

    struct task one_slot_task;
    struct tgroup one_slot_group;
    init_task(&one_slot_task, &one_slot_group, 1, 1);
    CHECK(!IS_ERR(one_slot_task.files), "单槽回滚表创建成功");
    struct fd *rejected_pair[2] = {
        fd_create(&test_fd_ops),
        fd_create(&test_fd_ops),
    };
    CHECK(rejected_pair[0] != NULL && rejected_pair[1] != NULL,
            "单槽回滚对象创建成功");
    before_close = closed_fds;
    CHECK(f_install_pair_task_tracked(&one_slot_task, rejected_pair,
                    0, pair_numbers, pair_generations) == _EMFILE &&
            f_get_task(&one_slot_task, 0) == NULL &&
            closed_fds == before_close + 2,
            "第二槽不足时原子回滚首端并消费两端引用");
    fdtable_release(one_slot_task.files);

    current = NULL;
    fdtable_release(current_task.files);
    fdtable_release(target_task.files);
    fdtable_release(small_task.files);
    CHECK(closed_fds == 11, "清理阶段恰好释放全部十一个测试 fd");
    return 0;
}
