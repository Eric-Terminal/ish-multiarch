#include <assert.h>
#include <string.h>

#include "guest/aarch64/decode.h"
#include "guest/aarch64/execute.h"

#define ADVSIMD_LOGICAL_FIXED_MASK UINT32_C(0x9f20fc00)
#define ADVSIMD_LOGICAL_FIXED_BITS UINT32_C(0x0e201c00)
#define ADVSIMD_LOGICAL_VARIABLE_MASK UINT32_C(0x60df03ff)
#define ADVSIMD_NOT_FIXED_MASK UINT32_C(0xbffffc00)
#define ADVSIMD_NOT_FIXED_BITS UINT32_C(0x2e205800)
#define ADVSIMD_NOT_VARIABLE_MASK UINT32_C(0x400003ff)
#define ADVSIMD_TWO_REGISTER_MISC_BITS UINT32_C(0x0e205800)

static dword_t encode(bool q, byte_t operation,
        byte_t rm, byte_t rn, byte_t rd) {
    return ADVSIMD_LOGICAL_FIXED_BITS |
            (dword_t) q << 30 |
            (dword_t) (operation >> 2) << 29 |
            (dword_t) (operation & 3) << 22 |
            (dword_t) rm << 16 |
            (dword_t) rn << 5 |
            rd;
}

static dword_t encode_not(bool q, byte_t rn, byte_t rd) {
    return ADVSIMD_NOT_FIXED_BITS |
            (dword_t) q << 30 |
            (dword_t) rn << 5 |
            rd;
}

static enum aarch64_opcode opcode_for(byte_t operation) {
    static const enum aarch64_opcode opcodes[] = {
        AARCH64_OP_ADVSIMD_AND,
        AARCH64_OP_ADVSIMD_BIC,
        AARCH64_OP_ADVSIMD_ORR,
        AARCH64_OP_ADVSIMD_ORN,
        AARCH64_OP_ADVSIMD_EOR,
        AARCH64_OP_ADVSIMD_BSL,
        AARCH64_OP_ADVSIMD_BIT,
        AARCH64_OP_ADVSIMD_BIF,
    };
    return opcodes[operation];
}

static bool is_logical_opcode(enum aarch64_opcode opcode) {
    return opcode >= AARCH64_OP_ADVSIMD_AND &&
            opcode <= AARCH64_OP_ADVSIMD_BIF;
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
        byte_t width, byte_t rd, byte_t rn, byte_t rm) {
    struct aarch64_decoded instruction = decode(word);
    assert(instruction.opcode == opcode);
    assert(instruction.width == width);
    assert(instruction.operands.advsimd_three_same.rd == rd);
    assert(instruction.operands.advsimd_three_same.rn == rn);
    assert(instruction.operands.advsimd_three_same.rm == rm);
    assert(instruction.operands.advsimd_three_same.element_size == 1);
}

static void assert_not_decode(dword_t word, byte_t width,
        byte_t rd, byte_t rn) {
    struct aarch64_decoded instruction = decode(word);
    assert(instruction.opcode == AARCH64_OP_ADVSIMD_NOT);
    assert(instruction.width == width);
    assert(instruction.operands.advsimd_unary.rd == rd);
    assert(instruction.operands.advsimd_unary.rn == rn);
}

static void test_llvm_vectors(void) {
    assert_decode(UINT32_C(0x4e221c20), AARCH64_OP_ADVSIMD_AND,
            128, 0, 1, 2);
    assert_decode(UINT32_C(0x4e651c83), AARCH64_OP_ADVSIMD_BIC,
            128, 3, 4, 5);
    assert_decode(UINT32_C(0x4ea81ce6), AARCH64_OP_ADVSIMD_ORR,
            128, 6, 7, 8);
    assert_decode(UINT32_C(0x4eeb1d49), AARCH64_OP_ADVSIMD_ORN,
            128, 9, 10, 11);
    assert_decode(UINT32_C(0x6e2e1dac), AARCH64_OP_ADVSIMD_EOR,
            128, 12, 13, 14);
    assert_decode(UINT32_C(0x6e711e0f), AARCH64_OP_ADVSIMD_BSL,
            128, 15, 16, 17);
    assert_decode(UINT32_C(0x6eb41e72), AARCH64_OP_ADVSIMD_BIT,
            128, 18, 19, 20);
    assert_decode(UINT32_C(0x6ef71ed5), AARCH64_OP_ADVSIMD_BIF,
            128, 21, 22, 23);
    assert_decode(UINT32_C(0x4ebd1fde), AARCH64_OP_ADVSIMD_ORR,
            128, 30, 30, 29);
    assert_decode(UINT32_C(0x0e3a1f38), AARCH64_OP_ADVSIMD_AND,
            64, 24, 25, 26);
    assert_decode(UINT32_C(0x2efd1f9b), AARCH64_OP_ADVSIMD_BIF,
            64, 27, 28, 29);
    assert_not_decode(UINT32_C(0x2e205820), 64, 0, 1);
    assert_not_decode(UINT32_C(0x6e205862), 128, 2, 3);
    assert_not_decode(UINT32_C(0x2e205bff), 64, 31, 31);
    assert_not_decode(UINT32_C(0x6e205bdf), 128, 31, 30);
}

static void test_encoding_space(void) {
    assert((ADVSIMD_LOGICAL_FIXED_MASK &
            ADVSIMD_LOGICAL_VARIABLE_MASK) == 0);
    assert((ADVSIMD_LOGICAL_FIXED_MASK |
            ADVSIMD_LOGICAL_VARIABLE_MASK) == UINT32_MAX);
    unsigned decoded_count = 0;
    for (unsigned q = 0; q < 2; q++) {
        for (unsigned operation = 0; operation < 8; operation++) {
            for (unsigned rm = 0; rm < 32; rm++) {
                for (unsigned rn = 0; rn < 32; rn++) {
                    for (unsigned rd = 0; rd < 32; rd++) {
                        struct aarch64_decoded instruction;
                        bool decoded = aarch64_decode(encode(q,
                                (byte_t) operation, (byte_t) rm,
                                (byte_t) rn, (byte_t) rd), &instruction);
                        assert(decoded);
                        if (!decoded)
                            continue;
                        decoded_count++;
                        assert(instruction.opcode == opcode_for(
                                (byte_t) operation));
                        assert(instruction.width == (q ? 128 : 64));
                        assert(instruction.operands.advsimd_three_same.rd ==
                                rd);
                        assert(instruction.operands.advsimd_three_same.rn ==
                                rn);
                        assert(instruction.operands.advsimd_three_same.rm ==
                                rm);
                    }
                }
            }
        }
    }
    assert(decoded_count == 524288);
}

static void test_fixed_bits(void) {
    dword_t base = encode(true, 2, 29, 30, 30);
    for (unsigned bit = 0; bit < 32; bit++) {
        if ((ADVSIMD_LOGICAL_FIXED_MASK & (UINT32_C(1) << bit)) == 0)
            continue;
        struct aarch64_decoded instruction;
        bool decoded = aarch64_decode(base ^ (UINT32_C(1) << bit),
                &instruction);
        assert(!decoded || !is_logical_opcode(instruction.opcode));
    }
}

static void test_not_encoding_space(void) {
    assert((ADVSIMD_NOT_FIXED_MASK & ADVSIMD_NOT_VARIABLE_MASK) == 0);
    assert((ADVSIMD_NOT_FIXED_MASK |
            ADVSIMD_NOT_VARIABLE_MASK) == UINT32_MAX);
    unsigned decoded_count = 0;
    for (unsigned q = 0; q < 2; q++) {
        for (unsigned rn = 0; rn < 32; rn++) {
            for (unsigned rd = 0; rd < 32; rd++) {
                struct aarch64_decoded instruction = {0};
                bool decoded = aarch64_decode(encode_not(q,
                        (byte_t) rn, (byte_t) rd), &instruction);
                assert(decoded);
                if (!decoded)
                    continue;
                decoded_count++;
                assert(instruction.opcode == AARCH64_OP_ADVSIMD_NOT);
                assert(instruction.width == (q ? 128 : 64));
                assert(instruction.operands.advsimd_unary.rd == rd);
                assert(instruction.operands.advsimd_unary.rn == rn);
            }
        }
    }
    assert(decoded_count == 2048);
}

static void assert_not_vector_not(dword_t word) {
    struct aarch64_decoded instruction = {0};
    bool decoded = aarch64_decode(word, &instruction);
    assert(!decoded || instruction.opcode != AARCH64_OP_ADVSIMD_NOT);
}

static void test_not_boundaries(void) {
    dword_t product = encode_not(true, 30, 31);
    assert(product == UINT32_C(0x6e205bdf));
    for (unsigned bit = 0; bit < 32; bit++) {
        if ((ADVSIMD_NOT_FIXED_MASK & (UINT32_C(1) << bit)) != 0)
            assert_not_vector_not(product ^ (UINT32_C(1) << bit));
    }

    unsigned not_count = 0;
    unsigned neighbor_count = 0;
    for (unsigned q = 0; q < 2; q++) {
        for (unsigned u = 0; u < 2; u++) {
            for (unsigned size = 0; size < 4; size++) {
                for (unsigned rn = 0; rn < 32; rn++) {
                    for (unsigned rd = 0; rd < 32; rd++) {
                        dword_t word = ADVSIMD_TWO_REGISTER_MISC_BITS |
                                (dword_t) q << 30 |
                                (dword_t) u << 29 |
                                (dword_t) size << 22 |
                                (dword_t) rn << 5 |
                                rd;
                        struct aarch64_decoded instruction = {0};
                        bool decoded = aarch64_decode(word, &instruction);
                        bool is_not = decoded &&
                                instruction.opcode ==
                                        AARCH64_OP_ADVSIMD_NOT;
                        bool expected = u == 1 && size == 0;
                        assert(is_not == expected);
                        if (expected)
                            not_count++;
                        else
                            neighbor_count++;
                    }
                }
            }
        }
    }
    assert(not_count == 2048);
    assert(neighbor_count == 14336);

    assert_not_vector_not(UINT32_C(0x4e205bdf));
    assert_not_vector_not(UINT32_C(0x6e605bdf));
    assert_not_vector_not(UINT32_C(0x6e005bdf));
    assert_not_vector_not(UINT32_C(0x6e207bdf));
    assert_not_vector_not(UINT32_C(0x6e204bdf));
    assert_not_vector_not(UINT32_C(0x6e2053df));
    assert_not_vector_not(UINT32_C(0x6e205fdf));
}

static qword_t reference_word(byte_t operation,
        qword_t old, qword_t left, qword_t right) {
    if (operation == 0)
        return left & right;
    if (operation == 1)
        return left & ~right;
    if (operation == 2)
        return left | right;
    if (operation == 3)
        return left | ~right;
    if (operation == 4)
        return left ^ right;
    if (operation == 5)
        return (old & left) | (~old & right);
    if (operation == 6)
        return (right & left) | (~right & old);
    return (~right & left) | (right & old);
}

static void test_known_answers(void) {
    static const qword_t expected[] = {
        UINT64_C(0x0303c0c002244220),
        UINT64_C(0x0c0c303010101458),
        UINT64_C(0x3f3ffcfc97755779),
        UINT64_C(0xcfcff3f37abefefe),
        UINT64_C(0x3c3c3c3c95511559),
        UINT64_C(0x0f33f0cc07344370),
        UINT64_C(0xcf03f3c02a34ea74),
        UINT64_C(0x3f0cfc3092551659),
    };
    for (byte_t operation = 0; operation < 8; operation++) {
        struct cpu_state cpu = {.pc = UINT64_C(0x2000)};
        cpu.v[0].d[0] = UINT64_C(0xff00ff00aa55aa55);
        cpu.v[0].d[1] = UINT64_MAX;
        cpu.v[1].d[0] = UINT64_C(0x0f0ff0f012345678);
        cpu.v[2].d[0] = UINT64_C(0x3333cccc87654321);
        execute_instruction(&cpu, encode(false, operation, 2, 1, 0));
        assert(cpu.v[0].d[0] == expected[operation]);
        assert(cpu.v[0].d[1] == 0);
        assert(cpu.pc == UINT64_C(0x2004));
    }
}

static void fill_registers(struct cpu_state *cpu) {
    for (unsigned reg = 0; reg < 32; reg++) {
        cpu->v[reg].d[0] = UINT64_C(0x0102040810204080) * (reg + 1);
        cpu->v[reg].d[1] = ~cpu->v[reg].d[0];
    }
}

static struct cpu_state initial_not_cpu(void) {
    struct cpu_state cpu = {
        .cycle = UINT64_C(0x1020304050607080),
        .sp = UINT64_C(0x1122334455667788),
        .pc = UINT64_C(0x1000),
        .nzcv = UINT32_C(0xa0000000),
        .fpcr = UINT32_C(0x07c09f00),
        .fpsr = UINT32_C(0x0800009f),
        .tpidr_el0 = UINT64_C(0x8877665544332211),
        .segfault_addr = UINT64_C(0x123456789abcdef0),
        .segfault_was_write = true,
        .trapno = UINT32_C(0x13572468),
        .single_step = true,
        ._poked = true,
    };
    for (unsigned reg = 0; reg < 31; reg++)
        cpu.x[reg] = UINT64_C(0x0102030405060708) ^ reg;
    fill_registers(&cpu);
    return cpu;
}

static void test_not_execution_space(void) {
    for (unsigned q = 0; q < 2; q++) {
        byte_t bytes = q ? 16 : 8;
        for (unsigned rn = 0; rn < 32; rn++) {
            for (unsigned rd = 0; rd < 32; rd++) {
                struct cpu_state cpu = initial_not_cpu();
                struct cpu_state expected = cpu;
                expected.pc += 4;
                expected.v[rd] = (union aarch64_vector_reg) {0};
                for (unsigned byte = 0; byte < bytes; byte++) {
                    expected.v[rd].b[byte] =
                            cpu.v[rn].b[byte] ^ UINT8_C(0xff);
                }
                execute_instruction(&cpu, encode_not(q,
                        (byte_t) rn, (byte_t) rd));
                assert(memcmp(&cpu, &expected, sizeof(cpu)) == 0);
            }
        }
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
        {30, 30, 29},
    };
    for (unsigned q = 0; q < 2; q++) {
        for (unsigned operation = 0; operation < 8; operation++) {
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
                union aarch64_vector_reg expected = {0};
                expected.d[0] = reference_word((byte_t) operation,
                        before.v[rd].d[0], before.v[rn].d[0],
                        before.v[rm].d[0]);
                if (q) {
                    expected.d[1] = reference_word((byte_t) operation,
                            before.v[rd].d[1], before.v[rn].d[1],
                            before.v[rm].d[1]);
                }
                dword_t word = encode(q, (byte_t) operation, rm, rn, rd);
                if (q && operation == 2 &&
                        rd == 30 && rn == 30 && rm == 29)
                    assert(word == UINT32_C(0x4ebd1fde));
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

int main(void) {
    test_llvm_vectors();
    test_encoding_space();
    test_fixed_bits();
    test_not_encoding_space();
    test_not_boundaries();
    test_known_answers();
    test_execution_space();
    test_not_execution_space();
    return 0;
}
