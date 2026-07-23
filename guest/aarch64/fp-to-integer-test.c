#include <assert.h>

#include "guest/aarch64/decode.h"
#include "guest/aarch64/execute.h"

#define CONVERSION_FIXED_MASK UINT32_C(0xfffffc00)

struct conversion_case {
    dword_t bits;
    byte_t source_width;
    byte_t destination_width;
};

static const struct conversion_case conversions[] = {
    {UINT32_C(0x1e380000), 32, 32},
    {UINT32_C(0x1e780000), 64, 32},
    {UINT32_C(0x9e380000), 32, 64},
    {UINT32_C(0x9e780000), 64, 64},
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
    assert(instruction.opcode == AARCH64_OP_FCVTZS_GENERAL);
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
    // BusyBox awk 的真实故障指令：fcvtzs w19, d0。
    assert_decode(UINT32_C(0x1e780013), 1, 19, 0);
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
    assert(decoded_count == 4096);
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
                    instruction.opcode == AARCH64_OP_FCVTZS_GENERAL;
            assert(conversion_decoded == is_conversion_encoding(candidate));
        }
    }
    assert(CONVERSION_FIXED_MASK == UINT32_C(0xfffffc00));
}

static void test_rejected_neighbors(void) {
    static const dword_t words[] = {
        UINT32_C(0x1e390020), // fcvtzu w0, s1
        UINT32_C(0x9e790062), // fcvtzu x2, d3
        UINT32_C(0x1ef800a4), // FP16 来源
        UINT32_C(0x1e18fca6), // 带定点缩放的编码族
    };
    for (unsigned i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
        struct aarch64_decoded instruction;
        bool decoded = aarch64_decode(words[i], &instruction);
        assert(!decoded || instruction.opcode != AARCH64_OP_FCVTZS_GENERAL);
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
            conversion < sizeof(conversions) / sizeof(conversions[0]);
            conversion++) {
        dword_t fpsr;
        qword_t value = run_conversion(conversion, sources[conversion],
                UINT32_C(0x00c00000), AARCH64_FPSR_QC, &fpsr);
        assert(value == expected[conversion]);
        assert(fpsr == (AARCH64_FPSR_QC | AARCH64_FPSR_IXC));
    }
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
    test_double_to_word_boundaries();
    test_double_to_xword_boundaries();
    test_subnormal_flush();
    test_zero_register_destination();
    test_rounding_mode_is_ignored();
    return 0;
}
