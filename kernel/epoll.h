#ifndef KERNEL_EPOLL_H
#define KERNEL_EPOLL_H

#include "misc.h"

struct task;

#define EPOLL_CTL_ADD_ 1
#define EPOLL_CTL_DEL_ 2
#define EPOLL_CTL_MOD_ 3

// 内核层只传递事件值；各 guest ABI 自行编解码对应 wire 布局。
struct epoll_event_value {
    dword_t events;
    dword_t reserved;
    qword_t data;
} __attribute__((aligned(8)));

_Static_assert(sizeof(struct epoll_event_value) == 16 &&
        __builtin_offsetof(struct epoll_event_value, events) == 0 &&
        __builtin_offsetof(struct epoll_event_value, data) == 8,
        "epoll 内核事件值必须保持稳定的 16 字节 host 布局");

fd_t epoll_create_task(struct task *task, int_t flags);
int_t epoll_ctl_task(struct task *task, fd_t epoll_fd,
        int_t operation, fd_t target_fd,
        const struct epoll_event_value *event);
// 成功返回正数时移交 events_out；其余返回值保持为空。
int_t epoll_wait_task(struct task *task, fd_t epoll_fd,
        int_t max_events, int_t timeout,
        struct epoll_event_value **events_out);

#endif
