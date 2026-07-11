#include <assert.h>
#include <string.h>

#include "guest/linux/syscall-service.h"

qword_t guest_linux_service_backend_probe(
        const struct guest_linux_syscall_context *context,
        const struct guest_linux_syscall *syscall,
        struct guest_linux_user_fault *fault);

static bool read_probe(void *opaque, qword_t address,
        void *destination, dword_t size,
        struct guest_linux_user_fault *fault) {
    assert(*(const qword_t *) opaque == UINT64_C(0x0f1e2d3c4b5a6978));
    assert(address == UINT64_C(0xf123456789abcdef));
    assert(size == 3);
    static const byte_t bytes[] = {0x12, 0x34, 0x56};
    memcpy(destination, bytes, sizeof(bytes));
    *fault = (struct guest_linux_user_fault) {
        .address = UINT64_C(0xfedcba9876543000),
        .access = UINT32_C(0x11223344),
        .kind = UINT32_C(0x55667788),
    };
    return false;
}

static bool write_probe(void *opaque, qword_t address,
        const void *source, dword_t size,
        struct guest_linux_user_fault *fault) {
    use(fault);
    assert(*(const qword_t *) opaque == UINT64_C(0x0f1e2d3c4b5a6978));
    assert(address == UINT64_C(0xe123456789abcdef));
    static const byte_t expected[] = {0x9a, 0xbc};
    assert(size == sizeof(expected));
    assert(memcmp(source, expected, sizeof(expected)) == 0);
    return true;
}

int main(void) {
    static_assert(sizeof(guest_addr_t) == 8,
            "service 调用方探针必须按 AArch64 guest 类型编译");
    qword_t runtime = UINT64_C(0x1020304050607080);
    qword_t task = UINT64_C(0x8877665544332211);
    qword_t user = UINT64_C(0x0f1e2d3c4b5a6978);
    const struct guest_linux_syscall_context context = {
        .runtime_opaque = &runtime,
        .task_opaque = &task,
        .stack_pointer = UINT64_C(0xfedcba9876543210),
        .user = {
            .opaque = &user,
            .read = read_probe,
            .write = write_probe,
        },
    };
    struct guest_linux_syscall syscall = {
        .number = UINT64_C(0xfedcba9876543210),
    };
    for (unsigned i = 0; i < GUEST_LINUX_SYSCALL_ARGUMENT_COUNT; i++)
        syscall.arguments[i] = UINT64_C(0x8000000000000000) + i;

    struct guest_linux_user_fault fault = {0};
    qword_t result = guest_linux_service_backend_probe(
            &context, &syscall, &fault);
    assert(result == UINT64_C(0xc123456789abcdef));
    assert(fault.address == UINT64_C(0xd123456789abcdef));
    assert(fault.access == UINT32_C(0xa1b2c3d4));
    assert(fault.kind == UINT32_C(0xe5f60718));
    return 0;
}
