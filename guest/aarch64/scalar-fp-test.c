#include <assert.h>
#include <string.h>

#include "guest/aarch64/decode.h"
#include "guest/aarch64/execute.h"

#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

#define TEST_FPCR_RP UINT32_C(0x00400000)
#define TEST_FPCR_RM UINT32_C(0x00800000)
#define TEST_FPCR_RZ UINT32_C(0x00c00000)
#define TEST_FPCR_FZ UINT32_C(0x01000000)
#define TEST_FPCR_DN UINT32_C(0x02000000)

#define TEST_FPSR_IOC UINT32_C(0x00000001)
#define TEST_FPSR_OFC UINT32_C(0x00000004)
#define TEST_FPSR_UFC UINT32_C(0x00000008)
#define TEST_FPSR_IXC UINT32_C(0x00000010)
#define TEST_FPSR_IDC UINT32_C(0x00000080)
#define TEST_FPSR_QC UINT32_C(0x08000000)

#define TEST_NZCV_LESS UINT32_C(0x80000000)
#define TEST_NZCV_EQUAL UINT32_C(0x60000000)
#define TEST_NZCV_GREATER UINT32_C(0x20000000)
#define TEST_NZCV_UNORDERED UINT32_C(0x30000000)

#define FADD_S UINT32_C(0x1e202800)
#define FADD_D UINT32_C(0x1e602800)
#define FSUB_S UINT32_C(0x1e203800)
#define FSUB_D UINT32_C(0x1e603800)
#define FMUL_S UINT32_C(0x1e200800)
#define FMUL_D UINT32_C(0x1e600800)
#define FMOV_S UINT32_C(0x1e204000)
#define FMOV_D UINT32_C(0x1e604000)
#define FMOV_IMMEDIATE_S UINT32_C(0x1e201000)
#define FMOV_IMMEDIATE_D UINT32_C(0x1e601000)
#define FCMP_S UINT32_C(0x1e202000)
#define FCMP_D UINT32_C(0x1e602000)
#define FCMPE_S UINT32_C(0x1e202010)
#define FCMPE_D UINT32_C(0x1e602010)
#define FCMP_ZERO_S UINT32_C(0x1e202008)
#define FCMP_ZERO_D UINT32_C(0x1e602008)
#define FCMPE_ZERO_S UINT32_C(0x1e202018)
#define FCMPE_ZERO_D UINT32_C(0x1e602018)
#define FCVTZS_S UINT32_C(0x5ea1b800)
#define FCVTZS_D UINT32_C(0x5ee1b800)
#define SCVTF_S UINT32_C(0x5e21d800)
#define SCVTF_D UINT32_C(0x5e61d800)

static dword_t binary(dword_t base, byte_t rd, byte_t rn, byte_t rm) {
    return base | (dword_t) rm << 16 | (dword_t) rn << 5 | rd;
}

static dword_t unary(dword_t base, byte_t rd, byte_t rn) {
    return base | (dword_t) rn << 5 | rd;
}

static dword_t immediate(dword_t base, byte_t rd, byte_t imm8) {
    return base | (dword_t) imm8 << 13 | rd;
}

static qword_t expected_immediate(byte_t width, byte_t imm8) {
    if (width == 32) {
        return (qword_t) (imm8 & UINT32_C(0x80)) << 24 |
                ((imm8 & UINT32_C(0x40)) != 0 ?
                        UINT32_C(0x3e000000) : UINT32_C(0x40000000)) |
                (qword_t) (imm8 & UINT32_C(0x3f)) << 19;
    }
    return (qword_t) (imm8 & UINT32_C(0x80)) << 56 |
            ((imm8 & UINT32_C(0x40)) != 0 ?
                    UINT64_C(0x3fc0000000000000) :
                    UINT64_C(0x4000000000000000)) |
            (qword_t) (imm8 & UINT32_C(0x3f)) << 48;
}

static dword_t comparison(dword_t base, byte_t rn, byte_t rm) {
    return base | (dword_t) rm << 16 | (dword_t) rn << 5;
}

static struct aarch64_decoded decode(dword_t word) {
    struct aarch64_decoded instruction = {0};
    bool decoded = aarch64_decode(word, &instruction);
    assert(decoded);
    use(decoded);
    return instruction;
}

static void execute_word(struct cpu_state *cpu, dword_t word) {
    struct aarch64_decoded instruction = decode(word);
    struct aarch64_execute_result result =
            aarch64_execute(cpu, NULL, &instruction);
    assert(result.stop == AARCH64_EXECUTE_RETIRED);
}

static struct cpu_state initial_cpu(void) {
    struct cpu_state cpu = {
        .pc = UINT64_C(0x1000),
        .sp = UINT64_C(0x1122334455667788),
        .nzcv = UINT32_C(0xa0000000),
        .fpcr = UINT32_C(0x00040000),
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

static qword_t scalar(const struct cpu_state *cpu,
        byte_t reg, byte_t width) {
    return width == 32 ? cpu->v[reg].s[0] : cpu->v[reg].d[0];
}

static void assert_scalar_result(const struct cpu_state *cpu,
        byte_t reg, byte_t width, qword_t expected) {
    assert(scalar(cpu, reg, width) == expected);
    assert(cpu->v[reg].d[1] == 0);
    if (width == 32)
        assert((cpu->v[reg].d[0] >> 32) == 0);
}

static void assert_non_fp_state(const struct cpu_state *cpu,
        qword_t expected_pc, dword_t expected_nzcv) {
    assert(cpu->pc == expected_pc);
    assert(cpu->sp == UINT64_C(0x1122334455667788));
    assert(cpu->nzcv == expected_nzcv);
    assert(cpu->x[3] == UINT64_C(0x0123456789abcdef));
    assert(cpu->x[19] == UINT64_C(0xfedcba9876543210));
    assert(cpu->tpidr_el0 == UINT64_C(0x8877665544332211));
}

static void test_busybox_sleep_path(void) {
    struct cpu_state cpu = initial_cpu();
    cpu.v[0].d[0] = UINT64_C(0x3ffc000000000000);
    cpu.v[15].d[0] = 0;
    cpu.v[15].d[1] = UINT64_MAX;

    // 这些指令字来自 sleep 的小数拆分路径，寄存器别名也保持原样。
    execute_word(&cpu, UINT32_C(0x1e6029ef)); // fadd d15, d15, d0
    assert_scalar_result(&cpu, 15, 64, UINT64_C(0x3ffc000000000000));
    execute_word(&cpu, UINT32_C(0x1e6041e0)); // fmov d0, d15
    assert_scalar_result(&cpu, 0, 64, UINT64_C(0x3ffc000000000000));

    execute_word(&cpu, UINT32_C(0x1e602018)); // fcmpe d0, #0.0
    assert(cpu.nzcv == TEST_NZCV_GREATER);

    cpu.v[31].d[0] = UINT64_C(0x43e0000000000000);
    execute_word(&cpu, UINT32_C(0x1e7f2010)); // fcmpe d0, d31
    assert(cpu.nzcv == TEST_NZCV_LESS);

    execute_word(&cpu, UINT32_C(0x5ee1b81f)); // fcvtzs d31, d0
    assert_scalar_result(&cpu, 31, 64, UINT64_C(1));
    execute_word(&cpu, UINT32_C(0x5e61dbff)); // scvtf d31, d31
    assert_scalar_result(&cpu, 31, 64, UINT64_C(0x3ff0000000000000));
    execute_word(&cpu, UINT32_C(0x1e7f3800)); // fsub d0, d0, d31
    assert_scalar_result(&cpu, 0, 64, UINT64_C(0x3fe8000000000000));

    cpu.v[31].d[0] = UINT64_C(0x41cdcd6500000000);
    execute_word(&cpu, UINT32_C(0x1e7f0800)); // fmul d0, d0, d31
    assert_scalar_result(&cpu, 0, 64, UINT64_C(0x41c65a0bc0000000));
    execute_word(&cpu, UINT32_C(0x5ee1b800)); // fcvtzs d0, d0
    assert_scalar_result(&cpu, 0, 64, UINT64_C(750000000));

    assert(cpu.fpsr == (TEST_FPSR_QC | TEST_FPSR_IXC));
    assert(cpu.fpcr == UINT32_C(0x00040000));
    assert_non_fp_state(&cpu, UINT64_C(0x1024), TEST_NZCV_LESS);
}

struct arithmetic_case {
    dword_t base;
    byte_t width;
    qword_t left;
    qword_t right;
    qword_t expected;
};

static void test_basic_arithmetic(void) {
    static const struct arithmetic_case cases[] = {
        {FADD_S, 32, UINT32_C(0x3fc00000), UINT32_C(0x40100000),
                UINT32_C(0x40700000)},
        {FSUB_S, 32, UINT32_C(0x3fc00000), UINT32_C(0x40100000),
                UINT32_C(0xbf400000)},
        {FMUL_S, 32, UINT32_C(0x3fc00000), UINT32_C(0x40100000),
                UINT32_C(0x40580000)},
        {FADD_D, 64, UINT64_C(0x3ff8000000000000),
                UINT64_C(0x4002000000000000),
                UINT64_C(0x400e000000000000)},
        {FSUB_D, 64, UINT64_C(0x3ff8000000000000),
                UINT64_C(0x4002000000000000),
                UINT64_C(0xbfe8000000000000)},
        {FMUL_D, 64, UINT64_C(0x3ff8000000000000),
                UINT64_C(0x4002000000000000),
                UINT64_C(0x400b000000000000)},
    };

    for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
        const struct arithmetic_case *test = &cases[i];
        struct cpu_state cpu = initial_cpu();
        set_scalar(&cpu, 5, test->width, test->left);
        set_scalar(&cpu, 6, test->width, test->right);
        union aarch64_vector_reg right = cpu.v[6];
        execute_word(&cpu, binary(test->base, 5, 5, 6));
        assert_scalar_result(&cpu, 5, test->width, test->expected);
        assert(memcmp(&cpu.v[6], &right, sizeof(right)) == 0);
        assert(cpu.fpsr == TEST_FPSR_QC);
        assert(cpu.fpcr == UINT32_C(0x00040000));
        assert_non_fp_state(&cpu, UINT64_C(0x1004), UINT32_C(0xa0000000));
    }

    struct cpu_state cpu = initial_cpu();
    set_scalar(&cpu, 5, 64, UINT64_C(0x3ff8000000000000));
    set_scalar(&cpu, 6, 64, UINT64_C(0x4002000000000000));
    execute_word(&cpu, binary(FSUB_D, 6, 5, 6));
    assert_scalar_result(&cpu, 6, 64, UINT64_C(0xbfe8000000000000));
}

static void test_scalar_fmov(void) {
    static const struct {
        dword_t base;
        byte_t width;
        qword_t bits;
    } cases[] = {
        {FMOV_S, 32, UINT32_C(0x7fc12345)},
        {FMOV_D, 64, UINT64_C(0x7ff8123456789abc)},
    };

    for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
        struct cpu_state cpu = initial_cpu();
        cpu.fpcr = TEST_FPCR_DN | TEST_FPCR_FZ | TEST_FPCR_RM;
        cpu.fpsr = TEST_FPSR_QC | TEST_FPSR_IOC | TEST_FPSR_IDC;
        set_scalar(&cpu, 7, cases[i].width, cases[i].bits);
        union aarch64_vector_reg source = cpu.v[7];
        execute_word(&cpu, unary(cases[i].base, 8, 7));
        assert_scalar_result(&cpu, 8, cases[i].width, cases[i].bits);
        assert(memcmp(&cpu.v[7], &source, sizeof(source)) == 0);
        assert(cpu.fpsr ==
                (TEST_FPSR_QC | TEST_FPSR_IOC | TEST_FPSR_IDC));

        execute_word(&cpu, unary(cases[i].base, 7, 7));
        assert_scalar_result(&cpu, 7, cases[i].width, cases[i].bits);
        assert(cpu.pc == UINT64_C(0x1008));
    }
}

static void test_fmov_immediate(void) {
    static const struct {
        dword_t base;
        byte_t width;
    } cases[] = {
        {FMOV_IMMEDIATE_S, 32},
        {FMOV_IMMEDIATE_D, 64},
    };

    assert(immediate(FMOV_IMMEDIATE_S, 3, UINT8_C(0x70)) ==
            UINT32_C(0x1e2e1003));
    assert(immediate(FMOV_IMMEDIATE_D, 0, UINT8_C(0x70)) ==
            UINT32_C(0x1e6e1000));
    for (unsigned kind = 0; kind < ARRAY_SIZE(cases); kind++) {
        for (unsigned value = 0; value < 256; value++) {
            byte_t rd = (byte_t) (value & 31);
            byte_t untouched = (byte_t) ((rd + 1) & 31);
            struct cpu_state cpu = initial_cpu();
            cpu.fpcr = TEST_FPCR_DN | TEST_FPCR_FZ | TEST_FPCR_RM;
            cpu.fpsr = TEST_FPSR_QC | TEST_FPSR_IOC |
                    TEST_FPSR_OFC | TEST_FPSR_UFC |
                    TEST_FPSR_IXC | TEST_FPSR_IDC;
            memset(&cpu.v[rd], 0xa5, sizeof(cpu.v[rd]));
            memset(&cpu.v[untouched], 0x5a, sizeof(cpu.v[untouched]));
            union aarch64_vector_reg saved = cpu.v[untouched];

            execute_word(&cpu, immediate(cases[kind].base,
                    rd, (byte_t) value));
            assert_scalar_result(&cpu, rd, cases[kind].width,
                    expected_immediate(cases[kind].width,
                            (byte_t) value));
            assert(memcmp(&cpu.v[untouched], &saved, sizeof(saved)) == 0);
            assert(cpu.fpcr ==
                    (TEST_FPCR_DN | TEST_FPCR_FZ | TEST_FPCR_RM));
            assert(cpu.fpsr == (TEST_FPSR_QC | TEST_FPSR_IOC |
                    TEST_FPSR_OFC | TEST_FPSR_UFC |
                    TEST_FPSR_IXC | TEST_FPSR_IDC));
            assert_non_fp_state(
                    &cpu, UINT64_C(0x1004), UINT32_C(0xa0000000));
        }
    }
}

struct rounding_case {
    dword_t fpcr;
    dword_t positive_s;
    dword_t negative_s;
    qword_t positive_d;
    qword_t negative_d;
};

static void test_arithmetic_rounding_modes(void) {
    static const struct rounding_case cases[] = {
        {0, UINT32_C(0x3f800000), UINT32_C(0xbf800000),
                UINT64_C(0x3ff0000000000000),
                UINT64_C(0xbff0000000000000)},
        {TEST_FPCR_RP, UINT32_C(0x3f800001), UINT32_C(0xbf800000),
                UINT64_C(0x3ff0000000000001),
                UINT64_C(0xbff0000000000000)},
        {TEST_FPCR_RM, UINT32_C(0x3f800000), UINT32_C(0xbf800001),
                UINT64_C(0x3ff0000000000000),
                UINT64_C(0xbff0000000000001)},
        {TEST_FPCR_RZ, UINT32_C(0x3f800000), UINT32_C(0xbf800000),
                UINT64_C(0x3ff0000000000000),
                UINT64_C(0xbff0000000000000)},
    };

    for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
        struct cpu_state cpu = initial_cpu();
        cpu.fpcr = cases[i].fpcr;
        cpu.fpsr = TEST_FPSR_QC | TEST_FPSR_IOC;
        set_scalar(&cpu, 1, 32, UINT32_C(0x3f800000));
        set_scalar(&cpu, 2, 32, UINT32_C(0x33800000));
        execute_word(&cpu, binary(FADD_S, 3, 1, 2));
        assert_scalar_result(&cpu, 3, 32, cases[i].positive_s);
        assert(cpu.fpsr ==
                (TEST_FPSR_QC | TEST_FPSR_IOC | TEST_FPSR_IXC));

        set_scalar(&cpu, 1, 32, UINT32_C(0xbf800000));
        set_scalar(&cpu, 2, 32, UINT32_C(0xb3800000));
        execute_word(&cpu, binary(FADD_S, 3, 1, 2));
        assert_scalar_result(&cpu, 3, 32, cases[i].negative_s);

        set_scalar(&cpu, 1, 64, UINT64_C(0x3ff0000000000000));
        set_scalar(&cpu, 2, 64, UINT64_C(0x3ca0000000000000));
        execute_word(&cpu, binary(FADD_D, 3, 1, 2));
        assert_scalar_result(&cpu, 3, 64, cases[i].positive_d);

        set_scalar(&cpu, 1, 64, UINT64_C(0xbff0000000000000));
        set_scalar(&cpu, 2, 64, UINT64_C(0xbca0000000000000));
        execute_word(&cpu, binary(FADD_D, 3, 1, 2));
        assert_scalar_result(&cpu, 3, 64, cases[i].negative_d);
        assert(cpu.fpsr ==
                (TEST_FPSR_QC | TEST_FPSR_IOC | TEST_FPSR_IXC));
    }

    struct cpu_state cancellation = initial_cpu();
    cancellation.fpcr = TEST_FPCR_RM;
    set_scalar(&cancellation, 1, 64, UINT64_C(0x3ff0000000000000));
    set_scalar(&cancellation, 2, 64, UINT64_C(0x3ff0000000000000));
    execute_word(&cancellation, binary(FSUB_D, 3, 1, 2));
    assert_scalar_result(&cancellation, 3, 64,
            UINT64_C(0x8000000000000000));
    assert(cancellation.fpsr == TEST_FPSR_QC);
}

static void test_nan_and_default_nan(void) {
    static const struct {
        dword_t base;
        byte_t width;
        qword_t one;
        qword_t quiet_nan;
        qword_t signaling_nan;
        qword_t default_nan;
    } cases[] = {
        {FADD_S, 32, UINT32_C(0x3f800000), UINT32_C(0x7fc12345),
                UINT32_C(0x7f812345), UINT32_C(0x7fc00000)},
        {FADD_D, 64, UINT64_C(0x3ff0000000000000),
                UINT64_C(0x7ff8123456789abc),
                UINT64_C(0x7ff0123456789abc),
                UINT64_C(0x7ff8000000000000)},
    };

    for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
        for (unsigned dn = 0; dn < 2; dn++) {
            struct cpu_state cpu = initial_cpu();
            cpu.fpcr = dn ? TEST_FPCR_DN : 0;
            set_scalar(&cpu, 1, cases[i].width, cases[i].quiet_nan);
            set_scalar(&cpu, 2, cases[i].width, cases[i].one);
            execute_word(&cpu, binary(cases[i].base, 3, 1, 2));
            assert_scalar_result(&cpu, 3, cases[i].width,
                    dn ? cases[i].default_nan : cases[i].quiet_nan);
            assert(cpu.fpsr == TEST_FPSR_QC);

            set_scalar(&cpu, 1, cases[i].width, cases[i].signaling_nan);
            execute_word(&cpu, binary(cases[i].base, 3, 1, 2));
            assert_scalar_result(&cpu, 3, cases[i].width,
                    dn ? cases[i].default_nan : cases[i].quiet_nan);
            assert(cpu.fpsr == (TEST_FPSR_QC | TEST_FPSR_IOC));
        }
    }

    struct cpu_state cpu = initial_cpu();
    set_scalar(&cpu, 1, 32, 0);
    set_scalar(&cpu, 2, 32, UINT32_C(0x7f800000));
    execute_word(&cpu, binary(FMUL_S, 3, 1, 2));
    assert_scalar_result(&cpu, 3, 32, UINT32_C(0x7fc00000));
    assert(cpu.fpsr == (TEST_FPSR_QC | TEST_FPSR_IOC));

    cpu.fpsr = TEST_FPSR_QC;
    set_scalar(&cpu, 1, 64, UINT64_C(0x7ff0000000000000));
    set_scalar(&cpu, 2, 64, UINT64_C(0x7ff0000000000000));
    execute_word(&cpu, binary(FSUB_D, 3, 1, 2));
    assert_scalar_result(&cpu, 3, 64, UINT64_C(0x7ff8000000000000));
    assert(cpu.fpsr == (TEST_FPSR_QC | TEST_FPSR_IOC));
}

static void test_flush_to_zero_and_sticky_flags(void) {
    struct cpu_state cpu = initial_cpu();

    set_scalar(&cpu, 1, 32, UINT32_C(0x7f800000));
    set_scalar(&cpu, 2, 32, UINT32_C(0xff800000));
    execute_word(&cpu, binary(FADD_S, 3, 1, 2));
    assert(cpu.fpsr == (TEST_FPSR_QC | TEST_FPSR_IOC));

    set_scalar(&cpu, 1, 32, UINT32_C(0x7f7fffff));
    set_scalar(&cpu, 2, 32, UINT32_C(0x7f7fffff));
    execute_word(&cpu, binary(FADD_S, 3, 1, 2));
    assert_scalar_result(&cpu, 3, 32, UINT32_C(0x7f800000));
    assert(cpu.fpsr == (TEST_FPSR_QC | TEST_FPSR_IOC |
            TEST_FPSR_OFC | TEST_FPSR_IXC));

    set_scalar(&cpu, 1, 32, UINT32_C(0x00000001));
    set_scalar(&cpu, 2, 32, UINT32_C(0x3f000000));
    execute_word(&cpu, binary(FMUL_S, 3, 1, 2));
    assert_scalar_result(&cpu, 3, 32, 0);
    assert(cpu.fpsr == (TEST_FPSR_QC | TEST_FPSR_IOC |
            TEST_FPSR_OFC | TEST_FPSR_UFC | TEST_FPSR_IXC));

    cpu.fpcr = TEST_FPCR_FZ;
    set_scalar(&cpu, 1, 32, UINT32_C(0x00000001));
    set_scalar(&cpu, 2, 32, UINT32_C(0x3f800000));
    execute_word(&cpu, binary(FADD_S, 3, 1, 2));
    assert_scalar_result(&cpu, 3, 32, UINT32_C(0x3f800000));
    assert(cpu.fpsr == (TEST_FPSR_QC | TEST_FPSR_IOC |
            TEST_FPSR_OFC | TEST_FPSR_UFC | TEST_FPSR_IXC |
            TEST_FPSR_IDC));

    dword_t all_flags = cpu.fpsr;
    execute_word(&cpu, unary(FMOV_S, 3, 3));
    assert(cpu.fpsr == all_flags);

    struct cpu_state gradual = initial_cpu();
    set_scalar(&gradual, 1, 64, UINT64_C(0x0010000000000000));
    set_scalar(&gradual, 2, 64, UINT64_C(0x3fe0000000000000));
    execute_word(&gradual, binary(FMUL_D, 3, 1, 2));
    assert_scalar_result(&gradual, 3, 64,
            UINT64_C(0x0008000000000000));
    assert(gradual.fpsr == TEST_FPSR_QC);

    struct cpu_state inexact_gradual = initial_cpu();
    set_scalar(&inexact_gradual, 1, 64,
            UINT64_C(0x0010000000000000));
    set_scalar(&inexact_gradual, 2, 64,
            UINT64_C(0x3fe0000000000001));
    execute_word(&inexact_gradual, binary(FMUL_D, 3, 1, 2));
    assert_scalar_result(&inexact_gradual, 3, 64,
            UINT64_C(0x0008000000000000));
    assert(inexact_gradual.fpsr ==
            (TEST_FPSR_QC | TEST_FPSR_UFC | TEST_FPSR_IXC));

    struct cpu_state rounded_normal = initial_cpu();
    set_scalar(&rounded_normal, 1, 64,
            UINT64_C(0x0010000000000000));
    set_scalar(&rounded_normal, 2, 64,
            UINT64_C(0x3fefffffffffffff));
    execute_word(&rounded_normal, binary(FMUL_D, 3, 1, 2));
    assert_scalar_result(&rounded_normal, 3, 64,
            UINT64_C(0x0010000000000000));
    assert(rounded_normal.fpsr ==
            (TEST_FPSR_QC | TEST_FPSR_UFC | TEST_FPSR_IXC));

    struct cpu_state rounded_normal_s = initial_cpu();
    rounded_normal_s.fpcr = 0;
    set_scalar(&rounded_normal_s, 1, 32, UINT32_C(0x007fffff));
    set_scalar(&rounded_normal_s, 2, 32, UINT32_C(0x3f800001));
    execute_word(&rounded_normal_s, binary(FMUL_S, 3, 1, 2));
    assert_scalar_result(&rounded_normal_s, 3, 32,
            UINT32_C(0x00800000));
    assert(rounded_normal_s.fpsr ==
            (TEST_FPSR_QC | TEST_FPSR_UFC | TEST_FPSR_IXC));

    struct cpu_state output_flush = initial_cpu();
    output_flush.fpcr = TEST_FPCR_FZ;
    set_scalar(&output_flush, 1, 64, UINT64_C(0x0010000000000000));
    set_scalar(&output_flush, 2, 64, UINT64_C(0x3fe0000000000000));
    execute_word(&output_flush, binary(FMUL_D, 3, 1, 2));
    assert_scalar_result(&output_flush, 3, 64, 0);
    assert(output_flush.fpsr == (TEST_FPSR_QC | TEST_FPSR_UFC));

    struct cpu_state inexact_output_flush = initial_cpu();
    inexact_output_flush.fpcr = TEST_FPCR_FZ;
    set_scalar(&inexact_output_flush, 1, 64,
            UINT64_C(0x0010000000000000));
    // 渐进舍入会进位到最小正规数，FZ 仍须按舍入前的微小结果清零。
    set_scalar(&inexact_output_flush, 2, 64,
            UINT64_C(0x3fefffffffffffff));
    execute_word(&inexact_output_flush, binary(FMUL_D, 3, 1, 2));
    assert_scalar_result(&inexact_output_flush, 3, 64, 0);
    assert(inexact_output_flush.fpsr ==
            (TEST_FPSR_QC | TEST_FPSR_UFC));
}

static void run_compare(byte_t width, dword_t base,
        qword_t left, qword_t right,
        dword_t initial_fpsr, dword_t expected_nzcv,
        dword_t expected_fpsr) {
    struct cpu_state cpu = initial_cpu();
    cpu.fpsr = initial_fpsr;
    set_scalar(&cpu, 4, width, left);
    set_scalar(&cpu, 5, width, right);
    union aarch64_vector_reg saved_left = cpu.v[4];
    union aarch64_vector_reg saved_right = cpu.v[5];
    execute_word(&cpu, comparison(base, 4, 5));
    assert(cpu.nzcv == expected_nzcv);
    assert(cpu.fpsr == expected_fpsr);
    assert(memcmp(&cpu.v[4], &saved_left, sizeof(saved_left)) == 0);
    assert(memcmp(&cpu.v[5], &saved_right, sizeof(saved_right)) == 0);
    assert(cpu.pc == UINT64_C(0x1004));
}

static void test_comparison_relations(void) {
    static const struct {
        byte_t width;
        dword_t base;
        qword_t minus_one;
        qword_t minus_zero;
        qword_t zero;
        qword_t one;
        qword_t two;
        qword_t quiet_nan;
    } cases[] = {
        {32, FCMP_S, UINT32_C(0xbf800000), UINT32_C(0x80000000), 0,
                UINT32_C(0x3f800000), UINT32_C(0x40000000),
                UINT32_C(0x7fc12345)},
        {64, FCMP_D, UINT64_C(0xbff0000000000000),
                UINT64_C(0x8000000000000000), 0,
                UINT64_C(0x3ff0000000000000),
                UINT64_C(0x4000000000000000),
                UINT64_C(0x7ff8123456789abc)},
    };

    for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
        run_compare(cases[i].width, cases[i].base,
                cases[i].minus_one, cases[i].one,
                TEST_FPSR_QC, TEST_NZCV_LESS, TEST_FPSR_QC);
        run_compare(cases[i].width, cases[i].base,
                cases[i].minus_zero, cases[i].zero,
                TEST_FPSR_QC, TEST_NZCV_EQUAL, TEST_FPSR_QC);
        run_compare(cases[i].width, cases[i].base,
                cases[i].two, cases[i].one,
                TEST_FPSR_QC, TEST_NZCV_GREATER, TEST_FPSR_QC);
        run_compare(cases[i].width, cases[i].base,
                cases[i].quiet_nan, cases[i].one,
                TEST_FPSR_QC, TEST_NZCV_UNORDERED, TEST_FPSR_QC);
    }
}

static void test_comparison_nan_signaling(void) {
    static const struct {
        byte_t width;
        dword_t fcmp;
        dword_t fcmpe;
        qword_t one;
        qword_t quiet_nan;
        qword_t signaling_nan;
    } cases[] = {
        {32, FCMP_S, FCMPE_S, UINT32_C(0x3f800000),
                UINT32_C(0x7fc12345), UINT32_C(0x7f812345)},
        {64, FCMP_D, FCMPE_D, UINT64_C(0x3ff0000000000000),
                UINT64_C(0x7ff8123456789abc),
                UINT64_C(0x7ff0123456789abc)},
    };

    for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
        run_compare(cases[i].width, cases[i].fcmp,
                cases[i].quiet_nan, cases[i].one,
                TEST_FPSR_QC, TEST_NZCV_UNORDERED, TEST_FPSR_QC);
        run_compare(cases[i].width, cases[i].fcmpe,
                cases[i].quiet_nan, cases[i].one,
                TEST_FPSR_QC, TEST_NZCV_UNORDERED,
                TEST_FPSR_QC | TEST_FPSR_IOC);
        run_compare(cases[i].width, cases[i].fcmp,
                cases[i].signaling_nan, cases[i].one,
                TEST_FPSR_QC, TEST_NZCV_UNORDERED,
                TEST_FPSR_QC | TEST_FPSR_IOC);
        run_compare(cases[i].width, cases[i].fcmpe,
                cases[i].signaling_nan, cases[i].one,
                TEST_FPSR_QC, TEST_NZCV_UNORDERED,
                TEST_FPSR_QC | TEST_FPSR_IOC);
    }
}

static void test_compare_with_zero_and_fz(void) {
    struct cpu_state cpu = initial_cpu();
    set_scalar(&cpu, 0, 64, UINT64_C(0xbff0000000000000));
    execute_word(&cpu, FCMPE_ZERO_D);
    assert(cpu.nzcv == TEST_NZCV_LESS);
    set_scalar(&cpu, 0, 64, 0);
    execute_word(&cpu, FCMPE_ZERO_D);
    assert(cpu.nzcv == TEST_NZCV_EQUAL);
    set_scalar(&cpu, 0, 64, UINT64_C(0x3ff0000000000000));
    execute_word(&cpu, FCMPE_ZERO_D);
    assert(cpu.nzcv == TEST_NZCV_GREATER);

    set_scalar(&cpu, 0, 32, UINT32_C(0x7fc12345));
    execute_word(&cpu, FCMP_ZERO_S);
    assert(cpu.nzcv == TEST_NZCV_UNORDERED);
    assert(cpu.fpsr == TEST_FPSR_QC);

    cpu.fpcr = TEST_FPCR_FZ;
    set_scalar(&cpu, 0, 32, UINT32_C(0x00000001));
    execute_word(&cpu, FCMP_ZERO_S);
    assert(cpu.nzcv == TEST_NZCV_EQUAL);
    assert(cpu.fpsr == (TEST_FPSR_QC | TEST_FPSR_IDC));

    cpu.fpsr = TEST_FPSR_QC;
    set_scalar(&cpu, 4, 32, UINT32_C(0x7fc12345));
    set_scalar(&cpu, 5, 32, UINT32_C(0x00000001));
    execute_word(&cpu, comparison(FCMP_S, 4, 5));
    assert(cpu.nzcv == TEST_NZCV_UNORDERED);
    assert(cpu.fpsr == (TEST_FPSR_QC | TEST_FPSR_IDC));

    cpu.fpsr = TEST_FPSR_QC;
    execute_word(&cpu, comparison(FCMPE_S, 4, 5));
    assert(cpu.nzcv == TEST_NZCV_UNORDERED);
    assert(cpu.fpsr ==
            (TEST_FPSR_QC | TEST_FPSR_IOC | TEST_FPSR_IDC));
}

struct conversion_case {
    qword_t input;
    qword_t expected;
    dword_t flags;
};

static void test_fcvtzs_boundaries(void) {
    static const struct conversion_case single_cases[] = {
        {UINT32_C(0x00000000), UINT32_C(0x00000000), 0},
        {UINT32_C(0x80000000), UINT32_C(0x00000000), 0},
        {UINT32_C(0x3ff33333), UINT32_C(0x00000001), TEST_FPSR_IXC},
        {UINT32_C(0xbff33333), UINT32_C(0xffffffff), TEST_FPSR_IXC},
        {UINT32_C(0x4effffff), UINT32_C(0x7fffff80), 0},
        {UINT32_C(0x4f000000), UINT32_C(0x7fffffff), TEST_FPSR_IOC},
        {UINT32_C(0xcf000000), UINT32_C(0x80000000), 0},
        {UINT32_C(0xcf000001), UINT32_C(0x80000000), TEST_FPSR_IOC},
        {UINT32_C(0x7f800000), UINT32_C(0x7fffffff), TEST_FPSR_IOC},
        {UINT32_C(0xff800000), UINT32_C(0x80000000), TEST_FPSR_IOC},
        {UINT32_C(0x7fc12345), 0, TEST_FPSR_IOC},
        {UINT32_C(0x7f812345), 0, TEST_FPSR_IOC},
        {UINT32_C(0x00000001), 0, TEST_FPSR_IXC},
    };
    static const struct conversion_case double_cases[] = {
        {UINT64_C(0x0000000000000000), 0, 0},
        {UINT64_C(0x8000000000000000), 0, 0},
        {UINT64_C(0x3ffe666666666666), UINT64_C(1), TEST_FPSR_IXC},
        {UINT64_C(0xbffe666666666666), UINT64_MAX, TEST_FPSR_IXC},
        {UINT64_C(0x43dfffffffffffff), UINT64_C(0x7ffffffffffffc00), 0},
        {UINT64_C(0x43e0000000000000), UINT64_C(0x7fffffffffffffff),
                TEST_FPSR_IOC},
        {UINT64_C(0xc3e0000000000000), UINT64_C(0x8000000000000000), 0},
        {UINT64_C(0xc3e0000000000001), UINT64_C(0x8000000000000000),
                TEST_FPSR_IOC},
        {UINT64_C(0x7ff0000000000000), UINT64_C(0x7fffffffffffffff),
                TEST_FPSR_IOC},
        {UINT64_C(0xfff0000000000000), UINT64_C(0x8000000000000000),
                TEST_FPSR_IOC},
        {UINT64_C(0x7ff8123456789abc), 0, TEST_FPSR_IOC},
        {UINT64_C(0x7ff0123456789abc), 0, TEST_FPSR_IOC},
        {UINT64_C(0x0000000000000001), 0, TEST_FPSR_IXC},
    };

    for (unsigned width_index = 0; width_index < 2; width_index++) {
        byte_t width = width_index == 0 ? 32 : 64;
        dword_t base = width == 32 ? FCVTZS_S : FCVTZS_D;
        const struct conversion_case *cases = width == 32 ?
                single_cases : double_cases;
        unsigned count = width == 32 ?
                ARRAY_SIZE(single_cases) : ARRAY_SIZE(double_cases);
        for (unsigned i = 0; i < count; i++) {
            struct cpu_state cpu = initial_cpu();
            cpu.fpcr = TEST_FPCR_RP;
            cpu.fpsr = TEST_FPSR_QC | TEST_FPSR_OFC;
            set_scalar(&cpu, 11, width, cases[i].input);
            execute_word(&cpu, unary(base, 12, 11));
            assert_scalar_result(&cpu, 12, width, cases[i].expected);
            assert(cpu.fpsr ==
                    (TEST_FPSR_QC | TEST_FPSR_OFC | cases[i].flags));
            assert(cpu.fpcr == TEST_FPCR_RP);
            assert_non_fp_state(&cpu, UINT64_C(0x1004),
                    UINT32_C(0xa0000000));
        }
    }

    for (unsigned mode = 0; mode < 4; mode++) {
        struct cpu_state cpu = initial_cpu();
        cpu.fpcr = (dword_t) mode << 22;
        set_scalar(&cpu, 1, 32, UINT32_C(0x3ff33333));
        execute_word(&cpu, unary(FCVTZS_S, 2, 1));
        assert_scalar_result(&cpu, 2, 32, UINT32_C(1));
        assert(cpu.fpsr == (TEST_FPSR_QC | TEST_FPSR_IXC));
    }
}

static void run_scvtf(byte_t width, qword_t input, dword_t fpcr,
        qword_t expected, dword_t expected_flags) {
    struct cpu_state cpu = initial_cpu();
    cpu.fpcr = fpcr;
    cpu.fpsr = TEST_FPSR_QC | TEST_FPSR_IOC;
    set_scalar(&cpu, 13, width, input);
    execute_word(&cpu, unary(width == 32 ? SCVTF_S : SCVTF_D, 14, 13));
    assert_scalar_result(&cpu, 14, width, expected);
    assert(cpu.fpsr ==
            (TEST_FPSR_QC | TEST_FPSR_IOC | expected_flags));
    assert(cpu.fpcr == fpcr);
    assert_non_fp_state(&cpu, UINT64_C(0x1004), UINT32_C(0xa0000000));
}

static void test_scvtf_extremes_and_rounding(void) {
    run_scvtf(32, 0, 0, 0, 0);
    run_scvtf(32, UINT32_C(42), 0, UINT32_C(0x42280000), 0);
    run_scvtf(32, UINT32_C(0xffffffd6), 0,
            UINT32_C(0xc2280000), 0);
    run_scvtf(32, UINT32_C(0x80000000), TEST_FPCR_DN | TEST_FPCR_FZ,
            UINT32_C(0xcf000000), 0);

    static const dword_t modes[] = {
        0, TEST_FPCR_RP, TEST_FPCR_RM, TEST_FPCR_RZ,
    };
    static const dword_t positive_s[] = {
        UINT32_C(0x4f000000), UINT32_C(0x4f000000),
        UINT32_C(0x4effffff), UINT32_C(0x4effffff),
    };
    static const dword_t negative_s[] = {
        UINT32_C(0xcf000000), UINT32_C(0xceffffff),
        UINT32_C(0xcf000000), UINT32_C(0xceffffff),
    };
    static const qword_t positive_d[] = {
        UINT64_C(0x43e0000000000000), UINT64_C(0x43e0000000000000),
        UINT64_C(0x43dfffffffffffff), UINT64_C(0x43dfffffffffffff),
    };
    static const qword_t negative_d[] = {
        UINT64_C(0xc3e0000000000000), UINT64_C(0xc3dfffffffffffff),
        UINT64_C(0xc3e0000000000000), UINT64_C(0xc3dfffffffffffff),
    };

    for (unsigned i = 0; i < ARRAY_SIZE(modes); i++) {
        run_scvtf(32, UINT32_C(0x7fffffff), modes[i],
                positive_s[i], TEST_FPSR_IXC);
        run_scvtf(32, UINT32_C(0x80000001), modes[i],
                negative_s[i], TEST_FPSR_IXC);
        run_scvtf(64, UINT64_C(0x7fffffffffffffff), modes[i],
                positive_d[i], TEST_FPSR_IXC);
        run_scvtf(64, UINT64_C(0x8000000000000001), modes[i],
                negative_d[i], TEST_FPSR_IXC);
    }

    run_scvtf(64, UINT64_C(42), 0,
            UINT64_C(0x4045000000000000), 0);
    run_scvtf(64, UINT64_C(0xffffffffffffffd6), 0,
            UINT64_C(0xc045000000000000), 0);
    run_scvtf(64, UINT64_C(0x8000000000000000),
            TEST_FPCR_DN | TEST_FPCR_FZ,
            UINT64_C(0xc3e0000000000000), 0);
}

int main(void) {
    test_busybox_sleep_path();
    test_basic_arithmetic();
    test_scalar_fmov();
    test_fmov_immediate();
    test_arithmetic_rounding_modes();
    test_nan_and_default_nan();
    test_flush_to_zero_and_sticky_flags();
    test_comparison_relations();
    test_comparison_nan_signaling();
    test_compare_with_zero_and_fz();
    test_fcvtzs_boundaries();
    test_scvtf_extremes_and_rounding();
    return 0;
}
