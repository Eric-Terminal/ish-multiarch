#include <assert.h>
#include <string.h>

#include "guest/aarch64/runner.h"

#define CODE_PAGE UINT64_C(0x000023456789a000)
#define DATA_PAGE (CODE_PAGE + GUEST_MEMORY_PAGE_SIZE)

struct test_memory {
    byte_t code[GUEST_MEMORY_PAGE_SIZE];
    byte_t data[GUEST_MEMORY_PAGE_SIZE];
};

static enum guest_memory_fault_kind resolve_test_page(void *opaque,
        guest_addr_t page_base, enum guest_memory_access access,
        struct guest_page_view *view) {
    struct test_memory *memory = opaque;
    use(access);
    if (page_base == CODE_PAGE) {
        *view = (struct guest_page_view) {
            .host_page = memory->code,
            .permissions = GUEST_MEMORY_EXECUTE,
        };
        return GUEST_MEMORY_FAULT_NONE;
    }
    if (page_base == DATA_PAGE) {
        *view = (struct guest_page_view) {
            .host_page = memory->data,
            .permissions = GUEST_MEMORY_READ | GUEST_MEMORY_WRITE,
        };
        return GUEST_MEMORY_FAULT_NONE;
    }
    return GUEST_MEMORY_FAULT_UNMAPPED;
}

static const struct guest_address_space_ops test_ops = {
    .resolve_page = resolve_test_page,
};

static void put_instruction(byte_t *destination, dword_t instruction) {
    destination[0] = (byte_t) instruction;
    destination[1] = (byte_t) (instruction >> 8);
    destination[2] = (byte_t) (instruction >> 16);
    destination[3] = (byte_t) (instruction >> 24);
}

int main(void) {
    struct test_memory memory = {0};
    put_instruction(&memory.code[0], UINT32_C(0xd503201f));
    put_instruction(&memory.code[4], UINT32_C(0x91000400));
    put_instruction(&memory.code[8], 0);
    put_instruction(&memory.code[GUEST_MEMORY_PAGE_SIZE - 4],
            UINT32_C(0xd503201f));

    struct guest_address_space space;
    guest_address_space_init(&space, &test_ops, &memory, 48);
    struct guest_tlb tlb;
    guest_tlb_init(&tlb, &space);
    struct aarch64_runner runner;
    aarch64_runner_init(&runner, &tlb);
    struct cpu_state cpu = {.pc = CODE_PAGE, .x[0] = 7};

    struct aarch64_step_result result = aarch64_run_one(&runner, &cpu);
    assert(result.stop == AARCH64_STEP_RETIRED);
    assert(result.instruction == UINT32_C(0xd503201f));
    assert(cpu.pc == CODE_PAGE + 4);
    assert(cpu.cycle == 1);

    result = aarch64_run_one(&runner, &cpu);
    assert(result.stop == AARCH64_STEP_RETIRED);
    assert(cpu.x[0] == 8);
    assert(cpu.pc == CODE_PAGE + 8);
    assert(cpu.cycle == 2);

    result = aarch64_run_one(&runner, &cpu);
    assert(result.stop == AARCH64_STEP_UNDEFINED);
    assert(result.instruction == 0);
    assert(cpu.pc == CODE_PAGE + 8);
    assert(cpu.cycle == 2);

    cpu.pc = CODE_PAGE + GUEST_MEMORY_PAGE_SIZE - 4;
    result = aarch64_run_one(&runner, &cpu);
    assert(result.stop == AARCH64_STEP_RETIRED);
    assert(cpu.pc == CODE_PAGE + GUEST_MEMORY_PAGE_SIZE);

    cpu.pc = DATA_PAGE;
    result = aarch64_run_one(&runner, &cpu);
    assert(result.stop == AARCH64_STEP_FETCH_FAULT);
    assert(result.fault.kind == GUEST_MEMORY_FAULT_PERMISSION);
    assert(result.fault.address == DATA_PAGE);
    assert(cpu.pc == DATA_PAGE);

    cpu.pc = CODE_PAGE + 2;
    result = aarch64_run_one(&runner, &cpu);
    assert(result.stop == AARCH64_STEP_FETCH_FAULT);
    assert(result.fault.kind == GUEST_MEMORY_FAULT_ALIGNMENT);
    assert(cpu.pc == CODE_PAGE + 2);

    cpu.pc = CODE_PAGE + 2 * GUEST_MEMORY_PAGE_SIZE;
    result = aarch64_run_one(&runner, &cpu);
    assert(result.stop == AARCH64_STEP_FETCH_FAULT);
    assert(result.fault.kind == GUEST_MEMORY_FAULT_UNMAPPED);
    assert(result.fault.address == cpu.pc);
    return 0;
}
