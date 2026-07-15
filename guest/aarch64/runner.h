#ifndef GUEST_AARCH64_RUNNER_H
#define GUEST_AARCH64_RUNNER_H

#include "guest/aarch64/backend.h"
#include "guest/aarch64/threaded.h"
#include "guest/memory/tlb.h"

enum aarch64_step_stop {
    AARCH64_STEP_RETIRED,
    AARCH64_STEP_FETCH_FAULT,
    AARCH64_STEP_DATA_FAULT,
    AARCH64_STEP_UNDEFINED,
    AARCH64_STEP_SYSCALL,
};

struct aarch64_step_result {
    enum aarch64_step_stop stop;
    struct guest_memory_fault fault;
    dword_t instruction;
};

struct aarch64_runner {
    struct guest_tlb *tlb;
    enum aarch64_backend backend;
    struct aarch64_threaded_cache threaded_cache;
};

void aarch64_runner_init(struct aarch64_runner *runner,
        struct guest_tlb *tlb);
bool aarch64_runner_init_backend(struct aarch64_runner *runner,
        struct guest_tlb *tlb, enum aarch64_backend backend);
enum aarch64_backend aarch64_runner_backend(
        const struct aarch64_runner *runner);
const struct aarch64_threaded_stats *aarch64_runner_threaded_stats(
        const struct aarch64_runner *runner);
struct aarch64_step_result aarch64_run_one(
        struct aarch64_runner *runner, struct cpu_state *cpu);

#endif
