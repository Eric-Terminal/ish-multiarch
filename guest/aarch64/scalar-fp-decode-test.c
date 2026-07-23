#include <assert.h>

#include "guest/aarch64/decode.h"

#define BINARY_FIXED_MASK UINT32_C(0xffe0fc00)
#define SELECT_FIXED_MASK UINT32_C(0xffe00c00)
#define UNARY_FIXED_MASK UINT32_C(0xfffffc00)
#define PRECISION_FIXED_MASK UINT32_C(0xfffffc00)
#define IMMEDIATE_FIXED_MASK UINT32_C(0xffe01fe0)
#define COMPARE_REGISTER_FIXED_MASK UINT32_C(0xffe0fc1f)
#define COMPARE_ZERO_FIXED_MASK UINT32_C(0xfffffc1f)

struct binary_case {
    dword_t bits;
    enum aarch64_opcode opcode;
    byte_t width;
};

struct unary_case {
    dword_t bits;
    enum aarch64_opcode opcode;
    byte_t width;
};

struct select_case {
    dword_t bits;
    byte_t width;
};

struct precision_case {
    dword_t bits;
    byte_t source_width;
    byte_t destination_width;
};

struct immediate_case {
    dword_t bits;
    byte_t width;
};

struct compare_case {
    dword_t bits;
    dword_t mask;
    enum aarch64_opcode opcode;
    byte_t width;
    bool zero;
};

static const struct binary_case binary_operations[] = {
    {UINT32_C(0x1e202800), AARCH64_OP_FADD_SCALAR, 32},
    {UINT32_C(0x1e602800), AARCH64_OP_FADD_SCALAR, 64},
    {UINT32_C(0x1e203800), AARCH64_OP_FSUB_SCALAR, 32},
    {UINT32_C(0x1e603800), AARCH64_OP_FSUB_SCALAR, 64},
    {UINT32_C(0x1e200800), AARCH64_OP_FMUL_SCALAR, 32},
    {UINT32_C(0x1e600800), AARCH64_OP_FMUL_SCALAR, 64},
    {UINT32_C(0x1e201800), AARCH64_OP_FDIV_SCALAR, 32},
    {UINT32_C(0x1e601800), AARCH64_OP_FDIV_SCALAR, 64},
};

static const struct unary_case unary_operations[] = {
    {UINT32_C(0x1e204000), AARCH64_OP_FMOV_SCALAR, 32},
    {UINT32_C(0x1e604000), AARCH64_OP_FMOV_SCALAR, 64},
    {UINT32_C(0x5ea1b800), AARCH64_OP_FCVTZS_SCALAR, 32},
    {UINT32_C(0x5ee1b800), AARCH64_OP_FCVTZS_SCALAR, 64},
    {UINT32_C(0x5e21d800), AARCH64_OP_SCVTF_SCALAR, 32},
    {UINT32_C(0x5e61d800), AARCH64_OP_SCVTF_SCALAR, 64},
    {UINT32_C(0x7e21d800), AARCH64_OP_UCVTF_SCALAR, 32},
    {UINT32_C(0x7e61d800), AARCH64_OP_UCVTF_SCALAR, 64},
};

static const struct select_case select_operations[] = {
    {UINT32_C(0x1e200c00), 32},
    {UINT32_C(0x1e600c00), 64},
};

static const struct precision_case precision_conversions[] = {
    {UINT32_C(0x1e22c000), 32, 64},
    {UINT32_C(0x1e624000), 64, 32},
};

static const struct immediate_case immediate_operations[] = {
    {UINT32_C(0x1e201000), 32},
    {UINT32_C(0x1e601000), 64},
};

static const struct compare_case comparisons[] = {
    {UINT32_C(0x1e202000), COMPARE_REGISTER_FIXED_MASK,
            AARCH64_OP_FCMP_SCALAR, 32, false},
    {UINT32_C(0x1e602000), COMPARE_REGISTER_FIXED_MASK,
            AARCH64_OP_FCMP_SCALAR, 64, false},
    {UINT32_C(0x1e202010), COMPARE_REGISTER_FIXED_MASK,
            AARCH64_OP_FCMPE_SCALAR, 32, false},
    {UINT32_C(0x1e602010), COMPARE_REGISTER_FIXED_MASK,
            AARCH64_OP_FCMPE_SCALAR, 64, false},
    {UINT32_C(0x1e202008), COMPARE_ZERO_FIXED_MASK,
            AARCH64_OP_FCMP_SCALAR, 32, true},
    {UINT32_C(0x1e602008), COMPARE_ZERO_FIXED_MASK,
            AARCH64_OP_FCMP_SCALAR, 64, true},
    {UINT32_C(0x1e202018), COMPARE_ZERO_FIXED_MASK,
            AARCH64_OP_FCMPE_SCALAR, 32, true},
    {UINT32_C(0x1e602018), COMPARE_ZERO_FIXED_MASK,
            AARCH64_OP_FCMPE_SCALAR, 64, true},
};

static dword_t encode_binary(unsigned operation, byte_t rn, byte_t rm,
        byte_t rd) {
    return binary_operations[operation].bits | (dword_t) rm << 16 |
            (dword_t) rn << 5 | rd;
}

static dword_t encode_unary(unsigned operation, byte_t rn, byte_t rd) {
    return unary_operations[operation].bits | (dword_t) rn << 5 | rd;
}

static dword_t encode_select(unsigned operation, byte_t condition,
        byte_t rn, byte_t rm, byte_t rd) {
    return select_operations[operation].bits | (dword_t) rm << 16 |
            (dword_t) condition << 12 | (dword_t) rn << 5 | rd;
}

static dword_t encode_precision(unsigned operation, byte_t rn, byte_t rd) {
    return precision_conversions[operation].bits |
            (dword_t) rn << 5 | rd;
}

static dword_t encode_immediate(
        unsigned operation, byte_t immediate, byte_t rd) {
    return immediate_operations[operation].bits |
            (dword_t) immediate << 13 | rd;
}

static qword_t expanded_immediate(byte_t width, byte_t immediate) {
    if (width == 32) {
        return (qword_t) (immediate & UINT32_C(0x80)) << 24 |
                ((immediate & UINT32_C(0x40)) != 0 ?
                        UINT32_C(0x3e000000) : UINT32_C(0x40000000)) |
                (qword_t) (immediate & UINT32_C(0x3f)) << 19;
    }
    return (qword_t) (immediate & UINT32_C(0x80)) << 56 |
            ((immediate & UINT32_C(0x40)) != 0 ?
                    UINT64_C(0x3fc0000000000000) :
                    UINT64_C(0x4000000000000000)) |
            (qword_t) (immediate & UINT32_C(0x3f)) << 48;
}

static dword_t encode_compare(unsigned comparison, byte_t rn, byte_t rm) {
    return comparisons[comparison].bits | (dword_t) rn << 5 |
            (comparisons[comparison].zero ? 0 : (dword_t) rm << 16);
}

static bool is_scalar_fp_opcode(enum aarch64_opcode opcode) {
    switch (opcode) {
        case AARCH64_OP_FADD_SCALAR:
        case AARCH64_OP_FSUB_SCALAR:
        case AARCH64_OP_FMUL_SCALAR:
        case AARCH64_OP_FDIV_SCALAR:
        case AARCH64_OP_FCSEL_SCALAR:
        case AARCH64_OP_FMOV_SCALAR:
        case AARCH64_OP_FCVT_SCALAR:
        case AARCH64_OP_FMOV_IMMEDIATE:
        case AARCH64_OP_FCMP_SCALAR:
        case AARCH64_OP_FCMPE_SCALAR:
        case AARCH64_OP_FCVTZS_SCALAR:
        case AARCH64_OP_SCVTF_SCALAR:
        case AARCH64_OP_UCVTF_SCALAR:
            return true;
        default:
            return false;
    }
}

static bool is_scalar_fp_encoding(dword_t word) {
    for (unsigned i = 0; i < sizeof(binary_operations) /
            sizeof(binary_operations[0]); i++) {
        if ((word & BINARY_FIXED_MASK) == binary_operations[i].bits)
            return true;
    }
    for (unsigned i = 0; i < sizeof(unary_operations) /
            sizeof(unary_operations[0]); i++) {
        if ((word & UNARY_FIXED_MASK) == unary_operations[i].bits)
            return true;
    }
    for (unsigned i = 0; i < sizeof(select_operations) /
            sizeof(select_operations[0]); i++) {
        if ((word & SELECT_FIXED_MASK) == select_operations[i].bits)
            return true;
    }
    for (unsigned i = 0; i < sizeof(precision_conversions) /
            sizeof(precision_conversions[0]); i++) {
        if ((word & PRECISION_FIXED_MASK) ==
                precision_conversions[i].bits)
            return true;
    }
    for (unsigned i = 0; i < sizeof(immediate_operations) /
            sizeof(immediate_operations[0]); i++) {
        if ((word & IMMEDIATE_FIXED_MASK) ==
                immediate_operations[i].bits)
            return true;
    }
    for (unsigned i = 0; i < sizeof(comparisons) /
            sizeof(comparisons[0]); i++) {
        if ((word & comparisons[i].mask) == comparisons[i].bits)
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

static void assert_classification(dword_t word) {
    struct aarch64_decoded instruction;
    bool decoded = aarch64_decode(word, &instruction);
    assert((decoded && is_scalar_fp_opcode(instruction.opcode)) ==
            is_scalar_fp_encoding(word));
}

static void assert_binary(dword_t word, unsigned operation, byte_t rd,
        byte_t rn, byte_t rm) {
    struct aarch64_decoded instruction = decode(word);
    assert(instruction.opcode == binary_operations[operation].opcode);
    assert(instruction.width == binary_operations[operation].width);
    assert(instruction.operands.data_processing_2source.rd == rd);
    assert(instruction.operands.data_processing_2source.rn == rn);
    assert(instruction.operands.data_processing_2source.rm == rm);
}

static void assert_unary(dword_t word, unsigned operation, byte_t rd,
        byte_t rn) {
    struct aarch64_decoded instruction = decode(word);
    assert(instruction.opcode == unary_operations[operation].opcode);
    assert(instruction.width == unary_operations[operation].width);
    assert(instruction.operands.data_processing_1source.rd == rd);
    assert(instruction.operands.data_processing_1source.rn == rn);
}

static void assert_select(dword_t word, unsigned operation, byte_t rd,
        byte_t rn, byte_t rm, byte_t condition) {
    struct aarch64_decoded instruction = decode(word);
    assert(instruction.opcode == AARCH64_OP_FCSEL_SCALAR);
    assert(instruction.width == select_operations[operation].width);
    assert(instruction.operands.conditional_select.rd == rd);
    assert(instruction.operands.conditional_select.rn == rn);
    assert(instruction.operands.conditional_select.rm == rm);
    assert(instruction.operands.conditional_select.condition == condition);
}

static void assert_precision(dword_t word, unsigned operation,
        byte_t rd, byte_t rn) {
    struct aarch64_decoded instruction = decode(word);
    assert(instruction.opcode == AARCH64_OP_FCVT_SCALAR);
    assert(instruction.width ==
            precision_conversions[operation].source_width);
    assert(instruction.operands.scalar_fp_conversion.rd == rd);
    assert(instruction.operands.scalar_fp_conversion.rn == rn);
    assert(instruction.operands.scalar_fp_conversion.destination_width ==
            precision_conversions[operation].destination_width);
}

static void assert_immediate(dword_t word, unsigned operation,
        byte_t rd, byte_t immediate) {
    struct aarch64_decoded instruction = decode(word);
    assert(instruction.opcode == AARCH64_OP_FMOV_IMMEDIATE);
    assert(instruction.width == immediate_operations[operation].width);
    assert(instruction.operands.scalar_fp_immediate.rd == rd);
    assert(instruction.operands.scalar_fp_immediate.immediate ==
            expanded_immediate(
                    immediate_operations[operation].width, immediate));
}

static void assert_compare(dword_t word, unsigned comparison, byte_t rn,
        byte_t rm) {
    struct aarch64_decoded instruction = decode(word);
    assert(instruction.opcode == comparisons[comparison].opcode);
    assert(instruction.width == comparisons[comparison].width);
    assert(instruction.operands.scalar_fp_compare.rn == rn);
    assert(instruction.operands.scalar_fp_compare.rm ==
            (comparisons[comparison].zero ? 0 : rm));
    assert(instruction.operands.scalar_fp_compare.zero ==
            comparisons[comparison].zero);
}

static void test_apple_clang_vectors(void) {
    assert_binary(UINT32_C(0x1e2728a3), 0, 3, 5, 7);
    assert_binary(UINT32_C(0x1e6728a3), 1, 3, 5, 7);
    assert_binary(UINT32_C(0x1e2738a3), 2, 3, 5, 7);
    assert_binary(UINT32_C(0x1e6738a3), 3, 3, 5, 7);
    assert_binary(UINT32_C(0x1e2708a3), 4, 3, 5, 7);
    assert_binary(UINT32_C(0x1e6708a3), 5, 3, 5, 7);
    assert_binary(UINT32_C(0x1e2718a3), 6, 3, 5, 7);
    assert_binary(UINT32_C(0x1e6718a3), 7, 3, 5, 7);
    assert_binary(UINT32_C(0x1e3e181e), 6, 30, 0, 30);

    assert_select(UINT32_C(0x1e220c20), 0, 0, 1, 2, 0);
    assert_select(UINT32_C(0x1e251c83), 0, 3, 4, 5, 1);
    assert_select(UINT32_C(0x1e3fefff), 0, 31, 31, 31, 14);
    assert_select(UINT32_C(0x1e7f4c00), 1, 0, 0, 31, 4);
    assert_select(UINT32_C(0x1e65cc83), 1, 3, 4, 5, 12);
    assert_select(UINT32_C(0x1e7fffff), 1, 31, 31, 31, 15);

    assert_unary(UINT32_C(0x1e2040a3), 0, 3, 5);
    assert_unary(UINT32_C(0x1e6040a3), 1, 3, 5);
    assert_unary(UINT32_C(0x5ea1b8a3), 2, 3, 5);
    assert_unary(UINT32_C(0x5ee1b8a3), 3, 3, 5);
    assert_unary(UINT32_C(0x5e21d8a3), 4, 3, 5);
    assert_unary(UINT32_C(0x5e61d8a3), 5, 3, 5);
    assert_unary(UINT32_C(0x7e21d8a3), 6, 3, 5);
    assert_unary(UINT32_C(0x7e61d8a3), 7, 3, 5);
    assert_unary(UINT32_C(0x7e61dbff), 7, 31, 31);

    assert_precision(UINT32_C(0x1e22c0a3), 0, 3, 5);
    assert_precision(UINT32_C(0x1e6240a3), 1, 3, 5);

    assert_immediate(UINT32_C(0x1e2e1003), 0, 3, UINT8_C(0x70));
    assert_immediate(UINT32_C(0x1e6e1003), 1, 3, UINT8_C(0x70));
    assert_immediate(UINT32_C(0x1e381005), 0, 5, UINT8_C(0xc0));
    assert_immediate(UINT32_C(0x1e67f007), 1, 7, UINT8_C(0x3f));

    assert_compare(UINT32_C(0x1e2720a0), 0, 5, 7);
    assert_compare(UINT32_C(0x1e6720a0), 1, 5, 7);
    assert_compare(UINT32_C(0x1e2720b0), 2, 5, 7);
    assert_compare(UINT32_C(0x1e6720b0), 3, 5, 7);
    assert_compare(UINT32_C(0x1e2020a8), 4, 5, 0);
    assert_compare(UINT32_C(0x1e6020a8), 5, 5, 0);
    assert_compare(UINT32_C(0x1e2020b8), 6, 5, 0);
    assert_compare(UINT32_C(0x1e6020b8), 7, 5, 0);
}

static void test_binary_encoding_space(void) {
    unsigned decoded_count = 0;
    for (unsigned operation = 0; operation < sizeof(binary_operations) /
            sizeof(binary_operations[0]); operation++) {
        for (unsigned rn = 0; rn < 32; rn++) {
            for (unsigned rm = 0; rm < 32; rm++) {
                for (unsigned rd = 0; rd < 32; rd++) {
                    assert_binary(encode_binary(operation, (byte_t) rn,
                            (byte_t) rm, (byte_t) rd), operation,
                            (byte_t) rd, (byte_t) rn, (byte_t) rm);
                    decoded_count++;
                }
            }
        }
    }
    assert(decoded_count == 262144);
}

static void test_unary_encoding_space(void) {
    unsigned decoded_count = 0;
    for (unsigned operation = 0; operation < sizeof(unary_operations) /
            sizeof(unary_operations[0]); operation++) {
        for (unsigned rn = 0; rn < 32; rn++) {
            for (unsigned rd = 0; rd < 32; rd++) {
                assert_unary(encode_unary(operation, (byte_t) rn,
                        (byte_t) rd), operation, (byte_t) rd, (byte_t) rn);
                decoded_count++;
            }
        }
    }
    assert(decoded_count == 8192);
}

static void test_select_encoding_space(void) {
    unsigned decoded_count = 0;
    for (unsigned operation = 0; operation < sizeof(select_operations) /
            sizeof(select_operations[0]); operation++) {
        for (unsigned condition = 0; condition < 16; condition++) {
            for (unsigned rn = 0; rn < 32; rn++) {
                for (unsigned rm = 0; rm < 32; rm++) {
                    for (unsigned rd = 0; rd < 32; rd++) {
                        assert_select(encode_select(operation,
                                (byte_t) condition, (byte_t) rn,
                                (byte_t) rm, (byte_t) rd), operation,
                                (byte_t) rd, (byte_t) rn, (byte_t) rm,
                                (byte_t) condition);
                        decoded_count++;
                    }
                }
            }
        }
    }
    assert(decoded_count == 1048576);
}

static void test_rejected_select_precision_spaces(void) {
    static const dword_t bases[] = {
        UINT32_C(0x1ea00c00),
        UINT32_C(0x1ee00c00),
    };
    unsigned rejected_count = 0;
    for (unsigned precision = 0; precision <
            sizeof(bases) / sizeof(bases[0]); precision++) {
        for (unsigned condition = 0; condition < 16; condition++) {
            for (unsigned rn = 0; rn < 32; rn++) {
                for (unsigned rm = 0; rm < 32; rm++) {
                    for (unsigned rd = 0; rd < 32; rd++) {
                        dword_t word = bases[precision] |
                                (dword_t) rm << 16 |
                                (dword_t) condition << 12 |
                                (dword_t) rn << 5 | rd;
                        struct aarch64_decoded instruction;
                        assert(!aarch64_decode(word, &instruction));
                        rejected_count++;
                    }
                }
            }
        }
    }
    assert(rejected_count == 1048576);
}

static void test_precision_encoding_space(void) {
    unsigned decoded_count = 0;
    for (unsigned operation = 0; operation <
            sizeof(precision_conversions) /
                    sizeof(precision_conversions[0]); operation++) {
        for (unsigned rn = 0; rn < 32; rn++) {
            for (unsigned rd = 0; rd < 32; rd++) {
                assert_precision(encode_precision(operation,
                        (byte_t) rn, (byte_t) rd), operation,
                        (byte_t) rd, (byte_t) rn);
                decoded_count++;
            }
        }
    }
    assert(decoded_count == 2048);
}

static void test_immediate_encoding_space(void) {
    unsigned decoded_count = 0;
    for (unsigned operation = 0; operation <
            sizeof(immediate_operations) /
                    sizeof(immediate_operations[0]); operation++) {
        for (unsigned immediate = 0; immediate < 256; immediate++) {
            for (unsigned rd = 0; rd < 32; rd++) {
                assert_immediate(encode_immediate(operation,
                        (byte_t) immediate, (byte_t) rd), operation,
                        (byte_t) rd, (byte_t) immediate);
                decoded_count++;
            }
        }
    }
    assert(decoded_count == 16384);
}

static void test_compare_encoding_space(void) {
    unsigned register_count = 0;
    unsigned zero_count = 0;
    for (unsigned comparison = 0; comparison < sizeof(comparisons) /
            sizeof(comparisons[0]); comparison++) {
        for (unsigned rn = 0; rn < 32; rn++) {
            unsigned rm_count = comparisons[comparison].zero ? 1 : 32;
            for (unsigned rm = 0; rm < rm_count; rm++) {
                assert_compare(encode_compare(comparison, (byte_t) rn,
                        (byte_t) rm), comparison, (byte_t) rn, (byte_t) rm);
                if (comparisons[comparison].zero)
                    zero_count++;
                else
                    register_count++;
            }
        }
    }
    assert(register_count == 4096);
    assert(zero_count == 128);
}

static void test_fixed_bits(void) {
    for (unsigned operation = 0; operation < sizeof(binary_operations) /
            sizeof(binary_operations[0]); operation++) {
        dword_t base = encode_binary(operation, 5, 7, 3);
        for (unsigned bit = 0; bit < 32; bit++) {
            if (BINARY_FIXED_MASK & (UINT32_C(1) << bit))
                assert_classification(base ^ (UINT32_C(1) << bit));
        }
    }
    for (unsigned operation = 0; operation < sizeof(unary_operations) /
            sizeof(unary_operations[0]); operation++) {
        dword_t base = encode_unary(operation, 5, 3);
        for (unsigned bit = 0; bit < 32; bit++) {
            if (UNARY_FIXED_MASK & (UINT32_C(1) << bit))
                assert_classification(base ^ (UINT32_C(1) << bit));
        }
    }
    for (unsigned operation = 0; operation < sizeof(select_operations) /
            sizeof(select_operations[0]); operation++) {
        dword_t base = encode_select(operation, 4, 5, 7, 3);
        for (unsigned bit = 0; bit < 32; bit++) {
            if (SELECT_FIXED_MASK & (UINT32_C(1) << bit))
                assert_classification(base ^ (UINT32_C(1) << bit));
        }
    }
    for (unsigned operation = 0; operation <
            sizeof(precision_conversions) /
                    sizeof(precision_conversions[0]); operation++) {
        dword_t base = encode_precision(operation, 5, 3);
        for (unsigned bit = 0; bit < 32; bit++) {
            if (PRECISION_FIXED_MASK & (UINT32_C(1) << bit))
                assert_classification(base ^ (UINT32_C(1) << bit));
        }
    }
    for (unsigned operation = 0; operation <
            sizeof(immediate_operations) /
                    sizeof(immediate_operations[0]); operation++) {
        dword_t base = encode_immediate(operation, UINT8_C(0x70), 3);
        for (unsigned bit = 0; bit < 32; bit++) {
            if (IMMEDIATE_FIXED_MASK & (UINT32_C(1) << bit))
                assert_classification(base ^ (UINT32_C(1) << bit));
        }
    }
    for (unsigned comparison = 0; comparison < sizeof(comparisons) /
            sizeof(comparisons[0]); comparison++) {
        dword_t base = encode_compare(comparison, 5, 7);
        for (unsigned bit = 0; bit < 32; bit++) {
            if (comparisons[comparison].mask & (UINT32_C(1) << bit))
                assert_classification(base ^ (UINT32_C(1) << bit));
        }
    }
    assert(BINARY_FIXED_MASK == UINT32_C(0xffe0fc00));
    assert(SELECT_FIXED_MASK == UINT32_C(0xffe00c00));
    assert(UNARY_FIXED_MASK == UINT32_C(0xfffffc00));
    assert(PRECISION_FIXED_MASK == UINT32_C(0xfffffc00));
    assert(IMMEDIATE_FIXED_MASK == UINT32_C(0xffe01fe0));
    assert(COMPARE_REGISTER_FIXED_MASK == UINT32_C(0xffe0fc1f));
    assert(COMPARE_ZERO_FIXED_MASK == UINT32_C(0xfffffc1f));
}

static void test_rejected_neighbors(void) {
    struct aarch64_decoded instruction;
    // 当前 guest 不声明 FP16，保留精度和半精度 FDIV 必须进入 SIGILL。
    assert(!aarch64_decode(UINT32_C(0x1ebe181e), &instruction));
    assert(!aarch64_decode(UINT32_C(0x1efe181e), &instruction));

    // FP16、定点、向量和通用寄存器形式由不同编码族处理。
    static const dword_t words[] = {
        UINT32_C(0x1ee728a3),
        UINT32_C(0x1ee738a3),
        UINT32_C(0x1ee708a3),
        UINT32_C(0x1ee040a3),
        UINT32_C(0x1ee720a0),
        UINT32_C(0x1ee720b0),
        UINT32_C(0x1ee020a8),
        UINT32_C(0x1ee020b8),
        UINT32_C(0x1ea728a3),
        UINT32_C(0x1ea040a3),
        UINT32_C(0x1ea720a0),
        UINT32_C(0x1e2120a8),
        UINT32_C(0x1e6120b8),
        UINT32_C(0x1e1718a3),
        UINT32_C(0x1e2798a3),
        UINT32_C(0x1eee1003),
        UINT32_C(0x1eae1003),
        UINT32_C(0x1e6704a0),
        UINT32_C(0x1ebf4c00),
        UINT32_C(0x1eff4c00),
        UINT32_C(0x1e7f4400),
        UINT32_C(0x1e7f4800),
        UINT32_C(0x1e7f4000),
        UINT32_C(0x1e5f4c00),
        UINT32_C(0x5ef9b8a3),
        UINT32_C(0x5e79d8a3),
        UINT32_C(0x7ea1b8a3),
        UINT32_C(0x7ee1b8a3),
        UINT32_C(0x7e79d8a3),
        UINT32_C(0x7ea1d8a3),
        UINT32_C(0x7ee1d8a3),
        UINT32_C(0x7e61c8a3),
        UINT32_C(0x3e61d8a3),
        UINT32_C(0xfe61d8a3),
        UINT32_C(0x7e69d8a3),
        UINT32_C(0x5f3ffca3),
        UINT32_C(0x5f7ffca3),
        UINT32_C(0x5f3fe4a3),
        UINT32_C(0x5f7fe4a3),
        UINT32_C(0x7f3fe4a3),
        UINT32_C(0x7f7fe4a3),
        UINT32_C(0x0e27d4a3),
        UINT32_C(0x4e27d4a3),
        UINT32_C(0x4e67d4a3),
        UINT32_C(0x0ea7d4a3),
        UINT32_C(0x4ea7d4a3),
        UINT32_C(0x4ee7d4a3),
        UINT32_C(0x2e27dca3),
        UINT32_C(0x6e27dca3),
        UINT32_C(0x6e67dca3),
        UINT32_C(0x0ea1b8a3),
        UINT32_C(0x4ea1b8a3),
        UINT32_C(0x4ee1b8a3),
        UINT32_C(0x0e21d8a3),
        UINT32_C(0x4e21d8a3),
        UINT32_C(0x4e61d8a3),
        UINT32_C(0x2e21d8a3),
        UINT32_C(0x6e21d8a3),
        UINT32_C(0x6e61d8a3),
        UINT32_C(0x1e3800a3),
        UINT32_C(0x1e7800a3),
        UINT32_C(0x9e3800a3),
        UINT32_C(0x9e7800a3),
        UINT32_C(0x1e2200a3),
        UINT32_C(0x1e6200a3),
        UINT32_C(0x9e2200a3),
        UINT32_C(0x9e6200a3),
        UINT32_C(0x1e2300a3),
        UINT32_C(0x1e6300a3),
        UINT32_C(0x9e2300a3),
        UINT32_C(0x9e6300a3),
        UINT32_C(0x9e3d00a3),
        UINT32_C(0x1e7d00a3),
        // 当前 guest 不声明可选 FP16，相关精度转换必须保持未定义。
        UINT32_C(0x1e23c000),
        UINT32_C(0x1e63c000),
        UINT32_C(0x1ee24000),
        UINT32_C(0x1ee2c000),
    };
    for (unsigned i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
        bool decoded = aarch64_decode(words[i], &instruction);
        assert(!decoded || !is_scalar_fp_opcode(instruction.opcode));
        assert(!is_scalar_fp_encoding(words[i]));
    }
}

int main(void) {
    test_apple_clang_vectors();
    test_binary_encoding_space();
    test_unary_encoding_space();
    test_select_encoding_space();
    test_rejected_select_precision_spaces();
    test_precision_encoding_space();
    test_immediate_encoding_space();
    test_compare_encoding_space();
    test_fixed_bits();
    test_rejected_neighbors();
    return 0;
}
