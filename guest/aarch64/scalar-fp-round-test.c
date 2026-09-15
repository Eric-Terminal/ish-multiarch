#include "guest/aarch64/fp-common-test.h"

static const struct {
    dword_t bits;
    enum aarch64_opcode opcode;
} rounds[] = {
    {0x1e244000, AARCH64_OP_FRINTN_SCALAR},
    {0x1e24c000, AARCH64_OP_FRINTP_SCALAR},
    {0x1e254000, AARCH64_OP_FRINTM_SCALAR},
    {0x1e25c000, AARCH64_OP_FRINTZ_SCALAR},
    {0x1e264000, AARCH64_OP_FRINTA_SCALAR},
    {0x1e27c000, AARCH64_OP_FRINTI_SCALAR},
    {0x1e274000, AARCH64_OP_FRINTX_SCALAR},
};

static const struct {
    dword_t bits;
    enum aarch64_opcode opcode;
} conversions[] = {
    {0x1e200000, AARCH64_OP_FCVTNS_GENERAL},
    {0x1e210000, AARCH64_OP_FCVTNU_GENERAL},
    {0x1e280000, AARCH64_OP_FCVTPS_GENERAL},
    {0x1e290000, AARCH64_OP_FCVTPU_GENERAL},
    {0x1e300000, AARCH64_OP_FCVTMS_GENERAL},
    {0x1e310000, AARCH64_OP_FCVTMU_GENERAL},
    {0x1e240000, AARCH64_OP_FCVTAS_GENERAL},
    {0x1e250000, AARCH64_OP_FCVTAU_GENERAL},
    {0x1e380000, AARCH64_OP_FCVTZS_GENERAL},
    {0x1e390000, AARCH64_OP_FCVTZU_GENERAL},
};

static void test_decoding(void) {
    for (unsigned operation = 0; operation < 7; operation++) {
        for (unsigned wide = 0; wide < 2; wide++) {
            for (unsigned rn = 0; rn < 32; rn++) {
                for (unsigned rd = 0; rd < 32; rd++) {
                    dword_t word = rounds[operation].bits | wide << 22 | rn << 5 | rd;
                    struct aarch64_decoded decoded;
                    assert(aarch64_decode(word, &decoded));
                    assert(decoded.opcode == rounds[operation].opcode);
                    assert(decoded.width == (wide ? 64 : 32));
                    assert(decoded.operands.data_processing_1source.rn == rn);
                    assert(decoded.operands.data_processing_1source.rd == rd);
                }
            }
        }
    }
    for (unsigned operation = 0; operation < 10; operation++) {
        for (unsigned layout = 0; layout < 4; layout++) {
            for (unsigned rn = 0; rn < 32; rn++) {
                for (unsigned rd = 0; rd < 32; rd++) {
                    dword_t word = conversions[operation].bits |
                            (layout & 1) << 22 | (layout >> 1) << 31 | rn << 5 | rd;
                    struct aarch64_decoded decoded;
                    assert(aarch64_decode(word, &decoded));
                    assert(decoded.opcode == conversions[operation].opcode);
                    assert(decoded.width == (layout & 1 ? 64 : 32));
                    assert(decoded.operands.fp_to_integer.destination_width ==
                            (layout & 2 ? 64 : 32));
                    assert(decoded.operands.fp_to_integer.rn == rn);
                    assert(decoded.operands.fp_to_integer.rd == rd);
                }
            }
        }
    }
    // type=10 保留、type=11 要求尚未公布的 FP16，不能扩大掩码误接入。
    for (unsigned type = 2; type < 4; type++) {
        for (unsigned operation = 0; operation < 7; operation++) {
            struct aarch64_decoded decoded;
            assert(!aarch64_decode(rounds[operation].bits | type << 22, &decoded));
        }
        for (unsigned operation = 0; operation < 10; operation++) {
            struct aarch64_decoded decoded;
            assert(!aarch64_decode(conversions[operation].bits | type << 22, &decoded));
        }
    }
    struct aarch64_decoded decoded;
    assert(!aarch64_decode(UINT32_C(0x1e26c000), &decoded));
}

static void test_halfway_and_flags(void) {
    static const struct {
        dword_t single;
        qword_t double_precision;
        int rounded[5]; // nearest-even、+∞、-∞、zero、ties-away 的独立预期。
    } cases[] = {
        {0x3f000000, UINT64_C(0x3fe0000000000000), {0, 1, 0, 0, 1}},
        {0xbf000000, UINT64_C(0xbfe0000000000000), {0, 0, -1, 0, -1}},
        {0x3fc00000, UINT64_C(0x3ff8000000000000), {2, 2, 1, 1, 2}},
        {0xbfc00000, UINT64_C(0xbff8000000000000), {-2, -1, -2, -1, -2}},
        {0x40200000, UINT64_C(0x4004000000000000), {2, 3, 2, 2, 3}},
        {0xc0200000, UINT64_C(0xc004000000000000), {-2, -2, -3, -2, -3}},
    };
    const qword_t positive_double[] = {0, UINT64_C(0x3ff0000000000000),
            UINT64_C(0x4000000000000000), UINT64_C(0x4008000000000000)};
    const dword_t positive_single[] = {0, 0x3f800000, 0x40000000, 0x40400000};
    for (unsigned wide = 0; wide < 2; wide++) {
        for (unsigned operation = 0; operation < 7; operation++) {
            for (unsigned mode = 0; mode < 4; mode++) {
                for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
                    struct cpu_state cpu = fp_test_cpu();
                    cpu.fpcr = mode << AARCH64_FPCR_RMODE_SHIFT;
                    cpu.v[31].d[0] = wide ? cases[i].double_precision :
                            UINT64_C(0xdeadbeef00000000) | cases[i].single;
                    int value = cases[i].rounded[operation < 5 ? operation : mode];
                    unsigned magnitude = value < 0 ? (unsigned) -value : (unsigned) value;
                    qword_t bits = wide ? positive_double[magnitude] : positive_single[magnitude];
                    if (cases[i].single >> 31)
                        bits |= wide ? UINT64_C(0x8000000000000000) : UINT32_C(0x80000000);
                    struct cpu_state expected = cpu;
                    expected.pc += 4;
                    expected.v[31] = (union aarch64_vector_reg) {.d = {bits, 0}};
                    if (operation == 6)
                        expected.fpsr |= AARCH64_FPSR_IXC;
                    fp_test_instruction(rounds[operation].bits | wide << 22 | 31 << 5 | 31,
                            &cpu, &expected);
                }
            }
        }
    }
}

static void test_conversion_boundaries(void) {
    static const struct {
        unsigned operation;
        qword_t source;
        qword_t value;
        dword_t flags;
    } cases[] = {
        {0, UINT64_C(0x3fe0000000000000), 0, AARCH64_FPSR_IXC},
        {0, UINT64_C(0x3ff8000000000000), 2, AARCH64_FPSR_IXC},
        {6, UINT64_C(0xbfe0000000000000), UINT32_MAX, AARCH64_FPSR_IXC},
        {1, UINT64_C(0xbfe0000000000000), 0, AARCH64_FPSR_IXC},
        {7, UINT64_C(0xbfe0000000000000), 0, AARCH64_FPSR_IOC},
        {3, UINT64_C(0xbfe8000000000000), 0, AARCH64_FPSR_IXC},
        {5, UINT64_C(0xbfe8000000000000), 0, AARCH64_FPSR_IOC},
        {4, UINT64_C(0xc1e0000000100000), UINT32_C(0x80000000), AARCH64_FPSR_IOC},
        {8, UINT64_C(0xc1e0000000100000), UINT32_C(0x80000000), AARCH64_FPSR_IXC},
        {3, UINT64_C(0x41effffffff80000), UINT32_MAX, AARCH64_FPSR_IOC},
        {5, UINT64_C(0x41effffffff80000), UINT32_MAX, AARCH64_FPSR_IXC},
        {0, UINT64_C(0x7ff0000000000001), 0, AARCH64_FPSR_IOC},
        {3, UINT64_C(0x7ff0000000000000), UINT32_MAX, AARCH64_FPSR_IOC},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        for (unsigned discard = 0; discard < 2; discard++) {
            struct cpu_state cpu = fp_test_cpu();
            cpu.v[31].d[0] = cases[i].source;
            struct cpu_state expected = cpu;
            expected.pc += 4;
            expected.fpsr |= cases[i].flags;
            unsigned rd = discard ? 31 : 5;
            if (!discard)
                expected.x[rd] = cases[i].value;
            fp_test_instruction(conversions[cases[i].operation].bits | 1 << 22 | 31 << 5 | rd,
                    &cpu, &expected);
        }
    }
}

#if defined(__aarch64__)
static struct fp_native_result native_round(unsigned operation, bool wide,
        union aarch64_vector_reg left, dword_t fpcr, dword_t initial_fpsr) {
    union aarch64_vector_reg right = {0};
    struct fp_native_result native = {0};
#define ROUND_NATIVE(index, mnemonic) \
    case index * 2: FP_NATIVE_EXECUTE(mnemonic " s2, s0"); break; \
    case index * 2 + 1: FP_NATIVE_EXECUTE(mnemonic " d2, d0"); break
    switch (operation * 2 + wide) {
        ROUND_NATIVE(0, "frintn"); ROUND_NATIVE(1, "frintp");
        ROUND_NATIVE(2, "frintm"); ROUND_NATIVE(3, "frintz");
        ROUND_NATIVE(4, "frinta"); ROUND_NATIVE(5, "frinti");
        ROUND_NATIVE(6, "frintx");
        default: assert(false);
    }
#undef ROUND_NATIVE
    return native;
}

static struct fp_native_result native_conversion(unsigned operation, unsigned layout,
        union aarch64_vector_reg left, dword_t fpcr, dword_t initial_fpsr) {
    union aarch64_vector_reg right = {0};
    struct fp_native_result native = {0};
#define CONVERSION_NATIVE(index, mnemonic) \
    case index * 4: FP_NATIVE_EXECUTE(mnemonic " %w[integer], s0"); break; \
    case index * 4 + 1: FP_NATIVE_EXECUTE(mnemonic " %w[integer], d0"); break; \
    case index * 4 + 2: FP_NATIVE_EXECUTE(mnemonic " %[integer], s0"); break; \
    case index * 4 + 3: FP_NATIVE_EXECUTE(mnemonic " %[integer], d0"); break
    switch (operation * 4 + layout) {
        CONVERSION_NATIVE(0, "fcvtns"); CONVERSION_NATIVE(1, "fcvtnu");
        CONVERSION_NATIVE(2, "fcvtps"); CONVERSION_NATIVE(3, "fcvtpu");
        CONVERSION_NATIVE(4, "fcvtms"); CONVERSION_NATIVE(5, "fcvtmu");
        CONVERSION_NATIVE(6, "fcvtas"); CONVERSION_NATIVE(7, "fcvtau");
        CONVERSION_NATIVE(8, "fcvtzs"); CONVERSION_NATIVE(9, "fcvtzu");
        default: assert(false);
    }
#undef CONVERSION_NATIVE
    return native;
}

static void test_native_differential(void) {
    static const struct { dword_t single; qword_t double_precision; } boundaries[] = {
        {0, 0}, {0x80000000, UINT64_C(0x8000000000000000)},
        {1, 1}, {0x80000001, UINT64_C(0x8000000000000001)},
        {0x007fffff, UINT64_C(0x000fffffffffffff)},
        {0x807fffff, UINT64_C(0x800fffffffffffff)},
        {0x00800000, UINT64_C(0x0010000000000000)},
        {0x3effffff, UINT64_C(0x3fdfffffffffffff)},
        {0x3f000000, UINT64_C(0x3fe0000000000000)},
        {0x3f000001, UINT64_C(0x3fe0000000000001)},
        {0xbeffffff, UINT64_C(0xbfdfffffffffffff)},
        {0xbf000000, UINT64_C(0xbfe0000000000000)},
        {0xbf000001, UINT64_C(0xbfe0000000000001)},
        {0x3fc00000, UINT64_C(0x3ff8000000000000)},
        {0x40200000, UINT64_C(0x4004000000000000)},
        {0xc0600000, UINT64_C(0xc00c000000000000)},
        {0x4b000000, UINT64_C(0x4330000000000000)},
        {0x4effffff, UINT64_C(0x41dfffffffe00000)},
        {0x4f000000, UINT64_C(0x41dfffffffe00001)},
        {0x4f000001, UINT64_C(0x41e0000000000000)},
        {0xcf000000, UINT64_C(0xc1e0000000000000)},
        {0xcf000001, UINT64_C(0xc1e0000000000001)},
        {0x4f7fffff, UINT64_C(0x41efffffffe00000)},
        {0x4f800000, UINT64_C(0x41efffffffe00001)},
        {0x4f800001, UINT64_C(0x41f0000000000000)},
        {0x5effffff, UINT64_C(0x43dfffffffffffff)},
        {0x5f000000, UINT64_C(0x43e0000000000000)},
        {0xdf000000, UINT64_C(0xc3e0000000000000)},
        {0xdf000001, UINT64_C(0xc3e0000000000001)},
        {0x5f7fffff, UINT64_C(0x43efffffffffffff)},
        {0x5f800000, UINT64_C(0x43f0000000000000)},
        {0x7f800000, UINT64_C(0x7ff0000000000000)},
        {0xff800000, UINT64_C(0xfff0000000000000)},
        {0x7fc12345, UINT64_C(0x7ff8000000000123)},
        {0x7f812345, UINT64_C(0x7ff0000000000123)},
        {0xff812345, UINT64_C(0xfff0000000000123)},
    };
    qword_t seed = UINT64_C(0x91ceface145);
    unsigned count = 0;
    for (unsigned mode = 0; mode < 16; mode++) {
        for (unsigned layout = 0; layout < 4; layout++) {
            for (unsigned sample = 0; sample < sizeof(boundaries) / sizeof(boundaries[0]) + 128;
                    sample++) {
                struct cpu_state cpu = fp_test_cpu();
                cpu.fpcr = (mode & 3) << AARCH64_FPCR_RMODE_SHIFT;
                if (mode & 4) cpu.fpcr |= AARCH64_FPCR_FZ;
                if (mode & 8) cpu.fpcr |= AARCH64_FPCR_DN;
                cpu.fpsr |= sample & 1 ? AARCH64_FPSR_IXC : AARCH64_FPSR_DZC;
                qword_t source = fp_test_random(&seed);
                if (sample < sizeof(boundaries) / sizeof(boundaries[0]))
                    source = layout & 1 ? boundaries[sample].double_precision :
                            UINT64_C(0xa5a5a5a500000000) | boundaries[sample].single;
                cpu.v[31].d[0] = source;
                if (layout < 2) {
                    for (unsigned operation = 0; operation < 7; operation++) {
                        struct fp_native_result native = native_round(operation, layout,
                                cpu.v[31], cpu.fpcr, cpu.fpsr);
                        struct cpu_state expected = cpu;
                        unsigned rd = sample % 3 == 0 ? 31 : 2;
                        expected.pc += 4;
                        expected.v[rd] = native.vector;
                        expected.fpsr = native.fpsr;
                        fp_test_instruction(rounds[operation].bits | layout << 22 | 31 << 5 | rd,
                                &cpu, &expected);
                        count++;
                    }
                }
                for (unsigned operation = 0; operation < 10; operation++) {
                    struct fp_native_result native = native_conversion(operation, layout,
                            cpu.v[31], cpu.fpcr, cpu.fpsr);
                    struct cpu_state expected = cpu;
                    unsigned rd = sample % 3 == 0 ? 31 : 2;
                    expected.pc += 4;
                    if (rd != 31) expected.x[rd] = native.integer;
                    expected.fpsr = native.fpsr;
                    fp_test_instruction(conversions[operation].bits | (layout & 1) << 22 |
                            (layout >> 1) << 31 | 31 << 5 | rd, &cpu, &expected);
                    count++;
                }
            }
        }
    }
    printf("标量舍入与整数转换：%u 组原生 AArch64 对照通过\n", count);
}
#endif

int main(void) {
    test_decoding();
    test_halfway_and_flags();
    test_conversion_boundaries();
#if defined(__aarch64__)
    test_native_differential();
#endif
    return 0;
}
