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
        case GUEST_LINUX_SIGNAL_PAYLOAD_NONE:
        default:
            break;
    }
    return wire;
}
