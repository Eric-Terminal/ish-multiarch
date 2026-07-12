#include "kernel/calls.h"
#include "kernel/epoll.h"
#include "fs/poll.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define EPOLL_EVENT_BATCH_LIMIT 256

static struct fd_ops epoll_ops;

fd_t epoll_create_task(struct task *task, int_t flags) {
    if (flags & ~(O_CLOEXEC_))
        return _EINVAL;

    struct fd *fd = adhoc_fd_create(&epoll_ops);
    if (fd == NULL)
        return _ENOMEM;
    struct poll *poll = poll_create();
    if (IS_ERR(poll)) {
        int error = PTR_ERR(poll);
        fd_close(fd);
        return error;
    }
    fd->epollfd.poll = poll;
    return f_install_task(task, fd, flags);
}

fd_t sys_epoll_create(int_t flags) {
    STRACE("epoll_create(%#x)", flags);
    return epoll_create_task(current, flags);
}
fd_t sys_epoll_create0(void) {
    return sys_epoll_create(0);
}

struct i386_epoll_event {
    uint32_t events;
    uint64_t data;
} __attribute__((packed));

_Static_assert(sizeof(struct i386_epoll_event) == 12 &&
        __builtin_offsetof(struct i386_epoll_event, data) == 4,
        "i386 epoll_event ABI 必须保持 12 字节紧凑布局");

int_t epoll_ctl_task(struct task *task, fd_t epoll_f,
        int_t op, fd_t f, const struct epoll_event_value *event) {
    struct fd *epoll = f_get_task_retain(task, epoll_f);
    if (epoll == NULL)
        return _EBADF;
    if (epoll->ops != &epoll_ops) {
        fd_close(epoll);
        return _EINVAL;
    }
    struct fd *fd = f_get_task_retain(task, f);
    if (fd == NULL) {
        fd_close(epoll);
        return _EBADF;
    }

    int result;
    if (fd == epoll) {
        result = _EINVAL;
        goto out;
    }
    if (op == EPOLL_CTL_DEL_) {
        result = poll_del_fd(epoll->epollfd.poll, fd);
        goto out;
    }
    if (op != EPOLL_CTL_ADD_ && op != EPOLL_CTL_MOD_) {
        result = _EINVAL;
        goto out;
    }
    assert(event != NULL);

    if (op == EPOLL_CTL_ADD_) {
        result = poll_add_fd_unique(epoll->epollfd.poll, fd,
                event->events, (union poll_fd_info) event->data);
    } else {
        result = poll_mod_fd(epoll->epollfd.poll, fd,
                event->events, (union poll_fd_info) event->data);
    }

out:
    fd_close(fd);
    fd_close(epoll);
    return result;
}

int_t sys_epoll_ctl(fd_t epoll_f, int_t op, fd_t f, addr_t event_addr) {
    STRACE("epoll_ctl(%d, %d, %d, %#x)", epoll_f, op, f, event_addr);
    struct epoll_event_value event;
    struct epoll_event_value *event_pointer = NULL;
    if (op == EPOLL_CTL_ADD_ || op == EPOLL_CTL_MOD_) {
        struct i386_epoll_event wire;
        if (user_get(event_addr, wire))
            return _EFAULT;
        event = (struct epoll_event_value) {
            .events = wire.events,
            .data = wire.data,
        };
        event_pointer = &event;
        STRACE(" {events: %#x, data: %#llx}",
                event.events, (unsigned long long) event.data);
    }
    return epoll_ctl_task(current, epoll_f, op, f, event_pointer);
}

struct epoll_context {
    struct epoll_event_value *events;
    int n;
    int max_events;
};

static int epoll_callback(void *context, int types, union poll_fd_info info) {
    struct epoll_context *c = context;
    if (c->n >= c->max_events)
        return 0;
    c->events[c->n++] = (struct epoll_event_value) {
        .events = (dword_t) types,
        .data = info.num,
    };
    return 1;
}

int_t epoll_wait_task(struct task *task, fd_t epoll_f,
        int_t max_events, int_t timeout,
        struct epoll_event_value **events_out) {
    assert(events_out != NULL);
    assert(task == current);
    *events_out = NULL;
    struct fd *epoll = f_get_task_retain(task, epoll_f);
    if (epoll == NULL)
        return _EBADF;
    if (epoll->ops != &epoll_ops) {
        fd_close(epoll);
        return _EINVAL;
    }

    struct timespec timeout_ts;
    if (timeout >= 0) {
        timeout_ts.tv_sec = timeout / 1000;
        timeout_ts.tv_nsec = (timeout % 1000) * 1000000;
    }
    if (max_events <= 0) {
        fd_close(epoll);
        return _EINVAL;
    }
    int_t event_capacity = max_events < EPOLL_EVENT_BATCH_LIMIT ?
            max_events : EPOLL_EVENT_BATCH_LIMIT;
    struct epoll_event_value *events = malloc(
            (size_t) event_capacity * sizeof(*events));
    if (events == NULL) {
        fd_close(epoll);
        return _ENOMEM;
    }

    struct epoll_context context = {
        .events = events,
        .n = 0,
        .max_events = event_capacity,
    };
    STRACE("...\n");
    int res = poll_wait(epoll->epollfd.poll, epoll_callback, &context, timeout < 0 ? NULL : &timeout_ts);
    STRACE("%d end epoll_wait", current->pid);
    fd_close(epoll);
    if (res > 0) {
        assert(res == context.n && res <= event_capacity);
        *events_out = events;
    } else {
        free(events);
    }
    return res;
}

int_t sys_epoll_wait(fd_t epoll_f, addr_t events_addr,
        int_t max_events, int_t timeout) {
    STRACE("epoll_wait(%d, %#x, %d, %d)",
            epoll_f, events_addr, max_events, timeout);
    if (max_events <= 0 ||
            max_events > INT_MAX / (int) sizeof(struct i386_epoll_event))
        return _EINVAL;

    struct epoll_event_value *events;
    int_t result = epoll_wait_task(
            current, epoll_f, max_events, timeout, &events);
    if (result <= 0)
        return result;

    for (int index = 0; index < result; index++) {
        STRACE(" {events: %#x, data: %#llx}", events[index].events,
                (unsigned long long) events[index].data);
        struct i386_epoll_event wire = {
            .events = events[index].events,
            .data = events[index].data,
        };
        memcpy((byte_t *) events +
                (size_t) index * sizeof(wire), &wire, sizeof(wire));
    }
    if (user_write(events_addr, events,
            (size_t) result * sizeof(struct i386_epoll_event)))
        result = _EFAULT;
    free(events);
    return result;
}

int_t sys_epoll_pwait(fd_t epoll_f, addr_t events_addr, int_t max_events, int_t timeout, addr_t sigmask_addr, dword_t sigsetsize) {
    sigset_t_ mask;
    bool has_mask = sigmask_addr != 0;
    if (has_mask) {
        if (sigsetsize != sizeof(sigset_t_))
            return _EINVAL;
        if (user_get(sigmask_addr, mask))
            return _EFAULT;
        sigmask_set_temp(mask);
    }

    int_t result = sys_epoll_wait(
            epoll_f, events_addr, max_events, timeout);
    if (has_mask && result != _EINTR)
        sigmask_restore_temp_task(current);
    return result;
}

static int epoll_close(struct fd *fd) {
    if (fd->epollfd.poll != NULL)
        poll_destroy(fd->epollfd.poll);
    return 0;
}

static struct fd_ops epoll_ops = {
    .close = epoll_close,
};
