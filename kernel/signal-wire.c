#include "kernel/signal-info.h"

int must_check user_write(addr_t address, const void *source, size_t size);

struct i386_siginfo pack_i386_siginfo(const struct siginfo_ *info) {
    struct i386_siginfo wire = {
        .sig = info->sig,
        .sig_errno = info->sig_errno,
        .code = info->code,
    };
    switch (info->payload_kind) {
        case SIGNAL_INFO_PAYLOAD_KILL:
            wire.kill.pid = info->kill.pid;
            wire.kill.uid = info->kill.uid;
            break;
        case SIGNAL_INFO_PAYLOAD_TIMER:
            wire.timer.timer = info->timer.timer;
            wire.timer.overrun = info->timer.overrun;
            wire.timer.value = (dword_t) info->timer.value;
            wire.timer._private = info->timer._private;
            break;
        case SIGNAL_INFO_PAYLOAD_CHILD:
            wire.child.pid = info->child.pid;
            wire.child.uid = info->child.uid;
            wire.child.status = info->child.status;
            wire.child.utime = (sdword_t) info->child.utime;
            wire.child.stime = (sdword_t) info->child.stime;
            break;
        case SIGNAL_INFO_PAYLOAD_FAULT:
            wire.fault.addr = (dword_t) info->fault.addr;
            break;
        case SIGNAL_INFO_PAYLOAD_SIGSYS:
            wire.sigsys.addr = (dword_t) info->sigsys.addr;
            wire.sigsys.syscall = info->sigsys.syscall;
            wire.sigsys.arch = info->sigsys.arch;
            break;
        case SIGNAL_INFO_PAYLOAD_QUEUE:
            wire.queue.pid = info->queue.pid;
            wire.queue.uid = info->queue.uid;
            wire.queue.value = (dword_t) info->queue.value;
            break;
        case SIGNAL_INFO_PAYLOAD_NONE:
        default:
            break;
    }
    return wire;
}

struct siginfo_ unpack_i386_sigqueueinfo(
        int signal, const struct i386_siginfo *wire) {
    return (struct siginfo_) {
        .sig = signal,
        .sig_errno = wire->sig_errno,
        .code = wire->code,
        .payload_kind = SIGNAL_INFO_PAYLOAD_QUEUE,
        .queue = {
            .pid = wire->queue.pid,
            .uid = wire->queue.uid,
            .value = wire->queue.value,
        },
    };
}

int write_i386_siginfo(dword_t address, const struct siginfo_ *info) {
    struct i386_siginfo wire = pack_i386_siginfo(info);
    return user_write(address, &wire, sizeof(wire));
}
