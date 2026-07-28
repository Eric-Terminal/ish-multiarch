#ifndef KERNEL_EVENTFD_H
#define KERNEL_EVENTFD_H

#include "misc.h"

struct task;

#define EFD_SEMAPHORE_ (1 << 0)

fd_t eventfd_create_task(
        struct task *task, uint_t initial_value, int_t flags);

#endif
