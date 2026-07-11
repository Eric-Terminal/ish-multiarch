#ifndef UTIL_TIMER_H
#define UTIL_TIMER_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <pthread.h>
#include <assert.h>
#include "util/sync.h"

static inline struct timespec timespec_now(clockid_t clockid) {
    assert(clockid == CLOCK_MONOTONIC || clockid == CLOCK_REALTIME);
    struct timespec now;
    clock_gettime(clockid, &now); // can't fail, according to posix spec
    return now;
}

static inline struct timespec timespec_add(struct timespec x, struct timespec y) {
    x.tv_sec += y.tv_sec;
    x.tv_nsec += y.tv_nsec;
    if (x.tv_nsec >= 1000000000) {
        x.tv_nsec -= 1000000000;
        x.tv_sec++;
    }
    return x;
}

static inline struct timespec timespec_subtract(struct timespec x, struct timespec y) {
    struct timespec result;
    if (x.tv_nsec < y.tv_nsec) {
        x.tv_sec -= 1;
        x.tv_nsec += 1000000000;
    }
    result.tv_sec = x.tv_sec - y.tv_sec;
    result.tv_nsec = x.tv_nsec - y.tv_nsec;
    return result;
}

static inline bool timespec_is_zero(struct timespec ts) {
    return ts.tv_sec == 0 && ts.tv_nsec == 0;
}

static inline bool timespec_positive(struct timespec ts) {
    return ts.tv_sec > 0 || (ts.tv_sec == 0 && ts.tv_nsec > 0);
}

static inline struct timespec timespec_normalize(struct timespec ts) {
    ts.tv_sec += ts.tv_nsec / 1000000000;
    ts.tv_nsec %= 1000000000;
    return ts;
}

typedef void (*timer_callback_t)(void *data);

// 定时器时间值不能依赖 arm64_32 上仅有 32 位的 time_t。
struct timer_time {
    int64_t sec;
    int64_t nsec;
};

static inline struct timer_time timer_time_from_timespec(
        struct timespec time) {
    return (struct timer_time) {
        .sec = time.tv_sec,
        .nsec = time.tv_nsec,
    };
}

static inline struct timer_time timer_time_add(
        struct timer_time time, struct timer_time duration) {
    int64_t nsec = time.nsec + duration.nsec;
    int64_t carry = nsec >= INT64_C(1000000000);
    if (carry)
        nsec -= INT64_C(1000000000);

    int64_t sec;
    if (__builtin_add_overflow(time.sec, duration.sec, &sec))
        return duration.sec > 0
                ? (struct timer_time) {INT64_MAX, 999999999}
                : (struct timer_time) {INT64_MIN, 0};
    if (__builtin_add_overflow(sec, carry, &sec))
        return (struct timer_time) {INT64_MAX, 999999999};
    return (struct timer_time) {sec, nsec};
}

static inline struct timer_time timer_time_subtract(
        struct timer_time time, struct timer_time other) {
    int64_t nsec = time.nsec - other.nsec;
    int64_t borrow = nsec < 0;
    if (borrow)
        nsec += INT64_C(1000000000);

    int64_t sec;
    if (__builtin_sub_overflow(time.sec, other.sec, &sec))
        return other.sec < 0
                ? (struct timer_time) {INT64_MAX, 999999999}
                : (struct timer_time) {INT64_MIN, 0};
    if (__builtin_sub_overflow(sec, borrow, &sec))
        return (struct timer_time) {INT64_MIN, 0};
    return (struct timer_time) {sec, nsec};
}

static inline bool timer_time_is_zero(struct timer_time time) {
    return time.sec == 0 && time.nsec == 0;
}

static inline bool timer_time_positive(struct timer_time time) {
    return time.sec > 0 || (time.sec == 0 && time.nsec > 0);
}

struct timer {
    clockid_t clockid;
    struct timer_time end;
    struct timer_time interval;

    bool active;
    bool thread_running;
    uint64_t generation;
    uint64_t callback_generation;
    pthread_t thread;
    timer_callback_t callback;
    void *data;
    lock_t lock;
    cond_t changed;

    bool dead; // 销毁已经开始，禁止再次设置。
};

struct timer *timer_new(clockid_t clockid, timer_callback_t callback, void *data);
void timer_free(struct timer *timer);
// value 表示距下次触发的时长，interval 表示非零时的后续触发间隔。
struct timer_spec {
    struct timer_time value;
    struct timer_time interval;
};
int timer_set(struct timer *timer, struct timer_spec spec, struct timer_spec *oldspec);
// value 为所选时钟域中的绝对截止时间；全零仍表示停用。
int timer_set_absolute(
        struct timer *timer, struct timer_spec spec,
        struct timer_spec *oldspec);
// 仅供当前回调在取得外部状态锁后判断自己是否已被重设淘汰。
bool timer_callback_is_current(struct timer *timer);

#endif
