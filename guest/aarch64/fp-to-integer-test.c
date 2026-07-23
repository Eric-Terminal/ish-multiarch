#include <assert.h>

#include "guest/aarch64/decode.h"
#include "guest/aarch64/execute.h"

#define CONVERSION_FIXED_MASK UINT32_C(0xfffffc00)

struct conversion_case {
    dword_t bits;
    enum aarch64_opcode opcode;
    byte_t source_width;
    byte_t destination_width;
};

static const struct conversion_case conversions[] = {
    {UINT32_C(0x1e380000), AARCH64_OP_FCVTZS_GENERAL, 32, 32},
    {UINT32_C(0x1e780000), AARCH64_OP_FCVTZS_GENERAL, 64, 32},
    {UINT32_C(0x9e380000), AARCH64_OP_FCVTZS_GENERAL, 32, 64},
    {UINT32_C(0x9e780000), AARCH64_OP_FCVTZS_GENERAL, 64, 64},
    {UINT32_C(0x1e390000), AARCH64_OP_FCVTZU_GENERAL, 32, 32},
    {UINT32_C(0x1e790000), AARCH64_OP_FCVTZU_GENERAL, 64, 32},
    {UINT32_C(0x9e390000), AARCH64_OP_FCVTZU_GENERAL, 32, 64},
    {UINT32_C(0x9e790000), AARCH64_OP_FCVTZU_GENERAL, 64, 64},
};

static dword_t encode(unsigned conversion, byte_t rn, byte_t rd) {
    return conversions[conversion].bits | (dword_t) rn << 5 | rd;
}

static bool is_conversion_encoding(dword_t word) {
    dword_t bits = word & CONVERSION_FIXED_MASK;
    for (unsigned i = 0; i < sizeof(conversions) / sizeof(conversions[0]); i++) {
        if (bits == conversions[i].bits)
            return true;
    }
    return false;
}

static bool is_conversion_opcode(enum aarch64_opcode opcode) {
    return opcode == AARCH64_OP_FCVTZS_GENERAL ||
            opcode == AARCH64_OP_FCVTZU_GENERAL;
}

static struct aarch64_decoded decode(dword_t word) {
    struct aarch64_decoded instruction = {0};
    bool decoded = aarch64_decode(word, &instruction);
    assert(decoded);
    use(decoded);
    return instruction;
}

static void execute_instruction(struct cpu_state *cpu, dword_t word) {
    struct aarch64_decoded instruction = decode(word);
    struct aarch64_execute_result result =
            aarch64_execute(cpu, NULL, &instruction);
    assert(result.stop == AARCH64_EXECUTE_RETIRED);
}

static void assert_decode(dword_t word, unsigned conversion,
        byte_t rd, byte_t rn) {
    struct aarch64_decoded instruction = decode(word);
    assert(instruction.opcode == conversions[conversion].opcode);
    assert(instruction.width == conversions[conversion].source_width);
    assert(instruction.operands.fp_to_integer.destination_width ==
            conversions[conversion].destination_width);
    assert(instruction.operands.fp_to_integer.rd == rd);
    assert(instruction.operands.fp_to_integer.rn == rn);
}

static void test_llvm_vectors(void) {
    assert_decode(UINT32_C(0x1e3800a3), 0, 3, 5);
    assert_decode(UINT32_C(0x1e7800a3), 1, 3, 5);
    assert_decode(UINT32_C(0x9e3800a3), 2, 3, 5);
    assert_decode(UINT32_C(0x9e7800a3), 3, 3, 5);
    assert_decode(UINT32_C(0x1e3900a3), 4, 3, 5);
    assert_decode(UINT32_C(0x1e7900a3), 5, 3, 5);
    assert_decode(UINT32_C(0x9e3900a3), 6, 3, 5);
    assert_decode(UINT32_C(0x9e7900a3), 7, 3, 5);
    // BusyBox awk 的真实故障指令：fcvtzs w19, d0。
    assert_decode(UINT32_C(0x1e780013), 1, 19, 0);
    // Python 的真实故障指令：fcvtzu x1, d31。
    assert_decode(UINT32_C(0x9e7903e1), 7, 1, 31);
}

static void test_encoding_space(void) {
    unsigned decoded_count = 0;
    for (unsigned conversion = 0;
            conversion < sizeof(conversions) / sizeof(conversions[0]);
            conversion++) {
        for (unsigned rn = 0; rn < 32; rn++) {
            for (unsigned rd = 0; rd < 32; rd++) {
                assert_decode(encode(conversion, (byte_t) rn, (byte_t) rd),
                        conversion, (byte_t) rd, (byte_t) rn);
                decoded_count++;
            }
        }
    }
    assert(decoded_count == 8192);
}

static void test_fixed_bits(void) {
    for (unsigned conversion = 0;
            conversion < sizeof(conversions) / sizeof(conversions[0]);
            conversion++) {
        dword_t base = encode(conversion, 1, 0);
        for (unsigned bit = 10; bit < 32; bit++) {
            struct aarch64_decoded instruction;
            dword_t candidate = base ^ (UINT32_C(1) << bit);
            bool decoded = aarch64_decode(candidate, &instruction);
            bool conversion_decoded = decoded &&
                    is_conversion_opcode(instruction.opcode);
            assert(conversion_decoded == is_conversion_encoding(candidate));
        }
    }
    assert(CONVERSION_FIXED_MASK == UINT32_C(0xfffffc00));
}

static void test_rejected_neighbors(void) {
    static const dword_t words[] = {
        UINT32_C(0x1ef800a4), // FP16 来源
        UINT32_C(0x1ef900a4), // FCVTZU 的 FP16 来源
        UINT32_C(0x1eb900a4), // 保留的 type=10
        UINT32_C(0x1e18fca6), // 带定点缩放的编码族
        UINT32_C(0x9e590000), // FCVTZU X,D,#64
        UINT32_C(0x9e59fc00), // FCVTZU X,D,#1
        UINT32_C(0x1e210020), // FCVTNU W,S
        UINT32_C(0x1e250020), // FCVTAU W,S
        UINT32_C(0x1e290020), // FCVTPU W,S
        UINT32_C(0x1e310020), // FCVTMU W,S
        UINT32_C(0x7ea1b820), // AdvSIMD scalar FCVTZU S,S
        UINT32_C(0x2ea1b820), // AdvSIMD vector FCVTZU V.2S,V.2S
        UINT32_C(0x1e770020), // Armv9.6 FPRCVT S,D
    };
    for (unsigned i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
        struct aarch64_decoded instruction;
        bool decoded = aarch64_decode(words[i], &instruction);
        assert(!decoded || !is_conversion_opcode(instruction.opcode));
    }
}

static qword_t run_conversion(unsigned conversion, qword_t source,
        dword_t fpcr, dword_t initial_fpsr, dword_t *fpsr) {
    struct cpu_state cpu = {
        .pc = UINT64_C(0x1000),
        .sp = UINT64_C(0x1122334455667788),
        .nzcv = UINT32_C(0xa0000000),
        .fpcr = fpcr,
        .fpsr = initial_fpsr,
    };
    cpu.x[7] = UINT64_C(0xfeedfacecafebeef);
    cpu.v[5].d[0] = source;
    cpu.v[5].d[1] = UINT64_C(0x0123456789abcdef);
    execute_instruction(&cpu, encode(conversion, 5, 7));

    assert(cpu.pc == UINT64_C(0x1004));
    assert(cpu.sp == UINT64_C(0x1122334455667788));
    assert(cpu.nzcv == UINT32_C(0xa0000000));
    assert(cpu.fpcr == fpcr);
    assert(cpu.v[5].d[0] == source);
    assert(cpu.v[5].d[1] == UINT64_C(0x0123456789abcdef));
    if (conversions[conversion].destination_width == 32)
        assert((cpu.x[7] >> 32) == 0);
    *fpsr = cpu.fpsr;
    return cpu.x[7];
}

static void test_basic_conversions(void) {
    static const qword_t sources[] = {
        UINT32_C(0x3ff33333),
        UINT64_C(0xbffe666666666666),
        UINT32_C(0xc0700000),
        UINT64_C(0x4021800000000000),
    };
    static const qword_t expected[] = {
        UINT32_C(1),
        UINT32_C(0xffffffff),
        UINT64_C(0xfffffffffffffffd),
        UINT64_C(8),
    };
    for (unsigned conversion = 0;
            conversion < 4;
            conversion++) {
        dword_t fpsr;
        qword_t value = run_conversion(conversion, sources[conversion],
                UINT32_C(0x00c00000), AARCH64_FPSR_QC, &fpsr);
        assert(value == expected[conversion]);
        assert(fpsr == (AARCH64_FPSR_QC | AARCH64_FPSR_IXC));
    }
}

static void test_product_conversion(void) {
    struct cpu_state cpu = {
        .pc = UINT64_C(0x1000),
        .sp = UINT64_C(0x1122334455667788),
        .nzcv = UINT32_C(0xa0000000),
        .fpcr = AARCH64_FPCR_DN | AARCH64_FPCR_FZ,
        .fpsr = AARCH64_FPSR_QC | AARCH64_FPSR_IXC,
    };
    cpu.x[1] = UINT64_C(16);
    cpu.v[31].d[0] = UINT64_C(0x403e000000000000);
    cpu.v[31].d[1] = UINT64_C(0x0123456789abcdef);
    execute_instruction(&cpu, UINT32_C(0x9e7903e1));

    assert(cpu.x[1] == UINT64_C(30));
    assert(cpu.v[31].d[0] == UINT64_C(0x403e000000000000));
    assert(cpu.v[31].d[1] == UINT64_C(0x0123456789abcdef));
    assert(cpu.pc == UINT64_C(0x1004));
    assert(cpu.sp == UINT64_C(0x1122334455667788));
    assert(cpu.nzcv == UINT32_C(0xa0000000));
    assert(cpu.fpcr == (AARCH64_FPCR_DN | AARCH64_FPCR_FZ));
    assert(cpu.fpsr == (AARCH64_FPSR_QC | AARCH64_FPSR_IXC));
}

static void test_unsigned_boundaries(void) {
    struct unsigned_case {
        unsigned conversion;
        qword_t source;
        qword_t expected;
        dword_t exception;
    };
    static const struct unsigned_case cases[] = {
        {4, UINT32_C(0x00000000), 0, 0},
        {4, UINT32_C(0x80000000), 0, 0},
        {4, UINT32_C(0x3ff33333), 1, AARCH64_FPSR_IXC},
        {4, UINT32_C(0xbf400000), 0, AARCH64_FPSR_IXC},
        {4, UINT32_C(0xbf800000), 0, AARCH64_FPSR_IOC},
        {4, UINT32_C(0x4f7fffff), UINT32_C(0xffffff00), 0},
        {4, UINT32_C(0x4f800000), UINT32_MAX, AARCH64_FPSR_IOC},
        {4, UINT32_C(0x7f800000), UINT32_MAX, AARCH64_FPSR_IOC},
        {4, UINT32_C(0xff800000), 0, AARCH64_FPSR_IOC},
        {4, UINT32_C(0x7fc12345), 0, AARCH64_FPSR_IOC},
        {4, UINT32_C(0x7f812345), 0, AARCH64_FPSR_IOC},
        {5, UINT64_C(0x41efffffffe00000), UINT32_MAX, 0},
        {5, UINT64_C(0x41effffffff80000), UINT32_MAX,
                AARCH64_FPSR_IXC},
        {5, UINT64_C(0x41f0000000000000), UINT32_MAX,
                AARCH64_FPSR_IOC},
        {6, UINT32_C(0x5f7fffff), UINT64_C(0xffffff0000000000), 0},
        {6, UINT32_C(0x5f800000), UINT64_MAX, AARCH64_FPSR_IOC},
        {7, UINT64_C(0x43efffffffffffff),
                UINT64_C(0xfffffffffffff800), 0},
        {7, UINT64_C(0x43f0000000000000), UINT64_MAX,
                AARCH64_FPSR_IOC},
    };

    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        dword_t fpsr;
        qword_t value = run_conversion(cases[i].conversion,
                cases[i].source, AARCH64_FPCR_DN,
                AARCH64_FPSR_QC | AARCH64_FPSR_OFC, &fpsr);
        assert(value == cases[i].expected);
        assert(fpsr == (AARCH64_FPSR_QC | AARCH64_FPSR_OFC |
                cases[i].exception));
    }
}

static void test_unsigned_subnormal_flush(void) {
    for (unsigned sign = 0; sign < 2; sign++) {
        qword_t source = UINT64_C(1) | (sign == 0 ? 0 :
                UINT64_C(0x8000000000000000));
        dword_t fpsr;
        qword_t value = run_conversion(7, source, 0, 0, &fpsr);
        assert(value == 0);
        assert(fpsr == AARCH64_FPSR_IXC);

        value = run_conversion(7, source, AARCH64_FPCR_FZ, 0, &fpsr);
        assert(value == 0);
        assert(fpsr == AARCH64_FPSR_IDC);
    }
}

static void test_unsigned_rounding_mode_is_ignored(void) {
    for (unsigned mode = 0; mode < 4; mode++) {
        dword_t fpcr = (dword_t) mode << AARCH64_FPCR_RMODE_SHIFT;
        dword_t fpsr;
        qword_t value = run_conversion(7,
                UINT64_C(0x403ec00000000000), fpcr, 0, &fpsr);
        assert(value == 30);
        assert(fpsr == AARCH64_FPSR_IXC);

        value = run_conversion(7, UINT64_C(0xbfe8000000000000),
                fpcr, 0, &fpsr);
        assert(value == 0);
        assert(fpsr == AARCH64_FPSR_IXC);
    }
}

static void test_unsigned_zero_register_destination(void) {
    struct cpu_state cpu = {
        .pc = UINT64_C(0x1000),
        .sp = UINT64_C(0x1122334455667788),
        .fpsr = AARCH64_FPSR_QC,
    };
    cpu.v[31].d[0] = UINT64_C(0x43f0000000000000);
    execute_instruction(&cpu,
            UINT32_C(0x9e790000) | (dword_t) 31 << 5 | 31);

    assert(cpu.pc == UINT64_C(0x1004));
    assert(cpu.sp == UINT64_C(0x1122334455667788));
    assert(cpu.fpsr == (AARCH64_FPSR_QC | AARCH64_FPSR_IOC));
    assert(cpu.v[31].d[0] == UINT64_C(0x43f0000000000000));
}

static void test_double_to_word_boundaries(void) {
    struct boundary_case {
        qword_t source;
        qword_t expected;
        dword_t exception;
    } cases[] = {
        {UINT64_C(0x41dfffffffc00000), UINT32_C(0x7fffffff), 0},
        {UINT64_C(0x41dffffffff00000), UINT32_C(0x7fffffff),
                AARCH64_FPSR_IXC},
        {UINT64_C(0x41e0000000000000), UINT32_C(0x7fffffff),
                AARCH64_FPSR_IOC},
        {UINT64_C(0xc1e0000000000000), UINT32_C(0x80000000), 0},
        {UINT64_C(0xc1e0000000100000), UINT32_C(0x80000000),
                AARCH64_FPSR_IXC},
        {UINT64_C(0xc1e0000000180000), UINT32_C(0x80000000),
                AARCH64_FPSR_IXC},
        {UINT64_C(0xc1e0000000200000), UINT32_C(0x80000000),
                AARCH64_FPSR_IOC},
        {UINT64_C(0x7ff0000000000000), UINT32_C(0x7fffffff),
                AARCH64_FPSR_IOC},
        {UINT64_C(0xfff0000000000000), UINT32_C(0x80000000),
                AARCH64_FPSR_IOC},
        {UINT64_C(0x7ff8000000000000), 0, AARCH64_FPSR_IOC},
        {UINT64_C(0x7ff0000000000001), 0, AARCH64_FPSR_IOC},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        dword_t fpsr;
        qword_t value = run_conversion(1, cases[i].source, 0,
                AARCH64_FPSR_OFC, &fpsr);
        assert(value == cases[i].expected);
        assert(fpsr == (AARCH64_FPSR_OFC | cases[i].exception));
    }
}

static void test_double_to_xword_boundaries(void) {
    dword_t fpsr;
    qword_t value = run_conversion(3, UINT64_C(0x43e0000000000000),
            0, AARCH64_FPSR_DZC, &fpsr);
    assert(value == UINT64_C(0x7fffffffffffffff));
    assert(fpsr == (AARCH64_FPSR_DZC | AARCH64_FPSR_IOC));

    value = run_conversion(3, UINT64_C(0xc3e0000000000000),
            0, AARCH64_FPSR_DZC, &fpsr);
    assert(value == UINT64_C(0x8000000000000000));
    assert(fpsr == AARCH64_FPSR_DZC);
}

static void test_subnormal_flush(void) {
    dword_t fpsr;
    qword_t value = run_conversion(1, UINT64_C(1), 0, 0, &fpsr);
    assert(value == 0);
    assert(fpsr == AARCH64_FPSR_IXC);

    value = run_conversion(1, UINT64_C(1), AARCH64_FPCR_FZ, 0, &fpsr);
    assert(value == 0);
    assert(fpsr == AARCH64_FPSR_IDC);
}

static void test_zero_register_destination(void) {
    struct cpu_state cpu = {
        .pc = UINT64_C(0x1000),
        .sp = UINT64_C(0x1122334455667788),
        .fpsr = AARCH64_FPSR_QC,
    };
    cpu.v[5].d[0] = UINT64_C(0x3ff8000000000000);
    execute_instruction(&cpu, encode(1, 5, 31));

    assert(cpu.pc == UINT64_C(0x1004));
    assert(cpu.sp == UINT64_C(0x1122334455667788));
    assert(cpu.fpsr == (AARCH64_FPSR_QC | AARCH64_FPSR_IXC));
}

static void test_rounding_mode_is_ignored(void) {
    for (unsigned mode = 0; mode < 4; mode++) {
        dword_t fpsr;
        qword_t value = run_conversion(1,
                UINT64_C(0x3ffe666666666666),
                (dword_t) mode << AARCH64_FPCR_RMODE_SHIFT,
                0, &fpsr);
        assert(value == 1);
        assert(fpsr == AARCH64_FPSR_IXC);
    }
}

int main(void) {
    test_llvm_vectors();
    test_encoding_space();
    test_fixed_bits();
    test_rejected_neighbors();
    test_basic_conversions();
    test_product_conversion();
    test_unsigned_boundaries();
    test_unsigned_subnormal_flush();
    test_unsigned_rounding_mode_is_ignored();
    test_unsigned_zero_register_destination();
    test_double_to_word_boundaries();
    test_double_to_xword_boundaries();
    test_subnormal_flush();
    test_zero_register_destination();
    test_rounding_mode_is_ignored();
    return 0;
}
