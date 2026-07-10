#ifndef GUEST_AARCH64_EXECUTE_H
#define GUEST_AARCH64_EXECUTE_H

#include "emu/cpu.h"
#include "guest/aarch64/decode.h"
#include "guest/memory/tlb.h"

enum aarch64_execute_stop {
    AARCH64_EXECUTE_RETIRED,
    AARCH64_EXECUTE_DATA_FAULT,
    AARCH64_EXECUTE_SYSCALL,
};

struct aarch64_execute_result {
    enum aarch64_execute_stop stop;
    struct guest_memory_fault fault;
};

struct aarch64_execute_result aarch64_execute(struct cpu_state *cpu,
        struct guest_tlb *tlb, const struct aarch64_decoded *instruction);

#endif
