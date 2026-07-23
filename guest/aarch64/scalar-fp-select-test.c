#include <assert.h>
#include <string.h>

#include "guest/aarch64/decode.h"
#include "guest/aarch64/execute.h"

#define FCSEL_S UINT32_C(0x1e200c00)
#define FCSEL_D UINT32_C(0x1e600c00)

// 每一位对应一种 N/Z/C/V 组合，避免用生产条件判断器生成预期结果。
static const word_t condition_truth_masks[16] = {
    UINT16_C(0xf0f0), UINT16_C(0x0f0f),
    UINT16_C(0xcccc), UINT16_C(0x3333),
    UINT16_C(0xff00), UINT16_C(0x00ff),
    UINT16_C(0xaaaa), UINT16_C(0x5555),
    UINT16_C(0x0c0c), UINT16_C(0xf3f3),
    UINT16_C(0xaa55), UINT16_C(0x55aa),
    UINT16_C(0x0a05), UINT16_C(0xf5fa),
    UINT16_C(0xffff), UINT16_C(0xffff),
};

static dword_t encode_select(byte_t width, byte_t rd, byte_t rn,
        byte_t rm, byte_t condition) {
    return (width == 32 ? FCSEL_S : FCSEL_D) |
            (dword_t) rm << 16 | (dword_t) condition << 12 |
            (dword_t) rn << 5 | rd;
}

static struct cpu_state initial_cpu(void) {
    struct cpu_state cpu = {
        .cycle = UINT64_C(0x1020304050607080),
        .sp = UINT64_C(0x1122334455667788),
        .pc = UINT64_C(0x1000),
        .nzcv = UINT32_C(0xa0000000),
        .fpcr = AARCH64_FPCR_AHP | AARCH64_FPCR_DN |
                AARCH64_FPCR_FZ | AARCH64_FPCR_RMODE_MASK,
        .fpsr = AARCH64_FPSR_WRITE_MASK,
        .tpidr_el0 = UINT64_C(0x8877665544332211),
        .segfault_addr = UINT64_C(0x123456789abcdef0),
        .segfault_was_write = true,
        .trapno = UINT32_C(0x13572468),
        .single_step = true,
        ._poked = true,
    };
    for (unsigned reg = 0; reg < 31; reg++)
        cpu.x[reg] = UINT64_C(0x0102030405060708) ^ reg;
    for (unsigned reg = 0; reg < 32; reg++) {
        cpu.v[reg].d[0] = UINT64_C(0x1020304050607080) ^ reg;
        cpu.v[reg].d[1] = UINT64_C(0x90a0b0c0d0e0f000) ^ reg;
    }
    return cpu;
}

static qword_t scalar_bits(const struct cpu_state *cpu,
        byte_t reg, byte_t width) {
    return width == 32 ? cpu->v[reg].s[0] : cpu->v[reg].d[0];
}

static void execute_and_assert(struct cpu_state *cpu, dword_t word,
        byte_t width, byte_t rd, qword_t expected_bits) {
    struct aarch64_decoded instruction = {0};
    assert(aarch64_decode(word, &instruction));
    assert(instruction.opcode == AARCH64_OP_FCSEL_SCALAR);
    assert(instruction.width == width);

    struct cpu_state expected = *cpu;
    expected.pc += 4;
    expected.v[rd] = (union aarch64_vector_reg) {0};
    if (width == 32)
        expected.v[rd].s[0] = (dword_t) expected_bits;
    else
        expected.v[rd].d[0] = expected_bits;

    struct aarch64_execute_result result =
            aarch64_execute(cpu, NULL, &instruction);
    assert(result.stop == AARCH64_EXECUTE_RETIRED);
    assert(result.fault.kind == GUEST_MEMORY_FAULT_NONE);
    assert(memcmp(cpu, &expected, sizeof(*cpu)) == 0);
}

static void test_all_conditions_and_flags(void) {
    for (byte_t width = 32; width <= 64; width += 32) {
        for (byte_t condition = 0; condition < 16; condition++) {
            for (byte_t flags = 0; flags < 16; flags++) {
                struct cpu_state cpu = initial_cpu();
                cpu.nzcv = (dword_t) flags << 28;
                cpu.v[1].d[0] = width == 32 ?
                        UINT64_C(0xa5a5a5a57fa12345) :
                        UINT64_C(0xfff0123456789abc);
                cpu.v[1].d[1] = UINT64_C(0x1111222233334444);
                cpu.v[2].d[0] = width == 32 ?
                        UINT64_C(0x5a5a5a5a80000001) :
                        UINT64_C(0x0000000000000001);
                cpu.v[2].d[1] = UINT64_C(0x5555666677778888);
                bool choose_rn =
                        ((condition_truth_masks[condition] >> flags) & 1) != 0;
                qword_t expected = scalar_bits(
                        &cpu, choose_rn ? 1 : 2, width);
                execute_and_assert(&cpu,
                        encode_select(width, 3, 1, 2, condition),
                        width, 3, expected);
            }
        }
    }
}

static void test_product_word(void) {
    struct cpu_state taken = initial_cpu();
    taken.nzcv = UINT32_C(0x80000000);
    taken.v[0].d[0] = UINT64_C(0x7ff0123456789abc);
    taken.v[0].d[1] = UINT64_C(0x1111222233334444);
    taken.v[31].d[0] = UINT64_C(0x0000000000000001);
    taken.v[31].d[1] = UINT64_C(0x5555666677778888);
    execute_and_assert(&taken, UINT32_C(0x1e7f4c00), 64, 0,
            UINT64_C(0x7ff0123456789abc));

    struct cpu_state not_taken = initial_cpu();
    not_taken.nzcv = UINT32_C(0x60000000);
    not_taken.v[0].d[0] = UINT64_C(0x7ff0123456789abc);
    not_taken.v[0].d[1] = UINT64_C(0x1111222233334444);
    not_taken.v[31].d[0] = UINT64_C(0x0000000000000001);
    not_taken.v[31].d[1] = UINT64_C(0x5555666677778888);
    execute_and_assert(&not_taken, UINT32_C(0x1e7f4c00), 64, 0,
            UINT64_C(0x0000000000000001));
}

static void test_register_aliases(void) {
    struct cpu_state rd_rn_taken = initial_cpu();
    rd_rn_taken.nzcv = UINT32_C(0x40000000);
    rd_rn_taken.v[5].d[0] = UINT64_C(0xa5a5a5a57fa12345);
    rd_rn_taken.v[5].d[1] = UINT64_C(0x1111222233334444);
    execute_and_assert(&rd_rn_taken,
            encode_select(32, 5, 5, 6, 0), 32, 5,
            UINT32_C(0x7fa12345));

    struct cpu_state rd_rm_not_taken = initial_cpu();
    rd_rm_not_taken.nzcv = 0;
    rd_rm_not_taken.v[6].d[0] = UINT64_C(0xa5a5a5a580000001);
    rd_rm_not_taken.v[6].d[1] = UINT64_C(0x5555666677778888);
    execute_and_assert(&rd_rm_not_taken,
            encode_select(32, 6, 5, 6, 0), 32, 6,
            UINT32_C(0x80000001));

    struct cpu_state same_sources = initial_cpu();
    same_sources.nzcv = UINT32_C(0xf0000000);
    same_sources.v[7].d[0] = UINT64_C(0xfff0123456789abc);
    execute_and_assert(&same_sources,
            encode_select(64, 8, 7, 7, 13), 64, 8,
            UINT64_C(0xfff0123456789abc));

    struct cpu_state all_same = initial_cpu();
    all_same.nzcv = 0;
    all_same.v[9].d[0] = UINT64_C(0x0000000000000001);
    all_same.v[9].d[1] = UINT64_C(0x9999aaaabbbbcccc);
    execute_and_assert(&all_same,
            encode_select(64, 9, 9, 9, 15), 64, 9,
            UINT64_C(0x0000000000000001));
}

static void test_fp_state_is_ignored(void) {
    static const struct {
        dword_t fpcr;
        dword_t fpsr;
    } states[] = {
        {0, 0},
        {AARCH64_FPCR_DN | AARCH64_FPCR_RMODE_MASK,
                AARCH64_FPSR_IOC},
        {AARCH64_FPCR_FZ | UINT32_C(0x00800000),
                AARCH64_FPSR_IDC | AARCH64_FPSR_IXC},
        {AARCH64_FPCR_AHP | AARCH64_FPCR_DN | AARCH64_FPCR_FZ |
                AARCH64_FPCR_RMODE_MASK, AARCH64_FPSR_WRITE_MASK},
        {UINT32_C(0x03009f00), 0},
    };
    for (unsigned state = 0; state < sizeof(states) / sizeof(states[0]);
            state++) {
        struct cpu_state single = initial_cpu();
        single.nzcv = UINT32_C(0x40000000);
        single.fpcr = states[state].fpcr;
        single.fpsr = states[state].fpsr;
        single.v[1].d[0] = UINT64_C(0xa5a5a5a57f812345);
        single.v[1].d[1] = UINT64_MAX;
        execute_and_assert(&single,
                encode_select(32, 3, 1, 2, 0), 32, 3,
                UINT32_C(0x7f812345));

        struct cpu_state double_precision = initial_cpu();
        double_precision.nzcv = 0;
        double_precision.fpcr = states[state].fpcr;
        double_precision.fpsr = states[state].fpsr;
        double_precision.v[2].d[0] =
                UINT64_C(0x0000000000000001);
        double_precision.v[2].d[1] = UINT64_MAX;
        execute_and_assert(&double_precision,
                encode_select(64, 3, 1, 2, 0), 64, 3,
                UINT64_C(0x0000000000000001));
    }
}

int main(void) {
    test_all_conditions_and_flags();
    test_product_word();
    test_register_aliases();
    test_fp_state_is_ignored();
    return 0;
}
