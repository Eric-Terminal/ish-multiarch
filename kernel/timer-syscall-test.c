#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include "fs/fd.h"
#include "fs/poll.h"
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "kernel/mm.h"
#include "kernel/task.h"
#include "kernel/time.h"
#include "kernel/timerfd.h"

#define USER_PAGE UINT32_C(0x00100000)
#define INPUT_ADDRESS (USER_PAGE + 16)
#define OUTPUT_ADDRESS (USER_PAGE + 128)
#define TIMER_ID_ADDRESS (USER_PAGE + 256)

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "定时器系统调用测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        return 1; \
    } \
} while (0)

static int map_user_page(struct task *task) {
    write_wrlock(&task->mem->lock);
    int error = pt_map_nothing(task->mem, PAGE(USER_PAGE), 1, P_RWX);
    write_wrunlock(&task->mem->lock);
    return error;
}

static bool wait_for_timerfd_expiration(
        struct fd *fd, long timeout_milliseconds) {
    const struct timespec delay = {
        .tv_nsec = 1000000,
    };
    for (long elapsed = 0; elapsed < timeout_milliseconds; elapsed++) {
        lock(&fd->lock);
        bool expired = fd->timerfd.expirations != 0;
        unlock(&fd->lock);
        if (expired)
            return true;
        nanosleep(&delay, NULL);
    }
    return false;
}

int main(void) {
    struct task task = {0};
    struct tgroup group = {0};
    struct sighand sighand = {.refcount = 1};

    list_init(&group.threads);
    list_init(&group.session);
    list_init(&group.pgroup);
    lock_init(&group.lock);
    group.leader = &task;
    list_init(&task.group_links);
    list_add(&group.threads, &task.group_links);
    list_init(&task.children);
    list_init(&task.siblings);
    list_init(&task.queue);
    task.pid = task.tgid = 42;
    task.group = &group;
    group.limits[RLIMIT_NOFILE_] =
            (struct rlimit_) {16, 16};
    task.sighand = &sighand;
    lock_init(&sighand.lock);
    lock_init(&task.waiting_cond_lock);
    cond_init(&task.pause);
    task_thread_store(&task, pthread_self());
    task_set_mm(&task, mm_new());
    task.files = fdtable_new(4);
    CHECK(task.mm != NULL && !IS_ERR(task.files) &&
            map_user_page(&task) == 0,
            "创建并映射 i386 用户地址空间");
    current = &task;

    struct itimerval_ bad_timeval = {
        .value.usec = UINT32_C(1000000),
    };
    CHECK(user_put(INPUT_ADDRESS, bad_timeval) == 0 &&
            sys_setitimer(ITIMER_REAL_, INPUT_ADDRESS,
                    OUTPUT_ADDRESS) == _EINVAL &&
            group.itimer == NULL,
            "拒绝越界微秒且不创建 host 定时器");
    bad_timeval = (struct itimerval_) {
        .value.sec = UINT32_MAX,
    };
    CHECK(user_put(INPUT_ADDRESS, bad_timeval) == 0 &&
            sys_setitimer(ITIMER_REAL_, INPUT_ADDRESS,
                    OUTPUT_ADDRESS) == _EINVAL,
            "拒绝有符号 time_t 域中的负秒数");

    struct itimerval_ armed = {
        .interval.sec = 3,
        .value.sec = INT32_MAX,
    };
    CHECK(user_put(INPUT_ADDRESS, armed) == 0 &&
            sys_setitimer(ITIMER_REAL_, INPUT_ADDRESS,
                    OUTPUT_ADDRESS) == 0,
            "设置有符号 32 位最大相对定时器");
    struct itimerval_ old_value;
    CHECK(user_get(OUTPUT_ADDRESS, old_value) == 0 &&
            old_value.value.sec == 0 && old_value.value.usec == 0 &&
            old_value.interval.sec == 0 &&
            old_value.interval.usec == 0,
            "首次 setitimer 写回完整零旧值");

    struct itimerval_ disarmed = {
        .interval.sec = 7,
    };
    CHECK(user_put(INPUT_ADDRESS, disarmed) == 0 &&
            sys_setitimer(ITIMER_REAL_, INPUT_ADDRESS,
                    OUTPUT_ADDRESS) == 0 &&
            user_get(OUTPUT_ADDRESS, old_value) == 0 &&
            old_value.value.sec >= INT32_MAX - 1 &&
            old_value.interval.sec == 3,
            "停用 setitimer 时写回旧状态");
    CHECK(sys_setitimer(ITIMER_REAL_, INPUT_ADDRESS,
                    OUTPUT_ADDRESS) == 0 &&
            user_get(OUTPUT_ADDRESS, old_value) == 0 &&
            old_value.value.sec == 0 && old_value.value.usec == 0 &&
            old_value.interval.sec == 0 &&
            old_value.interval.usec == 0,
            "零 it_value 同时清除 setitimer 周期间隔");

    CHECK(sys_alarm(UINT32_MAX) == 0,
            "alarm 接受 unsigned int 最大输入并按 i386 上限设置");
    struct itimerval_ zero = {0};
    CHECK(user_put(INPUT_ADDRESS, zero) == 0 &&
            sys_setitimer(ITIMER_REAL_, INPUT_ADDRESS,
                    OUTPUT_ADDRESS) == 0 &&
            user_get(OUTPUT_ADDRESS, old_value) == 0 &&
            old_value.value.sec >= INT32_MAX - 1 &&
            (sdword_t) old_value.value.sec >= 0,
            "alarm 长时长写回仍是非负 i386 time_t");

    CHECK(sys_timer_create(CLOCK_REALTIME_COARSE_, 0,
                    TIMER_ID_ADDRESS) == _EINVAL &&
            sys_timerfd_create(CLOCK_REALTIME_COARSE_, 0) == _EINVAL &&
            sys_timerfd_create(CLOCK_REALTIME_, 1) == _EINVAL,
            "定时器 API 拒绝粗略时钟与未知 timerfd flags");

    struct task target = {
        .group = &group,
        .files = fdtable_new(2),
    };
    CHECK(!IS_ERR(target.files), "创建显式 timerfd 目标描述符表");
    current = &task;
    fd_t target_timer = timerfd_create_task(
            &target, CLOCK_MONOTONIC_,
            O_NONBLOCK_ | O_CLOEXEC_);
    CHECK(target_timer == 0 &&
            f_get_task(&task, 0) == NULL &&
            f_getfl_task(&target, target_timer) ==
                    (O_RDWR_ | O_NONBLOCK_) &&
            f_getfd_task(&target, target_timer) == FD_CLOEXEC_,
            "timerfd 仅安装进显式任务并分离状态与描述符 flag");
    CHECK(f_close_task(&target, target_timer) == 0,
            "关闭显式任务中的 timerfd");
    fdtable_release(target.files);

    fd_t timerfd = sys_timerfd_create(
            CLOCK_MONOTONIC_, O_NONBLOCK_ | O_CLOEXEC_);
    CHECK(timerfd == 0 &&
            f_getfl_task(&task, timerfd) ==
                    (O_RDWR_ | O_NONBLOCK_) &&
            f_getfd_task(&task, timerfd) == FD_CLOEXEC_,
            "i386 timerfd 包装保留 O_RDWR、NONBLOCK 与 CLOEXEC");
    struct fd *timerfd_object = f_get_task(&task, timerfd);
    CHECK(timerfd_object != NULL &&
            timerfd_object->timerfd.clockid == CLOCK_MONOTONIC,
            "timerfd 保存所选宿主时钟");

    struct itimerspec_ invalid_timerfd = {
        .value.nsec = UINT32_C(1000000000),
    };
    CHECK(sys_timerfd_settime(
                    15, 2, USER_PAGE + PAGE_SIZE, 0) == _EFAULT &&
            user_put(INPUT_ADDRESS, invalid_timerfd) == 0 &&
            sys_timerfd_settime(
                    15, 0, INPUT_ADDRESS, 0) == _EINVAL,
            "timerfd_settime 保持 copyin、参数校验与 fd 查找的错误顺序");
    invalid_timerfd = (struct itimerspec_) {
        .value.sec = UINT32_MAX,
    };
    CHECK(user_put(INPUT_ADDRESS, invalid_timerfd) == 0 &&
            sys_timerfd_settime(
                    timerfd, 0, INPUT_ADDRESS, 0) == _EINVAL,
            "i386 timerfd_settime 拒绝负的 32 位秒数");

    struct itimerspec_ periodic_timerfd = {
        .interval.nsec = 50000000,
        .value.nsec = 1000000,
    };
    CHECK(user_put(INPUT_ADDRESS, periodic_timerfd) == 0 &&
            sys_timerfd_settime(
                    timerfd, 0, INPUT_ADDRESS, OUTPUT_ADDRESS) == 0,
            "设置 timerfd 周期与首次到期");
    struct itimerspec_ previous_timerfd;
    CHECK(user_get(OUTPUT_ADDRESS, previous_timerfd) == 0 &&
            previous_timerfd.value.sec == 0 &&
            previous_timerfd.value.nsec == 0 &&
            previous_timerfd.interval.sec == 0 &&
            previous_timerfd.interval.nsec == 0,
            "首次 timerfd_settime 返回停用旧状态");
    CHECK(wait_for_timerfd_expiration(timerfd_object, 1000),
            "timerfd 首次到期唤醒");
    const struct timespec accumulate = {
        .tv_nsec = 120000000,
    };
    nanosleep(&accumulate, NULL);
    lock(&timerfd_object->lock);
    uint64_t callbacks_before_refresh =
            timerfd_object->timerfd.expirations;
    bool stopped_after_first =
            timerfd_object->timerfd.expired;
    unlock(&timerfd_object->lock);
    CHECK(callbacks_before_refresh == 1 && stopped_after_first,
            "无人读取时周期 timerfd 在首次回调后保持停止");

    CHECK(sys_timerfd_gettime(timerfd, OUTPUT_ADDRESS) == 0,
            "timerfd_gettime 批量刷新周期并重臂");
    struct itimerspec_ current_timerfd;
    CHECK(user_get(OUTPUT_ADDRESS, current_timerfd) == 0 &&
            current_timerfd.interval.sec == 0 &&
            current_timerfd.interval.nsec == 50000000 &&
            (current_timerfd.value.sec != 0 ||
                    current_timerfd.value.nsec != 0),
            "timerfd_gettime 返回周期与下一次相对到期");
    uint64_t expirations = 0;
    CHECK(file_read_task(&task, timerfd,
                    &expirations, sizeof(expirations)) ==
                    (ssize_t) sizeof(expirations) &&
            expirations >= 3,
            "周期 timerfd 一次读取包含暂停期间全部过期次数");

    struct itimerspec_ disarmed_timerfd = {
        .interval.sec = 7,
    };
    CHECK(user_put(INPUT_ADDRESS, disarmed_timerfd) == 0 &&
            sys_timerfd_settime(
                    timerfd, 0, INPUT_ADDRESS, OUTPUT_ADDRESS) == 0 &&
            file_read_task(&task, timerfd,
                    &expirations, sizeof(expirations)) == _EAGAIN &&
            sys_timerfd_gettime(timerfd, OUTPUT_ADDRESS) == 0 &&
            user_get(OUTPUT_ADDRESS, current_timerfd) == 0 &&
            current_timerfd.value.sec == 0 &&
            current_timerfd.value.nsec == 0 &&
            current_timerfd.interval.sec == 7 &&
            current_timerfd.interval.nsec == 0,
            "停用 timerfd 清空计数但保留新 interval");

    struct timer_spec huge_timerfd = {
        .value.sec = INT64_MAX,
    };
    struct timer_spec old_timerfd;
    CHECK(timerfd_settime_task(&task, timerfd, 0,
                    &huge_timerfd, &old_timerfd) == 0,
            "共享核心接受并饱和 AArch64 宽时间");
    lock(&timerfd_object->lock);
    struct timer_time saturated_next =
            timerfd_object->timerfd.next;
    unlock(&timerfd_object->lock);
    CHECK(saturated_next.sec == INT64_C(9223372036) &&
            saturated_next.nsec == INT64_C(854775807),
            "timerfd 绝对截止点饱和到 Linux KTIME_MAX");
    CHECK(timerfd_settime_task(&task, timerfd, 0,
                    &(struct timer_spec) {}, &old_timerfd) == 0 &&
            f_close_task(&task, timerfd) == 0,
            "停用并关闭 i386 timerfd");

    CHECK(sys_timer_create(CLOCK_REALTIME_, 0,
                    TIMER_ID_ADDRESS) == 0,
            "timer_create 的空 sigevent 使用 Linux 默认事件");
    dword_t timer_id;
    CHECK(user_get(TIMER_ID_ADDRESS, timer_id) == 0 &&
            timer_id < TIMERS_MAX &&
            group.posix_timers[timer_id].signal == SIGALRM_ &&
            group.posix_timers[timer_id].sig_value.sv_int ==
                    (int_t) timer_id &&
            group.posix_timers[timer_id].tgroup == &group,
            "默认事件携带 SIGALRM 与新 timer id");
    CHECK(sys_timer_delete(timer_id) == 0,
            "删除默认 POSIX 定时器");

    struct sigevent_ bad_event = {
        .signo = NUM_SIGS + 1,
        .method = 0,
    };
    CHECK(user_put(INPUT_ADDRESS, bad_event) == 0 &&
            sys_timer_create(CLOCK_REALTIME_, INPUT_ADDRESS,
                    TIMER_ID_ADDRESS) == _EINVAL,
            "拒绝会越界信号表的 POSIX 定时器事件");

    dword_t timer_ids[TIMERS_MAX];
    for (unsigned index = 0; index < TIMERS_MAX; index++) {
        CHECK(sys_timer_create(CLOCK_MONOTONIC_, 0,
                        TIMER_ID_ADDRESS) == 0 &&
                user_get(TIMER_ID_ADDRESS, timer_ids[index]) == 0,
                "分配 POSIX 定时器槽位");
    }
    CHECK(sys_timer_create(CLOCK_MONOTONIC_, 0,
                    TIMER_ID_ADDRESS) == _EAGAIN,
            "POSIX 定时器槽位耗尽返回 EAGAIN");
    for (unsigned index = 0; index < TIMERS_MAX; index++)
        CHECK(sys_timer_delete(timer_ids[index]) == 0,
                "回收 POSIX 定时器槽位");

    CHECK(sys_timer_create(CLOCK_MONOTONIC_, 0,
                    TIMER_ID_ADDRESS) == 0 &&
            user_get(TIMER_ID_ADDRESS, timer_id) == 0,
            "创建 timer_settime 语义用定时器");
    struct itimerspec_ bad_timespec = {
        .value.nsec = UINT32_C(1000000000),
    };
    CHECK(user_put(INPUT_ADDRESS, bad_timespec) == 0 &&
            sys_timer_settime(timer_id, 0, INPUT_ADDRESS,
                    OUTPUT_ADDRESS) == _EINVAL,
            "timer_settime 拒绝越界纳秒");
    struct itimerspec_ posix_armed = {
        .interval.sec = 5,
        .value.sec = INT32_MAX,
    };
    CHECK(user_put(INPUT_ADDRESS, posix_armed) == 0 &&
            sys_timer_settime(timer_id, 0, INPUT_ADDRESS,
                    OUTPUT_ADDRESS) == 0,
            "设置 POSIX 周期定时器");
    struct itimerspec_ posix_disarmed = {
        .interval.sec = 9,
    };
    struct itimerspec_ old_posix;
    CHECK(user_put(INPUT_ADDRESS, posix_disarmed) == 0 &&
            sys_timer_settime(timer_id, 0, INPUT_ADDRESS,
                    OUTPUT_ADDRESS) == 0 &&
            user_get(OUTPUT_ADDRESS, old_posix) == 0 &&
            old_posix.interval.sec == 5,
            "停用 POSIX 定时器时写回旧周期间隔");
    CHECK(sys_timer_settime(timer_id, 0, INPUT_ADDRESS,
                    OUTPUT_ADDRESS) == 0 &&
            user_get(OUTPUT_ADDRESS, old_posix) == 0 &&
            old_posix.value.sec == 0 && old_posix.value.nsec == 0 &&
            old_posix.interval.sec == 0 &&
            old_posix.interval.nsec == 0,
            "零 it_value 同时清除 POSIX 周期间隔");
    CHECK(sys_timer_delete(timer_id) == 0,
            "删除 timer_settime 语义用定时器");

    tgroup_timers_destroy(&group);
    fdtable_release(task.files);
    current = NULL;
    mm_release(task.mm);
    cond_destroy(&task.pause);
    return 0;
}
