#include "guest/aarch64/linux-syscall.h"

void aarch64_linux_read_syscall(const struct cpu_state *cpu,
        struct guest_linux_syscall *syscall) {
    syscall->number = cpu->x[8];
    for (unsigned i = 0; i < GUEST_LINUX_SYSCALL_ARGUMENT_COUNT; i++)
        syscall->arguments[i] = cpu->x[i];
}

void aarch64_linux_write_syscall_result(struct cpu_state *cpu,
        qword_t result) {
    cpu->x[0] = result;
}
