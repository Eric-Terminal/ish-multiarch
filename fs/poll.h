#ifndef FS_POLL_H
#define FS_POLL_H
#include "kernel/fs.h"

struct real_poll {
    int fd;
};

struct poll {
    struct list poll_fds;
    struct real_poll real;
    int notify_pipe[2];
    int waiters; // if nonzero, notify_pipe exists
    // signal-safe 等待期间拒绝共享后端上的其他等待者。
    bool exclusive_waiter;
    // 销毁先拒绝新操作，再等待所有宿主等待调用离开。
    bool destroying;
    cond_t drained;

    // This is used to solve the race/UaF described here: https://lwn.net/Articles/520012/
    // thread 1: calls poll_wait, real_poll_wait returns an event with a pointer to a poll_fd
    // thread 2: calls poll_del_fd which frees the same poll_fd
    //
    // This can't be solved by adding locks because thread 1 could get
    // suspended after real_poll_wait returns but before it has a chance to
    // lock anything.
    //
    // An attempt was made to solve this with a Linux kernel patch, which
    // almost went in 3.7 but was backed out after discussion at
    // https://lkml.org/lkml/2012/10/16/302, and anyway wouldn't have solved
    // the problem on Darwin. My solution is to just not free poll_fds, and
    // instead move them to a freelist where they can be reused.
    struct list pollfd_freelist;

    lock_t lock;
};

struct poll_fd {
    // locked by containing struct poll
    struct fd *fd;
    struct list fds;
    int types;
    union poll_fd_info {
        void *ptr;
        int fd;
        uint64_t num;
    } info;
    // Used to implement edge-triggered notifications. When an event is
    // returned its bits are set here, and those bits are ignored on the next
    // call to poll_wait. The bits are cleared by poll_wakeup.
    int triggered_types;
    // 单次触发后保留登记对象但停止投递，直到 MOD 显式重置。
    bool enabled;

    // locked by containing struct fd
    struct poll *poll;
    struct list polls;
};

// these are defined in system headers somewhere
#undef POLL_IN
#undef POLL_OUT
#undef POLL_MSG
#undef POLL_ERR
#undef POLL_PRI
#undef POLL_HUP

#define POLL_READ 1
#define POLL_PRI 2
#define POLL_WRITE 4
#define POLL_ERR 8
#define POLL_HUP 16
#define POLL_NVAL 32
#define POLL_ONESHOT (1 << 30)
#define POLL_EDGETRIGGERED (1ul << 31)
struct poll_event {
    struct fd *fd;
    int types;
};
struct poll *poll_create(void);
bool poll_has_fd(struct poll *poll, struct fd *fd);
int poll_add_fd(struct poll *poll, struct fd *fd, int types, union poll_fd_info info);
// epoll ADD 使用该入口，在同一临界区内检查并建立唯一登记。
int poll_add_fd_unique(
        struct poll *poll, struct fd *fd, int types,
        union poll_fd_info info);
int poll_mod_fd(struct poll *poll, struct fd *fd, int types, union poll_fd_info info);
int poll_del_fd(struct poll *poll, struct fd *fd);
// Indicates that the specified events have been triggered. Each call will
// generate a new edge-triggered notification.
// please do not call this while holding any locks you would acquire in your poll operation
void poll_wakeup(struct fd *fd, int events);
// Waits for events on the fds in this poll, and calls the callback for each one found.
// Returns the number of times the callback returned 1, or negative for error.
typedef int (*poll_callback_t)(void *context, int types, union poll_fd_info info);
int poll_wait(struct poll *poll, poll_callback_t callback, void *context, struct timespec *timeout);
// 仅供每次调用新建、且只有当前 task 一个等待者的 poll 使用。
int poll_wait_signal_safe(struct poll *poll,
        poll_callback_t callback, void *context,
        struct timespec *timeout);
struct timer_time;
// deadline 使用 CLOCK_MONOTONIC 的绝对 64 位时间，避免 arm64_32 截断长等待。
int poll_wait_until(struct poll *poll, poll_callback_t callback,
        void *context, const struct timer_time *deadline);
int poll_wait_until_signal_safe(struct poll *poll,
        poll_callback_t callback, void *context,
        const struct timer_time *deadline);
// 阻止新操作、唤醒并排空等待者后再释放登记和宿主后端。
void poll_destroy(struct poll *poll);

// for fd_close
void poll_cleanup_fd(struct fd *fd);

#endif
