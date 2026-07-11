#include <assert.h>

#include "kernel/aarch64-signal-service.h"
#include "kernel/signal-delivery.h"
#include "kernel/task.h"

static struct guest_linux_signal_poll_result poll_signal(
        const struct guest_linux_signal_context *context,
        guest_linux_signal_installer installer,
        void *installer_opaque) {
    assert(context != NULL);
    struct task *task = context->task_opaque;
    assert(task != NULL && task == current);
    return task_poll_one_signal(task, installer, installer_opaque);
}

const struct guest_linux_signal_service ish_aarch64_linux_signal_service = {
    .poll = poll_signal,
};
