#ifdef __linux__
#define _GNU_SOURCE
#include <sys/resource.h>
#endif
#include "debug.h"
#include <time.h>
#include <signal.h>
#include <string.h>
#include <sys/time.h>
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/resource.h"
#include "kernel/time.h"
#include "kernel/timerfd.h"
#include "fs/poll.h"

static int clockid_to_real(uint_t clock, clockid_t *real) {
    switch (clock) {
        case CLOCK_REALTIME_:
        case CLOCK_REALTIME_COARSE_:
            *real = CLOCK_REALTIME; break;
        case CLOCK_MONOTONIC_: *real = CLOCK_MONOTONIC; break;
        default: return _EINVAL;
    }
    return 0;
}

static int timer_clockid_to_real(uint_t clock, clockid_t *real) {
    switch (clock) {
        case CLOCK_REALTIME_: *real = CLOCK_REALTIME; break;
        case CLOCK_MONOTONIC_: *real = CLOCK_MONOTONIC; break;
        default: return _EINVAL;
    }
    return 0;
}

static struct timer_spec timer_spec_to_real(struct itimerspec_ itspec) {
    struct timer_spec spec = {
        .value.sec = itspec.value.sec,
        .value.nsec = itspec.value.nsec,
        .interval.sec = itspec.interval.sec,
        .interval.nsec = itspec.interval.nsec,
    };
    return spec;
};

static dword_t timer_seconds_to_guest(int64_t seconds) {
    assert(seconds >= 0);
    if (seconds > INT32_MAX)
        return INT32_MAX;
    return (dword_t) seconds;
}

static dword_t timer_subsecond_to_guest(int64_t component) {
    assert(component >= 0);
    assert(component < INT64_C(1000000000));
    return (dword_t) component;
}

static struct itimerspec_ timer_spec_from_real(struct timer_spec spec) {
    struct itimerspec_ itspec = {
        .value.sec = timer_seconds_to_guest(spec.value.sec),
        .value.nsec = timer_subsecond_to_guest(spec.value.nsec),
        .interval.sec = timer_seconds_to_guest(spec.interval.sec),
        .interval.nsec = timer_subsecond_to_guest(spec.interval.nsec),
    };
    return itspec;
};

static bool valid_guest_timespec(struct timespec_ time) {
    return (sdword_t) time.sec >= 0 && (sdword_t) time.nsec >= 0 &&
            time.nsec < UINT32_C(1000000000);
}

static bool valid_guest_itimerspec(struct itimerspec_ spec) {
    return valid_guest_timespec(spec.value) &&
            valid_guest_timespec(spec.interval);
}

static bool valid_guest_timeval(struct timeval_ time) {
    return (sdword_t) time.sec >= 0 && (sdword_t) time.usec >= 0 &&
            time.usec < UINT32_C(1000000);
}

dword_t sys_time(addr_t time_out) {
    dword_t now = time(NULL);
    if (time_out != 0)
        if (user_put(time_out, now))
            return _EFAULT;
    return now;
}

dword_t sys_stime(addr_t UNUSED(time)) {
    return _EPERM;
}

dword_t sys_clock_gettime(dword_t clock, addr_t tp) {
    STRACE("clock_gettime(%d, 0x%x)", clock, tp);

    struct timespec ts;
    if (clock == CLOCK_PROCESS_CPUTIME_ID_) {
        // FIXME this is thread usage, not process usage
        struct rusage_ rusage = rusage_get_current();
        ts.tv_sec = rusage.utime.sec;
        ts.tv_nsec = rusage.utime.usec * 1000;
    } else {
        clockid_t clock_id;
        if (clockid_to_real(clock, &clock_id)) return _EINVAL;
        int err = clock_gettime(clock_id, &ts);
        if (err < 0)
            return errno_map();
    }
    struct timespec_ t;
    t.sec = ts.tv_sec;
    t.nsec = ts.tv_nsec;
    if (user_put(tp, t))
        return _EFAULT;
    STRACE(" {%lds %ldns}", t.sec, t.nsec);
    return 0;
}

dword_t sys_clock_getres(dword_t clock, addr_t res_addr) {
    STRACE("clock_getres(%d, %#x)", clock, res_addr);
    clockid_t clock_id;
    if (clockid_to_real(clock, &clock_id)) return _EINVAL;

    struct timespec res;
    int err = clock_getres(clock_id, &res);
    if (err < 0)
        return errno_map();
    struct timespec_ t;
    t.sec = res.tv_sec;
    t.nsec = res.tv_nsec;
    if (user_put(res_addr, t))
        return _EFAULT;
    return 0;
}

dword_t sys_clock_settime(dword_t UNUSED(clock), addr_t UNUSED(tp)) {
    return _EPERM;
}

static void itimer_notify(struct tgroup *group) {
    lock(&pids_lock);
    lock(&group->lock);
    struct timer *timer = group->itimer;
    bool current_generation = timer != NULL &&
            timer_callback_is_current(timer);
    unlock(&group->lock);
    if (!current_generation) {
        unlock(&pids_lock);
        return;
    }
    // ITIMER_REAL 与 Linux 一样生成 SI_KERNEL，不属于 exec 清理的 POSIX timer 节点。
    send_process_signal(group->leader, SIGALRM_, SIGINFO_NIL);
    unlock(&pids_lock);
}

static int itimer_set(struct tgroup *group, int which, struct timer_spec spec, struct timer_spec *old_spec) {
    if (which != ITIMER_REAL_) {
        FIXME("unimplemented setitimer %d", which);
        return _EINVAL;
    }

    if (!group->itimer) {
        struct timer *timer = timer_new(
                CLOCK_REALTIME, (timer_callback_t) itimer_notify, group);
        if (timer == NULL)
            return _ENOMEM;
        group->itimer = timer;
    }

    return timer_set(group->itimer, spec, old_spec);
}

int_t sys_setitimer(int_t which, addr_t new_val_addr, addr_t old_val_addr) {
    struct itimerval_ val;
    if (user_get(new_val_addr, val))
        return _EFAULT;
    if (!valid_guest_timeval(val.value) ||
            !valid_guest_timeval(val.interval))
        return _EINVAL;
    STRACE("setitimer(%d, {%ds %dus, %ds %dus}, 0x%x)", which, val.value.sec, val.value.usec, val.interval.sec, val.interval.usec, old_val_addr);

    struct timer_spec spec = {
        .interval.sec = val.interval.sec,
        .interval.nsec = (int64_t) val.interval.usec * 1000,
        .value.sec = val.value.sec,
        .value.nsec = (int64_t) val.value.usec * 1000,
    };
    struct timer_spec old_spec;
    if (timer_time_is_zero(spec.value))
        spec.interval = (struct timer_time) {0};

    struct tgroup *group = current->group;
    lock(&pids_lock);
    lock(&group->lock);
    int err = itimer_set(group, which, spec, &old_spec);
    unlock(&group->lock);
    unlock(&pids_lock);
    if (err < 0)
        return err;

    if (old_val_addr != 0) {
        struct itimerval_ old_val;
        old_val.interval.sec = timer_seconds_to_guest(
                old_spec.interval.sec);
        old_val.interval.usec = timer_subsecond_to_guest(
                old_spec.interval.nsec / 1000);
        old_val.value.sec = timer_seconds_to_guest(old_spec.value.sec);
        old_val.value.usec = timer_subsecond_to_guest(
                old_spec.value.nsec / 1000);
        if (user_put(old_val_addr, old_val))
            return _EFAULT;
    }

    return 0;
}

uint_t sys_alarm(uint_t seconds) {
    STRACE("alarm(%d)", seconds);
    if (seconds > INT32_MAX)
        seconds = INT32_MAX;
    struct timer_spec spec = {
        .value.sec = seconds,
    };
    struct timer_spec old_spec;

    struct tgroup *group = current->group;
    lock(&pids_lock);
    lock(&group->lock);
    int err = itimer_set(group, ITIMER_REAL_, spec, &old_spec);
    unlock(&group->lock);
    unlock(&pids_lock);
    if (err < 0)
        return err;

    // 与 Linux alarm 一致按半秒舍入，但活动定时器至少返回 1。
    uint64_t rounded = (uint64_t) old_spec.value.sec;
    if (old_spec.value.nsec >= 500000000)
        rounded++;
    if (rounded == 0 && !timer_time_is_zero(old_spec.value))
        rounded = 1;
    if (rounded > INT32_MAX)
        rounded = INT32_MAX;
    return (uint_t) rounded;
}

dword_t sys_nanosleep(addr_t req_addr, addr_t rem_addr) {
    struct timespec_ req_ts;
    if (user_get(req_addr, req_ts))
        return _EFAULT;
    STRACE("nanosleep({%d, %d}, 0x%x", req_ts.sec, req_ts.nsec, rem_addr);
    struct timespec req;
    req.tv_sec = req_ts.sec;
    req.tv_nsec = req_ts.nsec;
    struct timespec rem;
    if (nanosleep(&req, &rem) < 0)
        return errno_map();
    if (rem_addr != 0) {
        struct timespec_ rem_ts;
        rem_ts.sec = rem.tv_sec;
        rem_ts.nsec = rem.tv_nsec;
        if (user_put(rem_addr, rem_ts))
            return _EFAULT;
    }
    return 0;
}

dword_t sys_times(addr_t tbuf) {
    STRACE("times(0x%x)", tbuf);
    if (tbuf) {
        struct tms_ tmp;
        struct rusage_ rusage = rusage_get_current();
        tmp.tms_utime = clock_from_timeval(rusage.utime);
        tmp.tms_stime = clock_from_timeval(rusage.stime);
        tmp.tms_cutime = tmp.tms_utime;
        tmp.tms_cstime = tmp.tms_stime;
        if (user_put(tbuf, tmp))
            return _EFAULT;
    }
    return 0;
}

dword_t sys_gettimeofday(addr_t tv, addr_t tz) {
    STRACE("gettimeofday(0x%x, 0x%x)", tv, tz);
    struct timeval timeval;
    struct timezone timezone;
    if (gettimeofday(&timeval, &timezone) < 0) {
        return errno_map();
    }
    struct timeval_ tv_;
    struct timezone_ tz_;
    tv_.sec = timeval.tv_sec;
    tv_.usec = timeval.tv_usec;
    tz_.minuteswest = timezone.tz_minuteswest;
    tz_.dsttime = timezone.tz_dsttime;
    if ((tv && user_put(tv, tv_)) || (tz && user_put(tz, tz_))) {
        return _EFAULT;
    }
    return 0;
}

dword_t sys_settimeofday(addr_t UNUSED(tv), addr_t UNUSED(tz)) {
    return _EPERM;
}

static void posix_timer_callback(struct posix_timer *timer) {
    if (timer->tgroup == NULL)
        return;
    dword_t value_bits;
    memcpy(&value_bits, &timer->sig_value, sizeof(value_bits));
    struct siginfo_ info = {
        .code = SI_TIMER_,
        .payload_kind = SIGNAL_INFO_PAYLOAD_TIMER,
        .timer.timer = timer->timer_id,
        .timer.overrun = 0,
        // i386 sigval 的四字节原始位只在内部事件中零扩展。
        .timer.value = value_bits,
    };
    lock(&pids_lock);
    if (!timer_callback_is_current(timer->timer)) {
        unlock(&pids_lock);
        return;
    }
    struct task *thread = NULL;
    bool process_directed = timer->thread_pid == 0;
    if (timer->thread_pid != 0) {
        thread = pid_get_task(timer->thread_pid);
        if (thread != NULL && thread->group != timer->tgroup)
            thread = NULL;
    }
    // TODO: solve pid reuse. currently we have two ways of referring to a task: pid_t_ and struct task *. pids get reused. task struct pointers get freed on exit or reap. need a third option for cases like this, like a refcount layer.
    if (process_directed)
        send_process_signal(timer->tgroup->leader, timer->signal, info);
    else if (thread != NULL)
        send_signal_locked(thread, timer->signal, info);
    unlock(&pids_lock);
}

#define SIGEV_SIGNAL_ 0
#define SIGEV_NONE_ 1
#define SIGEV_THREAD_ID_ 4

int_t sys_timer_create(dword_t clock, addr_t sigevent_addr, addr_t timer_addr) {
    STRACE("timer_create(%d, %#x, %#x)", clock, sigevent_addr, timer_addr);
    clockid_t real_clockid;
    if (timer_clockid_to_real(clock, &real_clockid))
        return _EINVAL;
    bool default_event = sigevent_addr == 0;
    struct sigevent_ sigev = {
        .signo = SIGALRM_,
        .method = SIGEV_SIGNAL_,
    };
    if (!default_event && user_get(sigevent_addr, sigev))
        return _EFAULT;
    if (sigev.method != SIGEV_SIGNAL_ && sigev.method != SIGEV_NONE_ && sigev.method != SIGEV_THREAD_ID_)
        return _EINVAL;
    if (sigev.method != SIGEV_NONE_ &&
            (sigev.signo < 1 || sigev.signo > NUM_SIGS))
        return _EINVAL;

    if (sigev.method == SIGEV_THREAD_ID_) {
        lock(&pids_lock);
        struct task *target = pid_get_task(sigev.tid);
        if (target == NULL || target->group != current->group) {
            unlock(&pids_lock);
            return _EINVAL;
        }
        unlock(&pids_lock);
    }

    struct tgroup *group = current->group;
    lock(&group->lock);
    unsigned timer_id;
    for (timer_id = 0; timer_id < TIMERS_MAX; timer_id++) {
        if (group->posix_timers[timer_id].timer == NULL)
            break;
    }
    if (timer_id >= TIMERS_MAX) {
        unlock(&group->lock);
        return _EAGAIN;
    }
    if (default_event)
        sigev.value.sv_int = timer_id;
    struct posix_timer *timer = &group->posix_timers[timer_id];
    struct timer *host_timer = timer_new(
            real_clockid, (timer_callback_t) posix_timer_callback, timer);
    if (host_timer == NULL) {
        unlock(&group->lock);
        return _ENOMEM;
    }
    if (user_put(timer_addr, timer_id)) {
        unlock(&group->lock);
        timer_free(host_timer);
        return _EFAULT;
    }
    timer->timer_id = timer_id;
    timer->timer = host_timer;
    timer->deleting = false;
    timer->signal = sigev.signo;
    timer->sig_value = sigev.value;
    timer->tgroup = NULL;
    if (sigev.method == SIGEV_SIGNAL_) {
        timer->tgroup = group;
        timer->thread_pid = 0;
    } else if (sigev.method == SIGEV_THREAD_ID_) {
        timer->tgroup = group;
        timer->thread_pid = sigev.tid;
    }
    unlock(&group->lock);
    return 0;
}

#define TIMER_ABSTIME_ (1 << 0)

int_t sys_timer_settime(dword_t timer_id, int_t flags, addr_t new_value_addr, addr_t old_value_addr) {
    STRACE("timer_settime(%d, %d, %#x, %#x)", timer_id, flags, new_value_addr, old_value_addr);
    struct itimerspec_ value;
    if (user_get(new_value_addr, value))
        return _EFAULT;
    if (timer_id >= TIMERS_MAX || flags & ~TIMER_ABSTIME_ ||
            !valid_guest_itimerspec(value))
        return _EINVAL;

    struct tgroup *group = current->group;
    lock(&pids_lock);
    lock(&group->lock);
    struct posix_timer *timer = &group->posix_timers[timer_id];
    if (timer->timer == NULL || timer->deleting) {
        unlock(&group->lock);
        unlock(&pids_lock);
        return _EINVAL;
    }
    struct timer_spec spec = timer_spec_to_real(value);
    struct timer_spec old_spec;
    if (timer_time_is_zero(spec.value))
        spec.interval = (struct timer_time) {0};
    int err = flags & TIMER_ABSTIME_
            ? timer_set_absolute(timer->timer, spec, &old_spec)
            : timer_set(timer->timer, spec, &old_spec);
    unlock(&group->lock);
    unlock(&pids_lock);
    if (err < 0)
        return err;

    if (old_value_addr) {
        struct itimerspec_ old_value = timer_spec_from_real(old_spec);
        if (user_put(old_value_addr, old_value))
            return _EFAULT;
    }
    return 0;
}

int_t sys_timer_delete(dword_t timer_id) {
    STRACE("timer_delete(%d)\n", timer_id);
    if (timer_id >= TIMERS_MAX)
        return _EINVAL;

    struct tgroup *group = current->group;
    lock(&group->lock);
    struct posix_timer *timer = &group->posix_timers[timer_id];
    if (timer->timer == NULL || timer->deleting) {
        unlock(&group->lock);
        return _EINVAL;
    }
    struct timer *host_timer = timer->timer;
    timer->deleting = true;
    unlock(&group->lock);

    timer_free(host_timer);

    lock(&group->lock);
    assert(timer->deleting && timer->timer == host_timer);
    *timer = (struct posix_timer) {0};
    unlock(&group->lock);
    return 0;
}

static void posix_timers_begin_destroy_locked(
        struct tgroup *group,
        struct timer *posix_timers[TIMERS_MAX]) {
    for (unsigned i = 0; i < TIMERS_MAX; i++) {
        struct posix_timer *timer = &group->posix_timers[i];
        if (timer->timer != NULL) {
            assert(!timer->deleting);
            timer->deleting = true;
            posix_timers[i] = timer->timer;
        }
    }
}

static void posix_timers_finish_destroy(
        struct tgroup *group,
        struct timer *posix_timers[TIMERS_MAX]) {
    for (unsigned i = 0; i < TIMERS_MAX; i++)
        if (posix_timers[i] != NULL)
            timer_free(posix_timers[i]);

    lock(&group->lock);
    for (unsigned i = 0; i < TIMERS_MAX; i++) {
        if (posix_timers[i] != NULL) {
            assert(group->posix_timers[i].deleting &&
                    group->posix_timers[i].timer == posix_timers[i]);
            group->posix_timers[i] = (struct posix_timer) {0};
        }
    }
    unlock(&group->lock);
}

void tgroup_exec_posix_timers_destroy(struct tgroup *group) {
    struct timer *posix_timers[TIMERS_MAX] = {0};

    lock(&group->lock);
    posix_timers_begin_destroy_locked(group, posix_timers);
    unlock(&group->lock);
    posix_timers_finish_destroy(group, posix_timers);
}

void tgroup_timers_destroy(struct tgroup *group) {
    struct timer *posix_timers[TIMERS_MAX] = {0};

    lock(&group->lock);
    struct timer *itimer = group->itimer;
    group->itimer = NULL;
    posix_timers_begin_destroy_locked(group, posix_timers);
    unlock(&group->lock);

    if (itimer != NULL)
        timer_free(itimer);
    posix_timers_finish_destroy(group, posix_timers);
}

static struct fd_ops timerfd_ops;

#define TIMERFD_NANOSECONDS_PER_SECOND UINT64_C(1000000000)
#define TIMERFD_KTIME_MAX_SECONDS INT64_C(9223372036)
#define TIMERFD_KTIME_MAX_NANOSECONDS INT64_C(854775807)
#define TIMERFD_KTIME_MAX UINT64_C(9223372036854775807)

_Static_assert(
        (uint64_t) TIMERFD_KTIME_MAX_SECONDS *
                        TIMERFD_NANOSECONDS_PER_SECOND +
                TIMERFD_KTIME_MAX_NANOSECONDS ==
                TIMERFD_KTIME_MAX,
        "timerfd 时间上限必须匹配 Linux KTIME_MAX");

static bool valid_timerfd_spec(struct timer_spec spec) {
    return spec.value.sec >= 0 && spec.value.nsec >= 0 &&
            spec.value.nsec < (int64_t) TIMERFD_NANOSECONDS_PER_SECOND &&
            spec.interval.sec >= 0 && spec.interval.nsec >= 0 &&
            spec.interval.nsec <
                    (int64_t) TIMERFD_NANOSECONDS_PER_SECOND;
}

static struct timer_time timerfd_clamp_time(struct timer_time time) {
    assert(time.sec >= 0 && time.nsec >= 0 &&
            time.nsec < (int64_t) TIMERFD_NANOSECONDS_PER_SECOND);
    if (time.sec > TIMERFD_KTIME_MAX_SECONDS ||
            (time.sec == TIMERFD_KTIME_MAX_SECONDS &&
                    time.nsec > TIMERFD_KTIME_MAX_NANOSECONDS)) {
        return (struct timer_time) {
            .sec = TIMERFD_KTIME_MAX_SECONDS,
            .nsec = TIMERFD_KTIME_MAX_NANOSECONDS,
        };
    }
    return time;
}

static uint64_t timerfd_time_to_nanoseconds(struct timer_time time) {
    time = timerfd_clamp_time(time);
    uint64_t nanoseconds =
            (uint64_t) time.sec * TIMERFD_NANOSECONDS_PER_SECOND +
            (uint64_t) time.nsec;
    assert(nanoseconds <= TIMERFD_KTIME_MAX);
    return nanoseconds;
}

static struct timer_time timerfd_time_from_nanoseconds(
        uint64_t nanoseconds) {
    assert(nanoseconds <= TIMERFD_KTIME_MAX);
    return (struct timer_time) {
        .sec = (int64_t) (
                nanoseconds / TIMERFD_NANOSECONDS_PER_SECOND),
        .nsec = (int64_t) (
                nanoseconds % TIMERFD_NANOSECONDS_PER_SECOND),
    };
}

static uint64_t timerfd_now(clockid_t clockid) {
    struct timer_time now = timer_time_from_timespec(
            timespec_now(clockid));
    assert(now.sec >= 0 && now.nsec >= 0);
    return timerfd_time_to_nanoseconds(now);
}

static uint64_t timerfd_add_saturated(
        uint64_t time, uint64_t duration) {
    assert(time <= TIMERFD_KTIME_MAX &&
            duration <= TIMERFD_KTIME_MAX);
    return duration > TIMERFD_KTIME_MAX - time ?
            TIMERFD_KTIME_MAX : time + duration;
}

static uint64_t timerfd_forward_deadline(
        uint64_t deadline, uint64_t interval, uint64_t now,
        uint64_t *periods) {
    assert(deadline != 0 && interval != 0 &&
            deadline <= TIMERFD_KTIME_MAX &&
            interval <= TIMERFD_KTIME_MAX &&
            now <= TIMERFD_KTIME_MAX && periods != NULL);

    uint64_t elapsed = now >= deadline ? now - deadline : 0;
    uint64_t elapsed_periods = elapsed / interval;
    *periods = elapsed_periods + 1;
    uint64_t capacity = TIMERFD_KTIME_MAX - deadline;
    if (*periods > capacity / interval)
        return TIMERFD_KTIME_MAX;
    return deadline + *periods * interval;
}

// 调用方持有 fd->lock。底层定时器只接收一次性相对时长。
static int timerfd_program_locked(
        struct fd *fd, uint64_t deadline) {
    struct timer_spec one_shot = {};
    if (deadline != 0) {
        uint64_t now = timerfd_now(fd->timerfd.clockid);
        uint64_t remaining = deadline > now ? deadline - now : 1;
        one_shot.value = timerfd_time_from_nanoseconds(remaining);
    }
    return timer_set(fd->timerfd.timer, one_shot, NULL);
}

static void timerfd_snapshot_locked(
        struct fd *fd, uint64_t now,
        struct timer_spec *spec) {
    *spec = (struct timer_spec) {
        .interval = fd->timerfd.interval,
    };
    uint64_t deadline =
            timerfd_time_to_nanoseconds(fd->timerfd.next);
    if (deadline == 0)
        return;

    uint64_t interval =
            timerfd_time_to_nanoseconds(fd->timerfd.interval);
    if (interval != 0 && now >= deadline) {
        uint64_t periods;
        deadline = timerfd_forward_deadline(
                deadline, interval, now, &periods);
    }
    if (deadline > now)
        spec->value = timerfd_time_from_nanoseconds(deadline - now);
}

// 首次到期后保持 host timer 停止，直到 read/gettime 批量计数并重臂。
static int timerfd_refresh_periodic_locked(struct fd *fd) {
    uint64_t interval =
            timerfd_time_to_nanoseconds(fd->timerfd.interval);
    if (!fd->timerfd.expired || interval == 0)
        return 0;

    uint64_t deadline =
            timerfd_time_to_nanoseconds(fd->timerfd.next);
    assert(deadline != 0);
    uint64_t periods;
    uint64_t next = timerfd_forward_deadline(
            deadline, interval, timerfd_now(fd->timerfd.clockid),
            &periods);
    int error = timerfd_program_locked(fd, next);
    if (error < 0)
        return error;

    fd->timerfd.next = timerfd_time_from_nanoseconds(next);
    fd->timerfd.expirations += periods - 1;
    fd->timerfd.expired = false;
    return 0;
}

static void timerfd_callback(void *opaque) {
    struct fd *fd = opaque;
    lock(&fd->lock);
    if (!timer_callback_is_current(fd->timerfd.timer)) {
        unlock(&fd->lock);
        return;
    }
    if (!fd->timerfd.expired) {
        fd->timerfd.expired = true;
        fd->timerfd.expirations++;
    }
    notify(&fd->cond);
    unlock(&fd->lock);
    poll_wakeup(fd, POLL_READ);
}

fd_t timerfd_create_task(
        struct task *task, int_t clockid, int_t flags) {
    if (flags & ~(O_CLOEXEC_ | O_NONBLOCK_))
        return _EINVAL;
    clockid_t real_clockid;
    if (timer_clockid_to_real(clockid, &real_clockid))
        return _EINVAL;

    struct fd *fd = adhoc_fd_create(&timerfd_ops);
    if (fd == NULL)
        return _ENOMEM;

    fd->flags = O_RDWR_;
    fd->timerfd.clockid = real_clockid;
    fd->timerfd.timer = timer_new(
            real_clockid, timerfd_callback, fd);
    if (fd->timerfd.timer == NULL) {
        fd_close(fd);
        return _ENOMEM;
    }
    return f_install_task(task, fd, flags);
}

fd_t sys_timerfd_create(int_t clockid, int_t flags) {
    STRACE("timerfd_create(%d, %#x)", clockid, flags);
    return timerfd_create_task(current, clockid, flags);
}

int_t timerfd_settime_task(
        struct task *task, fd_t f, int_t flags,
        const struct timer_spec *new_spec,
        struct timer_spec *old_spec) {
    assert(task != NULL && new_spec != NULL);
    if ((flags & ~TFD_TIMER_ABSTIME_) ||
            !valid_timerfd_spec(*new_spec))
        return _EINVAL;

    struct timer_spec normalized = {
        .value = timerfd_clamp_time(new_spec->value),
        .interval = timerfd_clamp_time(new_spec->interval),
    };
    struct fd *fd = f_get_task_retain(task, f);
    if (fd == NULL)
        return _EBADF;
    if (fd->ops != &timerfd_ops) {
        fd_close(fd);
        return _EINVAL;
    }

    lock(&fd->lock);
    uint64_t now = timerfd_now(fd->timerfd.clockid);
    struct timer_spec previous;
    timerfd_snapshot_locked(fd, now, &previous);

    uint64_t value = timerfd_time_to_nanoseconds(normalized.value);
    uint64_t next = 0;
    if (value != 0) {
        next = flags & TFD_TIMER_ABSTIME_ ?
                value : timerfd_add_saturated(now, value);
    }
    int err = timerfd_program_locked(fd, next);
    if (err == 0) {
        fd->timerfd.interval = normalized.interval;
        fd->timerfd.next = timerfd_time_from_nanoseconds(next);
        fd->timerfd.expirations = 0;
        fd->timerfd.expired = false;
        if (old_spec != NULL)
            *old_spec = previous;
    }
    unlock(&fd->lock);
    fd_close(fd);
    return err;
}

int_t timerfd_gettime_task(
        struct task *task, fd_t f,
        struct timer_spec *current_spec) {
    assert(task != NULL && current_spec != NULL);
    struct fd *fd = f_get_task_retain(task, f);
    if (fd == NULL)
        return _EBADF;
    if (fd->ops != &timerfd_ops) {
        fd_close(fd);
        return _EINVAL;
    }

    lock(&fd->lock);
    int err = timerfd_refresh_periodic_locked(fd);
    if (err == 0) {
        timerfd_snapshot_locked(fd,
                timerfd_now(fd->timerfd.clockid), current_spec);
    }
    unlock(&fd->lock);
    fd_close(fd);
    return err;
}

int_t sys_timerfd_settime(
        fd_t f, int_t flags,
        addr_t new_value_addr, addr_t old_value_addr) {
    STRACE("timerfd_settime(%d, %d, %#x, %#x)",
            f, flags, new_value_addr, old_value_addr);
    struct itimerspec_ value;
    if (user_get(new_value_addr, value))
        return _EFAULT;
    if (!valid_guest_itimerspec(value))
        return _EINVAL;
    struct timer_spec spec = timer_spec_to_real(value);
    struct timer_spec previous;
    int err = timerfd_settime_task(
            current, f, flags, &spec, &previous);
    if (err < 0)
        return err;

    if (old_value_addr) {
        struct itimerspec_ old_value = timer_spec_from_real(previous);
        if (user_put(old_value_addr, old_value))
            return _EFAULT;
    }
    return 0;
}

int_t sys_timerfd_gettime(fd_t f, addr_t value_addr) {
    STRACE("timerfd_gettime(%d, %#x)", f, value_addr);
    struct timer_spec current_spec;
    int err = timerfd_gettime_task(current, f, &current_spec);
    if (err < 0)
        return err;
    struct itimerspec_ value = timer_spec_from_real(current_spec);
    if (user_put(value_addr, value))
        return _EFAULT;
    return 0;
}

static ssize_t timerfd_read(struct fd *fd, void *buf, size_t bufsize) {
    if (bufsize < sizeof(uint64_t))
        return _EINVAL;
    lock(&fd->lock);
    while (fd->timerfd.expirations == 0) {
        if (fd->flags & O_NONBLOCK_) {
            unlock(&fd->lock);
            return _EAGAIN;
        }
        int err = wait_for(&fd->cond, &fd->lock, NULL);
        if (err < 0) {
            unlock(&fd->lock);
            return err;
        }
    }

    int err = timerfd_refresh_periodic_locked(fd);
    if (err < 0) {
        unlock(&fd->lock);
        return err;
    }
    memcpy(buf, &fd->timerfd.expirations, sizeof(uint64_t));
    fd->timerfd.expirations = 0;
    if (timer_time_is_zero(fd->timerfd.interval))
        fd->timerfd.expired = false;
    unlock(&fd->lock);
    return sizeof(uint64_t);
}
static int timerfd_poll(struct fd *fd) {
    int res = 0;
    lock(&fd->lock);
    if (fd->timerfd.expirations != 0)
        res |= POLL_READ;
    unlock(&fd->lock);
    return res;
}
static int timerfd_close(struct fd *fd) {
    if (fd->timerfd.timer != NULL)
        timer_free(fd->timerfd.timer);
    return 0;
}

static struct fd_ops timerfd_ops = {
    .read = timerfd_read,
    .poll = timerfd_poll,
    .close = timerfd_close,
};
