#include <assert.h>
#include <string.h>

#include "guest/aarch64/decode.h"
#include "guest/aarch64/execute.h"

#define ADVSIMD_PAIRWISE_FIXED_MASK UINT32_C(0x9f20f400)
#define ADVSIMD_PAIRWISE_FIXED_BITS UINT32_C(0x0e20a400)
#define ADVSIMD_PAIRWISE_VARIABLE_MASK UINT32_C(0x60df0bff)

static dword_t encode(bool q, bool u, byte_t size, bool minimum,
        byte_t rm, byte_t rn, byte_t rd) {
    return ADVSIMD_PAIRWISE_FIXED_BITS |
            (dword_t) q << 30 |
            (dword_t) u << 29 |
            (dword_t) size << 22 |
            (dword_t) minimum << 11 |
            (dword_t) rm << 16 |
            (dword_t) rn << 5 |
            rd;
}

static enum aarch64_opcode opcode_for(bool u, bool minimum) {
    static const enum aarch64_opcode opcodes[2][2] = {
        {AARCH64_OP_ADVSIMD_SMAXP, AARCH64_OP_ADVSIMD_UMAXP},
        {AARCH64_OP_ADVSIMD_SMINP, AARCH64_OP_ADVSIMD_UMINP},
    };
    return opcodes[minimum][u];
}

static bool is_pairwise_opcode(enum aarch64_opcode opcode) {
    return opcode >= AARCH64_OP_ADVSIMD_SMAXP &&
            opcode <= AARCH64_OP_ADVSIMD_UMINP;
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
    assert_decode(UINT32_C(0x4e22a420), AARCH64_OP_ADVSIMD_SMAXP,
            128, 1, 0, 1, 2);
    assert_decode(UINT32_C(0x4e65ac83), AARCH64_OP_ADVSIMD_SMINP,
            128, 2, 3, 4, 5);
    assert_decode(UINT32_C(0x6ea8a4e6), AARCH64_OP_ADVSIMD_UMAXP,
            128, 4, 6, 7, 8);
    assert_decode(UINT32_C(0x6e2bad49), AARCH64_OP_ADVSIMD_UMINP,
            128, 1, 9, 10, 11);
    assert_decode(UINT32_C(0x6ebfa7ff), AARCH64_OP_ADVSIMD_UMAXP,
            128, 4, 31, 31, 31);
    assert_decode(UINT32_C(0x0e2ea5ac), AARCH64_OP_ADVSIMD_SMAXP,
            64, 1, 12, 13, 14);
    assert_decode(UINT32_C(0x0e71ae0f), AARCH64_OP_ADVSIMD_SMINP,
            64, 2, 15, 16, 17);
    assert_decode(UINT32_C(0x2eb4a672), AARCH64_OP_ADVSIMD_UMAXP,
            64, 4, 18, 19, 20);
}

static void test_encoding_space(void) {
    assert((ADVSIMD_PAIRWISE_FIXED_MASK &
            ADVSIMD_PAIRWISE_VARIABLE_MASK) == 0);
    assert((ADVSIMD_PAIRWISE_FIXED_MASK |
            ADVSIMD_PAIRWISE_VARIABLE_MASK) == UINT32_MAX);
    unsigned decoded_count = 0;
    for (unsigned q = 0; q < 2; q++) {
        for (unsigned u = 0; u < 2; u++) {
            for (unsigned size = 0; size < 4; size++) {
                for (unsigned minimum = 0; minimum < 2; minimum++) {
                    for (unsigned rm = 0; rm < 32; rm++) {
                        for (unsigned rn = 0; rn < 32; rn++) {
                            for (unsigned rd = 0; rd < 32; rd++) {
                                struct aarch64_decoded instruction;
                                bool decoded = aarch64_decode(encode(q, u,
                                        (byte_t) size, minimum,
                                        (byte_t) rm, (byte_t) rn,
                                        (byte_t) rd), &instruction);
                                bool expected = size != 3;
                                assert(decoded == expected);
                                if (!decoded)
                                    continue;
                                decoded_count++;
                                assert(instruction.opcode == opcode_for(
                                        u, minimum));
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
    assert(decoded_count == 786432);
}

static void test_fixed_bits(void) {
    dword_t base = encode(true, true, 2, false, 31, 31, 31);
    for (unsigned bit = 0; bit < 32; bit++) {
        if ((ADVSIMD_PAIRWISE_FIXED_MASK & (UINT32_C(1) << bit)) == 0)
            continue;
        struct aarch64_decoded instruction;
        bool decoded = aarch64_decode(base ^ (UINT32_C(1) << bit),
                &instruction);
        assert(!decoded || !is_pairwise_opcode(instruction.opcode));
    }
}

static qword_t read_lane(const union aarch64_vector_reg *reg,
        byte_t element_size, byte_t index) {
    if (element_size == 1)
        return reg->b[index];
    if (element_size == 2)
        return reg->h[index];
    return reg->s[index];
}

static void write_lane(union aarch64_vector_reg *reg,
        byte_t element_size, byte_t index, qword_t value) {
    if (element_size == 1)
        reg->b[index] = (byte_t) value;
    else if (element_size == 2)
        reg->h[index] = (word_t) value;
    else
        reg->s[index] = (dword_t) value;
}

static union aarch64_vector_reg reference_pairwise(
        const struct cpu_state *before, bool q, byte_t size,
        bool u, bool minimum, byte_t rm, byte_t rn) {
    byte_t element_size = (byte_t) (1U << size);
    byte_t lanes = (byte_t) ((q ? 128 : 64) / (element_size * 8));
    byte_t half = lanes / 2;
    qword_t sign = UINT64_C(1) << (element_size * 8 - 1);
    union aarch64_vector_reg result = {0};
    for (byte_t lane = 0; lane < lanes; lane++) {
        byte_t source = lane < half ? rn : rm;
        byte_t pair = (byte_t) (2 * (lane % half));
        qword_t left = read_lane(&before->v[source], element_size, pair);
        qword_t right = read_lane(
                &before->v[source], element_size, (byte_t) (pair + 1));
        qword_t ordered_left = u ? left : left ^ sign;
        qword_t ordered_right = u ? right : right ^ sign;
        bool select_left = minimum ? ordered_left <= ordered_right :
                ordered_left >= ordered_right;
        qword_t value = select_left ? left : right;
        write_lane(&result, element_size, lane, value);
    }
    return result;
}

static void test_known_answers(void) {
    static const dword_t left[] = {
        0, UINT32_MAX, UINT32_C(0x80000000), 5,
    };
    static const dword_t right[] = {
        UINT32_C(0x7fffffff), 3, 7, UINT32_C(0xfffffff8),
    };
    static const dword_t expected[2][2][4] = {
        {
            {0, 5, UINT32_C(0x7fffffff), 7},
            {UINT32_MAX, UINT32_C(0x80000000),
                    UINT32_C(0x7fffffff), UINT32_C(0xfffffff8)},
        },
        {
            {UINT32_MAX, UINT32_C(0x80000000),
                    3, UINT32_C(0xfffffff8)},
            {0, 5, 3, 7},
        },
    };
    for (unsigned minimum = 0; minimum < 2; minimum++) {
        for (unsigned u = 0; u < 2; u++) {
            struct cpu_state cpu = {.pc = UINT64_C(0x2000)};
            memcpy(cpu.v[1].s, left, sizeof(left));
            memcpy(cpu.v[2].s, right, sizeof(right));
            execute_instruction(&cpu, encode(true, u, 2, minimum, 2, 1, 0));
            assert(memcmp(cpu.v[0].s, expected[minimum][u],
                    sizeof(expected[0][0])) == 0);
            assert(cpu.pc == UINT64_C(0x2004));
        }
    }
}

static void fill_registers(struct cpu_state *cpu) {
    for (unsigned reg = 0; reg < 32; reg++) {
        for (unsigned byte = 0; byte < 16; byte++)
            cpu->v[reg].b[byte] = (byte_t) (reg * 29 + byte * 5);
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
    };
    for (unsigned q = 0; q < 2; q++) {
        for (unsigned u = 0; u < 2; u++) {
            for (unsigned size = 0; size < 3; size++) {
                for (unsigned minimum = 0; minimum < 2; minimum++) {
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
                        union aarch64_vector_reg expected = reference_pairwise(
                                &before, q, (byte_t) size, u, minimum, rm, rn);
                        dword_t word = encode(q, u, (byte_t) size,
                                minimum, rm, rn, rd);
                        if (q && u && size == 2 && !minimum &&
                                rd == 31 && rn == 31 && rm == 31)
                            assert(word == UINT32_C(0x6ebfa7ff));
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
    test_fixed_bits();
    test_known_answers();
    test_execution_space();
    return 0;
}
