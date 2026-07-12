#include <assert.h>
#include <string.h>

#include "guest/aarch64/decode.h"
#include "guest/aarch64/execute.h"

#define CONVERSION_FIXED_MASK UINT32_C(0xfffffc00)

_Static_assert((UINT32_C(1) << AARCH64_FPCR_RMODE_SHIFT) ==
        UINT32_C(0x00400000), "FPCR 舍入模式位必须匹配 AArch64 ABI");
_Static_assert(AARCH64_FPCR_RMODE_MASK == UINT32_C(0x00c00000),
        "FPCR 舍入模式掩码必须匹配 AArch64 ABI");
_Static_assert(AARCH64_FPSR_IXC == UINT32_C(0x00000010),
        "FPSR 不精确标志必须匹配 AArch64 ABI");

struct conversion_case {
    dword_t bits;
    enum aarch64_opcode opcode;
    byte_t source_width;
    byte_t destination_width;
};

static const struct conversion_case conversions[] = {
    {UINT32_C(0x1e220000), AARCH64_OP_SCVTF_GENERAL, 32, 32},
    {UINT32_C(0x1e620000), AARCH64_OP_SCVTF_GENERAL, 32, 64},
    {UINT32_C(0x9e220000), AARCH64_OP_SCVTF_GENERAL, 64, 32},
    {UINT32_C(0x9e620000), AARCH64_OP_SCVTF_GENERAL, 64, 64},
    {UINT32_C(0x1e230000), AARCH64_OP_UCVTF_GENERAL, 32, 32},
    {UINT32_C(0x1e630000), AARCH64_OP_UCVTF_GENERAL, 32, 64},
    {UINT32_C(0x9e230000), AARCH64_OP_UCVTF_GENERAL, 64, 32},
    {UINT32_C(0x9e630000), AARCH64_OP_UCVTF_GENERAL, 64, 64},
};

static dword_t encode(unsigned conversion, byte_t rn, byte_t rd) {
    return conversions[conversion].bits | (dword_t) rn << 5 | rd;
}

static bool is_conversion_opcode(enum aarch64_opcode opcode) {
    return opcode == AARCH64_OP_SCVTF_GENERAL ||
            opcode == AARCH64_OP_UCVTF_GENERAL;
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
    assert(instruction.opcode == conversions[conversion].opcode);
    assert(instruction.width == conversions[conversion].source_width);
    assert(instruction.operands.integer_to_fp.destination_width ==
            conversions[conversion].destination_width);
    assert(instruction.operands.integer_to_fp.rd == rd);
    assert(instruction.operands.integer_to_fp.rn == rn);
}

static void test_llvm_vectors(void) {
    assert_decode(UINT32_C(0x1e2200a3), 0, 3, 5);
    assert_decode(UINT32_C(0x1e6200a3), 1, 3, 5);
    assert_decode(UINT32_C(0x9e2200a3), 2, 3, 5);
    assert_decode(UINT32_C(0x9e6200a3), 3, 3, 5);
    assert_decode(UINT32_C(0x1e2300a3), 4, 3, 5);
    assert_decode(UINT32_C(0x1e6300a3), 5, 3, 5);
    assert_decode(UINT32_C(0x9e2300a3), 6, 3, 5);
    assert_decode(UINT32_C(0x9e6300a3), 7, 3, 5);
    assert_decode(UINT32_C(0x9e630000), 7, 0, 0);
}

static void test_encoding_space(void) {
    unsigned decoded_count = 0;
    for (unsigned conversion = 0;
            conversion < sizeof(conversions) / sizeof(conversions[0]);
            conversion++) {
        for (unsigned rn = 0; rn < 32; rn++) {
            for (unsigned rd = 0; rd < 32; rd++) {
                struct aarch64_decoded instruction;
                bool decoded = aarch64_decode(encode(conversion,
                        (byte_t) rn, (byte_t) rd), &instruction);
                assert(decoded);
                if (!decoded)
                    continue;
                decoded_count++;
                assert(instruction.opcode == conversions[conversion].opcode);
                assert(instruction.width ==
                        conversions[conversion].source_width);
                assert(instruction.operands.integer_to_fp.
                        destination_width ==
                        conversions[conversion].destination_width);
                assert(instruction.operands.integer_to_fp.rd == rd);
                assert(instruction.operands.integer_to_fp.rn == rn);
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
            assert(conversion_decoded ==
                    is_conversion_encoding(candidate));
        }
    }
    assert(CONVERSION_FIXED_MASK == UINT32_C(0xfffffc00));
}

static void test_rejected_neighbors(void) {
    static const dword_t words[] = {
        UINT32_C(0x1ee20020),
        UINT32_C(0x9ee30062),
        UINT32_C(0x5e21d8a4),
        UINT32_C(0x7e61d8e6),
        UINT32_C(0x0e21d928),
        UINT32_C(0x6e61d96a),
        UINT32_C(0x1e02fdac),
        UINT32_C(0x9e4301ee),
    };
    for (unsigned i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
        struct aarch64_decoded instruction;
        bool decoded = aarch64_decode(words[i], &instruction);
        assert(!decoded || !is_conversion_opcode(instruction.opcode));
    }
}

static qword_t converted_bits(unsigned conversion, qword_t source,
        dword_t fpcr, dword_t initial_fpsr, dword_t *fpsr) {
    struct cpu_state cpu = {
        .pc = UINT64_C(0x1000),
        .fpcr = fpcr,
        .fpsr = initial_fpsr,
    };
    cpu.x[5] = source;
    cpu.v[6].d[0] = UINT64_MAX;
    cpu.v[6].d[1] = UINT64_MAX;
    execute_instruction(&cpu, encode(conversion, 5, 6));
    assert(cpu.pc == UINT64_C(0x1004));
    assert(cpu.x[5] == source);
    assert(cpu.v[6].d[1] == 0);
    if (conversions[conversion].destination_width == 32)
        assert((cpu.v[6].d[0] >> 32) == 0);
    *fpsr = cpu.fpsr;
    return conversions[conversion].destination_width == 32 ?
            cpu.v[6].s[0] : cpu.v[6].d[0];
}

static void test_basic_conversions(void) {
    static const qword_t sources[] = {
        UINT64_C(0xdeadbeefffffffd6),
        UINT64_C(0xdeadbeefffffffd6),
        UINT64_C(0xffffffffffffffd6),
        UINT64_C(0xffffffffffffffd6),
        UINT64_C(0xdeadbeef0000002a),
        UINT64_C(0xdeadbeef0000002a),
        UINT64_C(42),
        UINT64_C(42),
    };
    static const qword_t expected[] = {
        UINT32_C(0xc2280000),
        UINT64_C(0xc045000000000000),
        UINT32_C(0xc2280000),
        UINT64_C(0xc045000000000000),
        UINT32_C(0x42280000),
        UINT64_C(0x4045000000000000),
        UINT32_C(0x42280000),
        UINT64_C(0x4045000000000000),
    };
    for (unsigned conversion = 0;
            conversion < sizeof(conversions) / sizeof(conversions[0]);
            conversion++) {
        struct cpu_state cpu = {
            .pc = UINT64_C(0x2000),
            .sp = UINT64_C(0x1122334455667788),
            .nzcv = UINT32_C(0xa0000000),
            .fpcr = UINT32_C(0x01000000),
            .fpsr = UINT32_C(0x08000001),
        };
        cpu.x[5] = sources[conversion];
        cpu.x[7] = UINT64_C(0x8877665544332211);
        cpu.v[6].d[0] = UINT64_MAX;
        cpu.v[6].d[1] = UINT64_MAX;
        cpu.v[7].d[0] = UINT64_C(0x0123456789abcdef);
        cpu.v[7].d[1] = UINT64_C(0xfedcba9876543210);
        execute_instruction(&cpu, encode(conversion, 5, 6));
        assert(cpu.v[6].d[0] == expected[conversion]);
        assert(cpu.v[6].d[1] == 0);
        assert(cpu.x[5] == sources[conversion]);
        assert(cpu.x[7] == UINT64_C(0x8877665544332211));
        assert(cpu.v[7].d[0] == UINT64_C(0x0123456789abcdef));
        assert(cpu.v[7].d[1] == UINT64_C(0xfedcba9876543210));
        assert(cpu.pc == UINT64_C(0x2004));
        assert(cpu.sp == UINT64_C(0x1122334455667788));
        assert(cpu.nzcv == UINT32_C(0xa0000000));
        assert(cpu.fpcr == UINT32_C(0x01000000));
        assert(cpu.fpsr == UINT32_C(0x08000001));
    }
}

static void test_nearest_even_boundaries(void) {
    dword_t fpsr;
    qword_t result = converted_bits(6,
            (UINT64_C(1) << 24) + 1, 0, UINT32_C(0x08000001), &fpsr);
    assert(result == UINT32_C(0x4b800000));
    assert(fpsr == UINT32_C(0x08000011));

    result = converted_bits(6,
            (UINT64_C(1) << 24) + 3, 0, 0, &fpsr);
    assert(result == UINT32_C(0x4b800002));
    assert(fpsr == AARCH64_FPSR_IXC);

    result = converted_bits(7,
            (UINT64_C(1) << 53) + 1, 0, 0, &fpsr);
    assert(result == UINT64_C(0x4340000000000000));
    assert(fpsr == AARCH64_FPSR_IXC);

    result = converted_bits(7,
            (UINT64_C(1) << 53) + 3, 0, 0, &fpsr);
    assert(result == UINT64_C(0x4340000000000002));
    assert(fpsr == AARCH64_FPSR_IXC);
}

static void test_directed_rounding(void) {
    static const dword_t modes[] = {
        0,
        UINT32_C(1) << AARCH64_FPCR_RMODE_SHIFT,
        UINT32_C(2) << AARCH64_FPCR_RMODE_SHIFT,
        UINT32_C(3) << AARCH64_FPCR_RMODE_SHIFT,
    };
    static const qword_t positive[] = {
        UINT64_C(0x4340000000000000),
        UINT64_C(0x4340000000000001),
        UINT64_C(0x4340000000000000),
        UINT64_C(0x4340000000000000),
    };
    static const qword_t negative[] = {
        UINT64_C(0xc340000000000000),
        UINT64_C(0xc340000000000000),
        UINT64_C(0xc340000000000001),
        UINT64_C(0xc340000000000000),
    };
    static const qword_t positive_single[] = {
        UINT32_C(0x4b800000),
        UINT32_C(0x4b800001),
        UINT32_C(0x4b800000),
        UINT32_C(0x4b800000),
    };
    static const qword_t negative_single[] = {
        UINT32_C(0xcb800000),
        UINT32_C(0xcb800000),
        UINT32_C(0xcb800001),
        UINT32_C(0xcb800000),
    };
    for (unsigned mode = 0; mode < sizeof(modes) / sizeof(modes[0]); mode++) {
        dword_t fpsr;
        qword_t result = converted_bits(7,
                (UINT64_C(1) << 53) + 1,
                modes[mode], UINT32_C(0x40000000), &fpsr);
        assert(result == positive[mode]);
        assert(fpsr == (UINT32_C(0x40000000) | AARCH64_FPSR_IXC));

        qword_t magnitude = (UINT64_C(1) << 53) + 1;
        result = converted_bits(3, 0 - magnitude,
                modes[mode], UINT32_C(0x40000000), &fpsr);
        assert(result == negative[mode]);
        assert(fpsr == (UINT32_C(0x40000000) | AARCH64_FPSR_IXC));

        for (unsigned source_width = 0; source_width < 2; source_width++) {
            unsigned unsigned_conversion = source_width == 0 ? 4 : 6;
            unsigned signed_conversion = source_width == 0 ? 0 : 2;
            result = converted_bits(unsigned_conversion,
                    (UINT64_C(1) << 24) + 1,
                    modes[mode], 0, &fpsr);
            assert(result == positive_single[mode]);
            assert(fpsr == AARCH64_FPSR_IXC);

            qword_t negative_source = source_width == 0 ?
                    UINT32_C(0xfeffffff) : UINT64_C(0xfffffffffeffffff);
            result = converted_bits(signed_conversion, negative_source,
                    modes[mode], 0, &fpsr);
            assert(result == negative_single[mode]);
            assert(fpsr == AARCH64_FPSR_IXC);
        }
    }
}

static void test_extremes_and_exactness(void) {
    dword_t fpsr;
    qword_t result = converted_bits(7, UINT64_MAX,
            0, UINT32_C(0x08000000), &fpsr);
    assert(result == UINT64_C(0x43f0000000000000));
    assert(fpsr == UINT32_C(0x08000010));

    result = converted_bits(7, UINT64_MAX,
            UINT32_C(2) << AARCH64_FPCR_RMODE_SHIFT, 0, &fpsr);
    assert(result == UINT64_C(0x43efffffffffffff));
    assert(fpsr == AARCH64_FPSR_IXC);

    result = converted_bits(4, UINT32_MAX,
            0, 0, &fpsr);
    assert(result == UINT32_C(0x4f800000));
    assert(fpsr == AARCH64_FPSR_IXC);

    result = converted_bits(3, UINT64_C(1) << 63,
            0, UINT32_C(0x08000001), &fpsr);
    assert(result == UINT64_C(0xc3e0000000000000));
    assert(fpsr == UINT32_C(0x08000001));

    result = converted_bits(0, UINT32_C(0x80000000),
            0, 0, &fpsr);
    assert(result == UINT32_C(0xcf000000));
    assert(fpsr == 0);

    result = converted_bits(3, UINT64_C(0x8000000000000001),
            0, 0, &fpsr);
    assert(result == UINT64_C(0xc3e0000000000000));
    assert(fpsr == AARCH64_FPSR_IXC);

    result = converted_bits(7, 42, 0,
            UINT32_C(0x08000000) | AARCH64_FPSR_IXC, &fpsr);
    assert(result == UINT64_C(0x4045000000000000));
    assert(fpsr == (UINT32_C(0x08000000) | AARCH64_FPSR_IXC));
}

static void test_zero_register(void) {
    for (unsigned conversion = 0;
            conversion < sizeof(conversions) / sizeof(conversions[0]);
            conversion++) {
        struct cpu_state cpu = {
            .pc = UINT64_C(0x3000),
            .sp = UINT64_C(0x8877665544332211),
            .fpsr = UINT32_C(0x08000001),
        };
        cpu.v[31].d[0] = UINT64_MAX;
        cpu.v[31].d[1] = UINT64_MAX;
        execute_instruction(&cpu, encode(conversion, 31, 31));
        assert(cpu.v[31].d[0] == 0);
        assert(cpu.v[31].d[1] == 0);
        assert(cpu.sp == UINT64_C(0x8877665544332211));
        assert(cpu.fpsr == UINT32_C(0x08000001));
        assert(cpu.pc == UINT64_C(0x3004));
    }
}

int main(void) {
    test_llvm_vectors();
    test_encoding_space();
    test_fixed_bits();
    test_rejected_neighbors();
    test_basic_conversions();
    test_nearest_even_boundaries();
    test_directed_rounding();
    test_extremes_and_exactness();
    test_zero_register();
    return 0;
}
