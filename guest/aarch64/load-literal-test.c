#include <assert.h>
#include <string.h>

#include "guest/aarch64/runner.h"

#define CODE_PAGE UINT64_C(0x0000234567800000)
#define NEXT_PAGE (CODE_PAGE + GUEST_MEMORY_PAGE_SIZE)
#define DENIED_PAGE (NEXT_PAGE + GUEST_MEMORY_PAGE_SIZE)
#define UNMAPPED_PAGE (DENIED_PAGE + GUEST_MEMORY_PAGE_SIZE)

struct test_page {
    guest_addr_t address;
    byte_t *host_page;
    unsigned permissions;
};

struct test_memory {
    byte_t code[GUEST_MEMORY_PAGE_SIZE];
    byte_t next[GUEST_MEMORY_PAGE_SIZE];
    byte_t denied[GUEST_MEMORY_PAGE_SIZE];
    struct test_page pages[3];
    struct guest_address_space space;
    struct guest_tlb tlb;
};

static enum guest_memory_fault_kind resolve_test_page(void *opaque,
        guest_addr_t page_base, enum guest_memory_access access,
        struct guest_page_view *view) {
    struct test_memory *memory = opaque;
    use(access);
    for (size_t index = 0; index < array_size(memory->pages); index++) {
        if (memory->pages[index].address != page_base)
            continue;
        *view = (struct guest_page_view) {
            .host_page = memory->pages[index].host_page,
            .permissions = memory->pages[index].permissions,
        };
        return GUEST_MEMORY_FAULT_NONE;
    }
    return GUEST_MEMORY_FAULT_UNMAPPED;
}

static const struct guest_address_space_ops test_ops = {
    .resolve_page = resolve_test_page,
};

static void init_test_memory(struct test_memory *memory) {
    *memory = (struct test_memory) {0};
    memory->pages[0] = (struct test_page) {
        .address = CODE_PAGE,
        .host_page = memory->code,
        .permissions = GUEST_MEMORY_READ | GUEST_MEMORY_EXECUTE,
    };
    memory->pages[1] = (struct test_page) {
        .address = NEXT_PAGE,
        .host_page = memory->next,
        .permissions = GUEST_MEMORY_READ,
    };
    memory->pages[2] = (struct test_page) {
        .address = DENIED_PAGE,
        .host_page = memory->denied,
        .permissions = GUEST_MEMORY_EXECUTE,
    };
    guest_address_space_init(&memory->space, &test_ops, memory, 48);
    guest_tlb_init(&memory->tlb, &memory->space);
}

static void put_dword(byte_t *destination, dword_t value) {
    for (byte_t index = 0; index < 4; index++)
        destination[index] = (byte_t) (value >> (index * 8));
}

static void put_qword(byte_t *destination, qword_t value) {
    for (byte_t index = 0; index < 8; index++)
        destination[index] = (byte_t) (value >> (index * 8));
}

static dword_t encode_load_literal(byte_t rt, int64_t displacement) {
    assert((displacement & 3) == 0);
    int64_t immediate = displacement / 4;
    assert(immediate >= -INT64_C(0x40000));
    assert(immediate <= INT64_C(0x3ffff));
    return UINT32_C(0x58000000) |
            (((dword_t) immediate & UINT32_C(0x7ffff)) << 5) |
            rt;
}

static struct aarch64_step_result run_at(struct aarch64_runner *runner,
        struct test_memory *memory, struct cpu_state *cpu,
        guest_addr_t pc, dword_t instruction) {
    size_t offset = (size_t) (pc - CODE_PAGE);
    assert(offset <= sizeof(memory->code) - sizeof(instruction));
    put_dword(memory->code + offset, instruction);
    cpu->pc = pc;
    return aarch64_run_one(runner, cpu);
}

static void assert_retired(struct aarch64_step_result result,
        const struct cpu_state *cpu, guest_addr_t pc) {
    assert(result.stop == AARCH64_STEP_RETIRED);
    assert(cpu->pc == pc + 4);
}

static void test_decode(void) {
    for (byte_t rt = 0; rt < 32; rt++) {
        struct aarch64_decoded instruction;
        assert(aarch64_decode(UINT32_C(0x58000000) | rt, &instruction));
        assert(instruction.opcode == AARCH64_OP_LOAD_LITERAL);
        assert(instruction.width == 64);
        assert(instruction.operands.load_literal.rt == rt);
        assert(instruction.operands.load_literal.displacement == 0);
    }

    struct aarch64_decoded instruction;
    assert(aarch64_decode(UINT32_C(0x58000090), &instruction));
    assert(instruction.operands.load_literal.rt == 16);
    assert(instruction.operands.load_literal.displacement == 16);
    assert(aarch64_decode(UINT32_C(0x58ffffe3), &instruction));
    assert(instruction.operands.load_literal.rt == 3);
    assert(instruction.operands.load_literal.displacement == -4);
    assert(aarch64_decode(UINT32_C(0x58800000), &instruction));
    assert(instruction.operands.load_literal.displacement == -INT64_C(0x100000));
    assert(aarch64_decode(UINT32_C(0x587fffe0), &instruction));
    assert(instruction.operands.load_literal.displacement == INT64_C(0xffffc));

    static const dword_t unsupported[] = {
        UINT32_C(0x18000000),
        UINT32_C(0x98000000),
        UINT32_C(0xd8000000),
        UINT32_C(0x1c000000),
        UINT32_C(0x5c000000),
        UINT32_C(0x9c000000),
        UINT32_C(0xdc000000),
    };
    for (size_t index = 0; index < array_size(unsupported); index++)
        assert(!aarch64_decode(unsupported[index], &instruction));
}

static void test_loads(void) {
    struct test_memory memory;
    init_test_memory(&memory);
    struct aarch64_runner runner;
    assert(aarch64_runner_init_backend(
            &runner, &memory.tlb, AARCH64_BACKEND_C));
    struct cpu_state cpu = {.cycle = 7};

    put_qword(memory.code + 16, UINT64_C(0x8877665544332211));
    struct aarch64_step_result result = run_at(&runner, &memory, &cpu,
            CODE_PAGE, UINT32_C(0x58000090));
    assert_retired(result, &cpu, CODE_PAGE);
    assert(cpu.x[16] == UINT64_C(0x8877665544332211));
    assert(cpu.cycle == 8);

    guest_addr_t negative_pc = CODE_PAGE + 64;
    put_qword(memory.code + 32, UINT64_C(0x1020304050607080));
    result = run_at(&runner, &memory, &cpu, negative_pc,
            encode_load_literal(3, -32));
    assert_retired(result, &cpu, negative_pc);
    assert(cpu.x[3] == UINT64_C(0x1020304050607080));

    put_qword(memory.code + 96, UINT64_C(0xfedcba9876543210));
    cpu.x[0] = UINT64_C(0x0123456789abcdef);
    result = run_at(&runner, &memory, &cpu, CODE_PAGE + 80,
            encode_load_literal(31, 16));
    assert_retired(result, &cpu, CODE_PAGE + 80);
    assert(cpu.x[0] == UINT64_C(0x0123456789abcdef));

    guest_addr_t crossing_pc = NEXT_PAGE - 20;
    byte_t crossing[8];
    put_qword(crossing, UINT64_C(0xa1a2a3a4a5a6a7a8));
    memcpy(memory.code + GUEST_MEMORY_PAGE_SIZE - 4, crossing, 4);
    memcpy(memory.next, crossing + 4, 4);
    result = run_at(&runner, &memory, &cpu, crossing_pc,
            encode_load_literal(9, 16));
    assert_retired(result, &cpu, crossing_pc);
    assert(cpu.x[9] == UINT64_C(0xa1a2a3a4a5a6a7a8));
}

static void test_faults(void) {
    struct test_memory memory;
    init_test_memory(&memory);
    struct aarch64_runner runner;
    assert(aarch64_runner_init_backend(
            &runner, &memory.tlb, AARCH64_BACKEND_C));
    struct cpu_state cpu = {
        .cycle = 19,
        .x[5] = UINT64_C(0x1122334455667788),
    };

    struct aarch64_step_result result = run_at(&runner, &memory, &cpu,
            CODE_PAGE,
            encode_load_literal(5, DENIED_PAGE - CODE_PAGE - 4));
    assert(result.stop == AARCH64_STEP_DATA_FAULT);
    assert(result.fault.kind == GUEST_MEMORY_FAULT_PERMISSION);
    assert(result.fault.address == DENIED_PAGE);
    assert(result.fault.access == GUEST_MEMORY_READ);
    assert(cpu.x[5] == UINT64_C(0x1122334455667788));
    assert(cpu.pc == CODE_PAGE && cpu.cycle == 19);

    cpu.x[5] = UINT64_C(0x8877665544332211);
    result = run_at(&runner, &memory, &cpu, CODE_PAGE,
            encode_load_literal(5, UNMAPPED_PAGE - CODE_PAGE));
    assert(result.stop == AARCH64_STEP_DATA_FAULT);
    assert(result.fault.kind == GUEST_MEMORY_FAULT_UNMAPPED);
    assert(result.fault.address == UNMAPPED_PAGE);
    assert(result.fault.access == GUEST_MEMORY_READ);
    assert(cpu.x[5] == UINT64_C(0x8877665544332211));
    assert(cpu.pc == CODE_PAGE && cpu.cycle == 19);
}

static void test_threaded_fallback(void) {
    if (!aarch64_backend_available(AARCH64_BACKEND_THREADED))
        return;

    struct test_memory c_memory;
    struct test_memory threaded_memory;
    init_test_memory(&c_memory);
    init_test_memory(&threaded_memory);
    put_qword(c_memory.code + 16, UINT64_C(0x0123456789abcdef));
    put_qword(threaded_memory.code + 16, UINT64_C(0x0123456789abcdef));

    struct aarch64_runner c_runner;
    struct aarch64_runner threaded_runner;
    assert(aarch64_runner_init_backend(
            &c_runner, &c_memory.tlb, AARCH64_BACKEND_C));
    assert(aarch64_runner_init_backend(&threaded_runner,
            &threaded_memory.tlb, AARCH64_BACKEND_THREADED));
    struct cpu_state c_cpu = {0};
    struct cpu_state threaded_cpu = {0};

    struct aarch64_step_result c_result = run_at(&c_runner, &c_memory,
            &c_cpu, CODE_PAGE, UINT32_C(0x58000090));
    struct aarch64_step_result threaded_result = run_at(&threaded_runner,
            &threaded_memory, &threaded_cpu,
            CODE_PAGE, UINT32_C(0x58000090));
    assert(c_result.stop == AARCH64_STEP_RETIRED);
    assert(threaded_result.stop == c_result.stop);
    assert(threaded_result.instruction == c_result.instruction);
    assert(memcmp(threaded_cpu.x, c_cpu.x, sizeof(c_cpu.x)) == 0);
    assert(threaded_cpu.x[16] == UINT64_C(0x0123456789abcdef));
    assert(threaded_cpu.pc == c_cpu.pc);
    assert(threaded_cpu.cycle == c_cpu.cycle);
    assert(threaded_cpu.sp == c_cpu.sp);
    assert(threaded_cpu.nzcv == c_cpu.nzcv);

    const struct aarch64_threaded_stats *stats =
            aarch64_runner_threaded_stats(&threaded_runner);
    assert(stats->cache_hits == 0);
    assert(stats->cache_misses == 1);
    assert(stats->fast_dispatches == 0);
    assert(stats->c_fallbacks == 1);
}

int main(void) {
    test_decode();
    test_loads();
    test_faults();
    test_threaded_fallback();
    return 0;
}
