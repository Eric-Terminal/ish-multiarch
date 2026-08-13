#include <assert.h>
#include <string.h>

#include "guest/aarch64/decode.h"
#include "guest/aarch64/execute.h"

#define FSQRT_S UINT32_C(0x1e21c000)
#define FSQRT_D UINT32_C(0x1e61c000)
#define TEST_FPCR_RP (UINT32_C(1) << AARCH64_FPCR_RMODE_SHIFT)
#define TEST_FPCR_RM (UINT32_C(2) << AARCH64_FPCR_RMODE_SHIFT)
#define TEST_FPCR_RZ (UINT32_C(3) << AARCH64_FPCR_RMODE_SHIFT)

struct sqrt_case {
    byte_t width;
    qword_t input;
    dword_t fpcr;
    qword_t expected;
    dword_t exceptions;
};

static const struct sqrt_case cases[] = {
    {32, UINT32_C(0x00000000), 0, UINT32_C(0x00000000), 0},
    {32, UINT32_C(0x80000000), 0, UINT32_C(0x80000000), 0},
    {32, UINT32_C(0x3f800000), 0, UINT32_C(0x3f800000), 0},
    {32, UINT32_C(0x40800000), 0, UINT32_C(0x40000000), 0},
    {32, UINT32_C(0x40000000), 0, UINT32_C(0x3fb504f3),
            AARCH64_FPSR_IXC},
    {32, UINT32_C(0x00800000), 0, UINT32_C(0x20000000), 0},
    {32, UINT32_C(0x00000001), 0, UINT32_C(0x1a3504f3),
            AARCH64_FPSR_IXC},
    {32, UINT32_C(0x007fffff), 0, UINT32_C(0x1fffffff),
            AARCH64_FPSR_IXC},
    {32, UINT32_C(0x7f7fffff), 0, UINT32_C(0x5f7fffff),
            AARCH64_FPSR_IXC},
    {32, UINT32_C(0x7f800000), 0, UINT32_C(0x7f800000), 0},
    {32, UINT32_C(0xbf800000), 0, UINT32_C(0x7fc00000),
            AARCH64_FPSR_IOC},
    {32, UINT32_C(0xff800000), 0, UINT32_C(0x7fc00000),
            AARCH64_FPSR_IOC},
    {32, UINT32_C(0x7fc12345), 0, UINT32_C(0x7fc12345), 0},
    {32, UINT32_C(0xffc12345), 0, UINT32_C(0xffc12345), 0},
    {32, UINT32_C(0x7f812345), 0, UINT32_C(0x7fc12345),
            AARCH64_FPSR_IOC},
    {32, UINT32_C(0xff812345), AARCH64_FPCR_DN,
            UINT32_C(0x7fc00000), AARCH64_FPSR_IOC},
    {32, UINT32_C(0xffc12345), AARCH64_FPCR_DN,
            UINT32_C(0x7fc00000), 0},
    {32, UINT32_C(0x00000001), AARCH64_FPCR_FZ,
            UINT32_C(0x00000000), AARCH64_FPSR_IDC},
    {32, UINT32_C(0x80000001), AARCH64_FPCR_FZ,
            UINT32_C(0x80000000), AARCH64_FPSR_IDC},
    {64, UINT64_C(0x0000000000000000), 0,
            UINT64_C(0x0000000000000000), 0},
    {64, UINT64_C(0x8000000000000000), 0,
            UINT64_C(0x8000000000000000), 0},
    {64, UINT64_C(0x3ff0000000000000), 0,
            UINT64_C(0x3ff0000000000000), 0},
    {64, UINT64_C(0x4010000000000000), 0,
            UINT64_C(0x4000000000000000), 0},
    {64, UINT64_C(0x4000000000000000), 0,
            UINT64_C(0x3ff6a09e667f3bcd), AARCH64_FPSR_IXC},
    {64, UINT64_C(0x0010000000000000), 0,
            UINT64_C(0x2000000000000000), 0},
    {64, UINT64_C(0x0000000000000001), 0,
            UINT64_C(0x1e60000000000000), 0},
    {64, UINT64_C(0x000fffffffffffff), 0,
            UINT64_C(0x1fffffffffffffff), AARCH64_FPSR_IXC},
    {64, UINT64_C(0x7fefffffffffffff), 0,
            UINT64_C(0x5fefffffffffffff), AARCH64_FPSR_IXC},
    {64, UINT64_C(0x7ff0000000000000), 0,
            UINT64_C(0x7ff0000000000000), 0},
    {64, UINT64_C(0xbff0000000000000), 0,
            UINT64_C(0x7ff8000000000000), AARCH64_FPSR_IOC},
    {64, UINT64_C(0xfff0000000000000), 0,
            UINT64_C(0x7ff8000000000000), AARCH64_FPSR_IOC},
    {64, UINT64_C(0x7ff8123456789abc), 0,
            UINT64_C(0x7ff8123456789abc), 0},
    {64, UINT64_C(0xfff8123456789abc), 0,
            UINT64_C(0xfff8123456789abc), 0},
    {64, UINT64_C(0x7ff0123456789abc), 0,
            UINT64_C(0x7ff8123456789abc), AARCH64_FPSR_IOC},
    {64, UINT64_C(0xfff0123456789abc), AARCH64_FPCR_DN,
            UINT64_C(0x7ff8000000000000), AARCH64_FPSR_IOC},
    {64, UINT64_C(0xfff8123456789abc), AARCH64_FPCR_DN,
            UINT64_C(0x7ff8000000000000), 0},
    {64, UINT64_C(0x0000000000000001), AARCH64_FPCR_FZ,
            UINT64_C(0x0000000000000000), AARCH64_FPSR_IDC},
    {64, UINT64_C(0x8000000000000001), AARCH64_FPCR_FZ,
            UINT64_C(0x8000000000000000), AARCH64_FPSR_IDC},
};

static dword_t encode_sqrt(byte_t width, byte_t rd, byte_t rn) {
    return (width == 32 ? FSQRT_S : FSQRT_D) |
            (dword_t) rn << 5 | rd;
}

static struct cpu_state initial_cpu(void) {
    struct cpu_state cpu = {
        .cycle = UINT64_C(0x1020304050607080),
        .sp = UINT64_C(0x1122334455667788),
        .pc = UINT64_C(0x1000),
        .nzcv = UINT32_C(0xa0000000),
        .fpcr = AARCH64_FPCR_DN | AARCH64_FPCR_FZ,
        .fpsr = AARCH64_FPSR_QC,
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
        byte_t rd, byte_t rn, qword_t input, dword_t fpcr,
        dword_t initial_fpsr, qword_t expected_bits,
        dword_t exceptions) {
    struct aarch64_decoded instruction = {0};
    assert(aarch64_decode(word, &instruction));
    assert(instruction.opcode == AARCH64_OP_FSQRT_SCALAR);
    assert(instruction.width == width);
    assert(instruction.operands.data_processing_1source.rd == rd);
    assert(instruction.operands.data_processing_1source.rn == rn);

    struct cpu_state cpu = initial_cpu();
    cpu.fpcr = fpcr;
    cpu.fpsr = initial_fpsr;
    if (width == 32)
        cpu.v[rn].s[0] = (dword_t) input;
    else
        cpu.v[rn].d[0] = input;

    struct cpu_state expected = cpu;
    expected.pc += 4;
    expected.fpsr |= exceptions;
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

static void test_boundary_cases(void) {
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        execute_and_assert(encode_sqrt(cases[i].width, 3, 5),
                cases[i].width, 3, 5, cases[i].input,
                cases[i].fpcr, AARCH64_FPSR_QC,
                cases[i].expected, cases[i].exceptions);
    }
}

static void test_rounding_modes(void) {
    static const struct sqrt_case rounding_cases[] = {
        {32, UINT32_C(0x40000000), 0, UINT32_C(0x3fb504f3),
                AARCH64_FPSR_IXC},
        {32, UINT32_C(0x40000000), TEST_FPCR_RP,
                UINT32_C(0x3fb504f4), AARCH64_FPSR_IXC},
        {32, UINT32_C(0x40000000), TEST_FPCR_RM,
                UINT32_C(0x3fb504f3), AARCH64_FPSR_IXC},
        {32, UINT32_C(0x40000000), TEST_FPCR_RZ,
                UINT32_C(0x3fb504f3), AARCH64_FPSR_IXC},
        {32, UINT32_C(0x7f7fffff), TEST_FPCR_RP,
                UINT32_C(0x5f800000), AARCH64_FPSR_IXC},
        {64, UINT64_C(0x4000000000000000), 0,
                UINT64_C(0x3ff6a09e667f3bcd), AARCH64_FPSR_IXC},
        {64, UINT64_C(0x4000000000000000), TEST_FPCR_RP,
                UINT64_C(0x3ff6a09e667f3bcd), AARCH64_FPSR_IXC},
        {64, UINT64_C(0x4000000000000000), TEST_FPCR_RM,
                UINT64_C(0x3ff6a09e667f3bcc), AARCH64_FPSR_IXC},
        {64, UINT64_C(0x4000000000000000), TEST_FPCR_RZ,
                UINT64_C(0x3ff6a09e667f3bcc), AARCH64_FPSR_IXC},
        {64, UINT64_C(0x7fefffffffffffff), TEST_FPCR_RP,
                UINT64_C(0x5ff0000000000000), AARCH64_FPSR_IXC},
    };
    for (unsigned i = 0; i < sizeof(rounding_cases) /
            sizeof(rounding_cases[0]); i++) {
        execute_and_assert(encode_sqrt(
                rounding_cases[i].width, 3, 5),
                rounding_cases[i].width, 3, 5,
                rounding_cases[i].input, rounding_cases[i].fpcr,
                AARCH64_FPSR_QC | AARCH64_FPSR_DZC,
                rounding_cases[i].expected,
                rounding_cases[i].exceptions);
    }
}

static void test_product_word_and_alias(void) {
    execute_and_assert(UINT32_C(0x1e61c000), 64, 0, 0,
            UINT64_C(0x4010000000000000), 0,
            AARCH64_FPSR_IXC, UINT64_C(0x4000000000000000), 0);
    execute_and_assert(UINT32_C(0x1e21c3ff), 32, 31, 31,
            UINT32_C(0x40800000), 0,
            AARCH64_FPSR_QC, UINT32_C(0x40000000), 0);
}

int main(void) {
    test_boundary_cases();
    test_rounding_modes();
    test_product_word_and_alias();
    return 0;
}
