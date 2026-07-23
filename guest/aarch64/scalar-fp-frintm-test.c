#include <assert.h>
#include <string.h>

#include "guest/aarch64/decode.h"
#include "guest/aarch64/execute.h"

#define FRINTM_S UINT32_C(0x1e254000)
#define FRINTM_D UINT32_C(0x1e654000)

struct frintm_case {
    byte_t width;
    qword_t input;
    dword_t fpcr;
    dword_t initial_fpsr;
    qword_t expected;
    dword_t exceptions;
};

static const struct frintm_case cases[] = {
    {32, UINT32_C(0x00000000), 0, 0, UINT32_C(0x00000000), 0},
    {32, UINT32_C(0x80000000), 0, 0, UINT32_C(0x80000000), 0},
    {32, UINT32_C(0x3e800000), 0, 0, UINT32_C(0x00000000), 0},
    {32, UINT32_C(0xbe800000), 0, 0, UINT32_C(0xbf800000), 0},
    {32, UINT32_C(0x3fc00000), 0, 0, UINT32_C(0x3f800000), 0},
    {32, UINT32_C(0xbfc00000), 0, 0, UINT32_C(0xc0000000), 0},
    {32, UINT32_C(0x403fffff), 0, 0, UINT32_C(0x40000000), 0},
    {32, UINT32_C(0xc03fffff), 0, 0, UINT32_C(0xc0400000), 0},
    {32, UINT32_C(0x4affffff), 0, 0, UINT32_C(0x4afffffe), 0},
    {32, UINT32_C(0xcaffffff), 0, 0, UINT32_C(0xcb000000), 0},
    {32, UINT32_C(0x4b000000), 0, 0, UINT32_C(0x4b000000), 0},
    {32, UINT32_C(0x7f7fffff), 0, 0, UINT32_C(0x7f7fffff), 0},
    {32, UINT32_C(0x7f800000), 0, 0, UINT32_C(0x7f800000), 0},
    {32, UINT32_C(0xff800000), 0, 0, UINT32_C(0xff800000), 0},
    {32, UINT32_C(0x7fc12345), 0, 0, UINT32_C(0x7fc12345), 0},
    {32, UINT32_C(0xffc54321), 0, 0, UINT32_C(0xffc54321), 0},
    {32, UINT32_C(0xffc12345), AARCH64_FPCR_DN, 0,
            UINT32_C(0x7fc00000), 0},
    {32, UINT32_C(0x7f812345), 0, 0, UINT32_C(0x7fc12345),
            AARCH64_FPSR_IOC},
    {32, UINT32_C(0xff812345), AARCH64_FPCR_DN, 0,
            UINT32_C(0x7fc00000), AARCH64_FPSR_IOC},
    {32, UINT32_C(0x00000001), 0, 0, UINT32_C(0x00000000), 0},
    {32, UINT32_C(0x80000001), 0, 0, UINT32_C(0xbf800000), 0},
    {32, UINT32_C(0x00000001), AARCH64_FPCR_FZ, 0,
            UINT32_C(0x00000000), AARCH64_FPSR_IDC},
    {32, UINT32_C(0x80000001), AARCH64_FPCR_FZ, 0,
            UINT32_C(0x80000000), AARCH64_FPSR_IDC},
    {32, UINT32_C(0x007fffff), AARCH64_FPCR_FZ, 0,
            UINT32_C(0x00000000), AARCH64_FPSR_IDC},
    {32, UINT32_C(0x807fffff), AARCH64_FPCR_FZ, 0,
            UINT32_C(0x80000000), AARCH64_FPSR_IDC},
    {32, UINT32_C(0x00800000), AARCH64_FPCR_FZ, 0,
            UINT32_C(0x00000000), 0},
    {32, UINT32_C(0x80800000), AARCH64_FPCR_FZ, 0,
            UINT32_C(0xbf800000), 0},
    {64, UINT64_C(0x0000000000000000), 0, 0,
            UINT64_C(0x0000000000000000), 0},
    {64, UINT64_C(0x8000000000000000), 0, 0,
            UINT64_C(0x8000000000000000), 0},
    {64, UINT64_C(0x3fd0000000000000), 0, 0,
            UINT64_C(0x0000000000000000), 0},
    {64, UINT64_C(0xbfd0000000000000), 0, 0,
            UINT64_C(0xbff0000000000000), 0},
    {64, UINT64_C(0x3ff8000000000000), 0, 0,
            UINT64_C(0x3ff0000000000000), 0},
    {64, UINT64_C(0xbff8000000000000), 0, 0,
            UINT64_C(0xc000000000000000), 0},
    {64, UINT64_C(0x4007ffffffffffff), 0, 0,
            UINT64_C(0x4000000000000000), 0},
    {64, UINT64_C(0xc007ffffffffffff), 0, 0,
            UINT64_C(0xc008000000000000), 0},
    {64, UINT64_C(0x432fffffffffffff), 0, 0,
            UINT64_C(0x432ffffffffffffe), 0},
    {64, UINT64_C(0xc32fffffffffffff), 0, 0,
            UINT64_C(0xc330000000000000), 0},
    {64, UINT64_C(0x4330000000000000), 0, 0,
            UINT64_C(0x4330000000000000), 0},
    {64, UINT64_C(0x7fefffffffffffff), 0, 0,
            UINT64_C(0x7fefffffffffffff), 0},
    {64, UINT64_C(0x7ff0000000000000), 0, 0,
            UINT64_C(0x7ff0000000000000), 0},
    {64, UINT64_C(0xfff0000000000000), 0, 0,
            UINT64_C(0xfff0000000000000), 0},
    {64, UINT64_C(0x7ff8123456789abc), 0, 0,
            UINT64_C(0x7ff8123456789abc), 0},
    {64, UINT64_C(0xfff8543210abcdef), 0, 0,
            UINT64_C(0xfff8543210abcdef), 0},
    {64, UINT64_C(0xfff8123456789abc), AARCH64_FPCR_DN, 0,
            UINT64_C(0x7ff8000000000000), 0},
    {64, UINT64_C(0x7ff0123456789abc), 0, 0,
            UINT64_C(0x7ff8123456789abc), AARCH64_FPSR_IOC},
    {64, UINT64_C(0xfff0123456789abc), AARCH64_FPCR_DN, 0,
            UINT64_C(0x7ff8000000000000), AARCH64_FPSR_IOC},
    {64, UINT64_C(0x0000000000000001), 0, 0,
            UINT64_C(0x0000000000000000), 0},
    {64, UINT64_C(0x8000000000000001), 0, 0,
            UINT64_C(0xbff0000000000000), 0},
    {64, UINT64_C(0x0000000000000001), AARCH64_FPCR_FZ, 0,
            UINT64_C(0x0000000000000000), AARCH64_FPSR_IDC},
    {64, UINT64_C(0x8000000000000001), AARCH64_FPCR_FZ, 0,
            UINT64_C(0x8000000000000000), AARCH64_FPSR_IDC},
    {64, UINT64_C(0x000fffffffffffff), AARCH64_FPCR_FZ, 0,
            UINT64_C(0x0000000000000000), AARCH64_FPSR_IDC},
    {64, UINT64_C(0x800fffffffffffff), AARCH64_FPCR_FZ, 0,
            UINT64_C(0x8000000000000000), AARCH64_FPSR_IDC},
    {64, UINT64_C(0x0010000000000000), AARCH64_FPCR_FZ, 0,
            UINT64_C(0x0000000000000000), 0},
    {64, UINT64_C(0x8010000000000000), AARCH64_FPCR_FZ, 0,
            UINT64_C(0xbff0000000000000), 0},
    {64, UINT64_C(0xbff8000000000000), AARCH64_FPCR_AHP,
            AARCH64_FPSR_QC | AARCH64_FPSR_DZC |
                    AARCH64_FPSR_IXC,
            UINT64_C(0xc000000000000000), 0},
};

static dword_t encode_frintm(
        byte_t width, byte_t rd, byte_t rn) {
    return (width == 32 ? FRINTM_S : FRINTM_D) |
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
    assert(instruction.opcode == AARCH64_OP_FRINTM_SCALAR);
    assert(instruction.width == width);
    assert(instruction.operands.data_processing_1source.rd == rd);
    assert(instruction.operands.data_processing_1source.rn == rn);

    struct cpu_state cpu = initial_cpu();
    cpu.fpcr = fpcr;
    cpu.fpsr = initial_fpsr;
    cpu.v[rn].d[0] = UINT64_C(0xa5a5a5a55a5a5a5a);
    cpu.v[rn].d[1] = UINT64_C(0x0123456789abcdef);
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
        execute_and_assert(encode_frintm(cases[i].width, 3, 5),
                cases[i].width, 3, 5, cases[i].input,
                cases[i].fpcr, cases[i].initial_fpsr,
                cases[i].expected, cases[i].exceptions);
    }
}

static void test_rounding_mode_is_ignored(void) {
    for (dword_t mode = 0; mode < 4; mode++) {
        dword_t fpcr = mode << AARCH64_FPCR_RMODE_SHIFT;
        execute_and_assert(encode_frintm(32, 3, 5), 32, 3, 5,
                UINT32_C(0x3fe00000), fpcr, 0,
                UINT32_C(0x3f800000), 0);
        execute_and_assert(encode_frintm(32, 3, 5), 32, 3, 5,
                UINT32_C(0xbfe00000), fpcr, 0,
                UINT32_C(0xc0000000), 0);
        execute_and_assert(encode_frintm(64, 3, 5), 64, 3, 5,
                UINT64_C(0x3ffc000000000000), fpcr, 0,
                UINT64_C(0x3ff0000000000000), 0);
        execute_and_assert(encode_frintm(64, 3, 5), 64, 3, 5,
                UINT64_C(0xbffc000000000000), fpcr, 0,
                UINT64_C(0xc000000000000000), 0);
    }
}

static void test_product_word_and_alias(void) {
    execute_and_assert(UINT32_C(0x1e654000), 64, 0, 0,
            UINT64_C(0x42012e0be826d695), 0,
            AARCH64_FPSR_IXC, UINT64_C(0x42012e0be8200000), 0);
}

int main(void) {
    test_boundary_cases();
    test_rounding_mode_is_ignored();
    test_product_word_and_alias();
    return 0;
}
