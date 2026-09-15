#include "guest/aarch64/fp-common-test.h"

static const struct {
    dword_t bits;
    enum aarch64_opcode opcode;
} operations[] = {
    {0x1e20c000, AARCH64_OP_FABS_SCALAR},
    {0x1e205800, AARCH64_OP_FMIN_SCALAR},
    {0x1e204800, AARCH64_OP_FMAX_SCALAR},
    {0x1e207800, AARCH64_OP_FMINNM_SCALAR},
    {0x1e206800, AARCH64_OP_FMAXNM_SCALAR},
};

static void test_decoding(void) {
    for (unsigned operation = 0; operation < 5; operation++) {
        for (unsigned wide = 0; wide < 2; wide++) {
            for (unsigned rn = 0; rn < 32; rn++) {
                for (unsigned rm = 0; rm < (operation ? 32 : 1); rm++) {
                    for (unsigned rd = 0; rd < 32; rd++) {
                        struct aarch64_decoded decoded;
                        assert(aarch64_decode(operations[operation].bits |
                                wide << 22 | rm << 16 | rn << 5 | rd, &decoded));
                        assert(decoded.opcode == operations[operation].opcode);
                        assert(decoded.width == (wide ? 64 : 32));
                        if (operation) {
                            assert(decoded.operands.data_processing_2source.rn == rn);
                            assert(decoded.operands.data_processing_2source.rm == rm);
                            assert(decoded.operands.data_processing_2source.rd == rd);
                        } else {
                            assert(decoded.operands.data_processing_1source.rn == rn);
                            assert(decoded.operands.data_processing_1source.rd == rd);
                        }
                    }
                }
            }
        }
        for (unsigned type = 2; type < 4; type++) {
            struct aarch64_decoded decoded;
            assert(!aarch64_decode(operations[operation].bits | type << 22, &decoded));
        }
    }
}

static void test_special_values(void) {
    for (unsigned wide = 0; wide < 2; wide++) {
        qword_t sign = wide ? UINT64_C(0x8000000000000000) : UINT32_C(0x80000000);
        qword_t one = wide ? UINT64_C(0x3ff0000000000000) : UINT32_C(0x3f800000);
        qword_t quiet = wide ? UINT64_C(0x7ff8000000000123) : UINT32_C(0x7fc00123);
        qword_t signaling = wide ? UINT64_C(0x7ff0000000000456) : UINT32_C(0x7f800456);
        qword_t quiet_bit = wide ? UINT64_C(0x8000000000000) : UINT32_C(0x400000);
        struct {
            unsigned operation;
            qword_t left, right, result;
            dword_t fpcr, exceptions;
        } cases[] = {
            {0, sign, 0, 0, 0, 0},
            // FABS 是位操作：即使 FZ/DN 打开也保留非规格数和 signaling NaN。
            {0, sign | 1, 0, 1, AARCH64_FPCR_FZ | AARCH64_FPCR_DN, 0},
            {0, sign | signaling, 0, signaling, AARCH64_FPCR_DN, 0},
            {1, 0, sign, sign, 0, 0}, {1, sign, 0, sign, 0, 0},
            {2, 0, sign, 0, 0, 0}, {2, sign, 0, 0, 0, 0},
            {1, one | sign, one, one | sign, 0, 0},
            {2, one, one | sign, one, 0, 0},
            {1, quiet, one, quiet, 0, 0},
            {2, one, quiet, quiet, 0, 0},
            {3, quiet, one, one, AARCH64_FPCR_DN, 0},
            {4, one, quiet, one, 0, 0},
            {3, signaling, one, signaling | quiet_bit, 0, AARCH64_FPSR_IOC},
            {4, quiet, signaling, signaling | quiet_bit, 0, AARCH64_FPSR_IOC},
            {1, quiet, one, quiet & ~UINT64_C(0x123), AARCH64_FPCR_DN, 0},
            {1, 1 | sign, 0, sign, AARCH64_FPCR_FZ, AARCH64_FPSR_IDC},
            {2, 0, 1, 0, AARCH64_FPCR_FZ, AARCH64_FPSR_IDC},
        };
        for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            for (unsigned alias = 0; alias < 3; alias++) {
                struct cpu_state cpu = fp_test_cpu();
                cpu.fpcr = cases[i].fpcr;
                cpu.v[31].d[0] = cases[i].left;
                cpu.v[1].d[0] = cases[i].right;
                unsigned rd = alias == 0 ? 2 : alias == 1 ? 31 : 1;
                struct cpu_state expected = cpu;
                expected.pc += 4;
                expected.v[rd] = (union aarch64_vector_reg) {.d = {cases[i].result, 0}};
                expected.fpsr |= cases[i].exceptions;
                fp_test_instruction(operations[cases[i].operation].bits | wide << 22 |
                        (cases[i].operation ? 1 << 16 : 0) | 31 << 5 | rd, &cpu, &expected);
            }
        }
    }
}

#if defined(__aarch64__)
static struct fp_native_result native_operation(unsigned operation, bool wide,
        union aarch64_vector_reg left, union aarch64_vector_reg right,
        dword_t fpcr, dword_t initial_fpsr) {
    struct fp_native_result native = {0};
#define MINMAX_NATIVE(index, mnemonic) \
    case index * 2: FP_NATIVE_EXECUTE(mnemonic " s2, s0, s1"); break; \
    case index * 2 + 1: FP_NATIVE_EXECUTE(mnemonic " d2, d0, d1"); break
    switch (operation * 2 + wide) {
        case 0: FP_NATIVE_EXECUTE("fabs s2, s0"); break;
        case 1: FP_NATIVE_EXECUTE("fabs d2, d0"); break;
        MINMAX_NATIVE(1, "fmin"); MINMAX_NATIVE(2, "fmax");
        MINMAX_NATIVE(3, "fminnm"); MINMAX_NATIVE(4, "fmaxnm");
        default: assert(false);
    }
#undef MINMAX_NATIVE
    return native;
}

static void test_native_differential(void) {
    static const struct { dword_t single; qword_t double_precision; } values[] = {
        {0, 0}, {0x80000000, UINT64_C(0x8000000000000000)},
        {1, 1}, {0x80000001, UINT64_C(0x8000000000000001)},
        {0x007fffff, UINT64_C(0x000fffffffffffff)},
        {0x00800000, UINT64_C(0x0010000000000000)},
        {0x3f800000, UINT64_C(0x3ff0000000000000)},
        {0xbf800000, UINT64_C(0xbff0000000000000)},
        {0x7f7fffff, UINT64_C(0x7fefffffffffffff)},
        {0xff7fffff, UINT64_C(0xffefffffffffffff)},
        {0x7f800000, UINT64_C(0x7ff0000000000000)},
        {0xff800000, UINT64_C(0xfff0000000000000)},
        {0x7fc00123, UINT64_C(0x7ff8000000000123)},
        {0xffc00456, UINT64_C(0xfff8000000000456)},
        {0x7f800789, UINT64_C(0x7ff0000000000789)},
        {0xff800abc, UINT64_C(0xfff0000000000abc)},
    };
    qword_t seed = UINT64_C(0xb1739628e05ac4fd);
    unsigned count = 0;
    for (unsigned config = 0; config < 16; config++) {
        for (unsigned wide = 0; wide < 2; wide++) {
            for (unsigned sample = 0; sample < 16 * 16 + 128; sample++) {
                struct cpu_state cpu = fp_test_cpu();
                cpu.fpcr = (config & 3) << AARCH64_FPCR_RMODE_SHIFT |
                        (config & 4 ? AARCH64_FPCR_FZ : 0) |
                        (config & 8 ? AARCH64_FPCR_DN : 0);
                cpu.fpsr |= sample & 1 ? AARCH64_FPSR_IXC : AARCH64_FPSR_DZC;
                cpu.v[31].d[0] = sample < 256 ? (wide ?
                        values[sample / 16].double_precision : values[sample / 16].single) :
                        fp_test_random(&seed);
                cpu.v[1].d[0] = sample < 256 ? (wide ?
                        values[sample % 16].double_precision : values[sample % 16].single) :
                        fp_test_random(&seed);
                for (unsigned operation = 0; operation < 5; operation++) {
                    struct fp_native_result native = native_operation(operation, wide,
                            cpu.v[31], cpu.v[1], cpu.fpcr, cpu.fpsr);
                    unsigned rd = sample % 3 == 0 ? 31 : sample % 3 == 1 ? 1 : 2;
                    struct cpu_state expected = cpu;
                    expected.pc += 4;
                    expected.v[rd] = native.vector;
                    expected.fpsr = native.fpsr;
                    fp_test_instruction(operations[operation].bits | wide << 22 |
                            (operation ? 1 << 16 : 0) | 31 << 5 | rd, &cpu, &expected);
                    count++;
                }
            }
        }
    }
    printf("标量绝对值与极值：%u 组原生 AArch64 对照通过\n", count);
}
#endif

int main(void) {
    test_decoding();
    test_special_values();
#if defined(__aarch64__)
    test_native_differential();
#endif
    return 0;
}
