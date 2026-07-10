#include <assert.h>

#include "guest/aarch64/linux-syscall.h"
#include "guest/aarch64/runner.h"

#define CODE_PAGE UINT64_C(0x0000456789abc000)

struct test_memory {
    byte_t code[GUEST_MEMORY_PAGE_SIZE];
};

static enum guest_memory_fault_kind resolve_code_page(void *opaque,
        guest_addr_t page_base, enum guest_memory_access access,
        struct guest_page_view *view) {
    struct test_memory *memory = opaque;
    use(access);
    if (page_base != CODE_PAGE)
        return GUEST_MEMORY_FAULT_UNMAPPED;
    *view = (struct guest_page_view) {
        .host_page = memory->code,
        .permissions = GUEST_MEMORY_EXECUTE,
    };
    return GUEST_MEMORY_FAULT_NONE;
}

static const struct guest_address_space_ops test_ops = {
    .resolve_page = resolve_code_page,
};

static void put_instruction(byte_t *destination, dword_t instruction) {
    destination[0] = (byte_t) instruction;
    destination[1] = (byte_t) (instruction >> 8);
    destination[2] = (byte_t) (instruction >> 16);
    destination[3] = (byte_t) (instruction >> 24);
}

int main(void) {
    struct test_memory memory = {0};
    put_instruction(&memory.code[0], UINT32_C(0xd2800540));
    put_instruction(&memory.code[4], UINT32_C(0xd2800ba8));
    put_instruction(&memory.code[8], UINT32_C(0xd4000001));

    struct guest_address_space space;
    guest_address_space_init(&space, &test_ops, &memory, 48);
    struct guest_tlb tlb;
    guest_tlb_init(&tlb, &space);
    struct aarch64_runner runner;
    aarch64_runner_init(&runner, &tlb);
    struct cpu_state cpu = {.pc = CODE_PAGE};

    struct aarch64_step_result result = aarch64_run_one(&runner, &cpu);
    assert(result.stop == AARCH64_STEP_RETIRED);
    result = aarch64_run_one(&runner, &cpu);
    assert(result.stop == AARCH64_STEP_RETIRED);
    result = aarch64_run_one(&runner, &cpu);
    assert(result.stop == AARCH64_STEP_SYSCALL);
    assert(cpu.pc == CODE_PAGE + 12);
    assert(cpu.cycle == 3);

    struct guest_linux_syscall syscall;
    aarch64_linux_read_syscall(&cpu, &syscall);
    assert(syscall.number == 93);
    assert(syscall.arguments[0] == 42);

    aarch64_linux_write_syscall_result(&cpu,
            (qword_t) (sqword_t) -38);
    assert(cpu.x[0] == UINT64_C(0xffffffffffffffda));
    return 0;
}
