#include <limits.h>
#include <stdlib.h>
#include <time.h>
#include "debug.h"
#include "kernel/errno.h"
#include "util/timer.h"
#include "misc.h"

static struct timespec timer_time_to_timespec(struct timer_time time) {
    assert(timer_time_positive(time));
    int64_t seconds = time.sec;
    // 长等待统一分段，避免 64 位绝对超时换算再次越界。
    if (seconds > INT32_MAX)
        seconds = INT32_MAX;
    return (struct timespec) {
        .tv_sec = (time_t) seconds,
        .tv_nsec = (long) time.nsec,
    };
}

struct timer *timer_new(clockid_t clockid, timer_callback_t callback, void *data) {
    struct timer *timer = malloc(sizeof(struct timer));
    if (timer == NULL)
        return NULL;
    timer->clockid = clockid;
    timer->callback = callback;
    timer->data = data;
    timer->end = (struct timer_time) {0};
    timer->interval = (struct timer_time) {0};
    timer->active = false;
    timer->thread_running = false;
    timer->generation = 0;
    timer->callback_generation = 0;
    lock_init(&timer->lock);
    cond_init(&timer->changed);
    timer->dead = false;
    return timer;
}

void timer_free(struct timer *timer) {
    lock(&timer->lock);
    if (timer->dead)
        die("定时器正在被重复销毁");
    timer->dead = true;
    timer->active = false;
    if (timer->thread_running) {
        if (pthread_equal(timer->thread, pthread_self()))
            die("定时器回调不能同步销毁自身");
        notify(&timer->changed);
        while (timer->thread_running)
            wait_for_ignore_signals(
                    &timer->changed, &timer->lock, NULL);
    }
    unlock(&timer->lock);
    cond_destroy(&timer->changed);
    if (pthread_mutex_destroy(&timer->lock.m) != 0)
        die("无法销毁定时器锁");
    free(timer);
}

static void *timer_thread(void *param) {
    struct timer *timer = param;
    lock(&timer->lock);
    while (true) {
        struct timer_time remaining = timer_time_subtract(
                timer->end,
                timer_time_from_timespec(timespec_now(timer->clockid)));
        while (timer->active && timer_time_positive(remaining)) {
            struct timespec wait = timer_time_to_timespec(remaining);
            wait_for_ignore_signals(
                    &timer->changed, &timer->lock, &wait);
            remaining = timer_time_subtract(
                    timer->end,
                    timer_time_from_timespec(
                            timespec_now(timer->clockid)));
        }
        if (!timer->active)
            break;

        timer_callback_t callback = timer->callback;
        void *data = timer->data;
        timer->callback_generation = timer->generation;
        if (timer_time_positive(timer->interval)) {
            timer->end = timer_time_add(timer->end, timer->interval);
        } else {
            timer->active = false;
        }
        unlock(&timer->lock);
        callback(data);
        lock(&timer->lock);
    }
    timer->thread_running = false;
    notify(&timer->changed);
    unlock(&timer->lock);
    return NULL;
}

static int timer_set_common(
        struct timer *timer, struct timer_spec spec,
        struct timer_spec *oldspec, bool absolute) {
    lock(&timer->lock);
    bool invalid = spec.value.sec < 0 || spec.value.nsec < 0 ||
            spec.value.nsec >= INT64_C(1000000000) ||
            spec.interval.sec < 0 || spec.interval.nsec < 0 ||
            spec.interval.nsec >= INT64_C(1000000000);
    if (timer->dead || invalid) {
        unlock(&timer->lock);
        return _EINVAL;
    }
    struct timer_time now = timer_time_from_timespec(
            timespec_now(timer->clockid));
    struct timer_time previous_end = timer->end;
    struct timer_time previous_interval = timer->interval;
    bool previous_active = timer->active;
    uint64_t previous_generation = timer->generation;
    if (oldspec != NULL) {
        oldspec->value = (struct timer_time) {0};
        if (timer->active) {
            struct timer_time remaining = timer_time_subtract(
                    timer->end, now);
            if (timer_time_positive(remaining))
                oldspec->value = remaining;
        }
        oldspec->interval = timer->interval;
    }

    timer->end = absolute ? spec.value : timer_time_add(now, spec.value);
    timer->interval = spec.interval;
    timer->active = !timer_time_is_zero(spec.value);
    timer->generation++;
    if (timer->generation == 0)
        timer->generation = 1;
    if (timer->thread_running) {
        notify(&timer->changed);
    } else if (timer->active) {
        int err = pthread_create(&timer->thread, NULL, timer_thread, timer);
        if (err != 0) {
            timer->end = previous_end;
            timer->interval = previous_interval;
            timer->active = previous_active;
            timer->generation = previous_generation;
            unlock(&timer->lock);
            return _EAGAIN;
        }
        timer->thread_running = true;
        if (pthread_detach(timer->thread) != 0)
            die("无法分离定时器线程");
    }
    unlock(&timer->lock);
    return 0;
}

int timer_set(
        struct timer *timer, struct timer_spec spec,
        struct timer_spec *oldspec) {
    return timer_set_common(timer, spec, oldspec, false);
}

int timer_set_absolute(
        struct timer *timer, struct timer_spec spec,
        struct timer_spec *oldspec) {
    return timer_set_common(timer, spec, oldspec, true);
}

bool timer_callback_is_current(struct timer *timer) {
    lock(&timer->lock);
    bool current = !timer->dead &&
            timer->callback_generation == timer->generation;
    unlock(&timer->lock);
    return current;
}
