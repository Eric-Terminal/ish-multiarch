#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

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

struct concurrent_reservation_call {
    struct task *task;
    atomic_uint *ready;
    atomic_bool *release;
    struct fd_reservation reservation;
    int result;
};

static void *run_concurrent_reservation(void *opaque) {
    struct concurrent_reservation_call *call = opaque;
    call->result = f_reserve_task(
            call->task, 2, &call->reservation);
    atomic_fetch_add_explicit(call->ready, 1, memory_order_release);
    const struct timespec interval = {.tv_nsec = 1000000};
    while (!atomic_load_explicit(call->release, memory_order_acquire))
        nanosleep(&interval, NULL);
    if (call->result == 0)
        f_reservation_cancel(&call->reservation);
    return NULL;
}

struct receive_writer_context {
    struct task *task;
    struct fd *nested;
    int result;
    fd_t number;
    fd_t nested_number;
    fd_t duplicate_result;
    bool published_early;
};

static int receive_number_writer(void *opaque, fd_t number) {
    struct receive_writer_context *context = opaque;
    context->number = number;
    context->published_early =
            f_get_task(context->task, number) != NULL;
    if (context->nested != NULL) {
        context->nested_number = f_install_task(
                context->task, context->nested, 0);
        context->nested = NULL;
        context->duplicate_result = f_dup2_task(
                context->task, context->nested_number, number);
    }
    return context->result;
}

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

    struct task reservation_task;
    struct tgroup reservation_group;
    init_task(&reservation_task, &reservation_group, 4, 1);
    CHECK(!IS_ERR(reservation_task.files), "单槽预留表创建成功");
    struct fd *reservation_occupant = fd_create(&test_fd_ops);
    CHECK(reservation_occupant != NULL &&
            f_install_task(&reservation_task,
                    reservation_occupant, 0) == 0,
            "单槽预留前占用零号描述符");
    struct fd_reservation single_reservation = {};
    CHECK(f_reserve_task(&reservation_task, 1,
                    &single_reservation) == 0 &&
            single_reservation.numbers[0] == 1,
            "单槽事务预留最低空槽");
    CHECK(f_get_task(&reservation_task, 1) == NULL &&
            f_getfd_task(&reservation_task, 1) == _EBADF &&
            f_close_task(&reservation_task, 1) == _EBADF,
            "预留槽对查询、标志读取与关闭保持不可见");
    struct fd *reservation_nested = fd_create(&test_fd_ops);
    CHECK(reservation_nested != NULL &&
            f_install_task(&reservation_task,
                    reservation_nested, 0) == 2,
            "预留期间的嵌套安装跳过预留槽");
    struct fd *reservation_published = fd_create(&test_fd_ops);
    struct fd *single_fds[FD_RESERVATION_MAX] = {
        reservation_published,
        NULL,
    };
    qword_t single_generations[FD_RESERVATION_MAX] = {0, 0};
    CHECK(reservation_published != NULL &&
            f_reservation_publish(&single_reservation, single_fds,
                    O_CLOEXEC_ | O_NONBLOCK_,
                    single_generations) == 0 &&
            single_reservation.table == NULL &&
            f_get_task(&reservation_task, 1) ==
                    reservation_published &&
            f_getfd_task(&reservation_task, 1) == FD_CLOEXEC_ &&
            (fd_getflags(reservation_published) & O_NONBLOCK_) != 0 &&
            single_generations[0] == 1,
            "单槽发布一次提交对象、代数与全部安装标志");
    CHECK(f_close_task(&reservation_task, 2) == 0 &&
            f_close_task(&reservation_task, 1) == 0 &&
            f_close_task(&reservation_task, 0) == 0,
            "单槽事务成功项可独立关闭");

    CHECK(f_reserve_task(&reservation_task, 1,
                    &single_reservation) == 0 &&
            single_reservation.numbers[0] == 0,
            "取消测试再次预留最低空槽");
    f_reservation_cancel(&single_reservation);
    f_reservation_cancel(&single_reservation);
    struct fd *reservation_replacement = fd_create(&test_fd_ops);
    CHECK(reservation_replacement != NULL &&
            f_install_task(&reservation_task,
                    reservation_replacement, 0) == 0,
            "取消幂等且最低槽可立即复用");

    fdtable_release(reservation_task.files);

    struct task concurrent_reservation_task;
    struct tgroup concurrent_reservation_group;
    init_task(&concurrent_reservation_task,
            &concurrent_reservation_group, 4, 1);
    CHECK(!IS_ERR(concurrent_reservation_task.files),
            "并发双槽预留表创建成功");
    atomic_uint reservation_ready = 0;
    atomic_bool release_reservations = false;
    struct concurrent_reservation_call reservation_calls[2] = {
        {
            .task = &concurrent_reservation_task,
            .ready = &reservation_ready,
            .release = &release_reservations,
            .result = _EIO,
        },
        {
            .task = &concurrent_reservation_task,
            .ready = &reservation_ready,
            .release = &release_reservations,
            .result = _EIO,
        },
    };
    pthread_t reservation_threads[2];
    CHECK(pthread_create(&reservation_threads[0], NULL,
                    run_concurrent_reservation,
                    &reservation_calls[0]) == 0 &&
            pthread_create(&reservation_threads[1], NULL,
                    run_concurrent_reservation,
                    &reservation_calls[1]) == 0,
            "两个并发双槽预留线程启动成功");
    const struct timespec reservation_interval = {.tv_nsec = 1000000};
    for (unsigned elapsed = 0;
            elapsed < 1000 && atomic_load_explicit(
                    &reservation_ready, memory_order_acquire) != 2;
            elapsed++)
        nanosleep(&reservation_interval, NULL);
    CHECK(atomic_load_explicit(
                    &reservation_ready, memory_order_acquire) == 2,
            "两个并发预留都在释放门闩前完成");
    fd_t concurrent_numbers[4] = {
        reservation_calls[0].reservation.numbers[0],
        reservation_calls[0].reservation.numbers[1],
        reservation_calls[1].reservation.numbers[0],
        reservation_calls[1].reservation.numbers[1],
    };
    bool numbers_seen[4] = {false, false, false, false};
    bool unique_numbers = reservation_calls[0].result == 0 &&
            reservation_calls[1].result == 0;
    for (unsigned index = 0; index < 4 && unique_numbers; index++) {
        fd_t number = concurrent_numbers[index];
        unique_numbers = number >= 0 && number < 4 &&
                !numbers_seen[number];
        if (unique_numbers)
            numbers_seen[number] = true;
    }
    CHECK(unique_numbers,
            "并发扩容中的两个事务取得四个互异槽位");
    atomic_store_explicit(
            &release_reservations, true, memory_order_release);
    CHECK(pthread_join(reservation_threads[0], NULL) == 0 &&
            pthread_join(reservation_threads[1], NULL) == 0,
            "两个并发预留线程完成取消");
    struct fd_reservation after_concurrent = {};
    CHECK(f_reserve_task(&concurrent_reservation_task, 2,
                    &after_concurrent) == 0 &&
            after_concurrent.numbers[0] == 0 &&
            after_concurrent.numbers[1] == 1,
            "并发取消后最低两个槽位可再次预留");
    f_reservation_cancel(&after_concurrent);
    fdtable_release(concurrent_reservation_task.files);

    struct task zero_limit_task;
    struct tgroup zero_limit_group;
    init_task(&zero_limit_task, &zero_limit_group, 0, 4);
    CHECK(!IS_ERR(zero_limit_task.files),
            "零上限预留表创建成功");
    struct fd_reservation zero_limit_reservation = {};
    CHECK(f_reserve_task(&zero_limit_task, 1,
                    &zero_limit_reservation) == _EMFILE &&
            zero_limit_reservation.table == NULL,
            "RLIMIT_NOFILE 为零时不预留表内空槽");
    fdtable_release(zero_limit_task.files);

    struct task double_reservation_task;
    struct tgroup double_reservation_group;
    init_task(&double_reservation_task,
            &double_reservation_group, 4, 1);
    CHECK(!IS_ERR(double_reservation_task.files),
            "双槽预留表创建成功");
    struct fd *double_occupant = fd_create(&test_fd_ops);
    CHECK(double_occupant != NULL &&
            f_install_task(&double_reservation_task,
                    double_occupant, 0) == 0,
            "双槽预留前占用零号描述符");
    struct fd_reservation double_reservation = {};
    CHECK(f_reserve_task(&double_reservation_task, 2,
                    &double_reservation) == 0 &&
            double_reservation.numbers[0] == 1 &&
            double_reservation.numbers[1] == 2,
            "双槽事务原子预留两个最低空槽");
    struct fd *double_nested = fd_create(&test_fd_ops);
    CHECK(double_nested != NULL &&
            f_install_task(&double_reservation_task,
                    double_nested, 0) == 3,
            "普通安装同时跳过双槽预留");
    struct fd *double_fds[FD_RESERVATION_MAX] = {
        fd_create(&test_fd_ops),
        fd_create(&test_fd_ops),
    };
    qword_t double_generations[FD_RESERVATION_MAX] = {0, 0};
    CHECK(double_fds[0] != NULL && double_fds[1] != NULL &&
            f_reservation_publish(&double_reservation, double_fds,
                    O_CLOEXEC_, double_generations) == 0 &&
            f_get_task(&double_reservation_task, 1) == double_fds[0] &&
            f_get_task(&double_reservation_task, 2) == double_fds[1] &&
            f_getfd_task(&double_reservation_task, 1) == FD_CLOEXEC_ &&
            f_getfd_task(&double_reservation_task, 2) == FD_CLOEXEC_ &&
            double_generations[0] == 1 &&
            double_generations[1] == 1,
            "双槽发布在同一事务中提交两个对象与标志");
    fdtable_release(double_reservation_task.files);

    struct task reservation_limit_task;
    struct tgroup reservation_limit_group;
    init_task(&reservation_limit_task,
            &reservation_limit_group, 1, 1);
    CHECK(!IS_ERR(reservation_limit_task.files),
            "预留回滚表创建成功");
    struct fd_reservation rejected_reservation = {};
    CHECK(f_reserve_task(&reservation_limit_task, 2,
                    &rejected_reservation) == _EMFILE &&
            rejected_reservation.table == NULL,
            "仅剩一槽时双槽预留整体失败");
    struct fd *reservation_limit_fd = fd_create(&test_fd_ops);
    CHECK(reservation_limit_fd != NULL &&
            f_install_task(&reservation_limit_task,
                    reservation_limit_fd, 0) == 0,
            "双槽预留失败不遗留首槽预留");
    fdtable_release(reservation_limit_task.files);

    struct task reservation_lifetime_task;
    struct tgroup reservation_lifetime_group;
    init_task(&reservation_lifetime_task,
            &reservation_lifetime_group, 1, 1);
    CHECK(!IS_ERR(reservation_lifetime_task.files),
            "生命周期预留表创建成功");
    struct fd_reservation lifetime_reservation = {};
    CHECK(f_reserve_task(&reservation_lifetime_task, 1,
                    &lifetime_reservation) == 0 &&
            atomic_load_explicit(
                    &reservation_lifetime_task.files->refcount,
                    memory_order_acquire) == 2,
            "预留事务独立持有原 fdtable 引用");
    struct fdtable *original_lifetime_table =
            reservation_lifetime_task.files;
    struct fdtable *replacement_lifetime_table = fdtable_new(1);
    CHECK(!IS_ERR(replacement_lifetime_table),
            "生命周期替代表创建成功");
    lock(&pids_lock);
    reservation_lifetime_task.files = replacement_lifetime_table;
    unlock(&pids_lock);
    fdtable_release(original_lifetime_table);
    struct fd *lifetime_fd = fd_create(&test_fd_ops);
    struct fd *lifetime_fds[FD_RESERVATION_MAX] = {
        lifetime_fd,
        NULL,
    };
    before_close = closed_fds;
    CHECK(lifetime_fd != NULL &&
            f_reservation_publish(&lifetime_reservation,
                    lifetime_fds, 0, NULL) == 0 &&
            lifetime_reservation.table == NULL &&
            closed_fds == before_close + 1,
            "任务换表后仍安全发布到原表并随最后引用释放");
    fdtable_release(reservation_lifetime_task.files);

    struct task receive_task;
    struct tgroup receive_group;
    init_task(&receive_task, &receive_group, 3, 1);
    CHECK(!IS_ERR(receive_task.files), "接收事务表创建成功");
    struct fd *received_fd = fd_create(&test_fd_ops);
    struct receive_writer_context receive_context = {
        .task = &receive_task,
        .nested = fd_create(&test_fd_ops),
        .result = 0,
        .number = -1,
        .nested_number = -1,
        .duplicate_result = -1,
    };
    CHECK(received_fd != NULL && receive_context.nested != NULL,
            "接收事务对象创建成功");
    CHECK(f_receive_task(&receive_task, received_fd,
                    O_CLOEXEC_ | O_NONBLOCK_,
                    receive_number_writer, &receive_context) == 0 &&
            receive_context.number == 0 &&
            !receive_context.published_early,
            "接收事务在锁外写号且写号前不发布对象");
    CHECK(receive_context.nested_number == 1 &&
            receive_context.duplicate_result == _EBUSY,
            "重入安装跳过预留槽且精确复制不得覆盖预留项");
    CHECK(f_get_task(&receive_task, 0) == received_fd &&
            f_getfd_task(&receive_task, 0) == FD_CLOEXEC_ &&
            (fd_getflags(received_fd) & O_NONBLOCK_) != 0,
            "写号成功后原子发布对象及接收标志");
    CHECK(f_close_task(&receive_task, 1) == 0 &&
            f_close_task(&receive_task, 0) == 0,
            "接收事务成功项可独立关闭");

    struct fd *rejected_receive = fd_create(&test_fd_ops);
    struct receive_writer_context rejected_context = {
        .task = &receive_task,
        .result = _EFAULT,
        .number = -1,
        .nested_number = -1,
        .duplicate_result = -1,
    };
    CHECK(rejected_receive != NULL, "接收失败对象创建成功");
    before_close = closed_fds;
    CHECK(f_receive_task(&receive_task, rejected_receive, 0,
                    receive_number_writer, &rejected_context) == _EFAULT &&
            rejected_context.number == 0 &&
            f_get_task(&receive_task, 0) == NULL &&
            closed_fds == before_close + 1,
            "写号失败取消预留并释放未发布对象");
    struct fd *replacement = fd_create(&test_fd_ops);
    CHECK(replacement != NULL &&
            f_install_task(&receive_task, replacement, 0) == 0,
            "取消预留后最低编号可立即复用");
    fdtable_release(receive_task.files);

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
    CHECK(closed_fds == 25, "清理阶段恰好释放全部二十五个测试 fd");
    return 0;
}
