#include <assert.h>
#include <string.h>

#include "guest/aarch64/runner.h"
#include "aarch64-backend-config.h"

#define CODE_PAGE UINT64_C(0x0000456789abc000)
#define DATA_PAGE (CODE_PAGE + GUEST_MEMORY_PAGE_SIZE)

#define INSTRUCTION_NOP UINT32_C(0xd503201f)
#define INSTRUCTION_ADD_X0 UINT32_C(0x91000400)
#define INSTRUCTION_STR_X0 UINT32_C(0xf9000020)
#define INSTRUCTION_LDR_X2 UINT32_C(0xf9400022)
#define INSTRUCTION_ADD_X3 UINT32_C(0x8b000043)
#define INSTRUCTION_SVC UINT32_C(0xd4000001)
#define INSTRUCTION_USHLL_V31 UINT32_C(0x2f20a7ff)
#define INSTRUCTION_LDAR_X2_X1 UINT32_C(0xc8dffc22)
#define INSTRUCTION_STLR_X0_X1 UINT32_C(0xc89ffc20)
#define INSTRUCTION_PRFM_X0 UINT32_C(0xf9800000)
#define INSTRUCTION_STR_Q30_X21_X0 UINT32_C(0x3ca06abe)
#define INSTRUCTION_LD4_V28_X0 UINT32_C(0x4c40081c)
#define INSTRUCTION_USHR_D28_D31_39 UINT32_C(0x7f5907fc)
#define INSTRUCTION_XTN_V28_4H_V28_4S UINT32_C(0x0e612b9c)
#define INSTRUCTION_FCVT_D0_S0 UINT32_C(0x1e22c000)
#define INSTRUCTION_MOV_S15_V31_S3 UINT32_C(0x5e1c07ef)
#define INSTRUCTION_FDIV_S30_S0_S30 UINT32_C(0x1e3e181e)
#define INSTRUCTION_UCVTF_D31_D31 UINT32_C(0x7e61dbff)
#define INSTRUCTION_FCVTZU_X1_D31 UINT32_C(0x9e7903e1)
#define INSTRUCTION_SSHR_V30_2S_V31_2S_8 UINT32_C(0x0f3807fe)
#define INSTRUCTION_LD1_V31_B4_X1 UINT32_C(0x0d40103f)
#define INSTRUCTION_FCSEL_D0_D0_D31_MI UINT32_C(0x1e7f4c00)
#define INSTRUCTION_FRINTM_D0_D0 UINT32_C(0x1e654000)
#define INSTRUCTION_FNEG_D0_D0 UINT32_C(0x1e614000)
#define INSTRUCTION_EXT_V0_V27_V30_8 UINT32_C(0x6e1e4360)
#define INSTRUCTION_MVN_V31_V30 UINT32_C(0x6e205bdf)
#define INSTRUCTION_USHR_V30_2D_V30_2D_6 UINT32_C(0x6f7a07de)
#define INSTRUCTION_ADDP_D31_V29_2D UINT32_C(0x5ef1bbbf)
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
    assert(left->exclusive.address == right->exclusive.address);
    assert(left->exclusive.value_low == right->exclusive.value_low);
    assert(left->exclusive.value_high == right->exclusive.value_high);
    assert((left->exclusive.address_space == NULL) ==
            (right->exclusive.address_space == NULL));
    assert(left->exclusive.mapping_epoch ==
            right->exclusive.mapping_epoch);
    assert(left->exclusive.write_epoch == right->exclusive.write_epoch);
    assert(left->exclusive.sync_identity ==
            right->exclusive.sync_identity);
    assert(left->exclusive.size == right->exclusive.size);
    assert(left->exclusive.pair == right->exclusive.pair);
    assert(left->exclusive.valid == right->exclusive.valid);
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
        qword_t hits, qword_t misses, qword_t fast_dispatches,
        qword_t fallbacks) {
    const struct aarch64_threaded_stats *stats =
            aarch64_runner_threaded_stats(runner);
    assert(stats->cache_hits == hits);
    assert(stats->cache_misses == misses);
    assert(stats->fast_dispatches == fast_dispatches);
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
        INSTRUCTION_USHLL_V31,
        INSTRUCTION_SVC,
        INSTRUCTION_UNDEFINED,
    };
    for (unsigned index = 0; index < array_size(instructions); index++)
        write_instruction(&fixture->tlb, CODE_PAGE + index * 4,
                instructions[index]);
}

static dword_t encode_add_sub_immediate(bool is_64, bool subtract,
        bool set_flags, qword_t immediate, byte_t rn, byte_t rd) {
    bool shift_12 = immediate > UINT16_C(0xfff);
    if (shift_12) {
        assert((immediate & UINT16_C(0xfff)) == 0);
        immediate >>= 12;
    }
    assert(immediate <= UINT16_C(0xfff));
    assert(rn < 32);
    assert(rd < 32);
    return UINT32_C(0x11000000) |
            ((dword_t) is_64 << 31) |
            ((dword_t) subtract << 30) |
            ((dword_t) set_flags << 29) |
            ((dword_t) shift_12 << 22) |
            ((dword_t) immediate << 10) |
            ((dword_t) rn << 5) | rd;
}

static dword_t encode_move_wide(bool is_64, enum aarch64_opcode opcode,
        byte_t halfword, word_t immediate, byte_t rd) {
    byte_t operation;
    if (opcode == AARCH64_OP_MOVN)
        operation = 0;
    else if (opcode == AARCH64_OP_MOVZ)
        operation = 2;
    else {
        assert(opcode == AARCH64_OP_MOVK);
        operation = 3;
    }
    assert(halfword < (is_64 ? 4 : 2));
    assert(rd < 32);
    return UINT32_C(0x12800000) |
            ((dword_t) is_64 << 31) |
            ((dword_t) operation << 29) |
            ((dword_t) halfword << 21) |
            ((dword_t) immediate << 5) | rd;
}

static dword_t encode_branch_immediate(bool link, int64_t displacement) {
    assert((displacement & 3) == 0);
    return UINT32_C(0x14000000) |
            ((dword_t) link << 31) |
            ((dword_t) (displacement / 4) & UINT32_C(0x03ffffff));
}

static dword_t encode_branch_register(enum aarch64_opcode opcode,
        byte_t rn) {
    assert(rn < 31);
    dword_t base;
    if (opcode == AARCH64_OP_BR)
        base = UINT32_C(0xd61f0000);
    else if (opcode == AARCH64_OP_BLR)
        base = UINT32_C(0xd63f0000);
    else {
        assert(opcode == AARCH64_OP_RET);
        base = UINT32_C(0xd65f0000);
    }
    return base | ((dword_t) rn << 5);
}

static dword_t encode_conditional_branch(int64_t displacement,
        byte_t condition) {
    assert((displacement & 3) == 0);
    assert(condition < 16);
    return UINT32_C(0x54000000) |
            (((dword_t) (displacement / 4) & UINT32_C(0x7ffff)) << 5) |
            condition;
}

static dword_t encode_compare_branch(bool is_64, bool nonzero,
        int64_t displacement, byte_t rt) {
    assert((displacement & 3) == 0);
    assert(rt < 32);
    return UINT32_C(0x34000000) |
            ((dword_t) is_64 << 31) |
            ((dword_t) nonzero << 24) |
            (((dword_t) (displacement / 4) & UINT32_C(0x7ffff)) << 5) |
            rt;
}

static dword_t encode_test_branch(bool nonzero, byte_t bit,
        int64_t displacement, byte_t rt) {
    assert(bit < 64);
    assert((displacement & 3) == 0);
    assert(rt < 32);
    return UINT32_C(0x36000000) |
            ((dword_t) (bit >> 5) << 31) |
            ((dword_t) nonzero << 24) |
            ((dword_t) (bit & 0x1f) << 19) |
            (((dword_t) (displacement / 4) & UINT32_C(0x3fff)) << 5) |
            rt;
}
#endif

static void test_backend_selection(void) {
#if ISH_AARCH64_BACKEND_THREADED_DEFAULT
    const enum aarch64_backend expected_default = AARCH64_BACKEND_THREADED;
#else
    const enum aarch64_backend expected_default = AARCH64_BACKEND_C;
#endif
    assert(aarch64_backend_default() == expected_default);
    assert(aarch64_backend_available(AARCH64_BACKEND_C));

    struct test_fixture fixture;
    init_fixture(&fixture);
    struct aarch64_runner runner;
    aarch64_runner_init(&runner, &fixture.tlb);
    assert(aarch64_runner_backend(&runner) == expected_default);
    assert_stats(&runner, 0, 0, 0, 0);

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
static struct cpu_state run_fast_differential(dword_t instruction,
        struct cpu_state initial, enum aarch64_step_stop expected_stop) {
    struct test_fixture c_fixture;
    struct test_fixture threaded_fixture;
    init_fixture(&c_fixture);
    init_fixture(&threaded_fixture);
    write_instruction(&c_fixture.tlb, CODE_PAGE, instruction);
    write_instruction(&threaded_fixture.tlb, CODE_PAGE, instruction);

    struct aarch64_runner c_runner;
    struct aarch64_runner threaded_runner;
    assert(aarch64_runner_init_backend(
            &c_runner, &c_fixture.tlb, AARCH64_BACKEND_C));
    assert(aarch64_runner_init_backend(&threaded_runner,
            &threaded_fixture.tlb, AARCH64_BACKEND_THREADED));
    struct cpu_state c_cpu = initial;
    struct cpu_state threaded_cpu = initial;
    c_cpu.pc = CODE_PAGE;
    threaded_cpu.pc = CODE_PAGE;

    struct aarch64_step_result c_result =
            aarch64_run_one(&c_runner, &c_cpu);
    struct aarch64_step_result threaded_result =
            aarch64_run_one(&threaded_runner, &threaded_cpu);
    assert(c_result.stop == expected_stop);
    assert(c_result.instruction == instruction);
    assert_step_equal(&c_result, &threaded_result);
    assert_cpu_equal(&c_cpu, &threaded_cpu);
    assert_memory_equal(&c_fixture.memory, &threaded_fixture.memory);
    assert_stats(&threaded_runner, 0, 1, 1, 0);
    return threaded_cpu;
}

static void test_fast_data_processing_differential(void) {
    struct cpu_state initial;
    struct cpu_state result;

    init_differential_cpu(&initial);
    aarch64_set_exclusive(&initial, DATA_PAGE + 0x40, 8, false,
            UINT64_C(0x1122), 0, NULL, 3, 5, 7);
    result = run_fast_differential(
            INSTRUCTION_NOP, initial, AARCH64_STEP_RETIRED);
    assert(result.pc == CODE_PAGE + 4);
    assert(result.cycle == initial.cycle + 1);
    assert(result.exclusive.valid);
    assert(result.exclusive.write_epoch == 5);
    assert(result.exclusive.sync_identity == 7);

    init_differential_cpu(&initial);
    result = run_fast_differential(encode_move_wide(true,
            AARCH64_OP_MOVN, 1, UINT16_C(0x1234), 2),
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[2] == ~UINT64_C(0x0000000012340000));

    init_differential_cpu(&initial);
    initial.x[2] = UINT64_MAX;
    result = run_fast_differential(encode_move_wide(false,
            AARCH64_OP_MOVN, 0, 0, 2),
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[2] == UINT64_C(0x00000000ffffffff));

    init_differential_cpu(&initial);
    initial.x[2] = UINT64_MAX;
    result = run_fast_differential(encode_move_wide(true,
            AARCH64_OP_MOVZ, 1, UINT16_C(0x1234), 2),
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[2] == UINT64_C(0x0000000012340000));

    init_differential_cpu(&initial);
    initial.x[2] = UINT64_MAX;
    result = run_fast_differential(encode_move_wide(false,
            AARCH64_OP_MOVZ, 0, UINT16_C(0xabcd), 2),
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[2] == UINT64_C(0x000000000000abcd));

    init_differential_cpu(&initial);
    initial.x[2] = UINT64_C(0x1122334455667788);
    result = run_fast_differential(encode_move_wide(true,
            AARCH64_OP_MOVK, 2, UINT16_C(0xabcd), 2),
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[2] == UINT64_C(0x1122abcd55667788));

    init_differential_cpu(&initial);
    initial.x[2] = UINT64_C(0x1122334455667788);
    result = run_fast_differential(encode_move_wide(false,
            AARCH64_OP_MOVK, 1, UINT16_C(0xbeef), 2),
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[2] == UINT64_C(0x00000000beef7788));

    init_differential_cpu(&initial);
    qword_t old_sp = initial.sp;
    result = run_fast_differential(encode_move_wide(true,
            AARCH64_OP_MOVZ, 0, UINT16_C(0xffff), 31),
            initial, AARCH64_STEP_RETIRED);
    assert(result.sp == old_sp);

    init_differential_cpu(&initial);
    initial.sp = UINT64_C(0x1000);
    result = run_fast_differential(encode_add_sub_immediate(
            true, false, false, UINT16_C(0x20), 31, 31),
            initial, AARCH64_STEP_RETIRED);
    assert(result.sp == UINT64_C(0x1020));
    assert(result.nzcv == initial.nzcv);

    init_differential_cpu(&initial);
    initial.sp = UINT64_C(0x1000);
    result = run_fast_differential(encode_add_sub_immediate(
            true, true, false, UINT16_C(0x20), 31, 31),
            initial, AARCH64_STEP_RETIRED);
    assert(result.sp == UINT64_C(0x0fe0));
    assert(result.nzcv == initial.nzcv);

    init_differential_cpu(&initial);
    initial.x[3] = UINT64_C(0xaaaaaaaa80000000);
    result = run_fast_differential(encode_add_sub_immediate(
            false, false, false, 1, 3, 4),
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[4] == UINT64_C(0x0000000080000001));

    init_differential_cpu(&initial);
    initial.x[3] = UINT64_C(0x100000000);
    result = run_fast_differential(encode_add_sub_immediate(
            true, false, false, UINT64_C(0x123000), 3, 4),
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[4] == UINT64_C(0x100123000));

    init_differential_cpu(&initial);
    initial.x[3] = UINT64_C(0xaaaaaaaaffffffff);
    result = run_fast_differential(encode_add_sub_immediate(
            false, true, false, 1, 3, 4),
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[4] == UINT64_C(0x00000000fffffffe));

    init_differential_cpu(&initial);
    initial.sp = 0;
    result = run_fast_differential(encode_add_sub_immediate(
            true, false, true, 0, 31, 31),
            initial, AARCH64_STEP_RETIRED);
    assert(result.sp == 0);
    assert(result.nzcv == UINT32_C(0x40000000));

    init_differential_cpu(&initial);
    initial.sp = 0;
    result = run_fast_differential(encode_add_sub_immediate(
            true, true, true, 1, 31, 31),
            initial, AARCH64_STEP_RETIRED);
    assert(result.sp == 0);
    assert(result.nzcv == UINT32_C(0x80000000));

    init_differential_cpu(&initial);
    initial.x[3] = UINT32_C(0x7fffffff);
    result = run_fast_differential(encode_add_sub_immediate(
            false, false, true, 1, 3, 4),
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[4] == UINT64_C(0x0000000080000000));
    assert(result.nzcv == UINT32_C(0x90000000));

    init_differential_cpu(&initial);
    initial.x[3] = UINT64_MAX;
    result = run_fast_differential(encode_add_sub_immediate(
            true, false, true, 1, 3, 4),
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[4] == 0);
    assert(result.nzcv == UINT32_C(0x60000000));

    init_differential_cpu(&initial);
    initial.x[3] = 0;
    result = run_fast_differential(encode_add_sub_immediate(
            false, true, true, 1, 3, 4),
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[4] == UINT64_C(0x00000000ffffffff));
    assert(result.nzcv == UINT32_C(0x80000000));

    init_differential_cpu(&initial);
    initial.x[3] = UINT64_C(0x8000000000000000);
    result = run_fast_differential(encode_add_sub_immediate(
            true, true, true, 1, 3, 4),
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[4] == UINT64_C(0x7fffffffffffffff));
    assert(result.nzcv == UINT32_C(0x30000000));
}

static void test_fast_branch_differential(void) {
    struct cpu_state initial;
    struct cpu_state result;

    init_differential_cpu(&initial);
    result = run_fast_differential(
            encode_branch_immediate(false, 20),
            initial, AARCH64_STEP_RETIRED);
    assert(result.pc == CODE_PAGE + 20);

    init_differential_cpu(&initial);
    result = run_fast_differential(
            encode_branch_immediate(false, -12),
            initial, AARCH64_STEP_RETIRED);
    assert(result.pc == CODE_PAGE - 12);

    init_differential_cpu(&initial);
    result = run_fast_differential(
            encode_branch_immediate(true, 24),
            initial, AARCH64_STEP_RETIRED);
    assert(result.pc == CODE_PAGE + 24);
    assert(result.x[30] == CODE_PAGE + 4);

    init_differential_cpu(&initial);
    initial.x[5] = CODE_PAGE + 0x120;
    result = run_fast_differential(
            encode_branch_register(AARCH64_OP_BR, 5),
            initial, AARCH64_STEP_RETIRED);
    assert(result.pc == CODE_PAGE + 0x120);

    init_differential_cpu(&initial);
    initial.x[30] = CODE_PAGE + 0x180;
    result = run_fast_differential(
            encode_branch_register(AARCH64_OP_BLR, 30),
            initial, AARCH64_STEP_RETIRED);
    assert(result.pc == CODE_PAGE + 0x180);
    assert(result.x[30] == CODE_PAGE + 4);

    init_differential_cpu(&initial);
    initial.x[7] = CODE_PAGE + 0x1c0;
    result = run_fast_differential(
            encode_branch_register(AARCH64_OP_RET, 7),
            initial, AARCH64_STEP_RETIRED);
    assert(result.pc == CODE_PAGE + 0x1c0);

    init_differential_cpu(&initial);
    initial.nzcv = UINT32_C(0x40000000);
    result = run_fast_differential(
            encode_conditional_branch(24, 0),
            initial, AARCH64_STEP_RETIRED);
    assert(result.pc == CODE_PAGE + 24);

    init_differential_cpu(&initial);
    initial.nzcv = 0;
    result = run_fast_differential(
            encode_conditional_branch(24, 0),
            initial, AARCH64_STEP_RETIRED);
    assert(result.pc == CODE_PAGE + 4);

    init_differential_cpu(&initial);
    initial.x[3] = UINT64_C(0x1234567800000000);
    result = run_fast_differential(
            encode_compare_branch(false, false, 16, 3),
            initial, AARCH64_STEP_RETIRED);
    assert(result.pc == CODE_PAGE + 16);

    init_differential_cpu(&initial);
    initial.x[3] = UINT64_C(0x1234567800000000);
    result = run_fast_differential(
            encode_compare_branch(true, false, 16, 3),
            initial, AARCH64_STEP_RETIRED);
    assert(result.pc == CODE_PAGE + 4);

    init_differential_cpu(&initial);
    initial.x[4] = 1;
    result = run_fast_differential(
            encode_compare_branch(false, true, 16, 4),
            initial, AARCH64_STEP_RETIRED);
    assert(result.pc == CODE_PAGE + 16);

    init_differential_cpu(&initial);
    initial.x[4] = UINT64_C(0x100000000);
    result = run_fast_differential(
            encode_compare_branch(true, true, 16, 4),
            initial, AARCH64_STEP_RETIRED);
    assert(result.pc == CODE_PAGE + 16);

    init_differential_cpu(&initial);
    result = run_fast_differential(
            encode_compare_branch(true, false, 16, 31),
            initial, AARCH64_STEP_RETIRED);
    assert(result.pc == CODE_PAGE + 16);

    init_differential_cpu(&initial);
    result = run_fast_differential(
            encode_compare_branch(true, true, 16, 31),
            initial, AARCH64_STEP_RETIRED);
    assert(result.pc == CODE_PAGE + 4);

    init_differential_cpu(&initial);
    initial.x[5] = 0;
    result = run_fast_differential(
            encode_test_branch(false, 7, 12, 5),
            initial, AARCH64_STEP_RETIRED);
    assert(result.pc == CODE_PAGE + 12);

    init_differential_cpu(&initial);
    initial.x[5] = UINT64_C(1) << 7;
    result = run_fast_differential(
            encode_test_branch(false, 7, 12, 5),
            initial, AARCH64_STEP_RETIRED);
    assert(result.pc == CODE_PAGE + 4);

    init_differential_cpu(&initial);
    initial.x[6] = UINT64_C(1) << 42;
    result = run_fast_differential(
            encode_test_branch(true, 42, 12, 6),
            initial, AARCH64_STEP_RETIRED);
    assert(result.pc == CODE_PAGE + 12);

    init_differential_cpu(&initial);
    initial.x[6] = 0;
    result = run_fast_differential(
            encode_test_branch(true, 42, 12, 6),
            initial, AARCH64_STEP_RETIRED);
    assert(result.pc == CODE_PAGE + 4);
}

static void test_fast_svc_differential(void) {
    struct test_fixture c_fixture;
    struct test_fixture threaded_fixture;
    init_fixture(&c_fixture);
    init_fixture(&threaded_fixture);
    const dword_t instruction = UINT32_C(0xd4000541);
    write_instruction(&c_fixture.tlb, CODE_PAGE, instruction);
    write_instruction(&threaded_fixture.tlb, CODE_PAGE, instruction);

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
    aarch64_set_exclusive(&c_cpu, DATA_PAGE + 0x100, 8, true,
            UINT64_C(0x1122334455667788),
            UINT64_C(0x99aabbccddeeff00), &c_fixture.space, 7, 11, 13);
    aarch64_set_exclusive(&threaded_cpu, DATA_PAGE + 0x100, 8, true,
            UINT64_C(0x1122334455667788),
            UINT64_C(0x99aabbccddeeff00), &threaded_fixture.space,
            7, 11, 13);

    struct aarch64_step_result c_result =
            aarch64_run_one(&c_runner, &c_cpu);
    struct aarch64_step_result threaded_result =
            aarch64_run_one(&threaded_runner, &threaded_cpu);
    assert(c_result.stop == AARCH64_STEP_SYSCALL);
    assert_step_equal(&c_result, &threaded_result);
    assert_cpu_equal(&c_cpu, &threaded_cpu);
    assert_memory_equal(&c_fixture.memory, &threaded_fixture.memory);
    assert(c_cpu.pc == CODE_PAGE + 4);
    assert(c_cpu.cycle == 10);
    assert(!c_cpu.exclusive.valid);
    assert(c_cpu.exclusive.address == DATA_PAGE + 0x100);
    assert(c_cpu.exclusive.size == 8);
    assert(c_cpu.exclusive.pair);
    assert(c_cpu.exclusive.value_low == UINT64_C(0x1122334455667788));
    assert(c_cpu.exclusive.value_high == UINT64_C(0x99aabbccddeeff00));
    assert(c_cpu.exclusive.address_space == &c_fixture.space);
    assert(threaded_cpu.exclusive.address_space == &threaded_fixture.space);
    assert(c_cpu.exclusive.mapping_epoch == 7);
    assert(c_cpu.exclusive.write_epoch == 11);
    assert(c_cpu.exclusive.sync_identity == 13);
    assert_stats(&threaded_runner, 0, 1, 1, 0);
}

static void test_fast_dispatch_structure(void) {
    struct test_fixture fixture;
    init_fixture(&fixture);
    const dword_t hot_instructions[] = {
        INSTRUCTION_NOP,
        encode_add_sub_immediate(true, false, false, 1, 0, 0),
        encode_add_sub_immediate(true, false, true, 1, 0, 0),
        encode_add_sub_immediate(true, true, false, 1, 0, 0),
        encode_add_sub_immediate(true, true, true, 1, 0, 0),
        encode_move_wide(true, AARCH64_OP_MOVN, 0, 1, 2),
        encode_move_wide(true, AARCH64_OP_MOVZ, 0, 1, 2),
        encode_move_wide(true, AARCH64_OP_MOVK, 0, 1, 2),
        encode_branch_immediate(false, 4),
        encode_branch_immediate(true, 4),
        encode_branch_register(AARCH64_OP_BR, 5),
        encode_branch_register(AARCH64_OP_BLR, 6),
        encode_branch_register(AARCH64_OP_RET, 7),
        encode_conditional_branch(4, 0),
        encode_compare_branch(true, false, 4, 8),
        encode_compare_branch(true, true, 4, 8),
        encode_test_branch(false, 7, 4, 9),
        encode_test_branch(true, 42, 4, 9),
        UINT32_C(0xd4000541),
    };
    _Static_assert(array_size(hot_instructions) == 19,
            "threaded 热点结构门必须覆盖全部 19 个 opcode");
    qword_t seen_opcodes = 0;
    for (unsigned index = 0; index < array_size(hot_instructions); index++) {
        struct aarch64_decoded decoded;
        assert(aarch64_decode(hot_instructions[index], &decoded));
        assert((unsigned) decoded.opcode < 64);
        qword_t opcode_bit = UINT64_C(1) << (unsigned) decoded.opcode;
        assert((seen_opcodes & opcode_bit) == 0);
        seen_opcodes |= opcode_bit;
    }
    for (unsigned index = 0; index < array_size(hot_instructions); index++)
        write_instruction(&fixture.tlb, CODE_PAGE + index * 4,
                hot_instructions[index]);

    struct aarch64_runner runner;
    assert(aarch64_runner_init_backend(
            &runner, &fixture.tlb, AARCH64_BACKEND_THREADED));
    struct cpu_state cpu;
    init_differential_cpu(&cpu);
    cpu.x[5] = CODE_PAGE;
    cpu.x[6] = CODE_PAGE + 4;
    cpu.x[7] = CODE_PAGE + 8;
    cpu.x[8] = 1;
    cpu.x[9] = UINT64_C(1) << 42;

    for (unsigned index = 0; index < array_size(hot_instructions); index++) {
        enum aarch64_step_stop expected =
                index + 1 == array_size(hot_instructions) ?
                AARCH64_STEP_SYSCALL : AARCH64_STEP_RETIRED;
        assert(run_at(&runner, &cpu, CODE_PAGE + index * 4).stop ==
                expected);
    }
    assert_stats(&runner, 0, array_size(hot_instructions),
            array_size(hot_instructions), 0);

    const guest_addr_t fallback_pc =
            CODE_PAGE + array_size(hot_instructions) * 4;
    write_instruction(&fixture.tlb, fallback_pc, INSTRUCTION_LDR_X2);
    cpu.x[1] = DATA_PAGE;
    assert(run_at(&runner, &cpu, fallback_pc).stop ==
            AARCH64_STEP_RETIRED);
    assert_stats(&runner, 0, array_size(hot_instructions) + 1,
            array_size(hot_instructions), 1);

    fixture.memory.data[0] = UINT8_C(0x5a);
    cpu.x[2] = UINT64_MAX;
    assert(run_at(&runner, &cpu, fallback_pc).stop ==
            AARCH64_STEP_RETIRED);
    assert(cpu.x[2] == UINT64_C(0x5a));
    assert_stats(&runner, 1, array_size(hot_instructions) + 1,
            array_size(hot_instructions), 2);

    assert(run_at(&runner, &cpu, CODE_PAGE).stop ==
            AARCH64_STEP_RETIRED);
    assert_stats(&runner, 2, array_size(hot_instructions) + 1,
            array_size(hot_instructions) + 1, 2);
}

static void test_product_c_fallback(void) {
    struct test_fixture c_fixture;
    struct test_fixture threaded_fixture;
    init_fixture(&c_fixture);
    init_fixture(&threaded_fixture);
    write_instruction(&c_fixture.tlb,
            CODE_PAGE, INSTRUCTION_LDAR_X2_X1);
    write_instruction(&c_fixture.tlb,
            CODE_PAGE + 4, INSTRUCTION_STLR_X0_X1);
    write_instruction(&c_fixture.tlb,
            CODE_PAGE + 8, INSTRUCTION_PRFM_X0);
    write_instruction(&c_fixture.tlb,
            CODE_PAGE + 12, INSTRUCTION_STR_Q30_X21_X0);
    write_instruction(&c_fixture.tlb,
            CODE_PAGE + 16, INSTRUCTION_LD4_V28_X0);
    write_instruction(&c_fixture.tlb,
            CODE_PAGE + 20, INSTRUCTION_USHR_D28_D31_39);
    write_instruction(&c_fixture.tlb,
            CODE_PAGE + 24, INSTRUCTION_XTN_V28_4H_V28_4S);
    write_instruction(&c_fixture.tlb,
            CODE_PAGE + 28, INSTRUCTION_FCVT_D0_S0);
    write_instruction(&c_fixture.tlb,
            CODE_PAGE + 32, INSTRUCTION_MOV_S15_V31_S3);
    write_instruction(&c_fixture.tlb,
            CODE_PAGE + 36, INSTRUCTION_FDIV_S30_S0_S30);
    write_instruction(&c_fixture.tlb,
            CODE_PAGE + 40, INSTRUCTION_UCVTF_D31_D31);
    write_instruction(&c_fixture.tlb,
            CODE_PAGE + 44, INSTRUCTION_FCVTZU_X1_D31);
    write_instruction(&c_fixture.tlb,
            CODE_PAGE + 48, INSTRUCTION_SSHR_V30_2S_V31_2S_8);
    write_instruction(&c_fixture.tlb,
            CODE_PAGE + 52, INSTRUCTION_LD1_V31_B4_X1);
    write_instruction(&c_fixture.tlb,
            CODE_PAGE + 56, INSTRUCTION_FCSEL_D0_D0_D31_MI);
    write_instruction(&c_fixture.tlb,
            CODE_PAGE + 60, INSTRUCTION_FRINTM_D0_D0);
    write_instruction(&c_fixture.tlb,
            CODE_PAGE + 64, INSTRUCTION_FNEG_D0_D0);
    write_instruction(&c_fixture.tlb,
            CODE_PAGE + 68, INSTRUCTION_EXT_V0_V27_V30_8);
    write_instruction(&c_fixture.tlb,
            CODE_PAGE + 72, INSTRUCTION_MVN_V31_V30);
    write_instruction(&c_fixture.tlb,
            CODE_PAGE + 76, INSTRUCTION_USHR_V30_2D_V30_2D_6);
    write_instruction(&c_fixture.tlb,
            CODE_PAGE + 80, INSTRUCTION_ADDP_D31_V29_2D);
    write_instruction(&threaded_fixture.tlb,
            CODE_PAGE, INSTRUCTION_LDAR_X2_X1);
    write_instruction(&threaded_fixture.tlb,
            CODE_PAGE + 4, INSTRUCTION_STLR_X0_X1);
    write_instruction(&threaded_fixture.tlb,
            CODE_PAGE + 8, INSTRUCTION_PRFM_X0);
    write_instruction(&threaded_fixture.tlb,
            CODE_PAGE + 12, INSTRUCTION_STR_Q30_X21_X0);
    write_instruction(&threaded_fixture.tlb,
            CODE_PAGE + 16, INSTRUCTION_LD4_V28_X0);
    write_instruction(&threaded_fixture.tlb,
            CODE_PAGE + 20, INSTRUCTION_USHR_D28_D31_39);
    write_instruction(&threaded_fixture.tlb,
            CODE_PAGE + 24, INSTRUCTION_XTN_V28_4H_V28_4S);
    write_instruction(&threaded_fixture.tlb,
            CODE_PAGE + 28, INSTRUCTION_FCVT_D0_S0);
    write_instruction(&threaded_fixture.tlb,
            CODE_PAGE + 32, INSTRUCTION_MOV_S15_V31_S3);
    write_instruction(&threaded_fixture.tlb,
            CODE_PAGE + 36, INSTRUCTION_FDIV_S30_S0_S30);
    write_instruction(&threaded_fixture.tlb,
            CODE_PAGE + 40, INSTRUCTION_UCVTF_D31_D31);
    write_instruction(&threaded_fixture.tlb,
            CODE_PAGE + 44, INSTRUCTION_FCVTZU_X1_D31);
    write_instruction(&threaded_fixture.tlb,
            CODE_PAGE + 48, INSTRUCTION_SSHR_V30_2S_V31_2S_8);
    write_instruction(&threaded_fixture.tlb,
            CODE_PAGE + 52, INSTRUCTION_LD1_V31_B4_X1);
    write_instruction(&threaded_fixture.tlb,
            CODE_PAGE + 56, INSTRUCTION_FCSEL_D0_D0_D31_MI);
    write_instruction(&threaded_fixture.tlb,
            CODE_PAGE + 60, INSTRUCTION_FRINTM_D0_D0);
    write_instruction(&threaded_fixture.tlb,
            CODE_PAGE + 64, INSTRUCTION_FNEG_D0_D0);
    write_instruction(&threaded_fixture.tlb,
            CODE_PAGE + 68, INSTRUCTION_EXT_V0_V27_V30_8);
    write_instruction(&threaded_fixture.tlb,
            CODE_PAGE + 72, INSTRUCTION_MVN_V31_V30);
    write_instruction(&threaded_fixture.tlb,
            CODE_PAGE + 76, INSTRUCTION_USHR_V30_2D_V30_2D_6);
    write_instruction(&threaded_fixture.tlb,
            CODE_PAGE + 80, INSTRUCTION_ADDP_D31_V29_2D);

    const qword_t original = UINT64_C(0x8877665544332211);
    memcpy(c_fixture.memory.data, &original, sizeof(original));
    memcpy(threaded_fixture.memory.data, &original, sizeof(original));
    for (unsigned element = 0; element < 4; element++) {
        for (unsigned structure = 0; structure < 4; structure++) {
            dword_t value = UINT32_C(0x10203040) +
                    element * 0x100 + structure;
            size_t offset = 64 + (element * 4 + structure) * sizeof(value);
            memcpy(c_fixture.memory.data + offset, &value, sizeof(value));
            memcpy(threaded_fixture.memory.data + offset,
                    &value, sizeof(value));
        }
    }

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
    c_cpu.x[0] = threaded_cpu.x[0] =
            UINT64_C(0x0123456789abcdef);
    c_cpu.x[1] = threaded_cpu.x[1] = DATA_PAGE;
    c_cpu.x[2] = threaded_cpu.x[2] = UINT64_MAX;

    struct aarch64_step_result c_result =
            aarch64_run_one(&c_runner, &c_cpu);
    struct aarch64_step_result threaded_result =
            aarch64_run_one(&threaded_runner, &threaded_cpu);
    assert(c_result.stop == AARCH64_STEP_RETIRED);
    assert_step_equal(&c_result, &threaded_result);
    assert_cpu_equal(&c_cpu, &threaded_cpu);
    assert_memory_equal(&c_fixture.memory, &threaded_fixture.memory);
    assert(c_cpu.x[2] == original);
    assert_stats(&threaded_runner, 0, 1, 0, 1);

    c_result = aarch64_run_one(&c_runner, &c_cpu);
    threaded_result = aarch64_run_one(&threaded_runner, &threaded_cpu);
    assert(c_result.stop == AARCH64_STEP_RETIRED);
    assert_step_equal(&c_result, &threaded_result);
    assert_cpu_equal(&c_cpu, &threaded_cpu);
    assert_memory_equal(&c_fixture.memory, &threaded_fixture.memory);
    qword_t stored;
    memcpy(&stored, c_fixture.memory.data, sizeof(stored));
    assert(stored == UINT64_C(0x0123456789abcdef));
    assert_stats(&threaded_runner, 0, 2, 0, 2);

    c_cpu.x[0] = threaded_cpu.x[0] = UINT64_MAX;
    c_result = aarch64_run_one(&c_runner, &c_cpu);
    threaded_result = aarch64_run_one(&threaded_runner, &threaded_cpu);
    assert(c_result.stop == AARCH64_STEP_RETIRED);
    assert_step_equal(&c_result, &threaded_result);
    assert_cpu_equal(&c_cpu, &threaded_cpu);
    assert_memory_equal(&c_fixture.memory, &threaded_fixture.memory);
    assert(c_cpu.x[0] == UINT64_MAX);
    assert_stats(&threaded_runner, 0, 3, 0, 3);

    c_cpu.x[0] = threaded_cpu.x[0] = 32;
    c_cpu.x[21] = threaded_cpu.x[21] = DATA_PAGE;
    c_cpu.v[30].d[0] = threaded_cpu.v[30].d[0] =
            UINT64_C(0x1122334455667788);
    c_cpu.v[30].d[1] = threaded_cpu.v[30].d[1] =
            UINT64_C(0x99aabbccddeeff00);
    c_result = aarch64_run_one(&c_runner, &c_cpu);
    threaded_result = aarch64_run_one(&threaded_runner, &threaded_cpu);
    assert(c_result.stop == AARCH64_STEP_RETIRED);
    assert_step_equal(&c_result, &threaded_result);
    assert_cpu_equal(&c_cpu, &threaded_cpu);
    assert_memory_equal(&c_fixture.memory, &threaded_fixture.memory);
    qword_t stored_low;
    qword_t stored_high;
    memcpy(&stored_low, c_fixture.memory.data + 32,
            sizeof(stored_low));
    memcpy(&stored_high, c_fixture.memory.data + 40,
            sizeof(stored_high));
    assert(stored_low == UINT64_C(0x1122334455667788));
    assert(stored_high == UINT64_C(0x99aabbccddeeff00));
    assert_stats(&threaded_runner, 0, 4, 0, 4);

    c_cpu.x[0] = threaded_cpu.x[0] = DATA_PAGE + 64;
    c_result = aarch64_run_one(&c_runner, &c_cpu);
    threaded_result = aarch64_run_one(&threaded_runner, &threaded_cpu);
    assert(c_result.stop == AARCH64_STEP_RETIRED);
    assert_step_equal(&c_result, &threaded_result);
    assert_cpu_equal(&c_cpu, &threaded_cpu);
    assert_memory_equal(&c_fixture.memory, &threaded_fixture.memory);
    for (unsigned structure = 0; structure < 4; structure++) {
        for (unsigned element = 0; element < 4; element++) {
            assert(c_cpu.v[28 + structure].s[element] ==
                    UINT32_C(0x10203040) +
                            element * 0x100 + structure);
        }
    }
    assert_stats(&threaded_runner, 0, 5, 0, 5);

    c_cpu.v[31].d[0] = threaded_cpu.v[31].d[0] =
            UINT64_C(0xfedcba9876543210);
    c_cpu.v[28].d[0] = threaded_cpu.v[28].d[0] = UINT64_MAX;
    c_cpu.v[28].d[1] = threaded_cpu.v[28].d[1] = UINT64_MAX;
    c_result = aarch64_run_one(&c_runner, &c_cpu);
    threaded_result = aarch64_run_one(&threaded_runner, &threaded_cpu);
    assert(c_result.stop == AARCH64_STEP_RETIRED);
    assert_step_equal(&c_result, &threaded_result);
    assert_cpu_equal(&c_cpu, &threaded_cpu);
    assert_memory_equal(&c_fixture.memory, &threaded_fixture.memory);
    assert(c_cpu.v[28].d[0] ==
            (UINT64_C(0xfedcba9876543210) >> 39));
    assert(c_cpu.v[28].d[1] == 0);
    assert_stats(&threaded_runner, 0, 6, 0, 6);

    c_cpu.v[28].s[0] = threaded_cpu.v[28].s[0] =
            UINT32_C(0x1111aaaa);
    c_cpu.v[28].s[1] = threaded_cpu.v[28].s[1] =
            UINT32_C(0x2222bbbb);
    c_cpu.v[28].s[2] = threaded_cpu.v[28].s[2] =
            UINT32_C(0x3333cccc);
    c_cpu.v[28].s[3] = threaded_cpu.v[28].s[3] =
            UINT32_C(0x4444dddd);
    c_result = aarch64_run_one(&c_runner, &c_cpu);
    threaded_result = aarch64_run_one(&threaded_runner, &threaded_cpu);
    assert(c_result.stop == AARCH64_STEP_RETIRED);
    assert_step_equal(&c_result, &threaded_result);
    assert_cpu_equal(&c_cpu, &threaded_cpu);
    assert_memory_equal(&c_fixture.memory, &threaded_fixture.memory);
    assert(c_cpu.v[28].d[0] == UINT64_C(0xddddccccbbbbaaaa));
    assert(c_cpu.v[28].d[1] == 0);
    assert_stats(&threaded_runner, 0, 7, 0, 7);

    c_cpu.v[0].s[0] = threaded_cpu.v[0].s[0] = UINT32_C(0x3fc00000);
    c_cpu.v[0].s[1] = threaded_cpu.v[0].s[1] = UINT32_MAX;
    c_cpu.v[0].d[1] = threaded_cpu.v[0].d[1] = UINT64_MAX;
    c_result = aarch64_run_one(&c_runner, &c_cpu);
    threaded_result = aarch64_run_one(&threaded_runner, &threaded_cpu);
    assert(c_result.stop == AARCH64_STEP_RETIRED);
    assert_step_equal(&c_result, &threaded_result);
    assert_cpu_equal(&c_cpu, &threaded_cpu);
    assert_memory_equal(&c_fixture.memory, &threaded_fixture.memory);
    assert(c_cpu.v[0].d[0] == UINT64_C(0x3ff8000000000000));
    assert(c_cpu.v[0].d[1] == 0);
    assert_stats(&threaded_runner, 0, 8, 0, 8);

    c_cpu.v[31].s[3] = threaded_cpu.v[31].s[3] =
            UINT32_C(0x55667788);
    c_cpu.v[15].q = threaded_cpu.v[15].q = ~(__uint128_t) 0;
    c_result = aarch64_run_one(&c_runner, &c_cpu);
    threaded_result = aarch64_run_one(&threaded_runner, &threaded_cpu);
    assert(c_result.stop == AARCH64_STEP_RETIRED);
    assert_step_equal(&c_result, &threaded_result);
    assert_cpu_equal(&c_cpu, &threaded_cpu);
    assert_memory_equal(&c_fixture.memory, &threaded_fixture.memory);
    assert(c_cpu.v[15].d[0] == UINT64_C(0x55667788));
    assert(c_cpu.v[15].d[1] == 0);
    assert_stats(&threaded_runner, 0, 9, 0, 9);

    c_cpu.v[0].s[0] = threaded_cpu.v[0].s[0] =
            UINT32_C(0x3f800000);
    c_cpu.v[30].s[0] = threaded_cpu.v[30].s[0] =
            UINT32_C(0x41800000);
    c_cpu.v[30].d[1] = threaded_cpu.v[30].d[1] = UINT64_MAX;
    c_result = aarch64_run_one(&c_runner, &c_cpu);
    threaded_result = aarch64_run_one(&threaded_runner, &threaded_cpu);
    assert(c_result.stop == AARCH64_STEP_RETIRED);
    assert_step_equal(&c_result, &threaded_result);
    assert_cpu_equal(&c_cpu, &threaded_cpu);
    assert_memory_equal(&c_fixture.memory, &threaded_fixture.memory);
    assert(c_cpu.v[30].d[0] == UINT64_C(0x3d800000));
    assert(c_cpu.v[30].d[1] == 0);
    assert_stats(&threaded_runner, 0, 10, 0, 10);

    c_cpu.v[31].d[0] = threaded_cpu.v[31].d[0] = UINT64_C(9);
    c_cpu.v[31].d[1] = threaded_cpu.v[31].d[1] = UINT64_MAX;
    c_result = aarch64_run_one(&c_runner, &c_cpu);
    threaded_result = aarch64_run_one(&threaded_runner, &threaded_cpu);
    assert(c_result.stop == AARCH64_STEP_RETIRED);
    assert_step_equal(&c_result, &threaded_result);
    assert_cpu_equal(&c_cpu, &threaded_cpu);
    assert_memory_equal(&c_fixture.memory, &threaded_fixture.memory);
    assert(c_cpu.v[31].d[0] == UINT64_C(0x4022000000000000));
    assert(c_cpu.v[31].d[1] == 0);
    assert_stats(&threaded_runner, 0, 11, 0, 11);

    c_cpu.v[31].d[0] = threaded_cpu.v[31].d[0] =
            UINT64_C(0x403e000000000000);
    c_cpu.v[31].d[1] = threaded_cpu.v[31].d[1] =
            UINT64_C(0x0123456789abcdef);
    c_cpu.x[1] = threaded_cpu.x[1] = UINT64_C(16);
    c_result = aarch64_run_one(&c_runner, &c_cpu);
    threaded_result = aarch64_run_one(&threaded_runner, &threaded_cpu);
    assert(c_result.stop == AARCH64_STEP_RETIRED);
    assert_step_equal(&c_result, &threaded_result);
    assert_cpu_equal(&c_cpu, &threaded_cpu);
    assert_memory_equal(&c_fixture.memory, &threaded_fixture.memory);
    assert(c_cpu.x[1] == UINT64_C(30));
    assert(c_cpu.v[31].d[0] == UINT64_C(0x403e000000000000));
    assert(c_cpu.v[31].d[1] == UINT64_C(0x0123456789abcdef));
    assert_stats(&threaded_runner, 0, 12, 0, 12);

    c_cpu.v[31].d[0] = threaded_cpu.v[31].d[0] = UINT64_C(1);
    c_cpu.v[31].d[1] = threaded_cpu.v[31].d[1] =
            UINT64_C(0x0123456789abcdef);
    c_cpu.v[30].d[0] = threaded_cpu.v[30].d[0] = UINT64_C(4);
    c_cpu.v[30].d[1] = threaded_cpu.v[30].d[1] = UINT64_C(1);
    c_result = aarch64_run_one(&c_runner, &c_cpu);
    threaded_result = aarch64_run_one(&threaded_runner, &threaded_cpu);
    assert(c_result.stop == AARCH64_STEP_RETIRED);
    assert_step_equal(&c_result, &threaded_result);
    assert_cpu_equal(&c_cpu, &threaded_cpu);
    assert_memory_equal(&c_fixture.memory, &threaded_fixture.memory);
    assert(c_cpu.v[30].d[0] == 0);
    assert(c_cpu.v[30].d[1] == 0);
    assert(c_cpu.v[31].d[0] == UINT64_C(1));
    assert(c_cpu.v[31].d[1] == UINT64_C(0x0123456789abcdef));
    assert_stats(&threaded_runner, 0, 13, 0, 13);

    c_fixture.memory.data[128] = 0xa5;
    threaded_fixture.memory.data[128] = 0xa5;
    c_cpu.x[1] = threaded_cpu.x[1] = DATA_PAGE + 128;
    c_cpu.v[31].d[0] = threaded_cpu.v[31].d[0] =
            UINT64_C(0x7766554433221100);
    c_cpu.v[31].d[1] = threaded_cpu.v[31].d[1] =
            UINT64_C(0xffeeddccbbaa9988);
    c_result = aarch64_run_one(&c_runner, &c_cpu);
    threaded_result = aarch64_run_one(&threaded_runner, &threaded_cpu);
    assert(c_result.stop == AARCH64_STEP_RETIRED);
    assert_step_equal(&c_result, &threaded_result);
    assert_cpu_equal(&c_cpu, &threaded_cpu);
    assert_memory_equal(&c_fixture.memory, &threaded_fixture.memory);
    assert(c_cpu.x[1] == DATA_PAGE + 128);
    assert(c_cpu.v[31].d[0] == UINT64_C(0x776655a533221100));
    assert(c_cpu.v[31].d[1] == UINT64_C(0xffeeddccbbaa9988));
    assert_stats(&threaded_runner, 0, 14, 0, 14);

    c_cpu.nzcv = threaded_cpu.nzcv = UINT32_C(0x60000000);
    c_cpu.v[0].d[0] = threaded_cpu.v[0].d[0] =
            UINT64_C(0x7ff0123456789abc);
    c_cpu.v[0].d[1] = threaded_cpu.v[0].d[1] =
            UINT64_C(0x1111222233334444);
    c_cpu.v[31].d[0] = threaded_cpu.v[31].d[0] =
            UINT64_C(0x0000000000000001);
    c_cpu.v[31].d[1] = threaded_cpu.v[31].d[1] =
            UINT64_C(0x5555666677778888);
    c_result = aarch64_run_one(&c_runner, &c_cpu);
    threaded_result = aarch64_run_one(&threaded_runner, &threaded_cpu);
    assert(c_result.stop == AARCH64_STEP_RETIRED);
    assert_step_equal(&c_result, &threaded_result);
    assert_cpu_equal(&c_cpu, &threaded_cpu);
    assert_memory_equal(&c_fixture.memory, &threaded_fixture.memory);
    assert(c_cpu.v[0].d[0] == UINT64_C(0x0000000000000001));
    assert(c_cpu.v[0].d[1] == 0);
    assert(c_cpu.v[31].d[0] == UINT64_C(0x0000000000000001));
    assert(c_cpu.v[31].d[1] == UINT64_C(0x5555666677778888));
    assert_stats(&threaded_runner, 0, 15, 0, 15);

    c_cpu.v[0].d[0] = threaded_cpu.v[0].d[0] =
            UINT64_C(0x42012e0be826d695);
    c_cpu.v[0].d[1] = threaded_cpu.v[0].d[1] =
            UINT64_C(0x0123456789abcdef);
    c_cpu.fpcr = threaded_cpu.fpcr = 0;
    c_cpu.fpsr = threaded_cpu.fpsr = AARCH64_FPSR_IXC;
    c_result = aarch64_run_one(&c_runner, &c_cpu);
    threaded_result = aarch64_run_one(&threaded_runner, &threaded_cpu);
    assert(c_result.stop == AARCH64_STEP_RETIRED);
    assert_step_equal(&c_result, &threaded_result);
    assert_cpu_equal(&c_cpu, &threaded_cpu);
    assert_memory_equal(&c_fixture.memory, &threaded_fixture.memory);
    assert(c_cpu.v[0].d[0] == UINT64_C(0x42012e0be8200000));
    assert(c_cpu.v[0].d[1] == 0);
    assert(c_cpu.fpsr == AARCH64_FPSR_IXC);
    assert_stats(&threaded_runner, 0, 16, 0, 16);

    c_cpu.v[0].d[0] = threaded_cpu.v[0].d[0] =
            UINT64_C(0x7ff0000000000000);
    c_cpu.v[0].d[1] = threaded_cpu.v[0].d[1] =
            UINT64_C(0x0123456789abcdef);
    c_cpu.fpcr = threaded_cpu.fpcr = 0;
    c_cpu.fpsr = threaded_cpu.fpsr = AARCH64_FPSR_IXC;
    c_result = aarch64_run_one(&c_runner, &c_cpu);
    threaded_result = aarch64_run_one(&threaded_runner, &threaded_cpu);
    assert(c_result.stop == AARCH64_STEP_RETIRED);
    assert_step_equal(&c_result, &threaded_result);
    assert_cpu_equal(&c_cpu, &threaded_cpu);
    assert_memory_equal(&c_fixture.memory, &threaded_fixture.memory);
    assert(c_cpu.v[0].d[0] == UINT64_C(0xfff0000000000000));
    assert(c_cpu.v[0].d[1] == 0);
    assert(c_cpu.fpsr == AARCH64_FPSR_IXC);
    assert_stats(&threaded_runner, 0, 17, 0, 17);

    c_cpu.v[27].d[0] = threaded_cpu.v[27].d[0] =
            UINT64_C(0x0706050403020100);
    c_cpu.v[27].d[1] = threaded_cpu.v[27].d[1] =
            UINT64_C(0x0f0e0d0c0b0a0908);
    c_cpu.v[30].d[0] = threaded_cpu.v[30].d[0] =
            UINT64_C(0x8786858483828180);
    c_cpu.v[30].d[1] = threaded_cpu.v[30].d[1] =
            UINT64_C(0x8f8e8d8c8b8a8988);
    const union aarch64_vector_reg source_n = c_cpu.v[27];
    const union aarch64_vector_reg source_m = c_cpu.v[30];
    c_result = aarch64_run_one(&c_runner, &c_cpu);
    threaded_result = aarch64_run_one(&threaded_runner, &threaded_cpu);
    assert(c_result.stop == AARCH64_STEP_RETIRED);
    assert_step_equal(&c_result, &threaded_result);
    assert_cpu_equal(&c_cpu, &threaded_cpu);
    assert_memory_equal(&c_fixture.memory, &threaded_fixture.memory);
    assert(c_cpu.v[0].d[0] == UINT64_C(0x0f0e0d0c0b0a0908));
    assert(c_cpu.v[0].d[1] == UINT64_C(0x8786858483828180));
    assert(memcmp(&c_cpu.v[27], &source_n, sizeof(source_n)) == 0);
    assert(memcmp(&c_cpu.v[30], &source_m, sizeof(source_m)) == 0);
    assert_stats(&threaded_runner, 0, 18, 0, 18);

    c_cpu.v[30].d[0] = threaded_cpu.v[30].d[0] =
            UINT64_C(0x0011223344556677);
    c_cpu.v[30].d[1] = threaded_cpu.v[30].d[1] =
            UINT64_C(0x8899aabbccddeeff);
    c_cpu.v[31].q = threaded_cpu.v[31].q = 0;
    const union aarch64_vector_reg not_source = c_cpu.v[30];
    c_result = aarch64_run_one(&c_runner, &c_cpu);
    threaded_result = aarch64_run_one(&threaded_runner, &threaded_cpu);
    assert(c_result.stop == AARCH64_STEP_RETIRED);
    assert_step_equal(&c_result, &threaded_result);
    assert_cpu_equal(&c_cpu, &threaded_cpu);
    assert_memory_equal(&c_fixture.memory, &threaded_fixture.memory);
    assert(c_cpu.v[31].d[0] == UINT64_C(0xffeeddccbbaa9988));
    assert(c_cpu.v[31].d[1] == UINT64_C(0x7766554433221100));
    assert(memcmp(&c_cpu.v[30], &not_source, sizeof(not_source)) == 0);
    assert_stats(&threaded_runner, 0, 19, 0, 19);

    const union aarch64_vector_reg not_result = c_cpu.v[31];
    c_result = aarch64_run_one(&c_runner, &c_cpu);
    threaded_result = aarch64_run_one(&threaded_runner, &threaded_cpu);
    assert(c_result.stop == AARCH64_STEP_RETIRED);
    assert_step_equal(&c_result, &threaded_result);
    assert_cpu_equal(&c_cpu, &threaded_cpu);
    assert_memory_equal(&c_fixture.memory, &threaded_fixture.memory);
    assert(c_cpu.v[30].d[0] == UINT64_C(0x00004488cd115599));
    assert(c_cpu.v[30].d[1] == UINT64_C(0x022266aaef3377bb));
    assert(memcmp(&c_cpu.v[31], &not_result, sizeof(not_result)) == 0);
    assert_stats(&threaded_runner, 0, 20, 0, 20);

    c_cpu.v[29].d[0] = threaded_cpu.v[29].d[0] =
            UINT64_C(0x7f7f7f7f7f7e7f7f);
    c_cpu.v[29].d[1] = threaded_cpu.v[29].d[1] =
            UINT64_C(0x7f7f7f7f7f7f7f7f);
    const union aarch64_vector_reg addp_source = c_cpu.v[29];
    c_result = aarch64_run_one(&c_runner, &c_cpu);
    threaded_result = aarch64_run_one(&threaded_runner, &threaded_cpu);
    assert(c_result.stop == AARCH64_STEP_RETIRED);
    assert_step_equal(&c_result, &threaded_result);
    assert_cpu_equal(&c_cpu, &threaded_cpu);
    assert_memory_equal(&c_fixture.memory, &threaded_fixture.memory);
    assert(c_cpu.v[31].d[0] == UINT64_C(0xfefefefefefdfefe));
    assert(c_cpu.v[31].d[1] == 0);
    assert(memcmp(&c_cpu.v[29], &addp_source, sizeof(addp_source)) == 0);
    assert_stats(&threaded_runner, 0, 21, 0, 21);
}

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
    c_cpu.v[31].s[0] = threaded_cpu.v[31].s[0] =
            UINT32_C(0x80000001);
    c_cpu.v[31].s[1] = threaded_cpu.v[31].s[1] =
            UINT32_C(0xfedcba98);

    const enum aarch64_step_stop expected_stops[] = {
        AARCH64_STEP_RETIRED,
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
        if (step == 5) {
            assert(c_cpu.v[31].d[0] ==
                    UINT64_C(0x0000000080000001));
            assert(c_cpu.v[31].d[1] ==
                    UINT64_C(0x00000000fedcba98));
            assert(c_cpu.pc == CODE_PAGE + 6 * 4);
            assert(c_cpu.cycle == 15);
            assert_stats(&threaded_runner, 0, 6, 2, 4);
        }
    }
    assert(c_cpu.x[0] == 8);
    assert(c_cpu.x[2] == 8);
    assert(c_cpu.x[3] == 16);
    assert(c_fixture.memory.data[0x80] == 8);
    assert_stats(&threaded_runner, 0, 7, 3, 4);

    struct aarch64_step_result c_undefined = run_at(
            &c_runner, &c_cpu, CODE_PAGE + 7 * 4);
    struct aarch64_step_result threaded_undefined = run_at(
            &threaded_runner, &threaded_cpu, CODE_PAGE + 7 * 4);
    assert(c_undefined.stop == AARCH64_STEP_UNDEFINED);
    assert_step_equal(&c_undefined, &threaded_undefined);
    assert_cpu_equal(&c_cpu, &threaded_cpu);
    assert_memory_equal(&c_fixture.memory, &threaded_fixture.memory);
    assert_stats(&threaded_runner, 0, 8, 3, 4);

    struct aarch64_step_result c_result = run_at(
            &c_runner, &c_cpu, CODE_PAGE);
    struct aarch64_step_result threaded_result = run_at(
            &threaded_runner, &threaded_cpu, CODE_PAGE);
    assert_step_equal(&c_result, &threaded_result);
    assert_cpu_equal(&c_cpu, &threaded_cpu);
    assert_memory_equal(&c_fixture.memory, &threaded_fixture.memory);
    assert_stats(&threaded_runner, 1, 8, 4, 4);
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
    assert_stats(&runner, 0, 1, 1, 0);
    assert(run_at(&runner, &cpu, first).stop == AARCH64_STEP_RETIRED);
    assert_stats(&runner, 1, 1, 2, 0);
    assert(run_at(&runner, &cpu, collision).stop == AARCH64_STEP_RETIRED);
    assert(cpu.x[0] == 3);
    assert_stats(&runner, 1, 2, 3, 0);

    write_instruction(&fixture.tlb, collision, INSTRUCTION_ADD_X0);
    assert(run_at(&runner, &cpu, collision).stop == AARCH64_STEP_RETIRED);
    assert(cpu.x[0] == 4);
    assert_stats(&runner, 1, 3, 4, 0);
    assert(run_at(&runner, &cpu, collision).stop == AARCH64_STEP_RETIRED);
    assert(cpu.x[0] == 5);
    assert_stats(&runner, 2, 3, 5, 0);
    assert(run_at(&runner, &cpu, first).stop == AARCH64_STEP_RETIRED);
    assert_stats(&runner, 2, 4, 6, 0);
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
    assert_stats(&runner, 0, 1, 1, 0);
    assert(run_at(&runner, &cpu, pc).stop == AARCH64_STEP_RETIRED);
    assert_stats(&runner, 1, 1, 2, 0);

    write_instruction(&fixture.tlb, pc, INSTRUCTION_ADD_X0);
    assert(run_at(&runner, &cpu, pc).stop == AARCH64_STEP_RETIRED);
    assert(cpu.x[0] == 11);
    assert_stats(&runner, 1, 2, 3, 0);

    write_instruction(&fixture.tlb, pc, INSTRUCTION_UNDEFINED);
    qword_t cycle = cpu.cycle;
    assert(run_at(&runner, &cpu, pc).stop == AARCH64_STEP_UNDEFINED);
    assert(cpu.pc == pc);
    assert(cpu.cycle == cycle);
    assert_stats(&runner, 1, 3, 3, 0);
    assert(run_at(&runner, &cpu, pc).stop == AARCH64_STEP_UNDEFINED);
    assert(cpu.pc == pc);
    assert(cpu.cycle == cycle);
    assert_stats(&runner, 2, 3, 3, 0);

    write_instruction(&fixture.tlb, pc, INSTRUCTION_NOP);
    assert(run_at(&runner, &cpu, pc).stop == AARCH64_STEP_RETIRED);
    assert(cpu.x[0] == 11);
    assert_stats(&runner, 2, 4, 4, 0);
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
    assert_stats(&runner, 1, 1, 2, 0);

    guest_address_space_changed(&fixture.space);
    assert(run_at(&runner, &cpu, pc).stop == AARCH64_STEP_RETIRED);
    assert_stats(&runner, 2, 1, 3, 0);

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
    assert_stats(&runner, 2, 1, 3, 0);

    fixture.memory.code_permissions = GUEST_MEMORY_READ |
            GUEST_MEMORY_WRITE | GUEST_MEMORY_EXECUTE;
    guest_address_space_changed(&fixture.space);
    assert(run_at(&runner, &cpu, pc).stop == AARCH64_STEP_RETIRED);
    assert_stats(&runner, 3, 1, 4, 0);

    put_instruction(fixture.memory.replacement_code + 0x80,
            INSTRUCTION_ADD_X0);
    fixture.memory.code_mapping = fixture.memory.replacement_code;
    guest_address_space_changed(&fixture.space);
    assert(run_at(&runner, &cpu, pc).stop == AARCH64_STEP_RETIRED);
    assert(cpu.x[0] == 1);
    assert_stats(&runner, 3, 2, 5, 0);
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
        assert_stats(&runners[index], 1, 1, 2, 0);
    }

    write_instruction(&fixture.tlb, pc, INSTRUCTION_ADD_X0);
    for (unsigned index = 0; index < array_size(runners); index++) {
        assert(run_at(&runners[index], &cpus[index], pc).stop ==
                AARCH64_STEP_RETIRED);
        assert(cpus[index].x[0] == 1);
        assert_stats(&runners[index], 1, 2, 3, 0);
    }
}
#endif

int main(void) {
    test_backend_selection();
#if defined(__aarch64__)
    test_fast_data_processing_differential();
    test_fast_branch_differential();
    test_fast_svc_differential();
    test_fast_dispatch_structure();
    test_product_c_fallback();
    test_c_and_threaded_differential();
    test_cache_keys_and_collision();
    test_rwx_self_modifying_code();
    test_mapping_invalidation();
    test_shared_space_invalidation();
#endif
    return 0;
}
