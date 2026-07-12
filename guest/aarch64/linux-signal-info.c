#include <assert.h>

#include "guest/aarch64/linux-signal-info.h"

struct aarch64_linux_siginfo aarch64_linux_pack_siginfo(
        const struct guest_linux_signal_info *info) {
    assert(info != NULL);
    struct aarch64_linux_siginfo wire = {
        .signo = info->signal,
        .error = info->error,
        .code = info->code,
    };
    switch (info->payload_kind) {
        case GUEST_LINUX_SIGNAL_PAYLOAD_KILL:
            wire.kill.pid = info->kill.pid;
            wire.kill.uid = info->kill.uid;
            break;
        case GUEST_LINUX_SIGNAL_PAYLOAD_TIMER:
            wire.timer.timer = info->timer.timer;
            wire.timer.overrun = info->timer.overrun;
            wire.timer.value = info->timer.value;
            wire.timer.private_value = info->timer.private_value;
            break;
        case GUEST_LINUX_SIGNAL_PAYLOAD_CHILD:
            wire.child.pid = info->child.pid;
            wire.child.uid = info->child.uid;
            wire.child.status = info->child.status;
            wire.child.utime = info->child.utime;
            wire.child.stime = info->child.stime;
            break;
        case GUEST_LINUX_SIGNAL_PAYLOAD_FAULT:
            wire.fault.address = info->fault.address;
            break;
        case GUEST_LINUX_SIGNAL_PAYLOAD_SIGSYS:
            wire.sigsys.address = info->sigsys.address;
            wire.sigsys.syscall = info->sigsys.syscall;
            wire.sigsys.architecture = info->sigsys.architecture;
            break;
        case GUEST_LINUX_SIGNAL_PAYLOAD_QUEUE:
            wire.queue.pid = info->queue.pid;
            wire.queue.uid = info->queue.uid;
            wire.queue.value = info->queue.value;
            break;
        case GUEST_LINUX_SIGNAL_PAYLOAD_NONE:
        default:
            break;
    }
    return wire;
}

struct guest_linux_signal_info aarch64_linux_unpack_sigqueueinfo(
        int signal, const struct aarch64_linux_siginfo *wire) {
    assert(wire != NULL);
    return (struct guest_linux_signal_info) {
        .signal = signal,
        .error = wire->error,
        .code = wire->code,
        .payload_kind = GUEST_LINUX_SIGNAL_PAYLOAD_QUEUE,
        .queue = {
            .pid = wire->queue.pid,
            .uid = wire->queue.uid,
            .value = wire->queue.value,
        },
    };
}
