#include <time.h>

#include "fs/fd.h"
#include "kernel/aarch64-resource-wire.h"
#include "kernel/resource.h"

static sqword_t widen_resource_value(dword_t value) {
    return (sqword_t) (sdword_t) value;
}

struct aarch64_linux_rusage aarch64_linux_pack_rusage(
        const struct rusage_ *rusage) {
    return (struct aarch64_linux_rusage) {
        .utime = {
            .sec = widen_resource_value(rusage->utime.sec),
            .usec = widen_resource_value(rusage->utime.usec),
        },
        .stime = {
            .sec = widen_resource_value(rusage->stime.sec),
            .usec = widen_resource_value(rusage->stime.usec),
        },
        .maxrss = widen_resource_value(rusage->maxrss),
        .ixrss = widen_resource_value(rusage->ixrss),
        .idrss = widen_resource_value(rusage->idrss),
        .isrss = widen_resource_value(rusage->isrss),
        .minflt = widen_resource_value(rusage->minflt),
        .majflt = widen_resource_value(rusage->majflt),
        .nswap = widen_resource_value(rusage->nswap),
        .inblock = widen_resource_value(rusage->inblock),
        .oublock = widen_resource_value(rusage->oublock),
        .msgsnd = widen_resource_value(rusage->msgsnd),
        .msgrcv = widen_resource_value(rusage->msgrcv),
        .nsignals = widen_resource_value(rusage->nsignals),
        .nvcsw = widen_resource_value(rusage->nvcsw),
        .nivcsw = widen_resource_value(rusage->nivcsw),
    };
}
