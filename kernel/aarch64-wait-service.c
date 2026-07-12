#include <assert.h>

#include "guest/aarch64/linux-signal-abi.h"
#include "guest/memory/address-space.h"
#include "kernel/aarch64-resource-wire.h"
#include "kernel/aarch64-wait-service.h"
#include "kernel/calls.h"

#define AARCH64_WAIT_USER_ADDRESS_LIMIT \
    (AARCH64_LINUX_USER_ADDRESS_MAX + UINT64_C(1))

static qword_t wait_result(sqword_t result) {
    return (qword_t) result;
}

static bool wait_output_fits(qword_t address, qword_t size) {
    return address <= AARCH64_WAIT_USER_ADDRESS_LIMIT &&
            size <= AARCH64_WAIT_USER_ADDRESS_LIMIT - address;
}

static qword_t wait_output_range_error(
        struct guest_linux_user_fault *fault, qword_t address) {
    *fault = (struct guest_linux_user_fault) {
        .address = address,
        .access = GUEST_MEMORY_WRITE,
        .kind = GUEST_MEMORY_FAULT_ADDRESS_SIZE,
    };
    return wait_result(_EFAULT);
}

qword_t aarch64_linux_dispatch_wait4(
        const struct guest_linux_syscall_context *context,
        const struct guest_linux_syscall *syscall,
        struct guest_linux_user_fault *fault) {
    struct wait4_result result;
    sdword_t waited = do_wait4(
            (pid_t_) (sdword_t) (dword_t) syscall->arguments[0],
            (dword_t) syscall->arguments[2], &result);
    if (waited <= 0)
        return wait_result(waited);

    qword_t status_address = syscall->arguments[1];
    if (status_address != 0) {
        if (!wait_output_fits(status_address, sizeof(result.status)))
            return wait_output_range_error(fault, status_address);
        assert(context->user.write != NULL);
        if (!context->user.write(context->user.opaque,
                status_address, &result.status,
                sizeof(result.status), fault))
            return wait_result(_EFAULT);
    }

    qword_t rusage_address = syscall->arguments[3];
    if (rusage_address != 0) {
        struct aarch64_linux_rusage wire =
                aarch64_linux_pack_rusage(&result.rusage);
        if (!wait_output_fits(rusage_address, sizeof(wire)))
            return wait_output_range_error(fault, rusage_address);
        assert(context->user.write != NULL);
        if (!context->user.write(context->user.opaque,
                rusage_address, &wire, sizeof(wire), fault))
            return wait_result(_EFAULT);
    }
    return (qword_t) waited;
}
