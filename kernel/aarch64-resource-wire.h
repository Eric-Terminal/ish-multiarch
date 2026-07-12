#ifndef KERNEL_AARCH64_RESOURCE_WIRE_H
#define KERNEL_AARCH64_RESOURCE_WIRE_H

#include "guest/aarch64/linux-resource-abi.h"

struct rusage_;

struct aarch64_linux_rusage aarch64_linux_pack_rusage(
        const struct rusage_ *rusage);

#endif
