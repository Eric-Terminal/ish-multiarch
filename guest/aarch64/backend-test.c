#include <assert.h>
#include <string.h>

#include "guest/aarch64/runner.h"

#define CODE_PAGE UINT64_C(0x0000456789abc000)
#define DATA_PAGE (CODE_PAGE + GUEST_MEMORY_PAGE_SIZE)

#define INSTRUCTION_NOP UINT32_C(0xd503201f)
#define INSTRUCTION_ADD_X0 UINT32_C(0x91000400)
#define INSTRUCTION_STR_X0 UINT32_C(0xf9000020)
#define INSTRUCTION_LDR_X2 UINT32_C(0xf9400022)
#define INSTRUCTION_ADD_X3 UINT32_C(0x8b000043)
#define INSTRUCTION_SVC UINT32_C(0xd4000001)
#define INSTRUCTION_UNDEFINED UINT32_C(0)

struct test_memory {
    byte_t primary_code[GUEST_MEMORY_PAGE_SIZE];
    byte_t replacement_code[GUEST_MEMORY_PAGE_SIZE];
    byte_t data[GUEST_MEMORY_PAGE_SIZE];
    byte_t *code_mapping;
    unsigned code_permissions;
    bool code_mapped;
};

struct test_fixture {
    struct test_memory memory;
    struct guest_address_space space;
    struct guest_tlb tlb;
};

static enum guest_memory_fault_kind resolve_test_page(void *opaque,
        guest_addr_t page_base, enum guest_memory_access access,
        struct guest_page_view *view) {
    struct test_memory *memory = opaque;
    use(access);
    if (page_base == CODE_PAGE && memory->code_mapped) {
        *view = (struct guest_page_view) {
            .host_page = memory->code_mapping,
            .permissions = memory->code_permissions,
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

static void init_fixture(struct test_fixture *fixture) {
    *fixture = (struct test_fixture) {0};
    fixture->memory.code_mapping = fixture->memory.primary_code;
    fixture->memory.code_permissions = GUEST_MEMORY_READ |
            GUEST_MEMORY_WRITE | GUEST_MEMORY_EXECUTE;
    fixture->memory.code_mapped = true;
    guest_address_space_init(
            &fixture->space, &test_ops, &fixture->memory, 48);
    guest_tlb_init(&fixture->tlb, &fixture->space);
}

#if defined(__aarch64__)
static void encode_instruction(byte_t bytes[4], dword_t instruction) {
    bytes[0] = (byte_t) instruction;
    bytes[1] = (byte_t) (instruction >> 8);
    bytes[2] = (byte_t) (instruction >> 16);
    bytes[3] = (byte_t) (instruction >> 24);
}

static void write_instruction(struct guest_tlb *tlb,
        guest_addr_t address, dword_t instruction) {
    byte_t bytes[4];
    encode_instruction(bytes, instruction);
    struct guest_memory_fault fault;
    assert(guest_tlb_write(tlb, address, bytes, sizeof(bytes), &fault));
}

static void put_instruction(byte_t *destination, dword_t instruction) {
    encode_instruction(destination, instruction);
}

static void assert_step_equal(const struct aarch64_step_result *left,
        const struct aarch64_step_result *right) {
    assert(left->stop == right->stop);
    assert(left->instruction == right->instruction);
    assert(left->fault.address == right->fault.address);
    assert(left->fault.access == right->fault.access);
    assert(left->fault.kind == right->fault.kind);
}

static void assert_cpu_equal(const struct cpu_state *left,
        const struct cpu_state *right) {
    assert(left->mmu == NULL);
    assert(right->mmu == NULL);
    assert(left->cycle == right->cycle);
    for (unsigned index = 0; index < array_size(left->x); index++)
        assert(left->x[index] == right->x[index]);
    assert(left->sp == right->sp);
    assert(left->pc == right->pc);
    assert(left->nzcv == right->nzcv);
    for (unsigned index = 0; index < array_size(left->v); index++) {
        assert(left->v[index].d[0] == right->v[index].d[0]);
        assert(left->v[index].d[1] == right->v[index].d[1]);
    }
    assert(left->fpcr == right->fpcr);
    assert(left->fpsr == right->fpsr);
    assert(left->tpidr_el0 == right->tpidr_el0);
    assert(!left->exclusive.valid);
    assert(!right->exclusive.valid);
    assert(left->segfault_addr == right->segfault_addr);
    assert(left->segfault_was_write == right->segfault_was_write);
    assert(left->trapno == right->trapno);
    assert(left->single_step == right->single_step);
    assert(left->poked_ptr == NULL);
    assert(right->poked_ptr == NULL);
    assert(left->_poked == right->_poked);
}

static void assert_memory_equal(const struct test_memory *left,
        const struct test_memory *right) {
    assert(memcmp(left->primary_code, right->primary_code,
            sizeof(left->primary_code)) == 0);
    assert(memcmp(left->replacement_code, right->replacement_code,
            sizeof(left->replacement_code)) == 0);
    assert(memcmp(left->data, right->data, sizeof(left->data)) == 0);
    assert(left->code_permissions == right->code_permissions);
    assert(left->code_mapped == right->code_mapped);
}
#endif

static void assert_stats(const struct aarch64_runner *runner,
        qword_t hits, qword_t misses, qword_t fallbacks) {
    const struct aarch64_threaded_stats *stats =
            aarch64_runner_threaded_stats(runner);
    assert(stats->cache_hits == hits);
    assert(stats->cache_misses == misses);
    assert(stats->c_fallbacks == fallbacks);
}

#if defined(__aarch64__)
static struct aarch64_step_result run_at(struct aarch64_runner *runner,
        struct cpu_state *cpu, guest_addr_t pc) {
    cpu->pc = pc;
    return aarch64_run_one(runner, cpu);
}

static void init_differential_cpu(struct cpu_state *cpu) {
    *cpu = (struct cpu_state) {
        .cycle = 9,
        .sp = DATA_PAGE + 0x300,
        .pc = CODE_PAGE,
        .nzcv = UINT32_C(0xa0000000),
        .fpcr = AARCH64_FPCR_FZ,
        .fpsr = AARCH64_FPSR_IXC,
        .tpidr_el0 = UINT64_C(0x1122334455667788),
        .segfault_addr = UINT64_C(0x778899aabbccddee),
        .segfault_was_write = true,
        .trapno = UINT32_C(0x12345678),
        .single_step = true,
        ._poked = true,
    };
    cpu->x[0] = 7;
    cpu->x[1] = DATA_PAGE + 0x80;
    cpu->x[10] = UINT64_C(0x1020304050607080);
    cpu->x[30] = UINT64_C(0x8877665544332211);
    cpu->v[0].d[0] = UINT64_C(0x0123456789abcdef);
    cpu->v[0].d[1] = UINT64_C(0xfedcba9876543210);
    cpu->v[31].d[0] = UINT64_C(0x13579bdf2468ace0);
    cpu->v[31].d[1] = UINT64_C(0x02468ace13579bdf);
}

static void write_differential_program(struct test_fixture *fixture) {
    const dword_t instructions[] = {
        INSTRUCTION_NOP,
        INSTRUCTION_ADD_X0,
        INSTRUCTION_STR_X0,
        INSTRUCTION_LDR_X2,
        INSTRUCTION_ADD_X3,
        INSTRUCTION_SVC,
        INSTRUCTION_UNDEFINED,
    };
    for (unsigned index = 0; index < array_size(instructions); index++)
        write_instruction(&fixture->tlb, CODE_PAGE + index * 4,
                instructions[index]);
}
#endif

static void test_backend_selection(void) {
    assert(aarch64_backend_default() == AARCH64_BACKEND_C);
    assert(aarch64_backend_available(AARCH64_BACKEND_C));

    struct test_fixture fixture;
    init_fixture(&fixture);
    struct aarch64_runner runner;
    aarch64_runner_init(&runner, &fixture.tlb);
    assert(aarch64_runner_backend(&runner) == AARCH64_BACKEND_C);
    assert_stats(&runner, 0, 0, 0);

#if defined(__aarch64__)
    assert(aarch64_backend_available(AARCH64_BACKEND_THREADED));
    assert(aarch64_runner_init_backend(&runner, &fixture.tlb,
            AARCH64_BACKEND_THREADED));
    assert(aarch64_runner_backend(&runner) == AARCH64_BACKEND_THREADED);
#else
    assert(!aarch64_backend_available(AARCH64_BACKEND_THREADED));
    assert(!aarch64_runner_init_backend(&runner, &fixture.tlb,
            AARCH64_BACKEND_THREADED));
#endif
}

#if defined(__aarch64__)
static void test_c_and_threaded_differential(void) {
    struct test_fixture c_fixture;
    struct test_fixture threaded_fixture;
    init_fixture(&c_fixture);
    init_fixture(&threaded_fixture);
    write_differential_program(&c_fixture);
    write_differential_program(&threaded_fixture);
    memset(c_fixture.memory.data, 0xa5, sizeof(c_fixture.memory.data));
    memset(threaded_fixture.memory.data, 0xa5,
            sizeof(threaded_fixture.memory.data));

    struct aarch64_runner c_runner;
    struct aarch64_runner threaded_runner;
    assert(aarch64_runner_init_backend(
            &c_runner, &c_fixture.tlb, AARCH64_BACKEND_C));
    assert(aarch64_runner_init_backend(&threaded_runner,
            &threaded_fixture.tlb, AARCH64_BACKEND_THREADED));
    struct cpu_state c_cpu;
    struct cpu_state threaded_cpu;
    init_differential_cpu(&c_cpu);
    init_differential_cpu(&threaded_cpu);

    const enum aarch64_step_stop expected_stops[] = {
        AARCH64_STEP_RETIRED,
        AARCH64_STEP_RETIRED,
        AARCH64_STEP_RETIRED,
        AARCH64_STEP_RETIRED,
        AARCH64_STEP_RETIRED,
        AARCH64_STEP_SYSCALL,
    };
    for (unsigned step = 0; step < array_size(expected_stops); step++) {
        struct aarch64_step_result c_result =
                aarch64_run_one(&c_runner, &c_cpu);
        struct aarch64_step_result threaded_result =
                aarch64_run_one(&threaded_runner, &threaded_cpu);
        assert(c_result.stop == expected_stops[step]);
        assert_step_equal(&c_result, &threaded_result);
        assert_cpu_equal(&c_cpu, &threaded_cpu);
        assert_memory_equal(&c_fixture.memory,
                &threaded_fixture.memory);
    }
    assert(c_cpu.x[0] == 8);
    assert(c_cpu.x[2] == 8);
    assert(c_cpu.x[3] == 16);
    assert(c_fixture.memory.data[0x80] == 8);
    assert_stats(&threaded_runner, 0, 6, 6);

    struct aarch64_step_result c_undefined = run_at(
            &c_runner, &c_cpu, CODE_PAGE + 6 * 4);
    struct aarch64_step_result threaded_undefined = run_at(
            &threaded_runner, &threaded_cpu, CODE_PAGE + 6 * 4);
    assert(c_undefined.stop == AARCH64_STEP_UNDEFINED);
    assert_step_equal(&c_undefined, &threaded_undefined);
    assert_cpu_equal(&c_cpu, &threaded_cpu);
    assert_memory_equal(&c_fixture.memory, &threaded_fixture.memory);
    assert_stats(&threaded_runner, 0, 7, 6);

    struct aarch64_step_result c_result = run_at(
            &c_runner, &c_cpu, CODE_PAGE);
    struct aarch64_step_result threaded_result = run_at(
            &threaded_runner, &threaded_cpu, CODE_PAGE);
    assert_step_equal(&c_result, &threaded_result);
    assert_cpu_equal(&c_cpu, &threaded_cpu);
    assert_memory_equal(&c_fixture.memory, &threaded_fixture.memory);
    assert_stats(&threaded_runner, 1, 7, 7);
}

static void test_cache_keys_and_collision(void) {
    struct test_fixture fixture;
    init_fixture(&fixture);
    const guest_addr_t first = CODE_PAGE + 0x100;
    const guest_addr_t collision = first +
            AARCH64_THREADED_CACHE_SIZE * 4;
    write_instruction(&fixture.tlb, first, INSTRUCTION_NOP);
    write_instruction(&fixture.tlb, collision, INSTRUCTION_NOP);

    struct aarch64_runner runner;
    assert(aarch64_runner_init_backend(
            &runner, &fixture.tlb, AARCH64_BACKEND_THREADED));
    struct cpu_state cpu = {.x[0] = 3};
    assert(run_at(&runner, &cpu, first).stop == AARCH64_STEP_RETIRED);
    assert_stats(&runner, 0, 1, 1);
    assert(run_at(&runner, &cpu, first).stop == AARCH64_STEP_RETIRED);
    assert_stats(&runner, 1, 1, 2);
    assert(run_at(&runner, &cpu, collision).stop == AARCH64_STEP_RETIRED);
    assert(cpu.x[0] == 3);
    assert_stats(&runner, 1, 2, 3);

    write_instruction(&fixture.tlb, collision, INSTRUCTION_ADD_X0);
    assert(run_at(&runner, &cpu, collision).stop == AARCH64_STEP_RETIRED);
    assert(cpu.x[0] == 4);
    assert_stats(&runner, 1, 3, 4);
    assert(run_at(&runner, &cpu, collision).stop == AARCH64_STEP_RETIRED);
    assert(cpu.x[0] == 5);
    assert_stats(&runner, 2, 3, 5);
    assert(run_at(&runner, &cpu, first).stop == AARCH64_STEP_RETIRED);
    assert_stats(&runner, 2, 4, 6);
}

static void test_rwx_self_modifying_code(void) {
    struct test_fixture fixture;
    init_fixture(&fixture);
    const guest_addr_t pc = CODE_PAGE + 0x40;
    struct aarch64_runner runner;
    assert(aarch64_runner_init_backend(
            &runner, &fixture.tlb, AARCH64_BACKEND_THREADED));
    struct cpu_state cpu = {.x[0] = 10};

    write_instruction(&fixture.tlb, pc, INSTRUCTION_NOP);
    assert(run_at(&runner, &cpu, pc).stop == AARCH64_STEP_RETIRED);
    assert_stats(&runner, 0, 1, 1);
    assert(run_at(&runner, &cpu, pc).stop == AARCH64_STEP_RETIRED);
    assert_stats(&runner, 1, 1, 2);

    write_instruction(&fixture.tlb, pc, INSTRUCTION_ADD_X0);
    assert(run_at(&runner, &cpu, pc).stop == AARCH64_STEP_RETIRED);
    assert(cpu.x[0] == 11);
    assert_stats(&runner, 1, 2, 3);

    write_instruction(&fixture.tlb, pc, INSTRUCTION_UNDEFINED);
    qword_t cycle = cpu.cycle;
    assert(run_at(&runner, &cpu, pc).stop == AARCH64_STEP_UNDEFINED);
    assert(cpu.pc == pc);
    assert(cpu.cycle == cycle);
    assert_stats(&runner, 1, 3, 3);
    assert(run_at(&runner, &cpu, pc).stop == AARCH64_STEP_UNDEFINED);
    assert(cpu.pc == pc);
    assert(cpu.cycle == cycle);
    assert_stats(&runner, 2, 3, 3);

    write_instruction(&fixture.tlb, pc, INSTRUCTION_NOP);
    assert(run_at(&runner, &cpu, pc).stop == AARCH64_STEP_RETIRED);
    assert(cpu.x[0] == 11);
    assert_stats(&runner, 2, 4, 4);
}

static void test_mapping_invalidation(void) {
    struct test_fixture fixture;
    init_fixture(&fixture);
    const guest_addr_t pc = CODE_PAGE + 0x80;
    write_instruction(&fixture.tlb, pc, INSTRUCTION_NOP);
    put_instruction(fixture.memory.replacement_code + 0x80,
            INSTRUCTION_NOP);

    struct aarch64_runner runner;
    assert(aarch64_runner_init_backend(
            &runner, &fixture.tlb, AARCH64_BACKEND_THREADED));
    struct cpu_state cpu = {0};
    assert(run_at(&runner, &cpu, pc).stop == AARCH64_STEP_RETIRED);
    assert(run_at(&runner, &cpu, pc).stop == AARCH64_STEP_RETIRED);
    assert_stats(&runner, 1, 1, 2);

    guest_address_space_changed(&fixture.space);
    assert(run_at(&runner, &cpu, pc).stop == AARCH64_STEP_RETIRED);
    assert_stats(&runner, 2, 1, 3);

    fixture.memory.code_permissions =
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE;
    guest_address_space_changed(&fixture.space);
    qword_t cycle = cpu.cycle;
    struct aarch64_step_result result = run_at(&runner, &cpu, pc);
    assert(result.stop == AARCH64_STEP_FETCH_FAULT);
    assert(result.fault.kind == GUEST_MEMORY_FAULT_PERMISSION);
    assert(result.fault.address == pc);
    assert(cpu.pc == pc);
    assert(cpu.cycle == cycle);
    assert_stats(&runner, 2, 1, 3);

    fixture.memory.code_permissions = GUEST_MEMORY_READ |
            GUEST_MEMORY_WRITE | GUEST_MEMORY_EXECUTE;
    guest_address_space_changed(&fixture.space);
    assert(run_at(&runner, &cpu, pc).stop == AARCH64_STEP_RETIRED);
    assert_stats(&runner, 3, 1, 4);

    put_instruction(fixture.memory.replacement_code + 0x80,
            INSTRUCTION_ADD_X0);
    fixture.memory.code_mapping = fixture.memory.replacement_code;
    guest_address_space_changed(&fixture.space);
    assert(run_at(&runner, &cpu, pc).stop == AARCH64_STEP_RETIRED);
    assert(cpu.x[0] == 1);
    assert_stats(&runner, 3, 2, 5);
}

static void test_shared_space_invalidation(void) {
    struct test_fixture fixture;
    init_fixture(&fixture);
    const guest_addr_t pc = CODE_PAGE + 0xc0;
    write_instruction(&fixture.tlb, pc, INSTRUCTION_NOP);

    struct guest_tlb second_tlb;
    guest_tlb_init(&second_tlb, &fixture.space);
    struct aarch64_runner runners[2];
    assert(aarch64_runner_init_backend(
            &runners[0], &fixture.tlb, AARCH64_BACKEND_THREADED));
    assert(aarch64_runner_init_backend(
            &runners[1], &second_tlb, AARCH64_BACKEND_THREADED));
    struct cpu_state cpus[2] = {{0}, {0}};
    for (unsigned index = 0; index < array_size(runners); index++) {
        assert(run_at(&runners[index], &cpus[index], pc).stop ==
                AARCH64_STEP_RETIRED);
        assert(run_at(&runners[index], &cpus[index], pc).stop ==
                AARCH64_STEP_RETIRED);
        assert_stats(&runners[index], 1, 1, 2);
    }

    write_instruction(&fixture.tlb, pc, INSTRUCTION_ADD_X0);
    for (unsigned index = 0; index < array_size(runners); index++) {
        assert(run_at(&runners[index], &cpus[index], pc).stop ==
                AARCH64_STEP_RETIRED);
        assert(cpus[index].x[0] == 1);
        assert_stats(&runners[index], 1, 2, 3);
    }
}
#endif

int main(void) {
    test_backend_selection();
#if defined(__aarch64__)
    test_c_and_threaded_differential();
    test_cache_keys_and_collision();
    test_rwx_self_modifying_code();
    test_mapping_invalidation();
    test_shared_space_invalidation();
#endif
    return 0;
}
