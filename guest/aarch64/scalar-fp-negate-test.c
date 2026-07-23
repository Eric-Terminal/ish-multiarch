#include <assert.h>
#include <string.h>

#include "guest/aarch64/decode.h"
#include "guest/aarch64/execute.h"

#define FNEG_S UINT32_C(0x1e214000)
#define FNEG_D UINT32_C(0x1e614000)

struct negate_case {
    byte_t width;
    qword_t input;
    qword_t expected;
};

// 固定十六进制预期避免由生产符号位逻辑反向生成测试答案。
static const struct negate_case cases[] = {
    {32, UINT32_C(0x00000000), UINT32_C(0x80000000)},
    {32, UINT32_C(0x80000000), UINT32_C(0x00000000)},
    {32, UINT32_C(0x3fa00000), UINT32_C(0xbfa00000)},
    {32, UINT32_C(0xbfa00000), UINT32_C(0x3fa00000)},
    {32, UINT32_C(0x00000001), UINT32_C(0x80000001)},
    {32, UINT32_C(0x80000001), UINT32_C(0x00000001)},
    {32, UINT32_C(0x007fffff), UINT32_C(0x807fffff)},
    {32, UINT32_C(0x00800000), UINT32_C(0x80800000)},
    {32, UINT32_C(0x7f800000), UINT32_C(0xff800000)},
    {32, UINT32_C(0xff800000), UINT32_C(0x7f800000)},
    {32, UINT32_C(0x7fc12345), UINT32_C(0xffc12345)},
    {32, UINT32_C(0xffc54321), UINT32_C(0x7fc54321)},
    {32, UINT32_C(0x7f812345), UINT32_C(0xff812345)},
    {32, UINT32_C(0xff854321), UINT32_C(0x7f854321)},
    {64, UINT64_C(0x0000000000000000),
            UINT64_C(0x8000000000000000)},
    {64, UINT64_C(0x8000000000000000),
            UINT64_C(0x0000000000000000)},
    {64, UINT64_C(0x3ff4000000000000),
            UINT64_C(0xbff4000000000000)},
    {64, UINT64_C(0xbff4000000000000),
            UINT64_C(0x3ff4000000000000)},
    {64, UINT64_C(0x0000000000000001),
            UINT64_C(0x8000000000000001)},
    {64, UINT64_C(0x8000000000000001),
            UINT64_C(0x0000000000000001)},
    {64, UINT64_C(0x000fffffffffffff),
            UINT64_C(0x800fffffffffffff)},
    {64, UINT64_C(0x0010000000000000),
            UINT64_C(0x8010000000000000)},
    {64, UINT64_C(0x7ff0000000000000),
            UINT64_C(0xfff0000000000000)},
    {64, UINT64_C(0xfff0000000000000),
            UINT64_C(0x7ff0000000000000)},
    {64, UINT64_C(0x7ff8123456789abc),
            UINT64_C(0xfff8123456789abc)},
    {64, UINT64_C(0xfff8543210abcdef),
            UINT64_C(0x7ff8543210abcdef)},
    {64, UINT64_C(0x7ff0123456789abc),
            UINT64_C(0xfff0123456789abc)},
    {64, UINT64_C(0xfff0543210abcdef),
            UINT64_C(0x7ff0543210abcdef)},
};

static dword_t encode_negate(
        byte_t width, byte_t rd, byte_t rn) {
    return (width == 32 ? FNEG_S : FNEG_D) |
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

static void execute_and_assert(dword_t word, byte_t width,
        byte_t rd, byte_t rn, qword_t input, qword_t expected_bits,
        dword_t fpcr, dword_t fpsr) {
    struct aarch64_decoded instruction = {0};
    assert(aarch64_decode(word, &instruction));
    assert(instruction.opcode == AARCH64_OP_FNEG_SCALAR);
    assert(instruction.width == width);
    assert(instruction.operands.data_processing_1source.rd == rd);
    assert(instruction.operands.data_processing_1source.rn == rn);

    struct cpu_state cpu = initial_cpu();
    cpu.fpcr = fpcr;
    cpu.fpsr = fpsr;
    cpu.v[rn].d[0] = UINT64_C(0xa5a5a5a55a5a5a5a);
    cpu.v[rn].d[1] = UINT64_C(0x0123456789abcdef);
    if (width == 32)
        cpu.v[rn].s[0] = (dword_t) input;
    else
        cpu.v[rn].d[0] = input;

    struct cpu_state expected = cpu;
    expected.pc += 4;
    expected.v[rd] = (union aarch64_vector_reg) {0};
    if (width == 32)
        expected.v[rd].s[0] = (dword_t) expected_bits;
    else
        expected.v[rd].d[0] = expected_bits;

    struct aarch64_execute_result result =
            aarch64_execute(&cpu, NULL, &instruction);
    assert(result.stop == AARCH64_EXECUTE_RETIRED);
    assert(result.fault.kind == GUEST_MEMORY_FAULT_NONE);
    assert(memcmp(&cpu, &expected, sizeof(cpu)) == 0);
}

static void test_bit_patterns(void) {
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        execute_and_assert(encode_negate(cases[i].width, 3, 5),
                cases[i].width, 3, 5, cases[i].input,
                cases[i].expected,
                AARCH64_FPCR_AHP | AARCH64_FPCR_DN |
                        AARCH64_FPCR_FZ | AARCH64_FPCR_RMODE_MASK,
                AARCH64_FPSR_WRITE_MASK);
    }
}

static void test_fp_state_is_ignored(void) {
    static const dword_t fpcrs[] = {
        0,
        AARCH64_FPCR_FZ,
        AARCH64_FPCR_DN,
        AARCH64_FPCR_AHP,
        UINT32_C(1) << AARCH64_FPCR_RMODE_SHIFT,
        UINT32_C(2) << AARCH64_FPCR_RMODE_SHIFT,
        UINT32_C(3) << AARCH64_FPCR_RMODE_SHIFT,
        AARCH64_FPCR_AHP | AARCH64_FPCR_DN | AARCH64_FPCR_FZ |
                AARCH64_FPCR_RMODE_MASK | UINT32_C(0x00009f00),
    };
    for (unsigned i = 0; i < sizeof(fpcrs) / sizeof(fpcrs[0]); i++) {
        dword_t fpsr = i % 2 == 0 ? 0 : AARCH64_FPSR_WRITE_MASK;
        execute_and_assert(encode_negate(32, 3, 5), 32, 3, 5,
                UINT32_C(0x7f812345), UINT32_C(0xff812345),
                fpcrs[i], fpsr);
        execute_and_assert(encode_negate(64, 3, 5), 64, 3, 5,
                UINT64_C(0x0000000000000001),
                UINT64_C(0x8000000000000001), fpcrs[i], fpsr);
    }
}

static void test_register_aliases_and_product_word(void) {
    execute_and_assert(encode_negate(32, 31, 31), 32, 31, 31,
            UINT32_C(0xff812345), UINT32_C(0x7f812345),
            AARCH64_FPCR_DN | AARCH64_FPCR_FZ,
            AARCH64_FPSR_IOC | AARCH64_FPSR_IDC);
    execute_and_assert(encode_negate(64, 31, 5), 64, 31, 5,
            UINT64_C(0x7ff8123456789abc),
            UINT64_C(0xfff8123456789abc), AARCH64_FPCR_DN,
            AARCH64_FPSR_IOC);
    execute_and_assert(UINT32_C(0x1e614000), 64, 0, 0,
            UINT64_C(0x7ff0000000000000),
            UINT64_C(0xfff0000000000000), 0, AARCH64_FPSR_IXC);
}

int main(void) {
    test_bit_patterns();
    test_fp_state_is_ignored();
    test_register_aliases_and_product_word();
    return 0;
}
