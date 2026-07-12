#include <assert.h>

#include "guest/aarch64/decode.h"

#define BINARY_FIXED_MASK UINT32_C(0xffe0fc00)
#define UNARY_FIXED_MASK UINT32_C(0xfffffc00)
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
};

static const struct unary_case unary_operations[] = {
    {UINT32_C(0x1e204000), AARCH64_OP_FMOV_SCALAR, 32},
    {UINT32_C(0x1e604000), AARCH64_OP_FMOV_SCALAR, 64},
    {UINT32_C(0x5ea1b800), AARCH64_OP_FCVTZS_SCALAR, 32},
    {UINT32_C(0x5ee1b800), AARCH64_OP_FCVTZS_SCALAR, 64},
    {UINT32_C(0x5e21d800), AARCH64_OP_SCVTF_SCALAR, 32},
    {UINT32_C(0x5e61d800), AARCH64_OP_SCVTF_SCALAR, 64},
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

static dword_t encode_compare(unsigned comparison, byte_t rn, byte_t rm) {
    return comparisons[comparison].bits | (dword_t) rn << 5 |
            (comparisons[comparison].zero ? 0 : (dword_t) rm << 16);
}

static bool is_scalar_fp_opcode(enum aarch64_opcode opcode) {
    switch (opcode) {
        case AARCH64_OP_FADD_SCALAR:
        case AARCH64_OP_FSUB_SCALAR:
        case AARCH64_OP_FMUL_SCALAR:
        case AARCH64_OP_FMOV_SCALAR:
        case AARCH64_OP_FCMP_SCALAR:
        case AARCH64_OP_FCMPE_SCALAR:
        case AARCH64_OP_FCVTZS_SCALAR:
        case AARCH64_OP_SCVTF_SCALAR:
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

    assert_unary(UINT32_C(0x1e2040a3), 0, 3, 5);
    assert_unary(UINT32_C(0x1e6040a3), 1, 3, 5);
    assert_unary(UINT32_C(0x5ea1b8a3), 2, 3, 5);
    assert_unary(UINT32_C(0x5ee1b8a3), 3, 3, 5);
    assert_unary(UINT32_C(0x5e21d8a3), 4, 3, 5);
    assert_unary(UINT32_C(0x5e61d8a3), 5, 3, 5);

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
    assert(decoded_count == 196608);
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
    assert(decoded_count == 6144);
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
    for (unsigned comparison = 0; comparison < sizeof(comparisons) /
            sizeof(comparisons[0]); comparison++) {
        dword_t base = encode_compare(comparison, 5, 7);
        for (unsigned bit = 0; bit < 32; bit++) {
            if (comparisons[comparison].mask & (UINT32_C(1) << bit))
                assert_classification(base ^ (UINT32_C(1) << bit));
        }
    }
    assert(BINARY_FIXED_MASK == UINT32_C(0xffe0fc00));
    assert(UNARY_FIXED_MASK == UINT32_C(0xfffffc00));
    assert(COMPARE_REGISTER_FIXED_MASK == UINT32_C(0xffe0fc1f));
    assert(COMPARE_ZERO_FIXED_MASK == UINT32_C(0xfffffc1f));
}

static void test_rejected_neighbors(void) {
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
        UINT32_C(0x1e6718a3),
        UINT32_C(0x1e6e1003),
        UINT32_C(0x1e670ca3),
        UINT32_C(0x1e6704a0),
        UINT32_C(0x5ef9b8a3),
        UINT32_C(0x5e79d8a3),
        UINT32_C(0x7ea1b8a3),
        UINT32_C(0x7ee1b8a3),
        UINT32_C(0x7e21d8a3),
        UINT32_C(0x7e61d8a3),
        UINT32_C(0x5f3ffca3),
        UINT32_C(0x5f7ffca3),
        UINT32_C(0x5f3fe4a3),
        UINT32_C(0x5f7fe4a3),
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
        UINT32_C(0x1e3800a3),
        UINT32_C(0x1e7800a3),
        UINT32_C(0x9e3800a3),
        UINT32_C(0x9e7800a3),
        UINT32_C(0x1e2200a3),
        UINT32_C(0x1e6200a3),
        UINT32_C(0x9e2200a3),
        UINT32_C(0x9e6200a3),
    };
    for (unsigned i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
        struct aarch64_decoded instruction;
        bool decoded = aarch64_decode(words[i], &instruction);
        assert(!decoded || !is_scalar_fp_opcode(instruction.opcode));
        assert(!is_scalar_fp_encoding(words[i]));
    }
}

int main(void) {
    test_apple_clang_vectors();
    test_binary_encoding_space();
    test_unary_encoding_space();
    test_compare_encoding_space();
    test_fixed_bits();
    test_rejected_neighbors();
    return 0;
}
