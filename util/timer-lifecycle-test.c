#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "util/timer.h"
#include "kernel/task.h"
#include "kernel/time.h"

_Static_assert(sizeof(((struct timer *) 0)->end.sec) == sizeof(int64_t),
        "定时器截止时间必须使用 64 位秒数");

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "定时器生命周期测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        return 1; \
    } \
} while (0)

struct event_state {
    lock_t lock;
    cond_t cond;
};

static void event_state_init(struct event_state *state) {
    lock_init(&state->lock);
    cond_init(&state->cond);
}

static void event_state_destroy(struct event_state *state) {
    cond_destroy(&state->cond);
}

static struct timespec milliseconds(long value) {
    return (struct timespec) {
        .tv_sec = value / 1000,
        .tv_nsec = value % 1000 * 1000000,
    };
}

static bool wait_for_flag(struct event_state *state, const bool *flag,
        long timeout_ms) {
    struct timespec deadline = timespec_add(
            timespec_now(CLOCK_MONOTONIC), milliseconds(timeout_ms));
    lock(&state->lock);
    while (!*flag) {
        struct timespec remaining = timespec_subtract(
                deadline, timespec_now(CLOCK_MONOTONIC));
        if (!timespec_positive(remaining))
            break;
        if (wait_for_ignore_signals(
                    &state->cond, &state->lock, &remaining) != 0)
            break;
    }
    bool reached = *flag;
    unlock(&state->lock);
    return reached;
}

static void wait_for_flag_locked(
        struct event_state *state, const bool *flag) {
    while (!*flag)
        wait_for_ignore_signals(&state->cond, &state->lock, NULL);
}

static struct timer_spec one_shot(long nanoseconds) {
    return (struct timer_spec) {
        .value = {.nsec = nanoseconds},
    };
}

struct self_rearm_state {
    struct event_state event;
    struct timer *timer;
    bool callback_done;
    int set_result;
    unsigned callback_count;
};

static void self_rearm_callback(void *opaque) {
    struct self_rearm_state *state = opaque;
    int result = timer_set(state->timer, (struct timer_spec) {}, NULL);

    lock(&state->event.lock);
    state->set_result = result;
    state->callback_count++;
    state->callback_done = true;
    notify(&state->event.cond);
    unlock(&state->event.lock);
}

static int test_callback_can_rearm(void) {
    struct self_rearm_state state = {};
    event_state_init(&state.event);
    state.timer = timer_new(
            CLOCK_MONOTONIC, self_rearm_callback, &state);
    CHECK(state.timer != NULL, "创建回调内重设用定时器");
    struct timer_spec old_spec = {
        .value = {.sec = 1},
        .interval = {.sec = 1},
    };
    CHECK(timer_set(state.timer, one_shot(1000000), &old_spec) == 0 &&
            timer_time_is_zero(old_spec.value) &&
            timer_time_is_zero(old_spec.interval),
            "首次设置返回完整初始化的停用旧状态");
    CHECK(wait_for_flag(&state.event, &state.callback_done, 1000),
            "回调内 timer_set 不应被定时器锁阻塞");

    timer_free(state.timer);
    CHECK(state.set_result == 0 && state.callback_count == 1,
            "回调内成功停用自身且只执行一次");
    event_state_destroy(&state.event);
    return 0;
}

struct inversion_state {
    struct event_state event;
    lock_t outer_lock;
    struct timer *timer;
    bool outer_locked;
    bool callback_entered;
    bool callback_finished;
    bool callback_current;
    bool setter_finished;
    int set_result;
};

static void inversion_callback(void *opaque) {
    struct inversion_state *state = opaque;

    lock(&state->event.lock);
    state->callback_entered = true;
    notify(&state->event.cond);
    unlock(&state->event.lock);

    lock(&state->outer_lock);
    bool callback_current = timer_callback_is_current(state->timer);
    unlock(&state->outer_lock);

    lock(&state->event.lock);
    state->callback_current = callback_current;
    state->callback_finished = true;
    notify(&state->event.cond);
    unlock(&state->event.lock);
}

static void *inversion_setter(void *opaque) {
    struct inversion_state *state = opaque;

    lock(&state->outer_lock);
    lock(&state->event.lock);
    state->outer_locked = true;
    notify(&state->event.cond);
    wait_for_flag_locked(&state->event, &state->callback_entered);
    unlock(&state->event.lock);

    int result = timer_set(state->timer, (struct timer_spec) {}, NULL);
    unlock(&state->outer_lock);

    lock(&state->event.lock);
    state->set_result = result;
    state->setter_finished = true;
    notify(&state->event.cond);
    unlock(&state->event.lock);
    return NULL;
}

static int test_external_lock_inversion(void) {
    struct inversion_state state = {};
    event_state_init(&state.event);
    lock_init(&state.outer_lock);
    state.timer = timer_new(CLOCK_MONOTONIC, inversion_callback, &state);
    CHECK(state.timer != NULL, "创建外部锁反转用定时器");

    pthread_t setter;
    CHECK(pthread_create(&setter, NULL, inversion_setter, &state) == 0,
            "创建持外部锁的重设线程");
    CHECK(wait_for_flag(&state.event, &state.outer_locked, 1000),
            "重设线程取得外部锁");
    CHECK(timer_set(state.timer, one_shot(1000000), NULL) == 0,
            "启动外部锁反转用定时器");
    CHECK(wait_for_flag(&state.event, &state.setter_finished, 1000),
            "timer_set 与等待外部锁的回调不形成锁反转");
    CHECK(wait_for_flag(&state.event, &state.callback_finished, 1000),
            "释放外部锁后回调正常结束");
    CHECK(pthread_join(setter, NULL) == 0, "回收外部锁重设线程");

    timer_free(state.timer);
    CHECK(state.set_result == 0 && !state.callback_current,
            "持外部锁重设定时器成功且淘汰已线性化的旧回调");
    event_state_destroy(&state.event);
    return 0;
}

struct free_state {
    struct event_state event;
    struct timer *timer;
    bool callback_entered;
    bool allow_callback_exit;
    bool callback_exited;
    bool free_started;
    bool free_returned;
    bool data_retired;
    bool access_after_retire;
    unsigned callback_count;
    unsigned callback_count_at_free_return;
    unsigned sequence;
    unsigned callback_exit_order;
    unsigned free_return_order;
};

static void free_lifecycle_callback(void *opaque) {
    struct free_state *state = opaque;

    lock(&state->event.lock);
    state->callback_count++;
    if (state->data_retired)
        state->access_after_retire = true;
    if (state->callback_count == 1) {
        state->callback_entered = true;
        notify(&state->event.cond);
        wait_for_flag_locked(&state->event, &state->allow_callback_exit);
        if (state->data_retired)
            state->access_after_retire = true;
        state->callback_exit_order = ++state->sequence;
        state->callback_exited = true;
    }
    notify(&state->event.cond);
    unlock(&state->event.lock);
}

static void *free_timer_thread(void *opaque) {
    struct free_state *state = opaque;

    lock(&state->event.lock);
    state->free_started = true;
    notify(&state->event.cond);
    unlock(&state->event.lock);

    timer_free(state->timer);

    lock(&state->event.lock);
    state->data_retired = true;
    state->callback_count_at_free_return = state->callback_count;
    state->free_return_order = ++state->sequence;
    state->free_returned = true;
    notify(&state->event.cond);
    unlock(&state->event.lock);
    return NULL;
}

static int test_free_waits_for_callback(void) {
    struct free_state state = {};
    event_state_init(&state.event);
    state.timer = timer_new(
            CLOCK_MONOTONIC, free_lifecycle_callback, &state);
    CHECK(state.timer != NULL, "创建释放生命周期用定时器");
    const struct timer_spec periodic = {
        .value = {.nsec = 1000000},
        .interval = {.nsec = 10000000},
    };
    CHECK(timer_set(state.timer, periodic, NULL) == 0,
            "启动释放生命周期用周期定时器");
    CHECK(wait_for_flag(&state.event, &state.callback_entered, 1000),
            "周期定时器进入首个回调");

    pthread_t freer;
    CHECK(pthread_create(&freer, NULL, free_timer_thread, &state) == 0,
            "创建定时器释放线程");
    CHECK(wait_for_flag(&state.event, &state.free_started, 1000),
            "释放线程开始调用 timer_free");
    bool returned_while_callback_blocked =
            wait_for_flag(&state.event, &state.free_returned, 100);

    lock(&state.event.lock);
    state.allow_callback_exit = true;
    notify(&state.event.cond);
    unlock(&state.event.lock);

    CHECK(wait_for_flag(&state.event, &state.callback_exited, 1000),
            "放行后回调正常结束");
    CHECK(wait_for_flag(&state.event, &state.free_returned, 1000),
            "回调结束后 timer_free 返回");
    CHECK(pthread_join(freer, NULL) == 0, "回收定时器释放线程");

    lock(&state.event.lock);
    unsigned callback_exit_order = state.callback_exit_order;
    unsigned free_return_order = state.free_return_order;
    unsigned callback_count_at_free_return =
            state.callback_count_at_free_return;
    unlock(&state.event.lock);

    bool accessed_late =
            wait_for_flag(&state.event, &state.access_after_retire, 100);
    lock(&state.event.lock);
    unsigned callback_count = state.callback_count;
    unlock(&state.event.lock);

    CHECK(!returned_while_callback_blocked,
            "timer_free 在正在执行的回调结束前不得返回");
    CHECK(callback_exit_order != 0 &&
                    free_return_order > callback_exit_order,
            "timer_free 返回顺序晚于回调结束顺序");
    CHECK(!accessed_late &&
                    callback_count == callback_count_at_free_return,
            "timer_free 返回后 data 不再被回调访问");
    event_state_destroy(&state.event);
    return 0;
}

struct generation_state {
    struct event_state event;
    struct timer *timer;
    bool first_callback_entered;
    bool allow_first_callback_exit;
    bool setter_finished;
    bool second_callback_entered;
    int set_result;
    unsigned callback_count;
    struct timespec rearm_started;
    struct timespec second_callback_time;
};

static void generation_callback(void *opaque) {
    struct generation_state *state = opaque;

    lock(&state->event.lock);
    state->callback_count++;
    if (state->callback_count == 1) {
        state->first_callback_entered = true;
        notify(&state->event.cond);
        wait_for_flag_locked(
                &state->event, &state->allow_first_callback_exit);
    } else if (state->callback_count == 2) {
        state->second_callback_time = timespec_now(CLOCK_MONOTONIC);
        state->second_callback_entered = true;
        notify(&state->event.cond);
    }
    unlock(&state->event.lock);
}

static void *generation_setter(void *opaque) {
    struct generation_state *state = opaque;
    struct timespec started = timespec_now(CLOCK_MONOTONIC);
    int result = timer_set(state->timer, one_shot(300000000), NULL);

    lock(&state->event.lock);
    state->rearm_started = started;
    state->set_result = result;
    state->setter_finished = true;
    notify(&state->event.cond);
    unlock(&state->event.lock);
    return NULL;
}

static int test_periodic_rearm_generation(void) {
    struct generation_state state = {};
    event_state_init(&state.event);
    state.timer = timer_new(CLOCK_MONOTONIC, generation_callback, &state);
    CHECK(state.timer != NULL, "创建周期重设用定时器");
    const struct timer_spec periodic = {
        .value = {.nsec = 1000000},
        .interval = {.nsec = 5000000},
    };
    CHECK(timer_set(state.timer, periodic, NULL) == 0,
            "启动周期重设用定时器");
    CHECK(wait_for_flag(
                &state.event, &state.first_callback_entered, 1000),
            "周期定时器进入首个回调");

    pthread_t setter;
    CHECK(pthread_create(&setter, NULL, generation_setter, &state) == 0,
            "创建周期定时器重设线程");
    CHECK(wait_for_flag(&state.event, &state.setter_finished, 1000),
            "首个回调尚未结束时 timer_set 完成重设");
    CHECK(pthread_join(setter, NULL) == 0, "回收周期定时器重设线程");

    lock(&state.event.lock);
    state.allow_first_callback_exit = true;
    notify(&state.event.cond);
    unlock(&state.event.lock);

    bool second_callback_arrived = wait_for_flag(
            &state.event, &state.second_callback_entered, 2000);
    timer_free(state.timer);

    lock(&state.event.lock);
    int set_result = state.set_result;
    unsigned callback_count = state.callback_count;
    struct timespec elapsed = timespec_subtract(
            state.second_callback_time, state.rearm_started);
    unlock(&state.event.lock);

    CHECK(set_result == 0, "首个回调执行期间重设周期定时器成功");
    CHECK(second_callback_arrived,
            "重设得到的新单次到期没有被旧周期收尾丢弃");
    CHECK(elapsed.tv_sec > 0 || elapsed.tv_nsec >= 200000000,
            "旧周期不得覆盖重设后的较晚到期时间");
    CHECK(callback_count == 2,
            "周期重设后仅执行首个旧回调与一个新回调");
    event_state_destroy(&state.event);
    return 0;
}

struct wide_deadline_state {
    atomic_uint callback_count;
};

static void wide_deadline_callback(void *opaque) {
    struct wide_deadline_state *state = opaque;
    atomic_fetch_add(&state->callback_count, 1);
}

static int test_deadline_crosses_32_bit_time(void) {
    struct wide_deadline_state state = {0};
    atomic_init(&state.callback_count, 0);
    struct timer *timer = timer_new(
            CLOCK_MONOTONIC, wide_deadline_callback, &state);
    CHECK(timer != NULL, "创建 32 位时间边界用定时器");

    struct timer_spec delayed = {
        .value.sec = UINT32_MAX,
    };
    CHECK(timer_set(timer, delayed, NULL) == 0,
            "启动跨越 32 位 time_t 上限的定时器");

    lock(&timer->lock);
    int64_t deadline_sec = timer->end.sec;
    unlock(&timer->lock);
    struct timer_spec old_spec;
    CHECK(timer_set(timer, (struct timer_spec) {}, &old_spec) == 0,
            "跨界定时器可被同步停用");
    timer_free(timer);

    CHECK(deadline_sec > INT32_MAX &&
            old_spec.value.sec > INT32_MAX &&
            timer_time_positive(old_spec.value) &&
            atomic_load(&state.callback_count) == 0,
            "64 位截止时间与时长不会在 arm64_32 上回绕并提前触发");
    return 0;
}

static int test_timer_time_saturates_at_64_bit_limits(void) {
    struct timer_time maximum = timer_time_add(
            (struct timer_time) {INT64_MAX, 999999999},
            (struct timer_time) {.nsec = 1});
    struct timer_time minimum = timer_time_subtract(
            (struct timer_time) {INT64_MIN, 0},
            (struct timer_time) {.nsec = 1});
    CHECK(maximum.sec == INT64_MAX && maximum.nsec == 999999999 &&
            minimum.sec == INT64_MIN && minimum.nsec == 0,
            "64 位时间极值使用饱和算术而不触发有符号溢出");
    return 0;
}

static int test_absolute_deadline_is_not_rebased(void) {
    struct wide_deadline_state state = {0};
    atomic_init(&state.callback_count, 0);
    struct timer *timer = timer_new(
            CLOCK_MONOTONIC, wide_deadline_callback, &state);
    CHECK(timer != NULL, "创建绝对截止时间用定时器");

    struct timer_time deadline = timer_time_add(
            timer_time_from_timespec(timespec_now(CLOCK_MONOTONIC)),
            (struct timer_time) {.sec = 10});
    struct timer_spec spec = {.value = deadline};
    CHECK(timer_set_absolute(timer, spec, NULL) == 0,
            "设置绝对截止时间");
    lock(&timer->lock);
    struct timer_time stored_deadline = timer->end;
    unlock(&timer->lock);
    CHECK(timer_set(timer, (struct timer_spec) {}, NULL) == 0,
            "停用绝对截止时间用定时器");
    timer_free(timer);

    CHECK(stored_deadline.sec == deadline.sec &&
            stored_deadline.nsec == deadline.nsec &&
            atomic_load(&state.callback_count) == 0,
            "绝对截止时间只采样一次且不被重新平移");
    return 0;
}

static int test_deadline_slice_preserves_wide_absolute_time(void) {
    const struct timer_time started = {
        .sec = 100,
        .nsec = 900000000,
    };
    const struct timer_time duration = {
        .sec = (int64_t) INT32_MAX + 2,
        .nsec = 500000000,
    };
    const struct timer_time deadline = timer_time_add(started, duration);
    struct timespec slice;
    CHECK(timer_time_deadline_slice(deadline, started, &slice) &&
            slice.tv_sec == INT32_MAX && slice.tv_nsec == 0,
            "超长 64 位期限的首个宿主分片不超过 32 位 time_t 上限");

    const struct timer_time after_first_slice = timer_time_add(
            started, (struct timer_time) {.sec = INT32_MAX});
    CHECK(timer_time_deadline_slice(
                    deadline, after_first_slice, &slice) &&
            slice.tv_sec == 2 && slice.tv_nsec == 500000000,
            "宿主分片结束后继续使用原绝对截止时间计算剩余时长");
    CHECK(!timer_time_deadline_slice(deadline, deadline, &slice),
            "达到绝对截止时间后不再创建等待分片");
    return 0;
}

struct group_cleanup_state {
    atomic_uint callback_count;
};

static void group_cleanup_callback(void *opaque) {
    struct group_cleanup_state *state = opaque;
    atomic_fetch_add(&state->callback_count, 1);
}

static bool group_posix_timers_empty(const struct tgroup *group) {
    for (unsigned i = 0; i < TIMERS_MAX; i++)
        if (group->posix_timers[i].timer != NULL ||
                group->posix_timers[i].deleting)
            return false;
    return true;
}

static int test_exec_cleanup_retires_only_posix_timers(void) {
    struct tgroup group = {0};
    lock_init(&group.lock);
    struct group_cleanup_state state = {0};
    atomic_init(&state.callback_count, 0);

    group.itimer = timer_new(
            CLOCK_MONOTONIC, group_cleanup_callback, &state);
    group.posix_timers[0].timer = timer_new(
            CLOCK_MONOTONIC, group_cleanup_callback, &state);
    group.posix_timers[TIMERS_MAX - 1].timer = timer_new(
            CLOCK_MONOTONIC, group_cleanup_callback, &state);
    CHECK(group.itimer != NULL &&
            group.posix_timers[0].timer != NULL &&
            group.posix_timers[TIMERS_MAX - 1].timer != NULL,
            "创建 exec 清理用定时器");

    const struct timer_spec delayed = {
        .value = {.sec = 2},
    };
    CHECK(timer_set(group.itimer, delayed, NULL) == 0 &&
            timer_set(group.posix_timers[0].timer, delayed, NULL) == 0 &&
            timer_set(group.posix_timers[TIMERS_MAX - 1].timer,
                    delayed, NULL) == 0,
            "启动 exec 清理用定时器");

    struct timer *preserved_itimer = group.itimer;
    tgroup_exec_posix_timers_destroy(&group);
    CHECK(group.itimer == preserved_itimer &&
            group_posix_timers_empty(&group) &&
            timer_set(group.itimer, (struct timer_spec) {}, NULL) == 0 &&
            atomic_load(&state.callback_count) == 0,
            "exec 清理仅同步退休 POSIX timers 并保留 ITIMER_REAL");

    tgroup_exec_posix_timers_destroy(&group);
    CHECK(group.itimer == preserved_itimer,
            "重复 exec 清理不影响保留的 ITIMER_REAL");
    tgroup_timers_destroy(&group);
    return 0;
}

static int test_group_cleanup_retires_all_timers(void) {
    struct tgroup group = {0};
    lock_init(&group.lock);
    struct group_cleanup_state state = {0};
    atomic_init(&state.callback_count, 0);

    group.itimer = timer_new(
            CLOCK_MONOTONIC, group_cleanup_callback, &state);
    group.posix_timers[0].timer = timer_new(
            CLOCK_MONOTONIC, group_cleanup_callback, &state);
    group.posix_timers[TIMERS_MAX - 1].timer = timer_new(
            CLOCK_MONOTONIC, group_cleanup_callback, &state);
    CHECK(group.itimer != NULL &&
            group.posix_timers[0].timer != NULL &&
            group.posix_timers[TIMERS_MAX - 1].timer != NULL,
            "创建线程组清理用定时器");

    const struct timer_spec delayed = {
        .value = {.sec = 2},
    };
    CHECK(timer_set(group.itimer, delayed, NULL) == 0 &&
            timer_set(group.posix_timers[0].timer, delayed, NULL) == 0 &&
            timer_set(group.posix_timers[TIMERS_MAX - 1].timer,
                    delayed, NULL) == 0,
            "启动线程组清理用定时器");

    tgroup_timers_destroy(&group);
    CHECK(group.itimer == NULL &&
            group.posix_timers[0].timer == NULL &&
            !group.posix_timers[0].deleting &&
            group.posix_timers[TIMERS_MAX - 1].timer == NULL &&
            !group.posix_timers[TIMERS_MAX - 1].deleting &&
            atomic_load(&state.callback_count) == 0,
            "线程组清理同步退休 itimer 与全部 POSIX timer");
    tgroup_timers_destroy(&group);
    return 0;
}

typedef int (*isolated_test_fn)(void);

static void host_wakeup_handler(int signal_number) {
    (void) signal_number;
}

// 每个锁场景独立运行，避免死锁或后台线程影响后续断言。
static bool run_isolated(const char *name, isolated_test_fn test) {
    pid_t child = fork();
    if (child < 0) {
        fprintf(stderr, "定时器生命周期测试无法 fork：%s\n",
                strerror(errno));
        return false;
    }
    if (child == 0) {
        struct sigaction wakeup = {.sa_handler = host_wakeup_handler};
        sigemptyset(&wakeup.sa_mask);
        if (sigaction(SIGUSR1, &wakeup, NULL) != 0)
            _exit(2);
        alarm(6);
        _exit(test() == 0 ? 0 : 1);
    }

    int status;
    pid_t waited;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited < 0) {
        fprintf(stderr, "定时器生命周期测试等待 %s 失败：%s\n",
                name, strerror(errno));
        return false;
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        return true;
    if (WIFSIGNALED(status)) {
        fprintf(stderr, "定时器生命周期测试失败：%s 被 host 信号 %d 终止\n",
                name, WTERMSIG(status));
    } else {
        fprintf(stderr, "定时器生命周期测试失败：%s 返回状态 %d\n",
                name, WEXITSTATUS(status));
    }
    return false;
}

int main(void) {
    unsigned failures = 0;
    failures += !run_isolated(
            "回调内重设", test_callback_can_rearm);
    failures += !run_isolated(
            "外部锁与重设", test_external_lock_inversion);
    failures += !run_isolated(
            "释放等待回调", test_free_waits_for_callback);
    failures += !run_isolated(
            "周期重设代际", test_periodic_rearm_generation);
    failures += !run_isolated(
            "32 位时间边界", test_deadline_crosses_32_bit_time);
    failures += !run_isolated(
            "64 位时间极值", test_timer_time_saturates_at_64_bit_limits);
    failures += !run_isolated(
            "绝对截止时间", test_absolute_deadline_is_not_rebased);
    failures += !run_isolated(
            "宽期限等待分片",
            test_deadline_slice_preserves_wide_absolute_time);
    failures += !run_isolated(
            "exec 定时器清理",
            test_exec_cleanup_retires_only_posix_timers);
    failures += !run_isolated(
            "线程组定时器清理", test_group_cleanup_retires_all_timers);
    return failures == 0 ? 0 : 1;
}
