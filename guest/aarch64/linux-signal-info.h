#ifndef GUEST_AARCH64_LINUX_SIGNAL_INFO_H
#define GUEST_AARCH64_LINUX_SIGNAL_INFO_H

#include "guest/aarch64/linux-signal-abi.h"
#include "guest/linux/signal-service.h"

struct aarch64_linux_siginfo aarch64_linux_pack_siginfo(
        const struct guest_linux_signal_info *info);
struct guest_linux_signal_info aarch64_linux_unpack_sigqueueinfo(
        int signal, const struct aarch64_linux_siginfo *wire);

#endif
