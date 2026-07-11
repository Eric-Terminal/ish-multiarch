#include <assert.h>
#include <string.h>

#include "guest/aarch64/decode.h"
#include "guest/aarch64/execute.h"

#define ADVSIMD_COMPARE_FIXED_MASK UINT32_C(0x9f200000)
#define ADVSIMD_COMPARE_FIXED_BITS UINT32_C(0x0e200000)
#define ADVSIMD_COMPARE_VARIABLE_MASK UINT32_C(0x60dfffff)

static const dword_t family_bits[] = {
    UINT32_C(0x3400),
    UINT32_C(0x3c00),
    UINT32_C(0x8c00),
};

static dword_t encode(bool q, bool u, byte_t size,
        byte_t family, byte_t rm, byte_t rn, byte_t rd) {
    return ADVSIMD_COMPARE_FIXED_BITS |
            (dword_t) q << 30 |
            (dword_t) u << 29 |
            (dword_t) size << 22 |
            (dword_t) rm << 16 |
            family_bits[family] |
            (dword_t) rn << 5 |
            rd;
}

static enum aarch64_opcode opcode_for(byte_t family,
        bool u) {
    static const enum aarch64_opcode opcodes[3][2] = {
        {AARCH64_OP_ADVSIMD_CMGT, AARCH64_OP_ADVSIMD_CMHI},
        {AARCH64_OP_ADVSIMD_CMGE, AARCH64_OP_ADVSIMD_CMHS},
        {AARCH64_OP_ADVSIMD_CMTST, AARCH64_OP_ADVSIMD_CMEQ},
    };
    return opcodes[family][u];
}

static bool is_compare_opcode(enum aarch64_opcode opcode) {
    return opcode >= AARCH64_OP_ADVSIMD_CMGT &&
            opcode <= AARCH64_OP_ADVSIMD_CMEQ;
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

static void assert_decode(dword_t word, enum aarch64_opcode opcode,
        byte_t width, byte_t element_size, byte_t rd, byte_t rn, byte_t rm) {
    struct aarch64_decoded instruction = decode(word);
    assert(instruction.opcode == opcode);
    assert(instruction.width == width);
    assert(instruction.operands.advsimd_three_same.rd == rd);
    assert(instruction.operands.advsimd_three_same.rn == rn);
    assert(instruction.operands.advsimd_three_same.rm == rm);
    assert(instruction.operands.advsimd_three_same.element_size ==
            element_size);
}

static void test_llvm_vectors(void) {
    assert_decode(UINT32_C(0x6e228c20), AARCH64_OP_ADVSIMD_CMEQ,
            128, 1, 0, 1, 2);
    assert_decode(UINT32_C(0x4e658c83), AARCH64_OP_ADVSIMD_CMTST,
            128, 2, 3, 4, 5);
    assert_decode(UINT32_C(0x4ea834e6), AARCH64_OP_ADVSIMD_CMGT,
            128, 4, 6, 7, 8);
    assert_decode(UINT32_C(0x4eeb3d49), AARCH64_OP_ADVSIMD_CMGE,
            128, 8, 9, 10, 11);
    assert_decode(UINT32_C(0x6e2e35ac), AARCH64_OP_ADVSIMD_CMHI,
            128, 1, 12, 13, 14);
    assert_decode(UINT32_C(0x6e713e0f), AARCH64_OP_ADVSIMD_CMHS,
            128, 2, 15, 16, 17);
    assert_decode(UINT32_C(0x0e343672), AARCH64_OP_ADVSIMD_CMGT,
            64, 1, 18, 19, 20);
    assert_decode(UINT32_C(0x2e7736d5), AARCH64_OP_ADVSIMD_CMHI,
            64, 2, 21, 22, 23);
    assert_decode(UINT32_C(0x0eba3f38), AARCH64_OP_ADVSIMD_CMGE,
            64, 4, 24, 25, 26);
    assert_decode(UINT32_C(0x6eb98fde), AARCH64_OP_ADVSIMD_CMEQ,
            128, 4, 30, 30, 25);
}

static void test_encoding_space(void) {
    assert((ADVSIMD_COMPARE_FIXED_MASK &
            ADVSIMD_COMPARE_VARIABLE_MASK) == 0);
    assert((ADVSIMD_COMPARE_FIXED_MASK |
            ADVSIMD_COMPARE_VARIABLE_MASK) == UINT32_MAX);
    unsigned decoded_count = 0;
    for (unsigned q = 0; q < 2; q++) {
        for (unsigned u = 0; u < 2; u++) {
            for (unsigned size = 0; size < 4; size++) {
                for (unsigned family = 0; family < 3; family++) {
                    for (unsigned rm = 0; rm < 32; rm++) {
                        for (unsigned rn = 0; rn < 32; rn++) {
                            for (unsigned rd = 0; rd < 32; rd++) {
                                struct aarch64_decoded instruction;
                                bool decoded = aarch64_decode(encode(q,
                                        u, (byte_t) size,
                                        (byte_t) family, (byte_t) rm,
                                        (byte_t) rn, (byte_t) rd),
                                        &instruction);
                                bool expected = q != 0 || size != 3;
                                assert(decoded == expected);
                                if (!decoded)
                                    continue;
                                decoded_count++;
                                assert(instruction.opcode == opcode_for(
                                        (byte_t) family,
                                        u));
                                assert(instruction.width == (q ? 128 : 64));
                                assert(instruction.operands.
                                        advsimd_three_same.rd == rd);
                                assert(instruction.operands.
                                        advsimd_three_same.rn == rn);
                                assert(instruction.operands.
                                        advsimd_three_same.rm == rm);
                                assert(instruction.operands.advsimd_three_same.
                                        element_size == (1U << size));
                            }
                        }
                    }
                }
            }
        }
    }
    assert(decoded_count == 1376256);
}

static void test_fixed_and_opcode_bits(void) {
    dword_t base = encode(true, true, 2, 2, 25, 30, 30);
    for (unsigned bit = 0; bit < 32; bit++) {
        if ((ADVSIMD_COMPARE_FIXED_MASK & (UINT32_C(1) << bit)) == 0)
            continue;
        struct aarch64_decoded instruction;
        bool decoded = aarch64_decode(base ^ (UINT32_C(1) << bit),
                &instruction);
        assert(!decoded || !is_compare_opcode(instruction.opcode));
    }
    for (unsigned opcode = 0; opcode < 64; opcode++) {
        dword_t word = ADVSIMD_COMPARE_FIXED_BITS |
                (dword_t) opcode << 10;
        struct aarch64_decoded instruction;
        bool decoded = aarch64_decode(word, &instruction);
        bool expected = opcode == 13 || opcode == 15 || opcode == 35;
        assert(!decoded || is_compare_opcode(instruction.opcode) == expected);
    }
}

static qword_t read_lane(const union aarch64_vector_reg *reg,
        byte_t element_size, byte_t index) {
    if (element_size == 1)
        return reg->b[index];
    if (element_size == 2)
        return reg->h[index];
    if (element_size == 4)
        return reg->s[index];
    return reg->d[index];
}

static void write_lane(union aarch64_vector_reg *reg,
        byte_t element_size, byte_t index, qword_t value) {
    if (element_size == 1)
        reg->b[index] = (byte_t) value;
    else if (element_size == 2)
        reg->h[index] = (word_t) value;
    else if (element_size == 4)
        reg->s[index] = (dword_t) value;
    else
        reg->d[index] = value;
}

static union aarch64_vector_reg reference_compare(
        const struct cpu_state *before, bool q, byte_t size,
        enum aarch64_opcode opcode, byte_t rm, byte_t rn) {
    byte_t element_size = (byte_t) (1U << size);
    byte_t lanes = (byte_t) ((q ? 128 : 64) / (element_size * 8));
    qword_t mask = element_size == 8 ? UINT64_MAX :
            (UINT64_C(1) << (element_size * 8)) - 1;
    qword_t sign = UINT64_C(1) << (element_size * 8 - 1);
    union aarch64_vector_reg result = {0};
    for (byte_t lane = 0; lane < lanes; lane++) {
        qword_t left = read_lane(&before->v[rn], element_size, lane);
        qword_t right = read_lane(&before->v[rm], element_size, lane);
        bool matches;
        if (opcode == AARCH64_OP_ADVSIMD_CMTST)
            matches = (left & right) != 0;
        else if (opcode == AARCH64_OP_ADVSIMD_CMEQ)
            matches = left == right;
        else if (opcode == AARCH64_OP_ADVSIMD_CMHI)
            matches = left > right;
        else if (opcode == AARCH64_OP_ADVSIMD_CMHS)
            matches = left >= right;
        else if (opcode == AARCH64_OP_ADVSIMD_CMGT)
            matches = (left ^ sign) > (right ^ sign);
        else
            matches = (left ^ sign) >= (right ^ sign);
        write_lane(&result, element_size, lane, matches ? mask : 0);
    }
    return result;
}

static void test_known_answers(void) {
    static const dword_t left[] = {
        0, UINT32_MAX, UINT32_C(0x80000000), 5,
    };
    static const dword_t right[] = {
        0, 1, UINT32_C(0x7fffffff), 3,
    };
    static const dword_t expected[3][2][4] = {
        {
            {0, 0, 0, UINT32_MAX},
            {0, UINT32_MAX, UINT32_MAX, UINT32_MAX},
        },
        {
            {UINT32_MAX, 0, 0, UINT32_MAX},
            {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX},
        },
        {
            {0, UINT32_MAX, 0, UINT32_MAX},
            {UINT32_MAX, 0, 0, 0},
        },
    };
    for (unsigned family = 0; family < 3; family++) {
        for (unsigned u = 0; u < 2; u++) {
            struct cpu_state cpu = {.pc = UINT64_C(0x2000)};
            memcpy(cpu.v[1].s, left, sizeof(left));
            memcpy(cpu.v[2].s, right, sizeof(right));
            execute_instruction(&cpu, encode(true, u,
                    2, (byte_t) family, 2, 1, 0));
            assert(memcmp(cpu.v[0].s,
                    expected[family][u],
                    sizeof(expected[0][0])) == 0);
            assert(cpu.pc == UINT64_C(0x2004));
        }
    }
}

static void fill_registers(struct cpu_state *cpu) {
    for (unsigned reg = 0; reg < 32; reg++) {
        for (unsigned byte = 0; byte < 16; byte++)
            cpu->v[reg].b[byte] = (byte_t) (reg * 23 + byte * 7);
    }
}

static void test_execution_space(void) {
    static const struct {
        byte_t rd;
        byte_t rn;
        byte_t rm;
    } registers[] = {
        {0, 1, 2},
        {1, 1, 2},
        {2, 1, 2},
        {0, 1, 1},
        {31, 31, 31},
        {30, 30, 25},
    };
    for (unsigned q = 0; q < 2; q++) {
        for (unsigned size = 0; size < 4; size++) {
            if (q == 0 && size == 3)
                continue;
            for (unsigned family = 0; family < 3; family++) {
                for (unsigned u = 0; u < 2; u++) {
                    enum aarch64_opcode opcode = opcode_for(
                            (byte_t) family, u);
                    for (unsigned form = 0; form <
                            sizeof(registers) / sizeof(registers[0]); form++) {
                        const byte_t rd = registers[form].rd;
                        const byte_t rn = registers[form].rn;
                        const byte_t rm = registers[form].rm;
                        struct cpu_state cpu = {
                            .pc = UINT64_C(0x1000),
                            .sp = UINT64_C(0x1122334455667788),
                            .nzcv = UINT32_C(0xa0000000),
                            .fpcr = UINT32_C(0x01000000),
                            .fpsr = UINT32_C(0x08000000),
                        };
                        fill_registers(&cpu);
                        struct cpu_state before = cpu;
                        union aarch64_vector_reg expected = reference_compare(
                                &before, q, (byte_t) size, opcode, rm, rn);
                        dword_t word = encode(q, u,
                                (byte_t) size, (byte_t) family, rm, rn, rd);
                        if (q && size == 2 && family == 2 &&
                                u && rd == 30 &&
                                rn == 30 && rm == 25)
                            assert(word == UINT32_C(0x6eb98fde));
                        execute_instruction(&cpu, word);
                        assert(memcmp(&cpu.v[rd], &expected,
                                sizeof(expected)) == 0);
                        for (unsigned reg = 0; reg < 32; reg++) {
                            if (reg != rd)
                                assert(memcmp(&cpu.v[reg], &before.v[reg],
                                        sizeof(cpu.v[reg])) == 0);
                        }
                        assert(cpu.pc == UINT64_C(0x1004));
                        assert(cpu.sp == UINT64_C(0x1122334455667788));
                        assert(cpu.nzcv == UINT32_C(0xa0000000));
                        assert(cpu.fpcr == UINT32_C(0x01000000));
                        assert(cpu.fpsr == UINT32_C(0x08000000));
                    }
                }
            }
        }
    }
}

int main(void) {
    test_llvm_vectors();
    test_encoding_space();
    test_fixed_and_opcode_bits();
    test_known_answers();
    test_execution_space();
    return 0;
}
