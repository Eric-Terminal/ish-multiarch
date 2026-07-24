#include <assert.h>
#include <string.h>

#include "guest/aarch64/decode.h"
#include "guest/aarch64/execute.h"

#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

#define FMADD_S UINT32_C(0x1f000000)
#define FMADD_D UINT32_C(0x1f400000)
#define FMSUB_S UINT32_C(0x1f008000)
#define FMSUB_D UINT32_C(0x1f408000)
#define FNMADD_S UINT32_C(0x1f200000)
#define FNMADD_D UINT32_C(0x1f600000)
#define FNMSUB_S UINT32_C(0x1f208000)
#define FNMSUB_D UINT32_C(0x1f608000)

#define FPCR_RP UINT32_C(0x00400000)
#define FPCR_RM UINT32_C(0x00800000)
#define FPCR_RZ UINT32_C(0x00c00000)

struct fused_operation {
    dword_t base;
    enum aarch64_opcode opcode;
    byte_t width;
};

static const struct fused_operation operations[] = {
    {FMADD_S, AARCH64_OP_FMADD_SCALAR, 32},
    {FMSUB_S, AARCH64_OP_FMSUB_SCALAR, 32},
    {FNMADD_S, AARCH64_OP_FNMADD_SCALAR, 32},
    {FNMSUB_S, AARCH64_OP_FNMSUB_SCALAR, 32},
    {FMADD_D, AARCH64_OP_FMADD_SCALAR, 64},
    {FMSUB_D, AARCH64_OP_FMSUB_SCALAR, 64},
    {FNMADD_D, AARCH64_OP_FNMADD_SCALAR, 64},
    {FNMSUB_D, AARCH64_OP_FNMSUB_SCALAR, 64},
};

static dword_t encode_fused(const struct fused_operation *operation,
        byte_t rd, byte_t rn, byte_t rm, byte_t ra) {
    return operation->base | (dword_t) rm << 16 |
            (dword_t) ra << 10 | (dword_t) rn << 5 | rd;
}

static struct cpu_state initial_cpu(void) {
    struct cpu_state cpu = {
        .cycle = UINT64_C(0x1020304050607080),
        .sp = UINT64_C(0x1122334455667788),
        .pc = UINT64_C(0x1000),
        .nzcv = UINT32_C(0xa0000000),
        .fpcr = 0,
        .fpsr = AARCH64_FPSR_QC,
        .tpidr_el0 = UINT64_C(0x8877665544332211),
        .exclusive = {
            .address = UINT64_C(0x12345000),
            .value_low = UINT64_C(0x0123456789abcdef),
            .value_high = UINT64_C(0xfedcba9876543210),
            .mapping_epoch = 17,
            .write_epoch = 29,
            .sync_identity = 31,
            .size = 16,
            .pair = true,
            .valid = true,
        },
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

static void set_scalar(struct cpu_state *cpu, byte_t reg,
        byte_t width, qword_t bits) {
    cpu->v[reg].d[0] = width == 32 ?
            UINT64_C(0xa5a5a5a500000000) | (dword_t) bits : bits;
    cpu->v[reg].d[1] = UINT64_C(0x5a5a5a5a5a5a5a5a);
}

static void set_expected_scalar(struct cpu_state *cpu, byte_t reg,
        byte_t width, qword_t bits) {
    cpu->v[reg] = (union aarch64_vector_reg) {0};
    if (width == 32)
        cpu->v[reg].s[0] = (dword_t) bits;
    else
        cpu->v[reg].d[0] = bits;
}

static void execute_and_assert(struct cpu_state *cpu,
        const struct fused_operation *operation, dword_t word,
        byte_t rd, byte_t rn, byte_t rm, byte_t ra,
        qword_t expected_bits, dword_t raised_exceptions) {
    struct aarch64_decoded instruction = {0};
    assert(aarch64_decode(word, &instruction));
    assert(instruction.opcode == operation->opcode);
    assert(instruction.width == operation->width);
    assert(instruction.operands.data_processing_3source.rd == rd);
    assert(instruction.operands.data_processing_3source.rn == rn);
    assert(instruction.operands.data_processing_3source.rm == rm);
    assert(instruction.operands.data_processing_3source.ra == ra);

    struct cpu_state expected = *cpu;
    expected.pc += 4;
    expected.fpsr |= raised_exceptions;
    set_expected_scalar(&expected, rd, operation->width, expected_bits);

    struct aarch64_execute_result result =
            aarch64_execute(cpu, NULL, &instruction);
    assert(result.stop == AARCH64_EXECUTE_RETIRED);
    assert(result.fault.kind == GUEST_MEMORY_FAULT_NONE);
    assert(memcmp(cpu, &expected, sizeof(*cpu)) == 0);
}

static void run_distinct(const struct fused_operation *operation,
        qword_t left, qword_t right, qword_t addend,
        dword_t fpcr, dword_t initial_fpsr,
        qword_t expected, dword_t raised_exceptions) {
    struct cpu_state cpu = initial_cpu();
    cpu.fpcr = fpcr;
    cpu.fpsr = initial_fpsr;
    set_scalar(&cpu, 1, operation->width, left);
    set_scalar(&cpu, 2, operation->width, right);
    set_scalar(&cpu, 3, operation->width, addend);
    execute_and_assert(&cpu, operation,
            encode_fused(operation, 4, 1, 2, 3),
            4, 1, 2, 3, expected, raised_exceptions);
}

static void test_four_operations_and_widths(void) {
    static const qword_t expected_single[] = {
        UINT32_C(0x41800000), UINT32_C(0x40800000),
        UINT32_C(0xc1800000), UINT32_C(0xc0800000),
    };
    static const qword_t expected_double[] = {
        UINT64_C(0x4030000000000000), UINT64_C(0x4010000000000000),
        UINT64_C(0xc030000000000000), UINT64_C(0xc010000000000000),
    };

    for (unsigned i = 0; i < ARRAY_SIZE(operations); i++) {
        const struct fused_operation *operation = &operations[i];
        bool single = operation->width == 32;
        run_distinct(operation,
                single ? UINT32_C(0x40000000) :
                        UINT64_C(0x4000000000000000),
                single ? UINT32_C(0x40400000) :
                        UINT64_C(0x4008000000000000),
                single ? UINT32_C(0x41200000) :
                        UINT64_C(0x4024000000000000),
                0, AARCH64_FPSR_QC,
                single ? expected_single[i] : expected_double[i - 4], 0);
    }
}

static void test_product_word_and_aliases(void) {
    struct cpu_state product = initial_cpu();
    set_scalar(&product, 0, 64, UINT64_C(0x4000000000000000));
    set_scalar(&product, 15, 64, UINT64_C(0x4008000000000000));
    set_scalar(&product, 31, 64, UINT64_C(0x4024000000000000));
    // 该机器码来自 GCC cc1，目标与加数同为 d31。
    execute_and_assert(&product, &operations[4], UINT32_C(0x1f4f7c1f),
            31, 0, 15, 31, UINT64_C(0x4030000000000000), 0);

    struct {
        byte_t rd;
        byte_t rn;
        byte_t rm;
        byte_t ra;
    } aliases[] = {
        {1, 1, 2, 3},
        {2, 1, 2, 3},
        {3, 1, 2, 3},
    };
    for (unsigned i = 0; i < ARRAY_SIZE(aliases); i++) {
        struct cpu_state cpu = initial_cpu();
        set_scalar(&cpu, 1, 64, UINT64_C(0x4000000000000000));
        set_scalar(&cpu, 2, 64, UINT64_C(0x4008000000000000));
        set_scalar(&cpu, 3, 64, UINT64_C(0x4024000000000000));
        execute_and_assert(&cpu, &operations[4],
                encode_fused(&operations[4], aliases[i].rd,
                        aliases[i].rn, aliases[i].rm, aliases[i].ra),
                aliases[i].rd, aliases[i].rn,
                aliases[i].rm, aliases[i].ra,
                UINT64_C(0x4030000000000000), 0);
    }

    struct cpu_state all_same = initial_cpu();
    set_scalar(&all_same, 5, 64, UINT64_C(0x4000000000000000));
    execute_and_assert(&all_same, &operations[4],
            encode_fused(&operations[4], 5, 5, 5, 5),
            5, 5, 5, 5, UINT64_C(0x4018000000000000), 0);
}

static void test_single_rounding(void) {
    // 乘法先舍入会得到零；融合运算保留了被消去后的精确尾数。
    run_distinct(&operations[0],
            UINT32_C(0x3f800001), UINT32_C(0x3f7ffffe),
            UINT32_C(0xbf800000), 0, AARCH64_FPSR_QC,
            UINT32_C(0xa8800000), 0);
    run_distinct(&operations[4],
            UINT64_C(0x3ff0000000000001),
            UINT64_C(0x3feffffffffffffe),
            UINT64_C(0xbff0000000000000), 0, AARCH64_FPSR_QC,
            UINT64_C(0xb970000000000000), 0);
}

static void test_rounding_modes(void) {
    static const struct {
        dword_t fpcr;
        qword_t positive;
        qword_t negative;
    } cases[] = {
        {0, UINT32_C(0x3f800000), UINT32_C(0xbf800000)},
        {FPCR_RP, UINT32_C(0x3f800001), UINT32_C(0xbf800000)},
        {FPCR_RM, UINT32_C(0x3f800000), UINT32_C(0xbf800001)},
        {FPCR_RZ, UINT32_C(0x3f800000), UINT32_C(0xbf800000)},
    };
    for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
        run_distinct(&operations[0],
                UINT32_C(0x3f800000), UINT32_C(0x3f800000),
                UINT32_C(0x33800000), cases[i].fpcr,
                AARCH64_FPSR_QC, cases[i].positive, AARCH64_FPSR_IXC);
        run_distinct(&operations[0],
                UINT32_C(0xbf800000), UINT32_C(0x3f800000),
                UINT32_C(0xb3800000), cases[i].fpcr,
                AARCH64_FPSR_QC, cases[i].negative, AARCH64_FPSR_IXC);
    }

    // 远小于 double 精度的减数仍须形成 sticky，并服从向负无穷舍入。
    run_distinct(&operations[4],
            UINT64_C(0x3ff0000000000000),
            UINT64_C(0x3ff0000000000000),
            UINT64_C(0x8000000000000001), FPCR_RM,
            AARCH64_FPSR_QC, UINT64_C(0x3fefffffffffffff),
            AARCH64_FPSR_IXC);
}

static void test_subnormal_flush_and_overflow(void) {
    run_distinct(&operations[0],
            UINT32_C(0x00800000), UINT32_C(0x34000000), 0,
            0, AARCH64_FPSR_QC, UINT32_C(0x00000001), 0);
    run_distinct(&operations[0],
            UINT32_C(0x00800000), UINT32_C(0x33800000), 0,
            0, AARCH64_FPSR_QC, 0,
            AARCH64_FPSR_UFC | AARCH64_FPSR_IXC);
    run_distinct(&operations[0],
            UINT32_C(0x00800000), UINT32_C(0x33800000), 0,
            FPCR_RP, AARCH64_FPSR_QC, UINT32_C(0x00000001),
            AARCH64_FPSR_UFC | AARCH64_FPSR_IXC);
    run_distinct(&operations[0],
            UINT32_C(0x80800000), UINT32_C(0x33800000), 0,
            FPCR_RM, AARCH64_FPSR_QC, UINT32_C(0x80000001),
            AARCH64_FPSR_UFC | AARCH64_FPSR_IXC);
    run_distinct(&operations[0],
            UINT32_C(0x00800000), UINT32_C(0x34000000), 0,
            AARCH64_FPCR_FZ, AARCH64_FPSR_QC, 0,
            AARCH64_FPSR_UFC);
    run_distinct(&operations[0],
            UINT32_C(0x80000001), UINT32_C(0x3f800000),
            UINT32_C(0x80000000), AARCH64_FPCR_FZ,
            AARCH64_FPSR_QC, UINT32_C(0x80000000),
            AARCH64_FPSR_IDC);

    run_distinct(&operations[0],
            UINT32_C(0x7f7fffff), UINT32_C(0x3f800000),
            UINT32_C(0x7f7fffff), 0, AARCH64_FPSR_QC,
            UINT32_C(0x7f800000),
            AARCH64_FPSR_OFC | AARCH64_FPSR_IXC);
    run_distinct(&operations[0],
            UINT32_C(0x7f7fffff), UINT32_C(0x3f800000),
            UINT32_C(0x7f7fffff), FPCR_RZ, AARCH64_FPSR_QC,
            UINT32_C(0x7f7fffff),
            AARCH64_FPSR_OFC | AARCH64_FPSR_IXC);

    run_distinct(&operations[4],
            UINT64_C(0x0010000000000000),
            UINT64_C(0x3cb0000000000000), 0,
            0, AARCH64_FPSR_QC, UINT64_C(0x0000000000000001), 0);
    run_distinct(&operations[4],
            UINT64_C(0x7fefffffffffffff),
            UINT64_C(0x3ff0000000000000),
            UINT64_C(0x7fefffffffffffff), 0, AARCH64_FPSR_QC,
            UINT64_C(0x7ff0000000000000),
            AARCH64_FPSR_OFC | AARCH64_FPSR_IXC);
}

static void test_nan_and_invalid_priority(void) {
    // signaling NaN 按加数、左乘数、右乘数优先，且先于 0 × Inf。
    run_distinct(&operations[0],
            UINT32_C(0x7f823456), UINT32_C(0x7f834567),
            UINT32_C(0x7f812345), 0, AARCH64_FPSR_QC,
            UINT32_C(0x7fc12345), AARCH64_FPSR_IOC);
    run_distinct(&operations[0],
            UINT32_C(0x7f823456), UINT32_C(0x7f834567),
            UINT32_C(0x7fc12345), 0, AARCH64_FPSR_QC,
            UINT32_C(0x7fc23456), AARCH64_FPSR_IOC);
    run_distinct(&operations[0],
            UINT32_C(0x7fc23456), UINT32_C(0x7f834567),
            UINT32_C(0x7fc12345), 0, AARCH64_FPSR_QC,
            UINT32_C(0x7fc34567), AARCH64_FPSR_IOC);
    run_distinct(&operations[0],
            0, UINT32_C(0x7f800000), UINT32_C(0x7f812345),
            0, AARCH64_FPSR_QC, UINT32_C(0x7fc12345),
            AARCH64_FPSR_IOC);

    // 0 × Inf 的无效操作先于 quiet NaN，普通 quiet NaN 则按 a、n、m 取值。
    run_distinct(&operations[0],
            0, UINT32_C(0x7f800000), UINT32_C(0x7fc12345),
            0, AARCH64_FPSR_QC, UINT32_C(0x7fc00000),
            AARCH64_FPSR_IOC);
    run_distinct(&operations[0],
            UINT32_C(0x7fc23456), UINT32_C(0x7fc34567),
            UINT32_C(0x7fc12345), 0, AARCH64_FPSR_QC,
            UINT32_C(0x7fc12345), 0);
    run_distinct(&operations[0],
            UINT32_C(0x7fc23456), UINT32_C(0x3f800000),
            UINT32_C(0x3f800000), AARCH64_FPCR_DN,
            AARCH64_FPSR_QC, UINT32_C(0x7fc00000), 0);

    // 操作定义中的符号翻转发生在 NaN 选择前。
    run_distinct(&operations[2],
            UINT32_C(0x3f800000), UINT32_C(0x3f800000),
            UINT32_C(0x7fc12345), 0, AARCH64_FPSR_QC,
            UINT32_C(0xffc12345), 0);
    run_distinct(&operations[1],
            UINT32_C(0x7f812345), UINT32_C(0x3f800000),
            UINT32_C(0x3f800000), 0, AARCH64_FPSR_QC,
            UINT32_C(0xffc12345), AARCH64_FPSR_IOC);

    run_distinct(&operations[0],
            UINT32_C(0x7f800000), UINT32_C(0x3f800000),
            UINT32_C(0xff800000), 0, AARCH64_FPSR_QC,
            UINT32_C(0x7fc00000), AARCH64_FPSR_IOC);
}

static void test_signed_zero_and_sticky_fpsr(void) {
    run_distinct(&operations[0],
            0, UINT32_C(0x3f800000), UINT32_C(0x80000000),
            0, AARCH64_FPSR_QC | AARCH64_FPSR_DZC, 0, 0);
    run_distinct(&operations[0],
            0, UINT32_C(0x3f800000), UINT32_C(0x80000000),
            FPCR_RM, AARCH64_FPSR_QC | AARCH64_FPSR_DZC,
            UINT32_C(0x80000000), 0);
    run_distinct(&operations[0],
            UINT32_C(0x80000000), UINT32_C(0x3f800000),
            UINT32_C(0x80000000), 0,
            AARCH64_FPSR_QC | AARCH64_FPSR_DZC,
            UINT32_C(0x80000000), 0);

    run_distinct(&operations[0],
            0, UINT32_C(0x7f800000), UINT32_C(0x3f800000),
            AARCH64_FPCR_DN | FPCR_RZ,
            AARCH64_FPSR_QC | AARCH64_FPSR_DZC,
            UINT32_C(0x7fc00000), AARCH64_FPSR_IOC);
}

int main(void) {
    test_four_operations_and_widths();
    test_product_word_and_aliases();
    test_single_rounding();
    test_rounding_modes();
    test_subnormal_flush_and_overflow();
    test_nan_and_invalid_priority();
    test_signed_zero_and_sticky_fpsr();
    return 0;
}
