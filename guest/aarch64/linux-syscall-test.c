#include <assert.h>

#include "guest/aarch64/linux-syscall.h"

int main(void) {
    struct cpu_state cpu = {0};
    cpu.x[8] = 93;
    for (unsigned i = 0; i < GUEST_LINUX_SYSCALL_ARGUMENT_COUNT; i++)
        cpu.x[i] = UINT64_C(0x1234567800000000) + i;

    struct guest_linux_syscall syscall;
    aarch64_linux_read_syscall(&cpu, &syscall);
    assert(syscall.number == 93);
    for (unsigned i = 0; i < GUEST_LINUX_SYSCALL_ARGUMENT_COUNT; i++)
        assert(syscall.arguments[i] == UINT64_C(0x1234567800000000) + i);

    aarch64_linux_write_syscall_result(&cpu,
            (qword_t) (sqword_t) -38);
    assert(cpu.x[0] == UINT64_C(0xffffffffffffffda));
    assert(cpu.x[1] == UINT64_C(0x1234567800000001));
    return 0;
}
