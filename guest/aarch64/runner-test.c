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

static void put_qword(byte_t *destination, qword_t value) {
    for (byte_t i = 0; i < 8; i++)
        destination[i] = (byte_t) (value >> (i * 8));
}

static void test_function_flow(struct test_memory *memory,
        struct aarch64_runner *runner) {
    const guest_addr_t flow = CODE_PAGE + 0x100;
    const guest_addr_t value = DATA_PAGE + 0x80;
    const guest_addr_t stack_top = DATA_PAGE + 0x300;
    const guest_addr_t return_pc = flow + 0x2c;
    const dword_t instructions[] = {
        UINT32_C(0xa9bf7bf3), // STP：保存 x19 与返回地址。
        UINT32_C(0xaa0003f3), // MOV：把参数保存到 x19。
        UINT32_C(0xb4000113), // CBZ：零值跳向失败哨兵。
        UINT32_C(0xf8408434), // LDR：读取数据并写回游标。
        UINT32_C(0xea14029f), // TST：为零值建立条件码。
        UINT32_C(0x54000040), // B.EQ：越过不可达哨兵。
        UINT32_C(0xd4000001), // 条件分支失败时提前停止。
        UINT32_C(0x8b140260), // ADD：合并参数与读取值。
        UINT32_C(0xa8c17bf3), // LDP：恢复 x19 与返回地址。
        UINT32_C(0xd65f03c0), // RET：返回调用点。
        UINT32_C(0xd4000001), // CBZ 错误跳转时提前停止。
        UINT32_C(0xd4000001), // 正常返回后的停止边界。
    };
    for (size_t i = 0; i < array_size(instructions); i++)
        put_instruction(&memory->code[0x100 + i * 4], instructions[i]);
    put_qword(&memory->data[0x80], 0);
    memset(&memory->data[0x2f0], 0, 16);

    const qword_t saved_x19 = UINT64_C(0x8877665544332211);
    struct cpu_state cpu = {
        .x[0] = 7,
        .x[1] = value,
        .x[19] = saved_x19,
        .x[20] = UINT64_MAX,
        .x[30] = return_pc,
        .sp = stack_top,
        .pc = flow,
        .nzcv = UINT32_C(0xf0000000),
    };
    const guest_addr_t retired_pcs[] = {
        flow + 0x04, flow + 0x08, flow + 0x0c,
        flow + 0x10, flow + 0x14, flow + 0x1c,
        flow + 0x20, flow + 0x24, flow + 0x2c,
    };
    for (size_t i = 0; i < array_size(retired_pcs); i++) {
        struct aarch64_step_result result = aarch64_run_one(runner, &cpu);
        assert(result.stop == AARCH64_STEP_RETIRED);
        assert(cpu.pc == retired_pcs[i]);
    }

    struct aarch64_step_result result = aarch64_run_one(runner, &cpu);
    assert(result.stop == AARCH64_STEP_SYSCALL);
    assert(result.instruction == UINT32_C(0xd4000001));
    assert(cpu.cycle == 10);
    assert(cpu.pc == return_pc + 4);
    assert(cpu.sp == stack_top);
    assert(cpu.x[19] == saved_x19);
    assert(cpu.x[30] == return_pc);
    assert(cpu.x[1] == value + 8);
    assert(cpu.x[20] == 0);
    assert(cpu.x[0] == 7);
    assert(cpu.nzcv == UINT32_C(0x40000000));

    byte_t saved_registers[16];
    put_qword(saved_registers, saved_x19);
    put_qword(saved_registers + 8, return_pc);
    assert(memcmp(&memory->data[0x2f0], saved_registers,
            sizeof(saved_registers)) == 0);
}

int main(void) {
    struct test_memory memory = {0};
    put_instruction(&memory.code[0], UINT32_C(0xd503201f));
    put_instruction(&memory.code[4], UINT32_C(0x91000400));
    put_instruction(&memory.code[8], 0);
    put_instruction(&memory.code[12], UINT32_C(0xd4000541));
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

    cpu.pc = CODE_PAGE + 12;
    result = aarch64_run_one(&runner, &cpu);
    assert(result.stop == AARCH64_STEP_SYSCALL);
    assert(result.instruction == UINT32_C(0xd4000541));
    assert(cpu.pc == CODE_PAGE + 16);
    assert(cpu.cycle == 3);

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

    test_function_flow(&memory, &runner);
    return 0;
}
