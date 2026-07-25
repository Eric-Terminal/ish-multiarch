#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "guest/aarch64/runner.h"

#define CODE_PAGE UINT64_C(0x0000456789abc000)
#define DATA_PAGE (CODE_PAGE + GUEST_MEMORY_PAGE_SIZE)
#define SAMPLE_COUNT 10
#define TARGET_SAMPLE_NS UINT64_C(100000000)
#define BASELINE_MAD_WARNING_PERCENT 10.0
#define INITIAL_ITERATIONS UINT64_C(1024)
#define NOP_COUNT 60

#define INSTRUCTION_NOP UINT32_C(0xd503201f)
#define INSTRUCTION_ADD_X1_IMMEDIATE UINT32_C(0x91000421)
#define INSTRUCTION_ADD_X1_SHIFTED UINT32_C(0x8b020021)
#define INSTRUCTION_ADDS_X1_SHIFTED UINT32_C(0xab020021)
#define INSTRUCTION_SUB_X4_X5_X2 UINT32_C(0xcb0200a4)
#define INSTRUCTION_SUB_W6_W3_W2 UINT32_C(0x4b020066)
#define INSTRUCTION_CCMP_W3_W2_4_NE UINT32_C(0x7a421064)
#define INSTRUCTION_CCMP_W1_1_4_LS UINT32_C(0x7a419824)
#define INSTRUCTION_CSINC_X4_XZR_XZR_NE UINT32_C(0x9a9f17e4)
#define INSTRUCTION_CSINC_W6_WZR_WZR_EQ UINT32_C(0x1a9f07e6)
#define INSTRUCTION_CSEL_X4_X5_X2_EQ UINT32_C(0x9a8200a4)
#define INSTRUCTION_CSEL_W6_W4_W3_NE UINT32_C(0x1a831086)
#define INSTRUCTION_ANDS_X4_X0_FF UINT32_C(0xf2401c04)
#define INSTRUCTION_TST_W4_2 UINT32_C(0x721f009f)
#define INSTRUCTION_CCMP_X2_3_0_EQ UINT32_C(0xfa430840)
#define INSTRUCTION_SUB_X0_X0_1 UINT32_C(0xd1000400)
#define INSTRUCTION_CBNZ_X0_NEG_16 UINT32_C(0xb5ffff80)
#define INSTRUCTION_REV32_X4_X5 UINT32_C(0xdac008a4)
#define INSTRUCTION_REV_W6_W2 UINT32_C(0x5ac00846)
#define INSTRUCTION_ADD_X4_X5_W3_SXTW_3 UINT32_C(0x8b23cca4)
#define INSTRUCTION_ADD_W6_W2_W3_UXTW_3 UINT32_C(0x0b234c46)
#define INSTRUCTION_LDR_X4_X3_POST_8 UINT32_C(0xf8408464)
#define INSTRUCTION_LDRB_W6_X3_PRE_NEG_8 UINT32_C(0x385f8c66)
#define INSTRUCTION_SUBS_W4_W0_W2_LSL_2 UINT32_C(0x6b020804)
#define INSTRUCTION_SUBS_X0_X0_X2_LSR_1 UINT32_C(0xeb420400)
#define INSTRUCTION_ADR_X1_PLUS_12 UINT32_C(0x10000061)
#define INSTRUCTION_SUB_X1_X1_X7 UINT32_C(0xcb070021)
#define INSTRUCTION_ADRP_X4_PLUS_86_PAGES UINT32_C(0xd00002a4)
#define INSTRUCTION_ADRP_X6_PLUS_122_PAGES UINT32_C(0xd00003c6)
#define INSTRUCTION_AND_X4_X5_FFFFFFFFFFFFFFF0 UINT32_C(0x927ceca4)
#define INSTRUCTION_AND_W6_W3_7FFFFFFF UINT32_C(0x12007866)
#define INSTRUCTION_AND_X4_X1_X2_LSL_2 UINT32_C(0x8a020824)
#define INSTRUCTION_UBFM_W4_W0_28_27 UINT32_C(0x531c6c04)
#define INSTRUCTION_SXTW_X2_W2 UINT32_C(0x93407c42)
#define INSTRUCTION_LSRV_W4_W10_W4 UINT32_C(0x1ac42544)
#define INSTRUCTION_MUL_X6_X6_X0 UINT32_C(0x9b007cc6)
#define INSTRUCTION_MUL_W2_W0_W2 UINT32_C(0x1b027c02)
#define INSTRUCTION_LSLV_X5_X7_X1 UINT32_C(0x9ac120e5)
#define INSTRUCTION_ADD_X7_X5_1 UINT32_C(0x910004a7)
#define INSTRUCTION_LSLV_W0_W0_W1 UINT32_C(0x1ac12000)
#define INSTRUCTION_ADD_W0_W0_1 UINT32_C(0x11000400)
#define INSTRUCTION_ORR_X5_XZR_X6 UINT32_C(0xaa0603e5)
#define INSTRUCTION_SUBS_X4_X4_1 UINT32_C(0xf1000484)
#define INSTRUCTION_B_NE_NEG_12 UINT32_C(0x54ffffa1)
#define INSTRUCTION_EXTR_W4_W1_W1_19 UINT32_C(0x13814c24)
#define INSTRUCTION_ORR_X1_XZR_X1 UINT32_C(0xaa0103e1)
#define INSTRUCTION_EOR_X1_XZR_X1 UINT32_C(0xca0103e1)
#define INSTRUCTION_LDR_X4_X3 UINT32_C(0xf9400064)
#define INSTRUCTION_LDR_X4_X3_X2_LSL_3 UINT32_C(0xf8627864)
#define INSTRUCTION_LDP_X4_X6_X3_48 UINT32_C(0xa9431864)
#define INSTRUCTION_STR_X5_X3_32 UINT32_C(0xf9001065)
#define INSTRUCTION_SUB_X4_X1_1 UINT32_C(0xd1000424)
#define INSTRUCTION_STR_X5_X3_X4_LSL_3 UINT32_C(0xf8247865)
#define INSTRUCTION_STR_XZR_X3_X1_LSL_3 UINT32_C(0xf821787f)
#define INSTRUCTION_STP_X5_X2_X3_32 UINT32_C(0xa9020865)
#define INSTRUCTION_STP_Q0_Q0_X3_32 UINT32_C(0xad010060)
#define INSTRUCTION_SUBS_X0 UINT32_C(0xf1000400)
#define INSTRUCTION_SVC UINT32_C(0xd4000001)
#define STORE_VALUE UINT64_C(0x8877665544332211)
#define PAIR_FIRST_VALUE UINT64_C(0x0123456789abcdef)
#define PAIR_SECOND_VALUE UINT64_C(0xfedcba9876543210)

struct benchmark_memory {
    byte_t code[GUEST_MEMORY_PAGE_SIZE];
    byte_t data[GUEST_MEMORY_PAGE_SIZE];
};

struct benchmark_environment {
    struct benchmark_memory memory;
    struct guest_address_space address_space;
    struct guest_tlb tlb;
    struct aarch64_runner runner;
};

struct benchmark_workload {
    const char *name;
    qword_t instructions_per_iteration;
    qword_t x1_increment_per_iteration;
    byte_t iteration_register;
    qword_t initial_x0;
    qword_t initial_x4;
    qword_t initial_x6;
    qword_t initial_x7;
    qword_t initial_x10;
    qword_t expected_x4;
    qword_t expected_x6;
    size_t expected_store_offset;
    byte_t expected_store_size;
    qword_t expected_store_values[4];
    qword_t fast_per_iteration;
    qword_t fallback_per_iteration;
    size_t program_instruction_count;
    void (*write_program)(byte_t code[GUEST_MEMORY_PAGE_SIZE]);
};

enum benchmark_cache_state {
    BENCHMARK_CACHE_UNUSED,
    BENCHMARK_CACHE_COLD,
    BENCHMARK_CACHE_HOT,
};

struct benchmark_run {
    uint64_t elapsed_ns;
    struct cpu_state cpu;
    struct aarch64_step_result final_step;
};

struct benchmark_sample {
    uint64_t c_ns;
    uint64_t threaded_ns;
    double speedup;
};

static void fail(const char *workload, const char *message) {
    fprintf(stderr, "AArch64 后端基准失败（%s）：%s\n",
            workload, message);
    exit(EXIT_FAILURE);
}

static void require(bool condition, const char *workload,
        const char *message) {
    if (!condition)
        fail(workload, message);
}

static enum guest_memory_fault_kind resolve_benchmark_page(void *opaque,
        guest_addr_t page_base, enum guest_memory_access access,
        struct guest_page_view *view) {
    struct benchmark_memory *memory = opaque;
    (void) access;
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

static const struct guest_address_space_ops benchmark_address_space_ops = {
    .resolve_page = resolve_benchmark_page,
};

static void put_instruction(byte_t *destination, dword_t instruction) {
    destination[0] = (byte_t) instruction;
    destination[1] = (byte_t) (instruction >> 8);
    destination[2] = (byte_t) (instruction >> 16);
    destination[3] = (byte_t) (instruction >> 24);
}

static dword_t encode_conditional_branch(int64_t displacement,
        byte_t condition) {
    dword_t immediate = (dword_t) (displacement / 4) &
            UINT32_C(0x7ffff);
    return UINT32_C(0x54000000) | immediate << 5 | condition;
}

static void write_fast_program(byte_t code[GUEST_MEMORY_PAGE_SIZE]) {
    put_instruction(code, INSTRUCTION_ADD_X1_IMMEDIATE);
    put_instruction(code + 4, INSTRUCTION_SUBS_X0);
    put_instruction(code + 8, encode_conditional_branch(-8, 1));
    put_instruction(code + 12, INSTRUCTION_SVC);
}

static void write_add_shifted_program(
        byte_t code[GUEST_MEMORY_PAGE_SIZE]) {
    put_instruction(code, INSTRUCTION_ADD_X1_SHIFTED);
    put_instruction(code + 4, INSTRUCTION_SUBS_X0);
    put_instruction(code + 8, encode_conditional_branch(-8, 1));
    put_instruction(code + 12, INSTRUCTION_SVC);
}

static void write_sub_shifted_program(
        byte_t code[GUEST_MEMORY_PAGE_SIZE]) {
    put_instruction(code, INSTRUCTION_SUB_X4_X5_X2);
    put_instruction(code + 4, INSTRUCTION_SUB_W6_W3_W2);
    put_instruction(code + 8, INSTRUCTION_SUBS_X0);
    put_instruction(code + 12, encode_conditional_branch(-12, 1));
    put_instruction(code + 16, INSTRUCTION_SVC);
}

static void write_ccmp_program(byte_t code[GUEST_MEMORY_PAGE_SIZE]) {
    put_instruction(code, INSTRUCTION_CCMP_W3_W2_4_NE);
    put_instruction(code + 4, INSTRUCTION_CCMP_W1_1_4_LS);
    put_instruction(code + 8, encode_conditional_branch(12, 1));
    put_instruction(code + 12, INSTRUCTION_SUBS_X0);
    put_instruction(code + 16, encode_conditional_branch(-16, 1));
    put_instruction(code + 20, INSTRUCTION_SVC);
}

static void write_csinc_program(byte_t code[GUEST_MEMORY_PAGE_SIZE]) {
    put_instruction(code, INSTRUCTION_SUBS_X0);
    put_instruction(code + 4, INSTRUCTION_CSINC_X4_XZR_XZR_NE);
    put_instruction(code + 8, INSTRUCTION_CSINC_W6_WZR_WZR_EQ);
    put_instruction(code + 12, encode_conditional_branch(-12, 1));
    put_instruction(code + 16, INSTRUCTION_SVC);
}

static void write_csel_program(byte_t code[GUEST_MEMORY_PAGE_SIZE]) {
    put_instruction(code, INSTRUCTION_SUBS_X0);
    put_instruction(code + 4, INSTRUCTION_CSEL_X4_X5_X2_EQ);
    put_instruction(code + 8, INSTRUCTION_CSEL_W6_W4_W3_NE);
    put_instruction(code + 12, encode_conditional_branch(-12, 1));
    put_instruction(code + 16, INSTRUCTION_SVC);
}

static void write_ands_immediate_program(
        byte_t code[GUEST_MEMORY_PAGE_SIZE]) {
    put_instruction(code, INSTRUCTION_ANDS_X4_X0_FF);
    put_instruction(code + 4, INSTRUCTION_TST_W4_2);
    put_instruction(code + 8, INSTRUCTION_CCMP_X2_3_0_EQ);
    put_instruction(code + 12, INSTRUCTION_SUB_X0_X0_1);
    put_instruction(code + 16, INSTRUCTION_CBNZ_X0_NEG_16);
    put_instruction(code + 20, INSTRUCTION_SVC);
}

static void write_rev32_program(byte_t code[GUEST_MEMORY_PAGE_SIZE]) {
    put_instruction(code, INSTRUCTION_REV32_X4_X5);
    put_instruction(code + 4, INSTRUCTION_REV_W6_W2);
    put_instruction(code + 8, INSTRUCTION_SUBS_X0);
    put_instruction(code + 12, encode_conditional_branch(-12, 1));
    put_instruction(code + 16, INSTRUCTION_SVC);
}

static void write_add_extended_program(
        byte_t code[GUEST_MEMORY_PAGE_SIZE]) {
    put_instruction(code, INSTRUCTION_ADD_X4_X5_W3_SXTW_3);
    put_instruction(code + 4, INSTRUCTION_ADD_W6_W2_W3_UXTW_3);
    put_instruction(code + 8, INSTRUCTION_SUBS_X0);
    put_instruction(code + 12, encode_conditional_branch(-12, 1));
    put_instruction(code + 16, INSTRUCTION_SVC);
}

static void write_load_imm9_program(
        byte_t code[GUEST_MEMORY_PAGE_SIZE]) {
    put_instruction(code, INSTRUCTION_LDR_X4_X3_POST_8);
    put_instruction(code + 4, INSTRUCTION_LDRB_W6_X3_PRE_NEG_8);
    put_instruction(code + 8, INSTRUCTION_SUBS_X0);
    put_instruction(code + 12, encode_conditional_branch(-12, 1));
    put_instruction(code + 16, INSTRUCTION_SVC);
}

static void write_subs_shifted_program(
        byte_t code[GUEST_MEMORY_PAGE_SIZE]) {
    put_instruction(code, INSTRUCTION_SUBS_W4_W0_W2_LSL_2);
    put_instruction(code + 4, INSTRUCTION_SUBS_X0_X0_X2_LSR_1);
    put_instruction(code + 8, encode_conditional_branch(-8, 1));
    put_instruction(code + 12, INSTRUCTION_SVC);
}

static void write_adr_program(byte_t code[GUEST_MEMORY_PAGE_SIZE]) {
    put_instruction(code, INSTRUCTION_ADR_X1_PLUS_12);
    put_instruction(code + 4, INSTRUCTION_SUB_X1_X1_X7);
    put_instruction(code + 8, INSTRUCTION_SUBS_X0);
    put_instruction(code + 12, INSTRUCTION_B_NE_NEG_12);
    put_instruction(code + 16, INSTRUCTION_SVC);
}

static void write_adrp_program(byte_t code[GUEST_MEMORY_PAGE_SIZE]) {
    put_instruction(code, INSTRUCTION_ADRP_X4_PLUS_86_PAGES);
    put_instruction(code + 4, INSTRUCTION_ADRP_X6_PLUS_122_PAGES);
    put_instruction(code + 8, INSTRUCTION_SUBS_X0);
    put_instruction(code + 12, encode_conditional_branch(-12, 1));
    put_instruction(code + 16, INSTRUCTION_SVC);
}

static void write_and_immediate_program(
        byte_t code[GUEST_MEMORY_PAGE_SIZE]) {
    put_instruction(code, INSTRUCTION_AND_X4_X5_FFFFFFFFFFFFFFF0);
    put_instruction(code + 4, INSTRUCTION_AND_W6_W3_7FFFFFFF);
    put_instruction(code + 8, INSTRUCTION_SUBS_X0);
    put_instruction(code + 12, encode_conditional_branch(-12, 1));
    put_instruction(code + 16, INSTRUCTION_SVC);
}

static void write_mixed_program(byte_t code[GUEST_MEMORY_PAGE_SIZE]) {
    put_instruction(code, INSTRUCTION_ADDS_X1_SHIFTED);
    put_instruction(code + 4, INSTRUCTION_SUBS_X0);
    put_instruction(code + 8, encode_conditional_branch(-8, 1));
    put_instruction(code + 12, INSTRUCTION_SVC);
}

static void write_and_program(byte_t code[GUEST_MEMORY_PAGE_SIZE]) {
    put_instruction(code, INSTRUCTION_AND_X4_X1_X2_LSL_2);
    put_instruction(code + 4, INSTRUCTION_SUBS_X0);
    put_instruction(code + 8, encode_conditional_branch(-8, 1));
    put_instruction(code + 12, INSTRUCTION_SVC);
}

static void write_ubfm_program(byte_t code[GUEST_MEMORY_PAGE_SIZE]) {
    put_instruction(code, INSTRUCTION_UBFM_W4_W0_28_27);
    put_instruction(code + 4, INSTRUCTION_SUBS_X0);
    put_instruction(code + 8, encode_conditional_branch(-8, 1));
    put_instruction(code + 12, INSTRUCTION_SVC);
}

static void write_sbfm_program(byte_t code[GUEST_MEMORY_PAGE_SIZE]) {
    put_instruction(code, INSTRUCTION_SXTW_X2_W2);
    put_instruction(code + 4, INSTRUCTION_SUBS_X0);
    put_instruction(code + 8, encode_conditional_branch(-8, 1));
    put_instruction(code + 12, INSTRUCTION_SVC);
}

static void write_lsrv_program(byte_t code[GUEST_MEMORY_PAGE_SIZE]) {
    put_instruction(code, INSTRUCTION_LSRV_W4_W10_W4);
    put_instruction(code + 4, INSTRUCTION_SUBS_X0);
    put_instruction(code + 8, encode_conditional_branch(-8, 1));
    put_instruction(code + 12, INSTRUCTION_SVC);
}

static void write_madd_program(byte_t code[GUEST_MEMORY_PAGE_SIZE]) {
    put_instruction(code, INSTRUCTION_MUL_X6_X6_X0);
    put_instruction(code + 4, INSTRUCTION_MUL_W2_W0_W2);
    put_instruction(code + 8, INSTRUCTION_SUBS_X4_X4_1);
    put_instruction(code + 12, INSTRUCTION_B_NE_NEG_12);
    put_instruction(code + 16, INSTRUCTION_SVC);
}

static void write_lslv_program(byte_t code[GUEST_MEMORY_PAGE_SIZE]) {
    put_instruction(code, INSTRUCTION_LSLV_X5_X7_X1);
    put_instruction(code + 4, INSTRUCTION_ADD_X7_X5_1);
    put_instruction(code + 8, INSTRUCTION_LSLV_W0_W0_W1);
    put_instruction(code + 12, INSTRUCTION_ADD_W0_W0_1);
    put_instruction(code + 16, INSTRUCTION_ORR_X5_XZR_X6);
    put_instruction(code + 20, INSTRUCTION_SUBS_X4_X4_1);
    put_instruction(code + 24, encode_conditional_branch(-24, 1));
    put_instruction(code + 28, INSTRUCTION_SVC);
}

static void write_extract_program(byte_t code[GUEST_MEMORY_PAGE_SIZE]) {
    put_instruction(code, INSTRUCTION_EXTR_W4_W1_W1_19);
    put_instruction(code + 4, INSTRUCTION_SUBS_X0);
    put_instruction(code + 8, encode_conditional_branch(-8, 1));
    put_instruction(code + 12, INSTRUCTION_SVC);
}

static void write_orr_program(byte_t code[GUEST_MEMORY_PAGE_SIZE]) {
    put_instruction(code, INSTRUCTION_ORR_X1_XZR_X1);
    put_instruction(code + 4, INSTRUCTION_SUBS_X0);
    put_instruction(code + 8, encode_conditional_branch(-8, 1));
    put_instruction(code + 12, INSTRUCTION_SVC);
}

static void write_eor_program(byte_t code[GUEST_MEMORY_PAGE_SIZE]) {
    put_instruction(code, INSTRUCTION_EOR_X1_XZR_X1);
    put_instruction(code + 4, INSTRUCTION_SUBS_X0);
    put_instruction(code + 8, encode_conditional_branch(-8, 1));
    put_instruction(code + 12, INSTRUCTION_SVC);
}

static void write_load_program(byte_t code[GUEST_MEMORY_PAGE_SIZE]) {
    put_instruction(code, INSTRUCTION_LDR_X4_X3);
    put_instruction(code + 4, INSTRUCTION_SUBS_X0);
    put_instruction(code + 8, encode_conditional_branch(-8, 1));
    put_instruction(code + 12, INSTRUCTION_SVC);
}

static void write_load_register_offset_program(
        byte_t code[GUEST_MEMORY_PAGE_SIZE]) {
    put_instruction(code, INSTRUCTION_LDR_X4_X3_X2_LSL_3);
    put_instruction(code + 4, INSTRUCTION_SUBS_X0);
    put_instruction(code + 8, encode_conditional_branch(-8, 1));
    put_instruction(code + 12, INSTRUCTION_SVC);
}

static void write_store_program(byte_t code[GUEST_MEMORY_PAGE_SIZE]) {
    put_instruction(code, INSTRUCTION_STR_X5_X3_32);
    put_instruction(code + 4, INSTRUCTION_SUBS_X0);
    put_instruction(code + 8, encode_conditional_branch(-8, 1));
    put_instruction(code + 12, INSTRUCTION_SVC);
}

static void write_store_register_offset_program(
        byte_t code[GUEST_MEMORY_PAGE_SIZE]) {
    put_instruction(code, INSTRUCTION_SUB_X4_X1_1);
    put_instruction(code + 4, INSTRUCTION_STR_X5_X3_X4_LSL_3);
    put_instruction(code + 8, INSTRUCTION_STR_XZR_X3_X1_LSL_3);
    put_instruction(code + 12, INSTRUCTION_SUBS_X0);
    put_instruction(code + 16, encode_conditional_branch(-16, 1));
    put_instruction(code + 20, INSTRUCTION_SVC);
}

static void write_store_pair_program(
        byte_t code[GUEST_MEMORY_PAGE_SIZE]) {
    put_instruction(code, INSTRUCTION_STP_X5_X2_X3_32);
    put_instruction(code + 4, INSTRUCTION_SUBS_X0);
    put_instruction(code + 8, encode_conditional_branch(-8, 1));
    put_instruction(code + 12, INSTRUCTION_SVC);
}

static void write_store_simd_pair_program(
        byte_t code[GUEST_MEMORY_PAGE_SIZE]) {
    put_instruction(code, INSTRUCTION_STP_Q0_Q0_X3_32);
    put_instruction(code + 4, INSTRUCTION_SUBS_X0);
    put_instruction(code + 8, encode_conditional_branch(-8, 1));
    put_instruction(code + 12, INSTRUCTION_SVC);
}

static void write_load_pair_program(
        byte_t code[GUEST_MEMORY_PAGE_SIZE]) {
    put_instruction(code, INSTRUCTION_LDP_X4_X6_X3_48);
    put_instruction(code + 4, INSTRUCTION_SUBS_X0);
    put_instruction(code + 8, encode_conditional_branch(-8, 1));
    put_instruction(code + 12, INSTRUCTION_SVC);
}

static void write_nop_program(byte_t code[GUEST_MEMORY_PAGE_SIZE]) {
    for (unsigned index = 0; index < NOP_COUNT; index++)
        put_instruction(code + index * 4, INSTRUCTION_NOP);
    put_instruction(code + NOP_COUNT * 4, INSTRUCTION_SUBS_X0);
    put_instruction(code + (NOP_COUNT + 1) * 4,
            encode_conditional_branch(-(int64_t) ((NOP_COUNT + 1) * 4), 1));
    put_instruction(code + (NOP_COUNT + 2) * 4, INSTRUCTION_SVC);
}

static const struct benchmark_workload workloads[] = {
    {
        .name = "快速热点环",
        .instructions_per_iteration = 3,
        .x1_increment_per_iteration = 1,
        .fast_per_iteration = 3,
        .fallback_per_iteration = 0,
        .program_instruction_count = 4,
        .write_program = write_fast_program,
    },
    {
        .name = "ADD shifted 热点环",
        .instructions_per_iteration = 3,
        .x1_increment_per_iteration = 3,
        .fast_per_iteration = 3,
        .fallback_per_iteration = 0,
        .program_instruction_count = 4,
        .write_program = write_add_shifted_program,
    },
    {
        .name = "SUB shifted 双宽度热缓存环",
        .instructions_per_iteration = 4,
        .x1_increment_per_iteration = 0,
        .expected_x4 = UINT64_C(0x887766554433220e),
        .expected_x6 = UINT64_C(0x0000000089abcffd),
        .fast_per_iteration = 4,
        .fallback_per_iteration = 0,
        .program_instruction_count = 5,
        .write_program = write_sub_shifted_program,
    },
    {
        .name = "CCMP 条件结果依赖热缓存环",
        .instructions_per_iteration = 5,
        .x1_increment_per_iteration = 0,
        .fast_per_iteration = 5,
        .fallback_per_iteration = 0,
        .program_instruction_count = 6,
        .write_program = write_ccmp_program,
    },
    {
        .name = "CSINC 双宽度条件热点环",
        .instructions_per_iteration = 4,
        .x1_increment_per_iteration = 0,
        .expected_x4 = 1,
        .expected_x6 = 0,
        .fast_per_iteration = 4,
        .fallback_per_iteration = 0,
        .program_instruction_count = 5,
        .write_program = write_csinc_program,
    },
    {
        .name = "CSEL 双宽度结果依赖热点环",
        .instructions_per_iteration = 4,
        .x1_increment_per_iteration = 0,
        .expected_x4 = STORE_VALUE,
        .expected_x6 = UINT32_C(0x89abd000),
        .fast_per_iteration = 4,
        .fallback_per_iteration = 0,
        .program_instruction_count = 5,
        .write_program = write_csel_program,
    },
    {
        .name = "ANDS immediate 双宽度标志依赖热点环",
        .instructions_per_iteration = 5,
        .x1_increment_per_iteration = 0,
        .expected_x4 = 1,
        .expected_x6 = 0,
        .fast_per_iteration = 5,
        .fallback_per_iteration = 0,
        .program_instruction_count = 6,
        .write_program = write_ands_immediate_program,
    },
    {
        .name = "REV32 双宽度热点环",
        .instructions_per_iteration = 4,
        .x1_increment_per_iteration = 0,
        .expected_x4 = UINT64_C(0x5566778811223344),
        .expected_x6 = UINT32_C(0x03000000),
        .fast_per_iteration = 4,
        .fallback_per_iteration = 0,
        .program_instruction_count = 5,
        .write_program = write_rev32_program,
    },
    {
        .name = "ADD extended 双宽度热点环",
        .instructions_per_iteration = 4,
        .x1_increment_per_iteration = 0,
        .expected_x4 = UINT64_C(0x887766519191a211),
        .expected_x6 = UINT32_C(0x4d5e8003),
        .fast_per_iteration = 4,
        .fallback_per_iteration = 0,
        .program_instruction_count = 5,
        .write_program = write_add_extended_program,
    },
    {
        .name = "LDR imm9 双写回热点环",
        .instructions_per_iteration = 4,
        .x1_increment_per_iteration = 0,
        .expected_x4 = 7,
        .expected_x6 = 7,
        .fast_per_iteration = 4,
        .fallback_per_iteration = 0,
        .program_instruction_count = 5,
        .write_program = write_load_imm9_program,
    },
    {
        .name = "SUBS shifted 热点环",
        .instructions_per_iteration = 3,
        .x1_increment_per_iteration = 0,
        .expected_x4 = UINT32_C(0xfffffff5),
        .fast_per_iteration = 3,
        .fallback_per_iteration = 0,
        .program_instruction_count = 4,
        .write_program = write_subs_shifted_program,
    },
    {
        .name = "ADRP 双站点热缓存环",
        .instructions_per_iteration = 4,
        .x1_increment_per_iteration = 0,
        .expected_x4 = CODE_PAGE + UINT64_C(86) *
                GUEST_MEMORY_PAGE_SIZE,
        .expected_x6 = CODE_PAGE + UINT64_C(122) *
                GUEST_MEMORY_PAGE_SIZE,
        .fast_per_iteration = 4,
        .fallback_per_iteration = 0,
        .program_instruction_count = 5,
        .write_program = write_adrp_program,
    },
    {
        .name = "AND immediate 双掩码热缓存环",
        .instructions_per_iteration = 4,
        .x1_increment_per_iteration = 0,
        .expected_x4 = UINT64_C(0x8877665544332210),
        .expected_x6 = UINT64_C(0x0000000009abd000),
        .fast_per_iteration = 4,
        .fallback_per_iteration = 0,
        .program_instruction_count = 5,
        .write_program = write_and_immediate_program,
    },
    {
        .name = "混合回落环",
        .instructions_per_iteration = 3,
        .x1_increment_per_iteration = 3,
        .fast_per_iteration = 2,
        .fallback_per_iteration = 1,
        .program_instruction_count = 4,
        .write_program = write_mixed_program,
    },
    {
        .name = "AND shifted 热点环",
        .instructions_per_iteration = 3,
        .x1_increment_per_iteration = 0,
        .expected_x4 = 4,
        .fast_per_iteration = 3,
        .fallback_per_iteration = 0,
        .program_instruction_count = 4,
        .write_program = write_and_program,
    },
    {
        .name = "UBFM/LSL 热点环",
        .instructions_per_iteration = 3,
        .x1_increment_per_iteration = 0,
        .expected_x4 = 16,
        .fast_per_iteration = 3,
        .fallback_per_iteration = 0,
        .program_instruction_count = 4,
        .write_program = write_ubfm_program,
    },
    {
        .name = "SXTW 画像结果依赖热点环",
        .instructions_per_iteration = 3,
        .x1_increment_per_iteration = 0,
        .fast_per_iteration = 3,
        .fallback_per_iteration = 0,
        .program_instruction_count = 4,
        .write_program = write_sbfm_program,
    },
    {
        .name = "LSRV 画像结果依赖热点环",
        .instructions_per_iteration = 3,
        .x1_increment_per_iteration = 0,
        .initial_x4 = 4,
        .initial_x10 = UINT64_C(0xaaaaaaaa80000040),
        .expected_x4 = UINT32_C(0x08000004),
        .fast_per_iteration = 3,
        .fallback_per_iteration = 0,
        .program_instruction_count = 4,
        .write_program = write_lsrv_program,
    },
    {
        .name = "MADD/MUL 双宽度画像热点环",
        .instructions_per_iteration = 4,
        .x1_increment_per_iteration = 0,
        .iteration_register = 4,
        .initial_x0 = 1,
        .initial_x6 = PAIR_FIRST_VALUE,
        .expected_x4 = 0,
        .expected_x6 = PAIR_FIRST_VALUE,
        .fast_per_iteration = 4,
        .fallback_per_iteration = 0,
        .program_instruction_count = 5,
        .write_program = write_madd_program,
    },
    {
        .name = "LSLV 双宽度画像结果依赖热点环",
        .instructions_per_iteration = 7,
        .x1_increment_per_iteration = 0,
        .iteration_register = 4,
        .initial_x0 = UINT32_C(0x10204081),
        .initial_x6 = STORE_VALUE,
        .initial_x7 = UINT64_C(0x8102040810204081),
        .expected_x4 = 0,
        .expected_x6 = STORE_VALUE,
        .fast_per_iteration = 7,
        .fallback_per_iteration = 0,
        .program_instruction_count = 8,
        .write_program = write_lslv_program,
    },
    {
        .name = "EXTR/ROR 热点环",
        .instructions_per_iteration = 3,
        .x1_increment_per_iteration = 0,
        .expected_x4 = UINT64_C(0xe000),
        .fast_per_iteration = 3,
        .fallback_per_iteration = 0,
        .program_instruction_count = 4,
        .write_program = write_extract_program,
    },
    {
        .name = "ORR/MOV 热点环",
        .instructions_per_iteration = 3,
        .x1_increment_per_iteration = 0,
        .fast_per_iteration = 3,
        .fallback_per_iteration = 0,
        .program_instruction_count = 4,
        .write_program = write_orr_program,
    },
    {
        .name = "EOR/MOV 热点环",
        .instructions_per_iteration = 3,
        .x1_increment_per_iteration = 0,
        .fast_per_iteration = 3,
        .fallback_per_iteration = 0,
        .program_instruction_count = 4,
        .write_program = write_eor_program,
    },
    {
        .name = "LDR imm12 热点环",
        .instructions_per_iteration = 3,
        .x1_increment_per_iteration = 0,
        .expected_x4 = 7,
        .fast_per_iteration = 3,
        .fallback_per_iteration = 0,
        .program_instruction_count = 4,
        .write_program = write_load_program,
    },
    {
        .name = "LDR reg-offset 热点环",
        .instructions_per_iteration = 3,
        .x1_increment_per_iteration = 0,
        .expected_x4 = 11,
        .fast_per_iteration = 3,
        .fallback_per_iteration = 0,
        .program_instruction_count = 4,
        .write_program = write_load_register_offset_program,
    },
    {
        .name = "STR imm12 热点环",
        .instructions_per_iteration = 3,
        .x1_increment_per_iteration = 0,
        .expected_x4 = 0,
        .expected_store_offset = 32,
        .expected_store_size = 8,
        .expected_store_values = {STORE_VALUE},
        .fast_per_iteration = 3,
        .fallback_per_iteration = 0,
        .program_instruction_count = 4,
        .write_program = write_store_program,
    },
    {
        .name = "STR reg-offset 双站点热点环",
        .instructions_per_iteration = 5,
        .x1_increment_per_iteration = 0,
        .expected_x4 = 6,
        .expected_store_offset = 48,
        .expected_store_size = 16,
        .expected_store_values = {STORE_VALUE, 0},
        .fast_per_iteration = 5,
        .fallback_per_iteration = 0,
        .program_instruction_count = 6,
        .write_program = write_store_register_offset_program,
    },
    {
        .name = "STP 热点环",
        .instructions_per_iteration = 3,
        .x1_increment_per_iteration = 0,
        .expected_x4 = 0,
        .expected_store_offset = 32,
        .expected_store_size = 16,
        .expected_store_values = {STORE_VALUE, 3},
        .fast_per_iteration = 3,
        .fallback_per_iteration = 0,
        .program_instruction_count = 4,
        .write_program = write_store_pair_program,
    },
    {
        .name = "STP SIMD Q 热点环",
        .instructions_per_iteration = 3,
        .x1_increment_per_iteration = 0,
        .expected_x4 = 0,
        .expected_x6 = 0,
        .expected_store_offset = 32,
        .expected_store_size = 32,
        .expected_store_values = {
            STORE_VALUE,
            PAIR_FIRST_VALUE,
            STORE_VALUE,
            PAIR_FIRST_VALUE,
        },
        .fast_per_iteration = 3,
        .fallback_per_iteration = 0,
        .program_instruction_count = 4,
        .write_program = write_store_simd_pair_program,
    },
    {
        .name = "LDP 热点环",
        .instructions_per_iteration = 3,
        .x1_increment_per_iteration = 0,
        .expected_x4 = PAIR_FIRST_VALUE,
        .expected_x6 = PAIR_SECOND_VALUE,
        .fast_per_iteration = 3,
        .fallback_per_iteration = 0,
        .program_instruction_count = 4,
        .write_program = write_load_pair_program,
    },
    {
        .name = "NOP 调度环",
        .instructions_per_iteration = NOP_COUNT + 2,
        .x1_increment_per_iteration = 0,
        .fast_per_iteration = NOP_COUNT + 2,
        .fallback_per_iteration = 0,
        .program_instruction_count = NOP_COUNT + 3,
        .write_program = write_nop_program,
    },
    {
        .name = "ADR 画像结果依赖热点环",
        .instructions_per_iteration = 4,
        .x1_increment_per_iteration = 0,
        .initial_x7 = CODE_PAGE + 5,
        .fast_per_iteration = 4,
        .fallback_per_iteration = 0,
        .program_instruction_count = 5,
        .write_program = write_adr_program,
    },
};

static void store_little_endian(
        byte_t *destination, byte_t size, qword_t value) {
    for (byte_t index = 0; index < size; index++)
        destination[index] = (byte_t) (value >> (index * 8));
}

static void reset_benchmark_data(
        byte_t data[GUEST_MEMORY_PAGE_SIZE]) {
    memset(data, 0, GUEST_MEMORY_PAGE_SIZE);
    data[0] = 7;
    data[24] = 11;
    memset(data + 32, 0xa5, 16);
    store_little_endian(data + 48, 8, PAIR_FIRST_VALUE);
    store_little_endian(data + 56, 8, PAIR_SECOND_VALUE);
}

static qword_t scaled_count(const struct benchmark_workload *workload,
        qword_t iterations, qword_t per_iteration, qword_t tail) {
    if (per_iteration != 0 &&
            iterations > (UINT64_MAX - tail) / per_iteration)
        fail(workload->name, "迭代次数导致计数溢出");
    return iterations * per_iteration + tail;
}

static uint64_t monotonic_time_ns(const char *workload) {
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time) != 0) {
        perror("AArch64 后端基准读取单调时钟失败");
        fail(workload, "无法读取单调时钟");
    }
    return (uint64_t) time.tv_sec * UINT64_C(1000000000) +
            (uint64_t) time.tv_nsec;
}

static void init_environment(struct benchmark_environment *environment,
        const struct benchmark_workload *workload,
        enum aarch64_backend backend) {
    *environment = (struct benchmark_environment) {0};
    workload->write_program(environment->memory.code);
    reset_benchmark_data(environment->memory.data);
    guest_address_space_init(&environment->address_space,
            &benchmark_address_space_ops, &environment->memory, 48);
    guest_tlb_init(&environment->tlb, &environment->address_space);
    require(aarch64_runner_init_backend(&environment->runner,
                    &environment->tlb, backend),
            workload->name, "请求的执行后端不可用");
}

static bool cpu_equal(const struct cpu_state *left,
        const struct cpu_state *right) {
    if (left->mmu != right->mmu || left->cycle != right->cycle ||
            memcmp(left->x, right->x, sizeof(left->x)) != 0 ||
            left->sp != right->sp || left->pc != right->pc ||
            left->nzcv != right->nzcv ||
            memcmp(left->v, right->v, sizeof(left->v)) != 0 ||
            left->fpcr != right->fpcr || left->fpsr != right->fpsr ||
            left->tpidr_el0 != right->tpidr_el0)
        return false;
    if (left->exclusive.address != right->exclusive.address ||
            left->exclusive.value_low != right->exclusive.value_low ||
            left->exclusive.value_high != right->exclusive.value_high ||
            left->exclusive.address_space != right->exclusive.address_space ||
            left->exclusive.mapping_epoch != right->exclusive.mapping_epoch ||
            left->exclusive.write_epoch != right->exclusive.write_epoch ||
            left->exclusive.sync_identity != right->exclusive.sync_identity ||
            left->exclusive.size != right->exclusive.size ||
            left->exclusive.pair != right->exclusive.pair ||
            left->exclusive.valid != right->exclusive.valid)
        return false;
    return left->segfault_addr == right->segfault_addr &&
            left->segfault_was_write == right->segfault_was_write &&
            left->trapno == right->trapno &&
            left->single_step == right->single_step &&
            left->poked_ptr == right->poked_ptr &&
            left->_poked == right->_poked;
}

static bool step_equal(const struct aarch64_step_result *left,
        const struct aarch64_step_result *right) {
    return left->stop == right->stop &&
            left->instruction == right->instruction &&
            left->fault.address == right->fault.address &&
            left->fault.access == right->fault.access &&
            left->fault.kind == right->fault.kind;
}

static struct aarch64_threaded_stats stats_delta(
        const struct aarch64_threaded_stats *before,
        const struct aarch64_threaded_stats *after,
        const char *workload) {
    require(after->cache_hits >= before->cache_hits &&
                    after->cache_misses >= before->cache_misses &&
                    after->fast_dispatches >= before->fast_dispatches &&
                    after->c_fallbacks >= before->c_fallbacks,
            workload, "threaded 统计计数发生回退");
    return (struct aarch64_threaded_stats) {
        .cache_hits = after->cache_hits - before->cache_hits,
        .cache_misses = after->cache_misses - before->cache_misses,
        .fast_dispatches = after->fast_dispatches - before->fast_dispatches,
        .c_fallbacks = after->c_fallbacks - before->c_fallbacks,
    };
}

static void verify_stats(const struct benchmark_workload *workload,
        enum aarch64_backend backend, enum benchmark_cache_state cache_state,
        qword_t iterations, const struct aarch64_threaded_stats *delta) {
    if (backend == AARCH64_BACKEND_C) {
        require(delta->cache_hits == 0 && delta->cache_misses == 0 &&
                        delta->fast_dispatches == 0 &&
                        delta->c_fallbacks == 0,
                workload->name, "C oracle 意外修改 threaded 统计");
        return;
    }

    qword_t total = scaled_count(workload, iterations,
            workload->instructions_per_iteration, 1);
    qword_t fast = scaled_count(workload, iterations,
            workload->fast_per_iteration, 1);
    qword_t fallbacks = scaled_count(workload, iterations,
            workload->fallback_per_iteration, 0);
    require(fast + fallbacks == total,
            workload->name, "工作负载的分派计数定义不一致");
    require(delta->fast_dispatches == fast &&
                    delta->c_fallbacks == fallbacks,
            workload->name, "threaded 快速或回落分派计数不符");

    if (cache_state == BENCHMARK_CACHE_COLD) {
        require(iterations == 1,
                workload->name, "冷缓存预热必须只运行一轮");
        require(delta->cache_hits == 0 &&
                        delta->cache_misses ==
                                workload->program_instruction_count,
                workload->name, "冷缓存没有恰好填满工作集");
    } else if (cache_state == BENCHMARK_CACHE_HOT) {
        require(delta->cache_hits == total && delta->cache_misses == 0,
                workload->name, "计时区间出现 threaded 缓存未命中");
    } else {
        fail(workload->name, "threaded 运行缺少缓存状态约束");
    }
}

static void verify_run(const struct benchmark_workload *workload,
        qword_t iterations,
        const struct benchmark_environment *environment,
        const struct benchmark_run *run) {
    qword_t expected_cycles = scaled_count(workload, iterations,
            workload->instructions_per_iteration, 1);
    qword_t expected_x1 = scaled_count(workload, iterations,
            workload->x1_increment_per_iteration, 7);
    qword_t expected_x0 = workload->iteration_register == 0 ?
            0 : workload->initial_x0;
    require(run->final_step.stop == AARCH64_STEP_SYSCALL &&
                    run->final_step.instruction == INSTRUCTION_SVC,
            workload->name, "程序没有停在预期的 SVC 边界");
    require(run->cpu.cycle == expected_cycles &&
                    run->cpu.x[0] == expected_x0 &&
                    run->cpu.x[1] == expected_x1 && run->cpu.x[2] == 3 &&
                    run->cpu.x[3] == DATA_PAGE &&
                    run->cpu.x[4] == workload->expected_x4 &&
                    run->cpu.x[5] == STORE_VALUE &&
                    run->cpu.x[6] == workload->expected_x6 &&
                    run->cpu.x[7] == workload->initial_x7 &&
                    run->cpu.x[10] == workload->initial_x10,
            workload->name, "循环次数或通用寄存器结果不符");
    require(run->cpu.pc == CODE_PAGE +
                    workload->program_instruction_count * 4 &&
                    run->cpu.nzcv == UINT32_C(0x60000000),
            workload->name, "PC 或 NZCV 最终状态不符");

    byte_t expected_data[GUEST_MEMORY_PAGE_SIZE];
    reset_benchmark_data(expected_data);
    if (workload->expected_store_size != 0) {
        for (byte_t offset = 0;
                offset < workload->expected_store_size; offset += 8) {
            byte_t remaining =
                    (byte_t) (workload->expected_store_size - offset);
            byte_t size = remaining < 8 ? remaining : 8;
            store_little_endian(expected_data +
                    workload->expected_store_offset + offset,
                    size, workload->expected_store_values[offset / 8]);
        }
    }
    require(memcmp(environment->memory.data,
                    expected_data, sizeof(expected_data)) == 0,
            workload->name, "数据页最终内容不符");
}

static struct benchmark_run run_workload(
        struct benchmark_environment *environment,
        const struct benchmark_workload *workload, qword_t iterations,
        enum benchmark_cache_state cache_state) {
    reset_benchmark_data(environment->memory.data);
    struct cpu_state cpu = {
        .x[0] = workload->initial_x0,
        .x[1] = 7,
        .x[2] = 3,
        .x[3] = DATA_PAGE,
        .x[4] = workload->initial_x4,
        .x[5] = STORE_VALUE,
        .x[6] = workload->initial_x6,
        .x[7] = workload->initial_x7,
        .x[10] = workload->initial_x10,
        .pc = CODE_PAGE,
        .nzcv = UINT32_C(0x90000000),
    };
    cpu.x[workload->iteration_register] = iterations;
    cpu.v[0].d[0] = STORE_VALUE;
    cpu.v[0].d[1] = PAIR_FIRST_VALUE;
    enum aarch64_backend backend =
            aarch64_runner_backend(&environment->runner);
    struct aarch64_threaded_stats before =
            *aarch64_runner_threaded_stats(&environment->runner);
    qword_t step_count = scaled_count(workload, iterations,
            workload->instructions_per_iteration, 1);
    uint64_t started = monotonic_time_ns(workload->name);
    struct aarch64_step_result step;
    for (qword_t index = 0; index < step_count; index++)
        step = aarch64_run_one(&environment->runner, &cpu);
    uint64_t finished = monotonic_time_ns(workload->name);
    struct aarch64_threaded_stats after =
            *aarch64_runner_threaded_stats(&environment->runner);
    struct aarch64_threaded_stats delta = stats_delta(
            &before, &after, workload->name);
    verify_stats(workload, backend, cache_state, iterations, &delta);

    struct benchmark_run run = {
        .elapsed_ns = finished - started,
        .cpu = cpu,
        .final_step = step,
    };
    verify_run(workload, iterations, environment, &run);
    return run;
}

static void verify_pair(const struct benchmark_workload *workload,
        const struct benchmark_run *c_run,
        const struct benchmark_run *threaded_run) {
    require(cpu_equal(&c_run->cpu, &threaded_run->cpu),
            workload->name, "C oracle 与 threaded 最终 CPU 状态不同");
    require(step_equal(&c_run->final_step, &threaded_run->final_step),
            workload->name, "C oracle 与 threaded 最终停止结果不同");
}

static qword_t calibrate_iterations(
        struct benchmark_environment *c_environment,
        struct benchmark_environment *threaded_environment,
        const struct benchmark_workload *workload) {
    qword_t iterations = INITIAL_ITERATIONS;
    while (true) {
        struct benchmark_run c_run = run_workload(c_environment,
                workload, iterations, BENCHMARK_CACHE_UNUSED);
        struct benchmark_run threaded_run = run_workload(
                threaded_environment, workload, iterations,
                BENCHMARK_CACHE_HOT);
        verify_pair(workload, &c_run, &threaded_run);
        if (c_run.elapsed_ns >= TARGET_SAMPLE_NS &&
                threaded_run.elapsed_ns >= TARGET_SAMPLE_NS)
            return iterations;
        if (iterations > UINT64_MAX / 2)
            fail(workload->name, "无法在计数范围内达到目标采样时长");
        iterations *= 2;
    }
}

static int compare_uint64(const void *left, const void *right) {
    uint64_t a = *(const uint64_t *) left;
    uint64_t b = *(const uint64_t *) right;
    return (a > b) - (a < b);
}

static int compare_double(const void *left, const void *right) {
    double a = *(const double *) left;
    double b = *(const double *) right;
    return (a > b) - (a < b);
}

static uint64_t median_uint64(const uint64_t values[SAMPLE_COUNT]) {
    uint64_t sorted[SAMPLE_COUNT];
    memcpy(sorted, values, sizeof(sorted));
    qsort(sorted, SAMPLE_COUNT, sizeof(sorted[0]), compare_uint64);
    uint64_t lower = sorted[SAMPLE_COUNT / 2 - 1];
    uint64_t upper = sorted[SAMPLE_COUNT / 2];
    return lower + (upper - lower) / 2;
}

static double median_double(const double values[SAMPLE_COUNT]) {
    double sorted[SAMPLE_COUNT];
    memcpy(sorted, values, sizeof(sorted));
    qsort(sorted, SAMPLE_COUNT, sizeof(sorted[0]), compare_double);
    double lower = sorted[SAMPLE_COUNT / 2 - 1];
    double upper = sorted[SAMPLE_COUNT / 2];
    return lower + (upper - lower) / 2;
}

static double duration_mad_percent(
        const uint64_t values[SAMPLE_COUNT], uint64_t median) {
    uint64_t deviations[SAMPLE_COUNT];
    for (unsigned index = 0; index < SAMPLE_COUNT; index++) {
        deviations[index] = values[index] >= median ?
                values[index] - median : median - values[index];
    }
    return median == 0 ? 0 :
            (double) median_uint64(deviations) * 100 / (double) median;
}

static double ratio_mad_percent(
        const double values[SAMPLE_COUNT], double median) {
    double deviations[SAMPLE_COUNT];
    for (unsigned index = 0; index < SAMPLE_COUNT; index++) {
        double difference = values[index] - median;
        deviations[index] = difference >= 0 ? difference : -difference;
    }
    return median == 0 ? 0 :
            median_double(deviations) * 100 / median;
}

static void benchmark_workload(const struct benchmark_workload *workload) {
    struct benchmark_environment c_environment;
    struct benchmark_environment threaded_environment;
    init_environment(&c_environment, workload, AARCH64_BACKEND_C);
    init_environment(&threaded_environment, workload,
            AARCH64_BACKEND_THREADED);

    // 一轮覆盖完整工作集，计时区间只衡量稳定命中路径。
    struct benchmark_run c_warm = run_workload(&c_environment,
            workload, 1, BENCHMARK_CACHE_UNUSED);
    struct benchmark_run threaded_warm = run_workload(
            &threaded_environment, workload, 1, BENCHMARK_CACHE_COLD);
    verify_pair(workload, &c_warm, &threaded_warm);

    qword_t iterations = calibrate_iterations(&c_environment,
            &threaded_environment, workload);
    struct benchmark_sample samples[SAMPLE_COUNT];
    for (unsigned index = 0; index < SAMPLE_COUNT; index++) {
        struct benchmark_run c_run;
        struct benchmark_run threaded_run;
        if ((index & 1) == 0) {
            c_run = run_workload(&c_environment, workload,
                    iterations, BENCHMARK_CACHE_UNUSED);
            threaded_run = run_workload(&threaded_environment,
                    workload, iterations, BENCHMARK_CACHE_HOT);
        } else {
            threaded_run = run_workload(&threaded_environment,
                    workload, iterations, BENCHMARK_CACHE_HOT);
            c_run = run_workload(&c_environment, workload,
                    iterations, BENCHMARK_CACHE_UNUSED);
        }
        verify_pair(workload, &c_run, &threaded_run);
        samples[index] = (struct benchmark_sample) {
            .c_ns = c_run.elapsed_ns,
            .threaded_ns = threaded_run.elapsed_ns,
            .speedup = (double) c_run.elapsed_ns /
                    (double) threaded_run.elapsed_ns,
        };
    }

    uint64_t c_values[SAMPLE_COUNT];
    uint64_t threaded_values[SAMPLE_COUNT];
    double ratios[SAMPLE_COUNT];
    for (unsigned index = 0; index < SAMPLE_COUNT; index++) {
        c_values[index] = samples[index].c_ns;
        threaded_values[index] = samples[index].threaded_ns;
        ratios[index] = samples[index].speedup;
    }
    uint64_t c_median = median_uint64(c_values);
    uint64_t threaded_median = median_uint64(threaded_values);
    double ratio_median = median_double(ratios);
    double c_mad = duration_mad_percent(c_values, c_median);
    double threaded_mad = duration_mad_percent(
            threaded_values, threaded_median);
    double ratio_mad = ratio_mad_percent(ratios, ratio_median);
    qword_t instruction_count = scaled_count(workload, iterations,
            workload->instructions_per_iteration, 1);

    printf("\n工作负载：%s\n", workload->name);
    printf("  每组迭代：%" PRIu64 "，guest 指令：%" PRIu64 "\n",
            iterations, instruction_count);
    printf("  C oracle：%.3f ns/guest 指令（中位数，MAD %.2f%%）\n",
            (double) c_median / (double) instruction_count,
            c_mad);
    printf("  threaded：%.3f ns/guest 指令（中位数，MAD %.2f%%）\n",
            (double) threaded_median / (double) instruction_count,
            threaded_mad);
    printf("  配对加速比：%.3fx（中位数，MAD %.2f%%）\n",
            ratio_median, ratio_mad);
    if (c_mad > BASELINE_MAD_WARNING_PERCENT ||
            threaded_mad > BASELINE_MAD_WARNING_PERCENT ||
            ratio_mad > BASELINE_MAD_WARNING_PERCENT) {
        printf("  警告：本轮离散度超过 %.0f%%，不宜作为性能回归基线。\n",
                BASELINE_MAD_WARNING_PERCENT);
    }
}

int main(void) {
    printf("AArch64 C/threaded 原生微基准\n");
    printf("每项自适应到至少 100 ms，共 %d 组交替次序配对样本。\n",
            SAMPLE_COUNT);
    for (unsigned index = 0; index <
            sizeof(workloads) / sizeof(workloads[0]); index++)
        benchmark_workload(&workloads[index]);
    return 0;
}
