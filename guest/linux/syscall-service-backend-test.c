#include <assert.h>
#include <string.h>

#include "guest/linux/syscall-service.h"

qword_t guest_linux_service_backend_probe(
        const struct guest_linux_syscall_context *context,
        const struct guest_linux_syscall *syscall,
        struct guest_linux_user_fault *fault) {
    static_assert(sizeof(guest_addr_t) == 4,
            "service backend 探针必须按 i386 guest 类型编译");
    assert(*(const qword_t *) context->runtime_opaque ==
            UINT64_C(0x1020304050607080));
    assert(*(const qword_t *) context->task_opaque ==
            UINT64_C(0x8877665544332211));
    assert(context->stack_pointer == UINT64_C(0xfedcba9876543210));
    assert(syscall->number == UINT64_C(0xfedcba9876543210));
    assert(syscall->arguments[5] == UINT64_C(0x8000000000000005));

    byte_t input[3] = {0};
    assert(!context->user.read(context->user.opaque,
            UINT64_C(0xf123456789abcdef), input, sizeof(input), fault));
    static const byte_t expected[] = {0x12, 0x34, 0x56};
    assert(memcmp(input, expected, sizeof(input)) == 0);
    assert(fault->address == UINT64_C(0xfedcba9876543000));
    assert(fault->access == UINT32_C(0x11223344));
    assert(fault->kind == UINT32_C(0x55667788));

    static const byte_t output[] = {0x9a, 0xbc};
    assert(context->user.write(context->user.opaque,
            UINT64_C(0xe123456789abcdef), output, sizeof(output), fault));
    *fault = (struct guest_linux_user_fault) {
        .address = UINT64_C(0xd123456789abcdef),
        .access = UINT32_C(0xa1b2c3d4),
        .kind = UINT32_C(0xe5f60718),
    };
    return UINT64_C(0xc123456789abcdef);
}
