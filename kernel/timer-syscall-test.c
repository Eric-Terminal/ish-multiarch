#include <stdint.h>
#include <stdio.h>

#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/mm.h"
#include "kernel/task.h"
#include "kernel/time.h"

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
    task.sighand = &sighand;
    lock_init(&sighand.lock);
    lock_init(&task.waiting_cond_lock);
    cond_init(&task.pause);
    task_thread_store(&task, pthread_self());
    task_set_mm(&task, mm_new());
    CHECK(task.mm != NULL && map_user_page(&task) == 0,
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
    current = NULL;
    mm_release(task.mm);
    cond_destroy(&task.pause);
    return 0;
}
