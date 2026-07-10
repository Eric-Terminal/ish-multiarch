#include "guest/aarch64/runner.h"

void aarch64_runner_init(struct aarch64_runner *runner,
        struct guest_tlb *tlb) {
    runner->tlb = tlb;
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

    struct aarch64_decoded decoded;
    if (!aarch64_decode(result.instruction, &decoded)) {
        result.stop = AARCH64_STEP_UNDEFINED;
        return result;
    }

    aarch64_execute(cpu, &decoded);
    cpu->cycle++;
    return result;
}
