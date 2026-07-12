#ifndef KERNEL_FUTEX_H
#define KERNEL_FUTEX_H

struct aarch64_linux_process;

int futex_wake(addr_t uaddr, dword_t val);
int futex_wake_aarch64(struct aarch64_linux_process *process,
        qword_t uaddr, dword_t val);

#endif
