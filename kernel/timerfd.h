#ifndef KERNEL_TIMERFD_H
#define KERNEL_TIMERFD_H

#include "fs/fd.h"

struct task;

#define TFD_TIMER_ABSTIME_ (1 << 0)

fd_t timerfd_create_task(
        struct task *task, int_t clockid, int_t flags);
int_t timerfd_settime_task(
        struct task *task, fd_t fd, int_t flags,
        const struct timer_spec *new_spec,
        struct timer_spec *old_spec);
int_t timerfd_gettime_task(
        struct task *task, fd_t fd,
        struct timer_spec *current_spec);

#endif
