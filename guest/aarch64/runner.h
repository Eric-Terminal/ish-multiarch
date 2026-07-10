#ifndef GUEST_AARCH64_RUNNER_H
#define GUEST_AARCH64_RUNNER_H

#include "guest/aarch64/execute.h"
#include "guest/memory/tlb.h"

enum aarch64_step_stop {
    AARCH64_STEP_RETIRED,
    AARCH64_STEP_FETCH_FAULT,
    AARCH64_STEP_DATA_FAULT,
    AARCH64_STEP_UNDEFINED,
};

struct aarch64_step_result {
    enum aarch64_step_stop stop;
    struct guest_memory_fault fault;
    dword_t instruction;
};

struct aarch64_runner {
    struct guest_tlb *tlb;
};

void aarch64_runner_init(struct aarch64_runner *runner,
        struct guest_tlb *tlb);
struct aarch64_step_result aarch64_run_one(
        struct aarch64_runner *runner, struct cpu_state *cpu);

#endif
