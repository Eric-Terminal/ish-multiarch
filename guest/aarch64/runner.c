#include <assert.h>

#include "guest/aarch64/runner.h"

void aarch64_runner_init(struct aarch64_runner *runner,
        struct guest_tlb *tlb) {
    bool initialized = aarch64_runner_init_backend(
            runner, tlb, aarch64_backend_default());
    assert(initialized);
    (void) initialized;
}

bool aarch64_runner_init_backend(struct aarch64_runner *runner,
        struct guest_tlb *tlb, enum aarch64_backend backend) {
    assert(runner != NULL);
    assert(tlb != NULL);
    if (!aarch64_backend_available(backend))
        return false;
    *runner = (struct aarch64_runner) {
        .tlb = tlb,
        .backend = backend,
    };
    return true;
}

enum aarch64_backend aarch64_runner_backend(
        const struct aarch64_runner *runner) {
    assert(runner != NULL);
    return runner->backend;
}

const struct aarch64_threaded_stats *aarch64_runner_threaded_stats(
        const struct aarch64_runner *runner) {
    assert(runner != NULL);
    return &runner->threaded_cache.stats;
}

static dword_t read_instruction_word(const byte_t bytes[4]) {
    return (dword_t) bytes[0] |
            (dword_t) bytes[1] << 8 |
            (dword_t) bytes[2] << 16 |
            (dword_t) bytes[3] << 24;
}

struct aarch64_step_result aarch64_run_one(
        struct aarch64_runner *runner, struct cpu_state *cpu) {
    struct aarch64_step_result result = {
        .stop = AARCH64_STEP_RETIRED,
        .fault = {
            .address = cpu->pc,
            .access = GUEST_MEMORY_EXECUTE,
            .kind = GUEST_MEMORY_FAULT_NONE,
        },
    };
    if ((cpu->pc & 3) != 0) {
        result.stop = AARCH64_STEP_FETCH_FAULT;
        result.fault.kind = GUEST_MEMORY_FAULT_ALIGNMENT;
        return result;
    }

    byte_t bytes[4];
    if (!guest_tlb_read(runner->tlb, cpu->pc, bytes, sizeof(bytes),
            GUEST_MEMORY_EXECUTE, &result.fault)) {
        result.stop = AARCH64_STEP_FETCH_FAULT;
        return result;
    }
    result.instruction = read_instruction_word(bytes);

    struct aarch64_execute_result execute_result;
    bool defined;
    switch (runner->backend) {
        case AARCH64_BACKEND_C: {
            struct aarch64_decoded decoded;
            defined = aarch64_decode(result.instruction, &decoded);
            if (defined)
                execute_result = aarch64_execute(
                        cpu, runner->tlb, &decoded);
            break;
        }
        case AARCH64_BACKEND_THREADED:
            defined = aarch64_threaded_execute(
                    &runner->threaded_cache, cpu, runner->tlb,
                    cpu->pc, result.instruction, &execute_result);
            break;
        default:
            assert(false);
            defined = false;
            break;
    }
    if (!defined) {
        result.stop = AARCH64_STEP_UNDEFINED;
        return result;
    }

    if (execute_result.stop == AARCH64_EXECUTE_DATA_FAULT) {
        result.stop = AARCH64_STEP_DATA_FAULT;
        result.fault = execute_result.fault;
        return result;
    }
    cpu->cycle++;
    if (execute_result.stop == AARCH64_EXECUTE_SYSCALL)
        result.stop = AARCH64_STEP_SYSCALL;
    return result;
}
