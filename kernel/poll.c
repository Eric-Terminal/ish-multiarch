#include <string.h>
#include "debug.h"
#include "kernel/fs.h"
#include "fs/fd.h"
#include "fs/poll.h"
#include "kernel/calls.h"
#include "util/timer.h"

#define SELECT_READ (POLL_READ | POLL_HUP | POLL_ERR)
#define SELECT_WRITE (POLL_WRITE | POLL_ERR)
#define SELECT_EX (POLL_PRI)
struct select_context {
    byte_t *requested_readfds;
    byte_t *requested_writefds;
    byte_t *requested_exceptfds;
    byte_t *ready_readfds;
    byte_t *ready_writefds;
    byte_t *ready_exceptfds;
    sqword_t ready_bits;
};
static int select_event_callback(void *context, int types, union poll_fd_info info) {
    struct select_context *c = context;
    bool ready = false;
    if (c->requested_readfds != NULL &&
            bit_test(info.fd, c->requested_readfds) &&
            types & SELECT_READ) {
        bit_set(info.fd, c->ready_readfds);
        c->ready_bits++;
        ready = true;
    }
    if (c->requested_writefds != NULL &&
            bit_test(info.fd, c->requested_writefds) &&
            types & SELECT_WRITE) {
        bit_set(info.fd, c->ready_writefds);
        c->ready_bits++;
        ready = true;
    }
    if (c->requested_exceptfds != NULL &&
            bit_test(info.fd, c->requested_exceptfds) &&
            types & SELECT_EX) {
        bit_set(info.fd, c->ready_exceptfds);
        c->ready_bits++;
        ready = true;
    }
    return ready;
}

sqword_t file_select_task(struct task *task, fd_t nfds,
        byte_t *readfds, byte_t *writefds, byte_t *exceptfds,
        size_t fdset_size, const struct timer_time *deadline) {
    assert(task != NULL && task == current && task->sighand != NULL);
    size_t minimum_fdset_size = nfds <= 0
            ? 0 : ((size_t) nfds + 7) / 8;
    if (nfds < 0 || (rlim_t_) nfds >
            rlimit_task(task, RLIMIT_NOFILE_) ||
            (size_t) nfds > SIZE_MAX / sizeof(struct fd *) ||
            fdset_size < minimum_fdset_size)
        return _EINVAL;

    struct fd **files = nfds == 0 ? NULL :
            calloc((size_t) nfds, sizeof(*files));
    if (nfds != 0 && files == NULL)
        return _ENOMEM;
    byte_t *ready_readfds = readfds == NULL || fdset_size == 0
            ? NULL : calloc(1, fdset_size);
    byte_t *ready_writefds = writefds == NULL || fdset_size == 0
            ? NULL : calloc(1, fdset_size);
    byte_t *ready_exceptfds = exceptfds == NULL || fdset_size == 0
            ? NULL : calloc(1, fdset_size);
    if ((readfds != NULL && fdset_size != 0 && ready_readfds == NULL) ||
            (writefds != NULL && fdset_size != 0 &&
                    ready_writefds == NULL) ||
            (exceptfds != NULL && fdset_size != 0 &&
                    ready_exceptfds == NULL)) {
        free(ready_readfds);
        free(ready_writefds);
        free(ready_exceptfds);
        free(files);
        return _ENOMEM;
    }
    struct poll *poll = poll_create();
    if (IS_ERR(poll)) {
        free(ready_readfds);
        free(ready_writefds);
        free(ready_exceptfds);
        free(files);
        return PTR_ERR(poll);
    }

    sqword_t result = 0;
    for (fd_t fd = 0; fd < nfds; fd++) {
        int events = 0;
        if (readfds != NULL && bit_test(fd, readfds))
            events |= SELECT_READ;
        if (writefds != NULL && bit_test(fd, writefds))
            events |= SELECT_WRITE;
        if (exceptfds != NULL && bit_test(fd, exceptfds))
            events |= SELECT_EX;
        if (events == 0)
            continue;

        files[fd] = f_get_task_retain(task, fd);
        if (files[fd] == NULL) {
            result = _EBADF;
            goto out;
        }
        result = poll_add_fd(poll, files[fd], events,
                (union poll_fd_info) fd);
        if (result < 0)
            goto out;
    }

    struct select_context context = {
        .requested_readfds = readfds,
        .requested_writefds = writefds,
        .requested_exceptfds = exceptfds,
        .ready_readfds = ready_readfds,
        .ready_writefds = ready_writefds,
        .ready_exceptfds = ready_exceptfds,
    };
    result = poll_wait_until(
            poll, select_event_callback, &context, deadline);
    if (result >= 0) {
        if (ready_readfds != NULL)
            memcpy(readfds, ready_readfds, fdset_size);
        if (ready_writefds != NULL)
            memcpy(writefds, ready_writefds, fdset_size);
        if (ready_exceptfds != NULL)
            memcpy(exceptfds, ready_exceptfds, fdset_size);
        result = context.ready_bits;
    }

out:
    poll_destroy(poll);
    for (fd_t fd = 0; fd < nfds; fd++)
        if (files[fd] != NULL)
            fd_close(files[fd]);
    free(ready_readfds);
    free(ready_writefds);
    free(ready_exceptfds);
    free(files);
    return result;
}

static dword_t select_common(fd_t nfds, addr_t readfds_addr, addr_t writefds_addr, addr_t exceptfds_addr, struct timespec *timeout_addr, const char *name) {
    if (nfds < 0 || (rlim_t_) nfds >
            rlimit_task(current, RLIMIT_NOFILE_) ||
            (size_t) nfds > SIZE_MAX / sizeof(struct fd *))
        return _EINVAL;

    size_t fdset_size = ((size_t) nfds + 7) / 8;
    char *readfds = fdset_size == 0 || readfds_addr == 0
            ? NULL : malloc(fdset_size);
    char *writefds = fdset_size == 0 || writefds_addr == 0
            ? NULL : malloc(fdset_size);
    char *exceptfds = fdset_size == 0 || exceptfds_addr == 0
            ? NULL : malloc(fdset_size);
    int result = 0;
    if ((readfds_addr != 0 && fdset_size != 0 && readfds == NULL) ||
            (writefds_addr != 0 && fdset_size != 0 && writefds == NULL) ||
            (exceptfds_addr != 0 && fdset_size != 0 && exceptfds == NULL)) {
        result = _ENOMEM;
        goto out;
    }

    if ((readfds != NULL &&
                    user_read(readfds_addr, readfds, fdset_size)) ||
            (writefds != NULL &&
                    user_read(writefds_addr, writefds, fdset_size)) ||
            (exceptfds != NULL &&
                    user_read(exceptfds_addr, exceptfds, fdset_size))) {
        result = _EFAULT;
        goto out;
    }

    STRACE("%s(%d, 0x%x, 0x%x, 0x%x, 0x%x {%lus %luns}) ",
           name, nfds, readfds_addr, writefds_addr, exceptfds_addr,
           timeout_addr, timeout_addr ? timeout_addr->tv_sec : 0,
           timeout_addr ? timeout_addr->tv_nsec : 0);

    struct timer_time deadline;
    const struct timer_time *deadline_pointer = NULL;
    if (timeout_addr != NULL) {
        if (timeout_addr->tv_sec < 0 || timeout_addr->tv_nsec < 0 ||
                timeout_addr->tv_nsec >= 1000000000) {
            result = _EINVAL;
            goto out;
        }
        deadline = timer_time_add(
                timer_time_from_timespec(timespec_now(CLOCK_MONOTONIC)),
                timer_time_from_timespec(*timeout_addr));
        deadline_pointer = &deadline;
    }
    result = (int) file_select_task(current, nfds,
            (byte_t *) readfds, (byte_t *) writefds,
            (byte_t *) exceptfds, fdset_size, deadline_pointer);
    STRACE("%d end %s ", current->pid, name);
    for (fd_t i = 0; i < nfds; i++) {
        bool read_ready = readfds != NULL && bit_test(i, readfds);
        bool write_ready = writefds != NULL && bit_test(i, writefds);
        bool except_ready = exceptfds != NULL && bit_test(i, exceptfds);
        if (read_ready || write_ready || except_ready) {
            STRACE("%d{%s%s%s} ", i,
                    read_ready ? "r" : "",
                    write_ready ? "w" : "",
                    except_ready ? "x" : "");
        }
    }
    if (result < 0)
        goto out;

    if (fdset_size != 0 && readfds_addr &&
            user_write(readfds_addr, readfds, fdset_size)) {
        result = _EFAULT;
        goto out;
    }
    if (fdset_size != 0 && writefds_addr &&
            user_write(writefds_addr, writefds, fdset_size)) {
        result = _EFAULT;
        goto out;
    }
    if (fdset_size != 0 && exceptfds_addr &&
            user_write(exceptfds_addr, exceptfds, fdset_size))
        result = _EFAULT;

out:
    free(readfds);
    free(writefds);
    free(exceptfds);
    return result;
}

dword_t sys_select(fd_t nfds, addr_t readfds_addr, addr_t writefds_addr, addr_t exceptfds_addr, addr_t timeout_addr) {
    struct timespec timeout_ts = {};
    struct timespec *timeout_ts_addr = NULL;
    if (timeout_addr != 0) {
        struct timeval_ timeout_timeval;
        if (user_get(timeout_addr, timeout_timeval))
            return _EFAULT;
        timeout_ts = convert_timeval(timeout_timeval);
        timeout_ts_addr = &timeout_ts;
    }
    // XXX we do not implement the Linux behavior of writing the timeout with remaining time here.
    return select_common(nfds, readfds_addr, writefds_addr, exceptfds_addr, timeout_ts_addr, "select");
}

struct poll_context {
    struct pollfd_ *polls;
    struct fd **files;
    size_t nfds;
};
#define POLL_ALWAYS_LISTENING (POLL_ERR|POLL_HUP|POLL_NVAL)
static int poll_event_callback(void *context, int types, union poll_fd_info info) {
    struct poll_context *c = context;
    struct pollfd_ *polls = c->polls;
    size_t nfds = c->nfds;
    int res = 0;
    for (size_t i = 0; i < nfds; i++) {
        if (c->files[i] == info.ptr) {
            polls[i].revents = types & (polls[i].events | POLL_ALWAYS_LISTENING);
            if (polls[i].revents != 0)
                res = 1;
        }
    }
    return res;
}

sqword_t file_poll_task(struct task *task, struct pollfd_ *polls,
        size_t nfds, struct timespec *timeout) {
    assert(task != NULL && task == current && task->sighand != NULL);
    struct fd **files = calloc(nfds, sizeof(*files));
    if (nfds != 0 && files == NULL)
        return _ENOMEM;
    struct poll *poll = poll_create();
    if (IS_ERR(poll)) {
        free(files);
        return PTR_ERR(poll);
    }

    for (size_t i = 0; i < nfds; i++) {
        files[i] = polls[i].fd < 0 ? NULL :
                f_get_task_retain(task, polls[i].fd);
        // revents 在构建阶段复用为“已合并”标记。
        polls[i].revents = 0;
    }

    int error = 0;
    for (size_t i = 0; i < nfds; i++) {
        if (polls[i].fd < 0 || polls[i].revents)
            continue;

        int events = polls[i].events;
        polls[i].revents = 1;
        if (files[i] == NULL)
            continue;
        for (size_t j = 0; j < nfds; j++) {
            if (polls[j].revents)
                continue;
            if (files[i] == files[j]) {
                events |= polls[j].events;
                polls[j].revents = 1;
            }
        }

        error = poll_add_fd(poll, files[i],
                events | POLL_ALWAYS_LISTENING,
                (union poll_fd_info) (void *) files[i]);
        if (error < 0)
            goto out;
    }

    int invalid = 0;
    for (size_t i = 0; i < nfds; i++) {
        polls[i].revents = 0;
        if (polls[i].fd >= 0 && files[i] == NULL) {
            polls[i].revents = POLL_NVAL;
            invalid++;
        }
    }
    struct poll_context context = {polls, files, nfds};
    struct timespec immediate = {0};
    if (invalid != 0)
        timeout = &immediate;
    error = poll_wait(poll, poll_event_callback, &context, timeout);
    if (error >= 0) {
        error = 0;
        for (size_t i = 0; i < nfds; i++)
            if (polls[i].revents != 0)
                error++;
    }

out:
    poll_destroy(poll);
    for (size_t i = 0; i < nfds; i++)
        if (files[i] != NULL)
            fd_close(files[i]);
    free(files);
    return error;
}

dword_t sys_poll(addr_t fds, dword_t nfds, int_t timeout) {
    STRACE("poll(0x%x, %d, %d)", fds, nfds, timeout);
    if ((qword_t) nfds > rlimit_task(current, RLIMIT_NOFILE_) ||
            nfds > UINT32_MAX / sizeof(struct pollfd_))
        return (dword_t) _EINVAL;
    struct pollfd_ *polls = malloc(sizeof(*polls) * nfds);
    if (nfds != 0 && polls == NULL)
        return (dword_t) _ENOMEM;
    if (nfds != 0 && user_read(fds, polls, sizeof(*polls) * nfds)) {
        free(polls);
        return (dword_t) _EFAULT;
    }
    for (unsigned i = 0; i < nfds; i++)
        STRACE(" {%d, %#x}", polls[i].fd, polls[i].events);
    STRACE("...\n");

    struct timespec timeout_ts;
    if (timeout >= 0) {
        timeout_ts.tv_sec = timeout / 1000;
        timeout_ts.tv_nsec = (timeout % 1000) * 1000000;
    }
    sqword_t res = file_poll_task(current, polls, nfds,
            timeout < 0 ? NULL : &timeout_ts);
    STRACE("%d end poll", current->pid);
    for (unsigned i = 0; i < nfds; i++)
        STRACE(" {%d, %#x}", polls[i].fd, polls[i].revents);

    if (res < 0) {
        free(polls);
        return (dword_t) res;
    }
    if (nfds != 0 && user_write(fds, polls, sizeof(*polls) * nfds)) {
        free(polls);
        return (dword_t) _EFAULT;
    }
    free(polls);
    return (dword_t) res;
}

dword_t sys_pselect(fd_t nfds, addr_t readfds_addr, addr_t writefds_addr, addr_t exceptfds_addr, addr_t timeout_addr, addr_t sigmask_addr) {
    struct timespec_ timeout_timespec;
    struct timespec timeout_ts;
    struct timespec *timeout_ts_addr = NULL;
    if (timeout_addr != 0) {
        if (user_get(timeout_addr, timeout_timespec))
            return _EFAULT;
        timeout_ts = convert_timespec(timeout_timespec);
        timeout_ts_addr = &timeout_ts;
    }
    // a system call can only take 6 parameters, so the last two need to be passed as a pointer to a struct
    struct {
        addr_t mask_addr;
        dword_t mask_size;
    } sigmask = {0};
    if (sigmask_addr != 0 && user_get(sigmask_addr, sigmask))
        return _EFAULT;
    sigset_t_ mask;

    bool has_mask = sigmask.mask_addr != 0;
    if (has_mask) {
        if (sigmask.mask_size != sizeof(sigset_t_))
            return _EINVAL;
        if (user_get(sigmask.mask_addr, mask))
            return _EFAULT;
        sigmask_set_temp(mask);
    }
    dword_t result = select_common(nfds, readfds_addr, writefds_addr,
            exceptfds_addr, timeout_ts_addr, "pselect");
    if (has_mask && result != (dword_t) _EINTR)
        sigmask_restore_temp_task(current);
    return result;
}

dword_t sys_ppoll(addr_t fds, dword_t nfds, addr_t timeout_addr, addr_t sigmask_addr, dword_t sigsetsize) {
    int timeout = -1;
    if (timeout_addr != 0) {
        struct timespec_ timeout_timespec;
        if (user_get(timeout_addr, timeout_timespec))
            return _EFAULT;
        timeout = timeout_timespec.sec * 1000 + timeout_timespec.nsec / 1000000;
    }

    sigset_t_ mask;
    bool has_mask = sigmask_addr != 0;
    if (has_mask) {
        if (sigsetsize != sizeof(sigset_t_))
            return _EINVAL;
        if (user_get(sigmask_addr, mask))
            return _EFAULT;
        sigmask_set_temp(mask);
    }

    dword_t result = sys_poll(fds, nfds, timeout);
    if (has_mask && result != (dword_t) _EINTR)
        sigmask_restore_temp_task(current);
    return result;
}
