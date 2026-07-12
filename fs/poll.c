#include "kernel/task.h"
#include <sched.h>
#include <string.h>
#include <poll.h>
#include <fcntl.h>
#include <limits.h>
#include "misc.h"
#include "util/list.h"
#include "util/timer.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "fs/fd.h"
#include "fs/poll.h"
#include "fs/real.h"

#include "fs/sockrestart.h"

extern const struct fd_ops socket_fdops;

#if defined(__linux__)
#include <sys/epoll.h>
#define HAVE_EPOLL 1
#elif defined(__APPLE__)
#include <sys/event.h>
#define HAVE_KQUEUE 1
#endif

static int real_poll_init(struct real_poll *real);
static void real_poll_close(struct real_poll *real);
struct real_poll_event {
#if HAVE_EPOLL
    struct epoll_event real;
#elif HAVE_KQUEUE
    struct kevent real;
#endif
};
static void *rpe_data(struct real_poll_event *rpe);
static int rpe_events(struct real_poll_event *rpe);
static int real_poll_wait(struct real_poll *real, struct real_poll_event *events, int max, struct timespec *timeout);
static int real_poll_update(struct real_poll *real, int fd, int types, void *data);

// lock order: fd, then poll

struct poll *poll_create(void) {
    struct poll *poll = malloc(sizeof(struct poll));
    if (poll == NULL)
        return ERR_PTR(_ENOMEM);
    int err = real_poll_init(&poll->real);
    if (err < 0)
        goto error;
    poll->waiters = 0;
    poll->destroying = false;
    poll->notify_pipe[0] = -1;
    poll->notify_pipe[1] = -1;
    list_init(&poll->poll_fds);
    list_init(&poll->pollfd_freelist);
    lock_init(&poll->lock);
    cond_init(&poll->drained);
    return poll;

error:
    err = errno_map();
    free(poll);
    return ERR_PTR(err);
}

static inline bool poll_fd_is_real(struct poll_fd *pollfd) {
    return pollfd->fd->ops->poll == realfs_poll;
}

// does not do its own locking
static struct poll_fd *poll_find_fd(struct poll *poll, struct fd *fd) {
    struct poll_fd *poll_fd, *tmp;
    list_for_each_entry_safe(&poll->poll_fds, poll_fd, tmp, fds) {
        if (poll_fd->fd == fd)
            return poll_fd;
    }
    return NULL;
}

// See comment on pollfd_freelist for context
static void poll_fd_free(struct poll_fd *poll_fd) {
    struct poll *poll = poll_fd->poll;
    memset(poll_fd, 0xba, sizeof(*poll_fd));
    poll_fd->poll = NULL; // used to mark it as free
    list_add(&poll->pollfd_freelist, &poll_fd->fds);
}

// 调用方持有 poll 锁；EAGAIN 表示管道中已有通知，无需重复写入。
static void poll_notify_waiters_locked(struct poll *poll) {
    if (poll->notify_pipe[1] == -1)
        return;

    ssize_t written;
    do {
        written = write(poll->notify_pipe[1], "", 1);
    } while (written < 0 && errno == EINTR);
    assert(written == 1 || (written < 0 &&
            (errno == EAGAIN || errno == EWOULDBLOCK)));
}

// 快照只能从活着的 fd 取得强引用；若最终关闭已将计数
// 降为零，它会在 poll 解锁后自行拆除登记，不得再复活。
static bool poll_fd_try_retain(struct fd *fd) {
    unsigned refcount = atomic_load_explicit(
            &fd->refcount, memory_order_relaxed);
    while (refcount != 0) {
        if (atomic_compare_exchange_weak_explicit(
                &fd->refcount, &refcount, refcount + 1,
                memory_order_acquire, memory_order_relaxed))
            return true;
    }
    return false;
}

bool poll_has_fd(struct poll *poll, struct fd *fd) {
    lock(&poll->lock);
    bool found = poll_find_fd(poll, fd) != NULL;
    unlock(&poll->lock);
    return found;
}

static int poll_add_fd_impl(
        struct poll *poll, struct fd *fd, int types,
        union poll_fd_info info, bool unique) {
    int err;
    lock(&fd->poll_lock);
    lock(&poll->lock);

    if (poll->destroying) {
        err = _EBADF;
        goto out;
    }

    if (unique && poll_find_fd(poll, fd) != NULL) {
        err = _EEXIST;
        goto out;
    }

    struct poll_fd *poll_fd;
    if (!list_empty(&poll->pollfd_freelist)) {
        poll_fd = list_first_entry(&poll->pollfd_freelist, struct poll_fd, fds);
        list_remove(&poll_fd->fds);
    } else {
        poll_fd = malloc(sizeof(struct poll_fd));
        if (poll_fd == NULL) {
            err = _ENOMEM;
            goto out;
        }
    }
    poll_fd->fd = fd;
    poll_fd->poll = poll;
    poll_fd->types = types;
    poll_fd->info = info;
    poll_fd->triggered_types = 0;
    poll_fd->enabled = true;

    if (poll_fd_is_real(poll_fd)) {
        err = real_poll_update(&poll->real, fd->real_fd, types, poll_fd);
        if (err < 0) {
            poll_fd_free(poll_fd);
            err = errno_map();
            goto out;
        }
    }

    list_add(&fd->poll_fds, &poll_fd->polls);
    list_add(&poll->poll_fds, &poll_fd->fds);
    poll_notify_waiters_locked(poll);

    err = 0;
out:
    unlock(&poll->lock);
    unlock(&fd->poll_lock);
    return err;
}

int poll_add_fd(
        struct poll *poll, struct fd *fd, int types,
        union poll_fd_info info) {
    return poll_add_fd_impl(poll, fd, types, info, false);
}

int poll_add_fd_unique(
        struct poll *poll, struct fd *fd, int types,
        union poll_fd_info info) {
    return poll_add_fd_impl(poll, fd, types, info, true);
}

int poll_del_fd(struct poll *poll, struct fd *fd) {
    int err;
    lock(&fd->poll_lock);
    lock(&poll->lock);
    if (poll->destroying) {
        err = _EBADF;
        goto out;
    }
    struct poll_fd *poll_fd = poll_find_fd(poll, fd);
    if (poll_fd == NULL) {
        err = _ENOENT;
        goto out;
    }

    if (poll_fd->enabled && poll_fd_is_real(poll_fd)) {
        err = real_poll_update(&poll->real, fd->real_fd, 0, poll_fd);
        if (err < 0) {
            err = errno_map();
            goto out;
        }
    }

    list_remove(&poll_fd->polls);
    list_remove(&poll_fd->fds);
    poll_fd_free(poll_fd);

    err = 0;
out:
    unlock(&poll->lock);
    unlock(&fd->poll_lock);
    return err;
}

int poll_mod_fd(struct poll *poll, struct fd *fd, int types, union poll_fd_info info) {
    int err;
    lock(&fd->poll_lock);
    lock(&poll->lock);
    if (poll->destroying) {
        err = _EBADF;
        goto out;
    }
    struct poll_fd *poll_fd = poll_find_fd(poll, fd);
    if (poll_fd == NULL) {
        err = _ENOENT;
        goto out;
    }

    if (poll_fd_is_real(poll_fd)) {
        err = real_poll_update(&poll->real, fd->real_fd, types, poll_fd);
        if (err < 0) {
            err = errno_map();
            goto out;
        }
    }

    poll_fd->types = types;
    poll_fd->info = info;
    poll_fd->triggered_types = 0;
    poll_fd->enabled = true;
    poll_notify_waiters_locked(poll);

    err = 0;
out:
    unlock(&poll->lock);
    unlock(&fd->poll_lock);
    return err;
}

void poll_cleanup_fd(struct fd *fd) {
    lock(&fd->poll_lock);
    struct poll_fd *poll_fd, *tmp;
    list_for_each_entry_safe(&fd->poll_fds, poll_fd, tmp, polls) {
        struct poll *poll = poll_fd->poll;
        lock(&poll->lock);
        if (poll_fd->enabled && poll_fd_is_real(poll_fd))
            real_poll_update(&poll->real, fd->real_fd, 0, poll_fd);
        list_remove(&poll_fd->polls);
        list_remove(&poll_fd->fds);
        poll_fd_free(poll_fd);
        unlock(&poll->lock);
    }
    unlock(&fd->poll_lock);
}

void poll_wakeup(struct fd *fd, int events) {
    struct poll_fd *poll_fd;
    lock(&fd->poll_lock);
    list_for_each_entry(&fd->poll_fds, poll_fd, polls) {
        struct poll *poll = poll_fd->poll;
        lock(&poll->lock);
        if (!poll_fd->enabled) {
            unlock(&poll->lock);
            continue;
        }
        if (poll_fd->types & POLL_EDGETRIGGERED)
            poll_fd->triggered_types &= ~events;
        poll_notify_waiters_locked(poll);
        unlock(&poll->lock);
    }
    unlock(&fd->poll_lock);
}

static bool poll_timeout_valid(const struct timespec *timeout) {
    return timeout == NULL || (timeout->tv_sec >= 0 &&
            timeout->tv_nsec >= 0 && timeout->tv_nsec < 1000000000);
}

static bool poll_deadline_remaining(
        struct timer_time deadline, struct timespec *slice) {
    struct timer_time remaining = timer_time_subtract(
            deadline,
            timer_time_from_timespec(timespec_now(CLOCK_MONOTONIC)));
    if (!timer_time_positive(remaining))
        return false;

    if (slice != NULL) {
        int64_t seconds = remaining.sec;
        long nanoseconds = (long) remaining.nsec;
        // watchOS arm64_32 的 time_t 只有 32 位；统一分片也避免把
        // 过大的相对时长直接交给不同宿主后端。
        if (seconds > INT32_MAX) {
            seconds = INT32_MAX;
            nanoseconds = 0;
        }
        *slice = (struct timespec) {
            .tv_sec = (time_t) seconds,
            .tv_nsec = nanoseconds,
        };
    }
    return true;
}

int poll_wait(struct poll *poll_, poll_callback_t callback, void *context, struct timespec *timeout) {
    if (!poll_timeout_valid(timeout))
        return _EINVAL;

    bool has_deadline = timeout != NULL;
    struct timer_time deadline = {0};
    if (has_deadline) {
        deadline = timer_time_add(
                timer_time_from_timespec(timespec_now(CLOCK_MONOTONIC)),
                timer_time_from_timespec(*timeout));
    }

    lock(&poll_->lock);

    if (poll_->destroying) {
        unlock(&poll_->lock);
        return _EBADF;
    }

    // acquire the pipe
    if (poll_->waiters++ == 0) {
        assert(poll_->notify_pipe[0] == -1 && poll_->notify_pipe[1] == -1);
        if (pipe(poll_->notify_pipe) < 0) {
            poll_->waiters--;
            unlock(&poll_->lock);
            return errno_map();
        }
        fcntl(poll_->notify_pipe[0], F_SETFL, O_NONBLOCK);
        fcntl(poll_->notify_pipe[1], F_SETFL, O_NONBLOCK);
        if (real_poll_update(&poll_->real, poll_->notify_pipe[0],
                POLL_READ, NULL) < 0) {
            int err = errno_map();
            close(poll_->notify_pipe[0]);
            close(poll_->notify_pipe[1]);
            poll_->notify_pipe[0] = -1;
            poll_->notify_pipe[1] = -1;
            poll_->waiters--;
            unlock(&poll_->lock);
            return err;
        }
    }

    int res = 0;
    while (true) {
        // check if any fds are ready
        struct poll_fd *poll_fd, *tmp;
        list_for_each_entry_safe(&poll_->poll_fds, poll_fd, tmp, fds) {
            if (!poll_fd->enabled)
                continue;
            struct fd *fd = poll_fd->fd;
            int poll_types = 0;
            if (fd->ops->poll)
                poll_types = fd->ops->poll(fd);
            poll_types &= poll_fd->types | POLL_HUP | POLL_ERR;
            if (poll_fd->types & POLL_EDGETRIGGERED) {
                poll_types &= ~poll_fd->triggered_types;
            }
            if (poll_types) {
                bool delivered = callback(
                        context, poll_types, poll_fd->info) == 1;
                if (delivered)
                    res++;

                // 宿主后端不直接使用 oneshot；持有 poll 锁可确保只有一个等待者
                // 消费本次事件。登记对象必须保留，MOD 才能按 Linux 语义重置它。
                if (delivered && poll_fd->types & POLL_ONESHOT) {
                    if (poll_fd_is_real(poll_fd))
                        real_poll_update(&poll_->real, fd->real_fd, 0, poll_fd);
                    poll_fd->enabled = false;
                }

                if (delivered && poll_fd->types & POLL_EDGETRIGGERED) {
                    poll_fd->triggered_types |= poll_types;
                }
            }
        }
        if (res > 0)
            break;

        lock(&current->sighand->lock);
        bool signal_pending = !!(current->pending & ~current->blocked);
        unlock(&current->sighand->lock);
        if (signal_pending) {
            res = _EINTR;
            break;
        }

        struct timespec wait_slice;
        if (has_deadline &&
                !poll_deadline_remaining(deadline, &wait_slice))
            break;

        // 对 begin 时的登记集合做精确快照；强引用使等待
        // 期间的 DEL 与最终关闭无法让 end 访问已释放的 fd。
        size_t listen_wait_capacity = 0;
        list_for_each_entry(&poll_->poll_fds, poll_fd, fds) {
            if (poll_fd->fd->ops == &socket_fdops)
                listen_wait_capacity++;
        }
        struct fd **listen_waits = NULL;
        if (listen_wait_capacity != 0) {
            listen_waits = malloc(
                    listen_wait_capacity * sizeof(*listen_waits));
            if (listen_waits == NULL) {
                res = _ENOMEM;
                break;
            }
        }
        size_t listen_wait_count = 0;
        list_for_each_entry(&poll_->poll_fds, poll_fd, fds) {
            if (poll_fd->fd->ops != &socket_fdops)
                continue;
            if (!poll_fd_try_retain(poll_fd->fd))
                continue;
            listen_waits[listen_wait_count++] = poll_fd->fd;
            sockrestart_begin_listen_wait(poll_fd->fd);
        }
        unlock(&poll_->lock);
        int err;
        int wait_errno;
        struct real_poll_event e[4];
        while (true) {
            err = real_poll_wait(&poll_->real, e,
                    sizeof(e)/sizeof(e[0]),
                    has_deadline ? &wait_slice : NULL);
            wait_errno = errno;
            bool restart_listen_wait =
                    sockrestart_should_restart_listen_wait();
            if (!(err < 0 && wait_errno == EINTR && restart_listen_wait))
                break;
            if (has_deadline &&
                    !poll_deadline_remaining(deadline, &wait_slice)) {
                err = 0;
                wait_errno = 0;
                break;
            }
        }
        for (size_t index = 0; index < listen_wait_count; index++)
            sockrestart_end_listen_wait(listen_waits[index]);
        lock(&poll_->lock);

        bool stop_waiting = false;
        if (poll_->destroying) {
            res = _EBADF;
            stop_waiting = true;
        } else if (err < 0) {
            errno = wait_errno;
            res = errno_map();
            stop_waiting = true;
        } else if (err == 0) {
            // 宿主分片可能先于总截止点结束，只有绝对截止点到达才超时。
            stop_waiting = !has_deadline ||
                    !poll_deadline_remaining(deadline, NULL);
        } else {
            // dead with any edge-triggered notifications
            for (int i = 0; i < err; i++) {
                struct poll_fd *triggered_poll_fd = rpe_data(&e[i]);
                if (triggered_poll_fd != NULL &&
                        triggered_poll_fd->poll != NULL &&
                        triggered_poll_fd->enabled &&
                        triggered_poll_fd->types & POLL_EDGETRIGGERED) {
                    triggered_poll_fd->triggered_types &= ~rpe_events(&e[i]);
                }
            }

            char fuck;
            if (read(poll_->notify_pipe[0], &fuck, 1) < 0 &&
                    errno != EAGAIN) {
                res = errno_map();
                stop_waiting = true;
            }
        }

        // 先在 poll 锁内消费宿主事件里的登记指针，再在锁外
        // 释放 fd；最后一个引用可能回调 poll_cleanup_fd。
        unlock(&poll_->lock);
        for (size_t index = 0; index < listen_wait_count; index++)
            fd_close(listen_waits[index]);
        free(listen_waits);
        lock(&poll_->lock);
        if (stop_waiting)
            break;
    }

    // release the pipe
    if (--poll_->waiters == 0) {
        close(poll_->notify_pipe[0]);
        close(poll_->notify_pipe[1]);
        poll_->notify_pipe[0] = -1;
        poll_->notify_pipe[1] = -1;
        if (poll_->destroying)
            notify(&poll_->drained);
    }

    unlock(&poll_->lock);
    return res;
}

void poll_destroy(struct poll *poll) {
    lock(&poll->lock);
    assert(!poll->destroying);
    poll->destroying = true;
    // 通知管道按电平触发；销毁中的等待者不会读取这个字节，因此一个
    // 未消费通知会持续唤醒共享宿主后端上的全部等待者。
    poll_notify_waiters_locked(poll);
    while (poll->waiters != 0)
        wait_for_ignore_signals(&poll->drained, &poll->lock, NULL);

    // fd 清理遵循 fd→poll 锁序；销毁路径持有 poll 时只尝试 fd 锁，
    // 失败便退让，让已持有 fd 锁的清理线程先完成。
    while (!list_empty(&poll->poll_fds)) {
        struct poll_fd *poll_fd = list_first_entry(
                &poll->poll_fds, struct poll_fd, fds);
        if (trylock(&poll_fd->fd->poll_lock) != 0) {
            unlock(&poll->lock);
            sched_yield();
            lock(&poll->lock);
            continue;
        }
        list_remove(&poll_fd->polls);
        list_remove(&poll_fd->fds);
        unlock(&poll_fd->fd->poll_lock);
        free(poll_fd);
    }

    struct poll_fd *poll_fd;
    struct poll_fd *tmp;
    list_for_each_entry_safe(&poll->pollfd_freelist, poll_fd, tmp, fds) {
        list_remove(&poll_fd->fds);
        free(poll_fd);
    }

    real_poll_close(&poll->real);
    unlock(&poll->lock);
    cond_destroy(&poll->drained);
    pthread_mutex_destroy(&poll->lock.m);
    free(poll);
}

// Platform-specific real_poll implementations

#if HAVE_EPOLL

static int real_poll_init(struct real_poll *real) {
    real->fd = epoll_create1(0);
    if (real->fd < 0)
        return -1;
    return 0;
}

static int real_poll_wait(struct real_poll *real, struct real_poll_event *events, int max, struct timespec *timeout) {
    int timeout_millis = -1;
    if (timeout != NULL) {
        if (timeout->tv_sec > INT_MAX / 1000) {
            timeout_millis = INT_MAX;
        } else {
            timeout_millis = (int) timeout->tv_sec * 1000;
            int partial_millis = timeout->tv_nsec == 0 ? 0 :
                    1 + (int) ((timeout->tv_nsec - 1) / 1000000);
            if (timeout_millis > INT_MAX - partial_millis)
                timeout_millis = INT_MAX;
            else
                timeout_millis += partial_millis;
        }
    }
    return epoll_wait(real->fd, (struct epoll_event *) events, max, timeout_millis);
}

static int real_poll_update(struct real_poll *real, int fd, int types, void *data) {
    types &= ~EPOLLONESHOT;
    if (types == 0)
        return epoll_ctl(real->fd, EPOLL_CTL_DEL, fd, NULL);
    struct epoll_event epevent = {.events = types, .data.ptr = data};
    int err = epoll_ctl(real->fd, EPOLL_CTL_MOD, fd, &epevent);
    if (err < 0 && errno == ENOENT)
        err = epoll_ctl(real->fd, EPOLL_CTL_ADD, fd, &epevent);
    return err;
}

static void *rpe_data(struct real_poll_event *rpe) {
    return rpe->real.data.ptr;
}
static int rpe_events(struct real_poll_event *rpe) {
    return rpe->real.events;
}

#elif HAVE_KQUEUE

static int real_poll_init(struct real_poll *real) {
    real->fd = kqueue();
    if (real->fd < 0)
        return -1;
    return 0;
}

static int real_poll_update(struct real_poll *real, int fd, int types, void *data) {
    struct kevent e[3] = {
        {.filter = EVFILT_READ, .flags = types & (POLL_READ | POLL_HUP) ? EV_ADD : EV_DELETE},
        {.filter = EVFILT_WRITE, .flags = types & POLL_WRITE ? EV_ADD : EV_DELETE},
        {.filter = EVFILT_EXCEPT, .flags = types & POLL_ERR ? EV_ADD : EV_DELETE},
    };
    if (!(types & POLL_READ) && types & POLL_HUP) {
        // Set the low water mark really high so we'll only get woken up on a hangup
        e[0].fflags = NOTE_LOWAT;
        e[0].data = INT_MAX;
    }
    for (int i = 0; i < 3; i++) {
        e[i].ident = fd;
        e[i].udata = data;
        e[i].flags |= EV_RECEIPT;
        if (types & POLL_EDGETRIGGERED)
            e[i].flags |= EV_CLEAR;
    }

    int receipt_count = kevent(real->fd, e, 3, e, 3, NULL);
    if (receipt_count < 0)
        return -1;

    for (int index = 0; index < receipt_count; index++) {
        if (!(e[index].flags & EV_ERROR)) {
            errno = EIO;
            return -1;
        }
        int receipt_error = (int) e[index].data;
        if (receipt_error == 0)
            continue;

        bool deleting;
        if (e[index].filter == EVFILT_READ)
            deleting = !(types & (POLL_READ | POLL_HUP));
        else if (e[index].filter == EVFILT_WRITE)
            deleting = !(types & POLL_WRITE);
        else if (e[index].filter == EVFILT_EXCEPT)
            deleting = !(types & POLL_ERR);
        else {
            errno = EIO;
            return -1;
        }
        if (deleting && receipt_error == ENOENT)
            continue;
        errno = receipt_error;
        return -1;
    }
    if (receipt_count != 3) {
        errno = EIO;
        return -1;
    }
    return 0;
}

static int real_poll_wait(struct real_poll *real, struct real_poll_event *events, int max, struct timespec *timeout) {
    return kevent(real->fd, NULL, 0, (struct kevent *) events, max, timeout);
}

static void *rpe_data(struct real_poll_event *rpe) {
    return rpe->real.udata;
}
static int rpe_events(struct real_poll_event *rpe) {
    if (rpe->real.filter == EVFILT_READ) {
        int events = 0;
        if (rpe->real.data > 0)
            events |= POLL_READ;
        if (rpe->real.flags & EV_EOF)
            events |= POLL_HUP;
        return events;
    }
    if (rpe->real.filter == EVFILT_WRITE) return POLL_WRITE;
    if (rpe->real.filter == EVFILT_EXCEPT) return POLL_ERR;
    return 0;
}

#endif

static void real_poll_close(struct real_poll *real) {
    close(real->fd);
}
