#ifndef GUEST_AARCH64_LINUX_SYSCALL_H
#define GUEST_AARCH64_LINUX_SYSCALL_H

#include "emu/cpu.h"
#include "guest/linux/syscall.h"

void aarch64_linux_read_syscall(const struct cpu_state *cpu,
        struct guest_linux_syscall *syscall);
void aarch64_linux_write_syscall_result(struct cpu_state *cpu,
        qword_t result);

#endif
