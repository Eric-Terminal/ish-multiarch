#include <assert.h>
#include <string.h>

#include "guest/aarch64/decode.h"
#include "guest/aarch64/execute.h"

#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

#define FDIV_S UINT32_C(0x1e201800)
#define FDIV_D UINT32_C(0x1e601800)

#define TEST_FPCR_RP UINT32_C(0x00400000)
#define TEST_FPCR_RM UINT32_C(0x00800000)
#define TEST_FPCR_RZ UINT32_C(0x00c00000)
#define TEST_FPCR_FZ UINT32_C(0x01000000)
#define TEST_FPCR_DN UINT32_C(0x02000000)

#define TEST_FPSR_IOC UINT32_C(0x00000001)
#define TEST_FPSR_DZC UINT32_C(0x00000002)
#define TEST_FPSR_OFC UINT32_C(0x00000004)
#define TEST_FPSR_UFC UINT32_C(0x00000008)
#define TEST_FPSR_IXC UINT32_C(0x00000010)
#define TEST_FPSR_IDC UINT32_C(0x00000080)
#define TEST_FPSR_QC UINT32_C(0x08000000)

struct divide_case {
    byte_t width;
    qword_t left;
    qword_t right;
    dword_t fpcr;
    qword_t expected;
    dword_t exceptions;
};

static dword_t encode_divide(
        byte_t width, byte_t rd, byte_t rn, byte_t rm) {
    return (width == 32 ? FDIV_S : FDIV_D) |
            (dword_t) rm << 16 | (dword_t) rn << 5 | rd;
}

static struct cpu_state initial_cpu(dword_t fpcr) {
    struct cpu_state cpu = {
        .pc = UINT64_C(0x1000),
        .sp = UINT64_C(0x1122334455667788),
        .nzcv = UINT32_C(0xa0000000),
        .fpcr = fpcr,
        .fpsr = TEST_FPSR_QC,
        .tpidr_el0 = UINT64_C(0x8877665544332211),
    };
    cpu.x[3] = UINT64_C(0x0123456789abcdef);
    cpu.x[19] = UINT64_C(0xfedcba9876543210);
    return cpu;
}

static void set_scalar(struct cpu_state *cpu, byte_t reg,
        byte_t width, qword_t bits) {
    cpu->v[reg].d[0] = width == 32 ?
            UINT64_C(0xa5a5a5a500000000) | (dword_t) bits : bits;
    cpu->v[reg].d[1] = UINT64_C(0x5a5a5a5a5a5a5a5a);
}

static qword_t scalar(
        const struct cpu_state *cpu, byte_t reg, byte_t width) {
    return width == 32 ? cpu->v[reg].s[0] : cpu->v[reg].d[0];
}

static void execute_word(struct cpu_state *cpu, dword_t word,
        byte_t expected_width) {
    struct aarch64_decoded instruction = {0};
    assert(aarch64_decode(word, &instruction));
    assert(instruction.opcode == AARCH64_OP_FDIV_SCALAR);
    assert(instruction.width == expected_width);
    struct aarch64_execute_result result =
            aarch64_execute(cpu, NULL, &instruction);
    assert(result.stop == AARCH64_EXECUTE_RETIRED);
}

static void assert_scalar_result(const struct cpu_state *cpu,
        byte_t reg, byte_t width, qword_t expected) {
    assert(scalar(cpu, reg, width) == expected);
    assert(cpu->v[reg].d[1] == 0);
    if (width == 32)
        assert((cpu->v[reg].d[0] >> 32) == 0);
}

static void assert_non_fp_state(
        const struct cpu_state *cpu, dword_t expected_fpcr) {
    assert(cpu->pc == UINT64_C(0x1004));
    assert(cpu->sp == UINT64_C(0x1122334455667788));
    assert(cpu->nzcv == UINT32_C(0xa0000000));
    assert(cpu->fpcr == expected_fpcr);
    assert(cpu->x[3] == UINT64_C(0x0123456789abcdef));
    assert(cpu->x[19] == UINT64_C(0xfedcba9876543210));
    assert(cpu->tpidr_el0 == UINT64_C(0x8877665544332211));
}

static void run_case(const struct divide_case *test) {
    struct cpu_state cpu = initial_cpu(test->fpcr);
    set_scalar(&cpu, 1, test->width, test->left);
    set_scalar(&cpu, 2, test->width, test->right);
    union aarch64_vector_reg left = cpu.v[1];
    union aarch64_vector_reg right = cpu.v[2];

    execute_word(&cpu, encode_divide(test->width, 3, 1, 2),
            test->width);
    assert_scalar_result(&cpu, 3, test->width, test->expected);
    assert(memcmp(&cpu.v[1], &left, sizeof(left)) == 0);
    assert(memcmp(&cpu.v[2], &right, sizeof(right)) == 0);
    assert(cpu.fpsr == (TEST_FPSR_QC | test->exceptions));
    assert_non_fp_state(&cpu, test->fpcr);
}

static void test_product_word_and_aliases(void) {
    struct cpu_state product = initial_cpu(0);
    set_scalar(&product, 0, 32, UINT32_C(0x3f800000));
    set_scalar(&product, 30, 32, UINT32_C(0x41800000));
    union aarch64_vector_reg left = product.v[0];
    execute_word(&product, UINT32_C(0x1e3e181e), 32);
    assert_scalar_result(
            &product, 30, 32, UINT32_C(0x3d800000));
    assert(memcmp(&product.v[0], &left, sizeof(left)) == 0);
    assert(product.fpsr == TEST_FPSR_QC);

    struct cpu_state aliased_left = initial_cpu(0);
    set_scalar(&aliased_left, 5, 32, UINT32_C(0x40c00000));
    set_scalar(&aliased_left, 6, 32, UINT32_C(0x40800000));
    union aarch64_vector_reg right = aliased_left.v[6];
    execute_word(&aliased_left, encode_divide(32, 5, 5, 6), 32);
    assert_scalar_result(
            &aliased_left, 5, 32, UINT32_C(0x3fc00000));
    assert(memcmp(&aliased_left.v[6], &right, sizeof(right)) == 0);

    struct cpu_state all_aliased = initial_cpu(0);
    set_scalar(&all_aliased, 7, 64,
            UINT64_C(0x400c000000000000));
    execute_word(&all_aliased, encode_divide(64, 7, 7, 7), 64);
    assert_scalar_result(&all_aliased, 7, 64,
            UINT64_C(0x3ff0000000000000));
}

static void test_rounding_modes(void) {
    static const struct divide_case cases[] = {
        {32, UINT32_C(0x3f800000), UINT32_C(0x40400000), 0,
                UINT32_C(0x3eaaaaab), TEST_FPSR_IXC},
        {32, UINT32_C(0x3f800000), UINT32_C(0x40400000),
                TEST_FPCR_RP, UINT32_C(0x3eaaaaab), TEST_FPSR_IXC},
        {32, UINT32_C(0x3f800000), UINT32_C(0x40400000),
                TEST_FPCR_RM, UINT32_C(0x3eaaaaaa), TEST_FPSR_IXC},
        {32, UINT32_C(0x3f800000), UINT32_C(0x40400000),
                TEST_FPCR_RZ, UINT32_C(0x3eaaaaaa), TEST_FPSR_IXC},
        {32, UINT32_C(0xbf800000), UINT32_C(0x40400000), 0,
                UINT32_C(0xbeaaaaab), TEST_FPSR_IXC},
        {32, UINT32_C(0xbf800000), UINT32_C(0x40400000),
                TEST_FPCR_RP, UINT32_C(0xbeaaaaaa), TEST_FPSR_IXC},
        {32, UINT32_C(0xbf800000), UINT32_C(0x40400000),
                TEST_FPCR_RM, UINT32_C(0xbeaaaaab), TEST_FPSR_IXC},
        {32, UINT32_C(0xbf800000), UINT32_C(0x40400000),
                TEST_FPCR_RZ, UINT32_C(0xbeaaaaaa), TEST_FPSR_IXC},
        {64, UINT64_C(0x3ff0000000000000),
                UINT64_C(0x4008000000000000), 0,
                UINT64_C(0x3fd5555555555555), TEST_FPSR_IXC},
        {64, UINT64_C(0x3ff0000000000000),
                UINT64_C(0x4008000000000000), TEST_FPCR_RP,
                UINT64_C(0x3fd5555555555556), TEST_FPSR_IXC},
        {64, UINT64_C(0x3ff0000000000000),
                UINT64_C(0x4008000000000000), TEST_FPCR_RM,
                UINT64_C(0x3fd5555555555555), TEST_FPSR_IXC},
        {64, UINT64_C(0x3ff0000000000000),
                UINT64_C(0x4008000000000000), TEST_FPCR_RZ,
                UINT64_C(0x3fd5555555555555), TEST_FPSR_IXC},
        {64, UINT64_C(0xbff0000000000000),
                UINT64_C(0x4008000000000000), 0,
                UINT64_C(0xbfd5555555555555), TEST_FPSR_IXC},
        {64, UINT64_C(0xbff0000000000000),
                UINT64_C(0x4008000000000000), TEST_FPCR_RP,
                UINT64_C(0xbfd5555555555555), TEST_FPSR_IXC},
        {64, UINT64_C(0xbff0000000000000),
                UINT64_C(0x4008000000000000), TEST_FPCR_RM,
                UINT64_C(0xbfd5555555555556), TEST_FPSR_IXC},
        {64, UINT64_C(0xbff0000000000000),
                UINT64_C(0x4008000000000000), TEST_FPCR_RZ,
                UINT64_C(0xbfd5555555555555), TEST_FPSR_IXC},
    };
    for (size_t i = 0; i < ARRAY_SIZE(cases); i++)
        run_case(&cases[i]);
}

static void test_special_values(void) {
    static const struct divide_case cases[] = {
        {32, UINT32_C(0x7fc12345), UINT32_C(0x3f800000), 0,
                UINT32_C(0x7fc12345), 0},
        {32, UINT32_C(0x7f812345), UINT32_C(0x3f800000), 0,
                UINT32_C(0x7fc12345), TEST_FPSR_IOC},
        {32, UINT32_C(0x7fc12345), UINT32_C(0x3f800000),
                TEST_FPCR_DN, UINT32_C(0x7fc00000), 0},
        {64, UINT64_C(0x7ff0123456789abc),
                UINT64_C(0x3ff0000000000000), 0,
                UINT64_C(0x7ff8123456789abc), TEST_FPSR_IOC},
        {32, 0, 0, 0, UINT32_C(0x7fc00000), TEST_FPSR_IOC},
        {64, UINT64_C(0x7ff0000000000000),
                UINT64_C(0x7ff0000000000000), 0,
                UINT64_C(0x7ff8000000000000), TEST_FPSR_IOC},
        {32, UINT32_C(0x3f800000), UINT32_C(0x80000000), 0,
                UINT32_C(0xff800000), TEST_FPSR_DZC},
        {32, UINT32_C(0xff800000), UINT32_C(0x80000000), 0,
                UINT32_C(0x7f800000), 0},
        {32, UINT32_C(0x80000000), UINT32_C(0x3f800000), 0,
                UINT32_C(0x80000000), 0},
        {32, UINT32_C(0x3f800000), UINT32_C(0xff800000), 0,
                UINT32_C(0x80000000), 0},
    };
    for (size_t i = 0; i < ARRAY_SIZE(cases); i++)
        run_case(&cases[i]);

    struct cpu_state sticky = initial_cpu(0);
    sticky.fpsr |= TEST_FPSR_IOC;
    set_scalar(&sticky, 1, 32, UINT32_C(0x3f800000));
    set_scalar(&sticky, 2, 32, 0);
    execute_word(&sticky, encode_divide(32, 3, 1, 2), 32);
    assert(sticky.fpsr ==
            (TEST_FPSR_QC | TEST_FPSR_IOC | TEST_FPSR_DZC));
}

static void test_subnormal_and_fz(void) {
    static const struct divide_case cases[] = {
        {32, UINT32_C(0x00000001), UINT32_C(0x00000001), 0,
                UINT32_C(0x3f800000), 0},
        {32, UINT32_C(0x00800000), UINT32_C(0x00000001), 0,
                UINT32_C(0x4b000000), 0},
        {32, UINT32_C(0x00000001), UINT32_C(0x00800000), 0,
                UINT32_C(0x34000000), 0},
        {32, UINT32_C(0x00000003), UINT32_C(0x00000002), 0,
                UINT32_C(0x3fc00000), 0},
        {64, UINT64_C(0x0000000000000001),
                UINT64_C(0x0000000000000001), 0,
                UINT64_C(0x3ff0000000000000), 0},
        {64, UINT64_C(0x0010000000000000),
                UINT64_C(0x0000000000000001), 0,
                UINT64_C(0x4330000000000000), 0},
        {64, UINT64_C(0x0000000000000001),
                UINT64_C(0x0010000000000000), 0,
                UINT64_C(0x3cb0000000000000), 0},
        {32, UINT32_C(0x00000001), UINT32_C(0x3f800000),
                TEST_FPCR_FZ, 0, TEST_FPSR_IDC},
        {32, UINT32_C(0x3f800000), UINT32_C(0x00000001),
                TEST_FPCR_FZ, UINT32_C(0x7f800000),
                TEST_FPSR_IDC | TEST_FPSR_DZC},
        {32, UINT32_C(0x00000001), 0, TEST_FPCR_FZ,
                UINT32_C(0x7fc00000),
                TEST_FPSR_IDC | TEST_FPSR_IOC},
        {32, UINT32_C(0x00800000), UINT32_C(0x40000000), 0,
                UINT32_C(0x00400000), 0},
        {32, UINT32_C(0x00800000), UINT32_C(0x40400000), 0,
                UINT32_C(0x002aaaab),
                TEST_FPSR_UFC | TEST_FPSR_IXC},
        {32, UINT32_C(0x00800000), UINT32_C(0x40000000),
                TEST_FPCR_FZ, 0, TEST_FPSR_UFC},
        {64, UINT64_C(0x0010000000000000),
                UINT64_C(0x4000000000000000), 0,
                UINT64_C(0x0008000000000000), 0},
        {64, UINT64_C(0x0010000000000000),
                UINT64_C(0x4008000000000000), 0,
                UINT64_C(0x0005555555555555),
                TEST_FPSR_UFC | TEST_FPSR_IXC},
        {64, UINT64_C(0x0010000000000000),
                UINT64_C(0x4000000000000000), TEST_FPCR_FZ,
                0, TEST_FPSR_UFC},
    };
    for (size_t i = 0; i < ARRAY_SIZE(cases); i++)
        run_case(&cases[i]);
}

static void test_overflow(void) {
    static const struct divide_case cases[] = {
        {32, UINT32_C(0x7f7fffff), UINT32_C(0x3f000000), 0,
                UINT32_C(0x7f800000),
                TEST_FPSR_OFC | TEST_FPSR_IXC},
        {32, UINT32_C(0x7f7fffff), UINT32_C(0x3f000000),
                TEST_FPCR_RZ, UINT32_C(0x7f7fffff),
                TEST_FPSR_OFC | TEST_FPSR_IXC},
        {64, UINT64_C(0x7fefffffffffffff),
                UINT64_C(0x3fe0000000000000), 0,
                UINT64_C(0x7ff0000000000000),
                TEST_FPSR_OFC | TEST_FPSR_IXC},
        {64, UINT64_C(0x7fefffffffffffff),
                UINT64_C(0x3fe0000000000000), TEST_FPCR_RZ,
                UINT64_C(0x7fefffffffffffff),
                TEST_FPSR_OFC | TEST_FPSR_IXC},
    };
    for (size_t i = 0; i < ARRAY_SIZE(cases); i++)
        run_case(&cases[i]);
}

int main(void) {
    test_product_word_and_aliases();
    test_rounding_modes();
    test_special_values();
    test_subnormal_and_fz();
    test_overflow();
    return 0;
}
