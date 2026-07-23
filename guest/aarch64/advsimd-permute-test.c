#include <assert.h>
#include <string.h>

#include "guest/aarch64/decode.h"
#include "guest/aarch64/execute.h"

#define ADVSIMD_PERMUTE_FIXED_MASK UINT32_C(0xbf208c00)
#define ADVSIMD_PERMUTE_FIXED_BITS UINT32_C(0x0e000800)
#define ADVSIMD_PERMUTE_VARIABLE_MASK UINT32_C(0x40df73ff)
#define ADVSIMD_EXT_FIXED_MASK UINT32_C(0xbfe08400)
#define ADVSIMD_EXT_FIXED_BITS UINT32_C(0x2e000000)
#define ADVSIMD_EXT_VARIABLE_MASK UINT32_C(0x401f7bff)

static dword_t encode(bool q, byte_t size, byte_t operation,
        byte_t rm, byte_t rn, byte_t rd) {
    return ADVSIMD_PERMUTE_FIXED_BITS |
            (dword_t) q << 30 |
            (dword_t) size << 22 |
            (dword_t) rm << 16 |
            (dword_t) operation << 12 |
            (dword_t) rn << 5 |
            rd;
}

static dword_t encode_extract(bool q, byte_t byte_offset,
        byte_t rm, byte_t rn, byte_t rd) {
    return ADVSIMD_EXT_FIXED_BITS |
            (dword_t) q << 30 |
            (dword_t) rm << 16 |
            (dword_t) byte_offset << 11 |
            (dword_t) rn << 5 |
            rd;
}

static bool operation_valid(byte_t operation) {
    return operation != 0 && operation != 4;
}

static enum aarch64_opcode opcode_for(byte_t operation) {
    static const enum aarch64_opcode first[] = {
        AARCH64_OP_ADVSIMD_UZP1,
        AARCH64_OP_ADVSIMD_TRN1,
        AARCH64_OP_ADVSIMD_ZIP1,
    };
    static const enum aarch64_opcode second[] = {
        AARCH64_OP_ADVSIMD_UZP2,
        AARCH64_OP_ADVSIMD_TRN2,
        AARCH64_OP_ADVSIMD_ZIP2,
    };
    assert(operation_valid(operation));
    return operation < 4 ? first[operation - 1] : second[operation - 5];
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

static void assert_extract_decode(dword_t word, byte_t width,
        byte_t byte_offset, byte_t rd, byte_t rn, byte_t rm) {
    struct aarch64_decoded instruction = decode(word);
    assert(instruction.opcode == AARCH64_OP_ADVSIMD_EXT);
    assert(instruction.width == width);
    assert(instruction.operands.advsimd_extract.rd == rd);
    assert(instruction.operands.advsimd_extract.rn == rn);
    assert(instruction.operands.advsimd_extract.rm == rm);
    assert(instruction.operands.advsimd_extract.byte_offset == byte_offset);
}

static void test_llvm_vectors(void) {
    assert_decode(UINT32_C(0x4e021820), AARCH64_OP_ADVSIMD_UZP1,
            128, 1, 0, 1, 2);
    assert_decode(UINT32_C(0x4e455883), AARCH64_OP_ADVSIMD_UZP2,
            128, 2, 3, 4, 5);
    assert_decode(UINT32_C(0x4e8828e6), AARCH64_OP_ADVSIMD_TRN1,
            128, 4, 6, 7, 8);
    assert_decode(UINT32_C(0x4ecb6949), AARCH64_OP_ADVSIMD_TRN2,
            128, 8, 9, 10, 11);
    assert_decode(UINT32_C(0x4e0e39ac), AARCH64_OP_ADVSIMD_ZIP1,
            128, 1, 12, 13, 14);
    assert_decode(UINT32_C(0x4e517a0f), AARCH64_OP_ADVSIMD_ZIP2,
            128, 2, 15, 16, 17);
    assert_decode(UINT32_C(0x0e141a72), AARCH64_OP_ADVSIMD_UZP1,
            64, 1, 18, 19, 20);
    assert_decode(UINT32_C(0x0e575ad5), AARCH64_OP_ADVSIMD_UZP2,
            64, 2, 21, 22, 23);
    assert_decode(UINT32_C(0x0e9a2b38), AARCH64_OP_ADVSIMD_TRN1,
            64, 4, 24, 25, 26);
    assert_decode(UINT32_C(0x4e1b3bfd), AARCH64_OP_ADVSIMD_ZIP1,
            128, 1, 29, 31, 27);
}

static void test_encoding_space(void) {
    assert((ADVSIMD_PERMUTE_FIXED_MASK &
            ADVSIMD_PERMUTE_VARIABLE_MASK) == 0);
    assert((ADVSIMD_PERMUTE_FIXED_MASK |
            ADVSIMD_PERMUTE_VARIABLE_MASK) == UINT32_MAX);
    unsigned decoded_count = 0;
    for (unsigned q = 0; q < 2; q++) {
        for (unsigned size = 0; size < 4; size++) {
            for (unsigned operation = 0; operation < 8; operation++) {
                for (unsigned rm = 0; rm < 32; rm++) {
                    for (unsigned rn = 0; rn < 32; rn++) {
                        for (unsigned rd = 0; rd < 32; rd++) {
                            struct aarch64_decoded instruction;
                            bool decoded = aarch64_decode(encode(q,
                                    (byte_t) size, (byte_t) operation,
                                    (byte_t) rm, (byte_t) rn, (byte_t) rd),
                                    &instruction);
                            bool expected = operation_valid(
                                    (byte_t) operation) &&
                                    (q != 0 || size != 3);
                            assert(decoded == expected);
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
                            assert(instruction.operands.advsimd_three_same.
                                    element_size == (1U << size));
                        }
                    }
                }
            }
        }
    }
    assert(decoded_count == 1376256);
}

static void test_fixed_bits(void) {
    dword_t base = encode(true, 0, 3, 27, 31, 29);
    for (unsigned bit = 0; bit < 32; bit++) {
        if ((ADVSIMD_PERMUTE_FIXED_MASK & (UINT32_C(1) << bit)) == 0)
            continue;
        struct aarch64_decoded instruction;
        bool decoded = aarch64_decode(base ^ (UINT32_C(1) << bit),
                &instruction);
        assert(!decoded || instruction.opcode < AARCH64_OP_ADVSIMD_UZP1 ||
                instruction.opcode > AARCH64_OP_ADVSIMD_ZIP2);
    }
}

static void test_extract_llvm_vectors(void) {
    assert_extract_decode(UINT32_C(0x2e020020), 64, 0, 0, 1, 2);
    assert_extract_decode(UINT32_C(0x2e053883), 64, 7, 3, 4, 5);
    assert_extract_decode(UINT32_C(0x2e1f3bff), 64, 7, 31, 31, 31);
    assert_extract_decode(UINT32_C(0x6e0800e6), 128, 0, 6, 7, 8);
    assert_extract_decode(UINT32_C(0x6e0b7949), 128, 15, 9, 10, 11);
    assert_extract_decode(UINT32_C(0x6e1e4360), 128, 8, 0, 27, 30);
    assert_extract_decode(UINT32_C(0x6e1f7bff), 128, 15, 31, 31, 31);
}

static void test_extract_encoding_space(void) {
    assert((ADVSIMD_EXT_FIXED_MASK & ADVSIMD_EXT_VARIABLE_MASK) == 0);
    assert((ADVSIMD_EXT_FIXED_MASK |
            ADVSIMD_EXT_VARIABLE_MASK) == UINT32_MAX);
    unsigned decoded_count = 0;
    unsigned reserved_count = 0;
    for (unsigned q = 0; q < 2; q++) {
        for (unsigned byte_offset = 0; byte_offset < 16; byte_offset++) {
            for (unsigned rm = 0; rm < 32; rm++) {
                for (unsigned rn = 0; rn < 32; rn++) {
                    for (unsigned rd = 0; rd < 32; rd++) {
                        struct aarch64_decoded instruction = {0};
                        bool decoded = aarch64_decode(encode_extract(q,
                                (byte_t) byte_offset, (byte_t) rm,
                                (byte_t) rn, (byte_t) rd), &instruction);
                        bool expected = q != 0 || byte_offset < 8;
                        assert(decoded == expected);
                        if (!expected) {
                            reserved_count++;
                            continue;
                        }
                        decoded_count++;
                        assert(instruction.opcode ==
                                AARCH64_OP_ADVSIMD_EXT);
                        assert(instruction.width == (q ? 128 : 64));
                        assert(instruction.operands.advsimd_extract.rd == rd);
                        assert(instruction.operands.advsimd_extract.rn == rn);
                        assert(instruction.operands.advsimd_extract.rm == rm);
                        assert(instruction.operands.advsimd_extract.
                                byte_offset == byte_offset);
                    }
                }
            }
        }
    }
    assert(decoded_count == 786432);
    assert(reserved_count == 262144);
}

static void assert_not_extract(dword_t word) {
    struct aarch64_decoded instruction = {0};
    bool decoded = aarch64_decode(word, &instruction);
    assert(!decoded || instruction.opcode != AARCH64_OP_ADVSIMD_EXT);
}

static void test_extract_boundaries(void) {
    dword_t base = UINT32_C(0x6e1e4360);
    for (unsigned bit = 0; bit < 32; bit++) {
        if ((ADVSIMD_EXT_FIXED_MASK & (UINT32_C(1) << bit)) != 0)
            assert_not_extract(base ^ (UINT32_C(1) << bit));
    }

    struct aarch64_decoded instruction = {0};
    assert(!aarch64_decode(encode_extract(false, 8, 2, 1, 0),
            &instruction));
    assert(!aarch64_decode(encode_extract(false, 15, 2, 1, 0),
            &instruction));
    assert_not_extract(UINT32_C(0x4e1e4360));
    assert_not_extract(UINT32_C(0x6e1e4760));
    assert_not_extract(UINT32_C(0x6e3e4360));
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

static union aarch64_vector_reg reference_permute(
        const struct cpu_state *before, bool q, byte_t size,
        byte_t operation, byte_t rm, byte_t rn) {
    byte_t element_size = (byte_t) (1U << size);
    byte_t lanes = (byte_t) ((q ? 128 : 64) / (element_size * 8));
    byte_t half = lanes / 2;
    byte_t family = operation & 3;
    bool second = operation >= 4;
    union aarch64_vector_reg result = {0};
    for (byte_t lane = 0; lane < lanes; lane++) {
        byte_t source;
        byte_t index;
        if (family == 1) {
            source = lane < half ? rn : rm;
            index = (byte_t) (2 * (lane % half) + second);
        } else if (family == 2) {
            source = (lane & 1) == 0 ? rn : rm;
            index = (byte_t) (2 * (lane / 2) + second);
        } else {
            source = (lane & 1) == 0 ? rn : rm;
            index = (byte_t) ((second ? half : 0) + lane / 2);
        }
        write_lane(&result, element_size, lane,
                read_lane(&before->v[source], element_size, index));
    }
    return result;
}

static void fill_registers(struct cpu_state *cpu) {
    for (unsigned reg = 0; reg < 32; reg++) {
        for (unsigned byte = 0; byte < 16; byte++)
            cpu->v[reg].b[byte] = (byte_t) (reg * 19 + byte * 11);
    }
}

static struct cpu_state initial_extract_cpu(void) {
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

static union aarch64_vector_reg reference_extract(
        const struct cpu_state *before, bool q, byte_t byte_offset,
        byte_t rn, byte_t rm) {
    byte_t bytes = q ? 16 : 8;
    byte_t concatenated[32] = {0};
    union aarch64_vector_reg result = {0};
    memcpy(concatenated, before->v[rn].b, bytes);
    memcpy(&concatenated[bytes], before->v[rm].b, bytes);
    memcpy(result.b, &concatenated[byte_offset], bytes);
    return result;
}

static void execute_extract_and_assert(bool q, byte_t byte_offset,
        byte_t rd, byte_t rn, byte_t rm) {
    dword_t word = encode_extract(q, byte_offset, rm, rn, rd);
    struct aarch64_decoded instruction = decode(word);
    assert(instruction.opcode == AARCH64_OP_ADVSIMD_EXT);
    assert(instruction.width == (q ? 128 : 64));
    assert(instruction.operands.advsimd_extract.rd == rd);
    assert(instruction.operands.advsimd_extract.rn == rn);
    assert(instruction.operands.advsimd_extract.rm == rm);
    assert(instruction.operands.advsimd_extract.byte_offset == byte_offset);
    if (q && byte_offset == 8 && rd == 0 && rn == 27 && rm == 30)
        assert(word == UINT32_C(0x6e1e4360));

    struct cpu_state cpu = initial_extract_cpu();
    struct cpu_state expected = cpu;
    expected.pc += 4;
    expected.v[rd] = reference_extract(&cpu, q, byte_offset, rn, rm);
    struct aarch64_execute_result result =
            aarch64_execute(&cpu, NULL, &instruction);
    assert(result.stop == AARCH64_EXECUTE_RETIRED);
    assert(result.fault.kind == GUEST_MEMORY_FAULT_NONE);
    assert(memcmp(&cpu, &expected, sizeof(cpu)) == 0);
}

static void test_extract_known_answers(void) {
    static const struct {
        bool q;
        byte_t byte_offset;
        byte_t expected[16];
    } cases[] = {
        {false, 0, {0, 1, 2, 3, 4, 5, 6, 7}},
        {false, 1, {1, 2, 3, 4, 5, 6, 7, 0x80}},
        {false, 7, {7, 0x80, 0x81, 0x82,
                0x83, 0x84, 0x85, 0x86}},
        {true, 1, {1, 2, 3, 4, 5, 6, 7, 8,
                9, 10, 11, 12, 13, 14, 15, 0x80}},
        {true, 8, {8, 9, 10, 11, 12, 13, 14, 15,
                0x80, 0x81, 0x82, 0x83,
                0x84, 0x85, 0x86, 0x87}},
        {true, 15, {15, 0x80, 0x81, 0x82,
                0x83, 0x84, 0x85, 0x86,
                0x87, 0x88, 0x89, 0x8a,
                0x8b, 0x8c, 0x8d, 0x8e}},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        struct cpu_state cpu = initial_extract_cpu();
        for (unsigned byte = 0; byte < 16; byte++) {
            cpu.v[1].b[byte] = (byte_t) byte;
            cpu.v[2].b[byte] = (byte_t) (0x80 + byte);
        }
        cpu.v[0].q = ~(__uint128_t) 0;
        struct cpu_state expected = cpu;
        expected.pc += 4;
        memcpy(expected.v[0].b, cases[i].expected,
                sizeof(cases[i].expected));

        struct aarch64_decoded instruction =
                decode(encode_extract(cases[i].q,
                        cases[i].byte_offset, 2, 1, 0));
        struct aarch64_execute_result result =
                aarch64_execute(&cpu, NULL, &instruction);
        assert(result.stop == AARCH64_EXECUTE_RETIRED);
        assert(result.fault.kind == GUEST_MEMORY_FAULT_NONE);
        assert(memcmp(&cpu, &expected, sizeof(cpu)) == 0);
    }
}

static void test_extract_execution_space(void) {
    static const struct {
        byte_t rd;
        byte_t rn;
        byte_t rm;
    } registers[] = {
        {0, 1, 2},
        {1, 1, 2},
        {2, 1, 2},
        {0, 1, 1},
        {1, 1, 1},
        {31, 1, 2},
        {0, 31, 2},
        {0, 1, 31},
        {31, 31, 31},
        {0, 27, 30},
    };
    for (unsigned q = 0; q < 2; q++) {
        unsigned bytes = q ? 16 : 8;
        for (unsigned byte_offset = 0;
                byte_offset < bytes; byte_offset++) {
            for (unsigned form = 0;
                    form < sizeof(registers) / sizeof(registers[0]);
                    form++) {
                execute_extract_and_assert(q, (byte_t) byte_offset,
                        registers[form].rd, registers[form].rn,
                        registers[form].rm);
            }
        }
    }
}

static void test_known_answers(void) {
    static const byte_t operations[] = {1, 2, 3, 5, 6, 7};
    static const byte_t expected[][16] = {
        {0, 2, 4, 6, 8, 10, 12, 14,
                0x80, 0x82, 0x84, 0x86, 0x88, 0x8a, 0x8c, 0x8e},
        {0, 0x80, 2, 0x82, 4, 0x84, 6, 0x86,
                8, 0x88, 10, 0x8a, 12, 0x8c, 14, 0x8e},
        {0, 0x80, 1, 0x81, 2, 0x82, 3, 0x83,
                4, 0x84, 5, 0x85, 6, 0x86, 7, 0x87},
        {1, 3, 5, 7, 9, 11, 13, 15,
                0x81, 0x83, 0x85, 0x87, 0x89, 0x8b, 0x8d, 0x8f},
        {1, 0x81, 3, 0x83, 5, 0x85, 7, 0x87,
                9, 0x89, 11, 0x8b, 13, 0x8d, 15, 0x8f},
        {8, 0x88, 9, 0x89, 10, 0x8a, 11, 0x8b,
                12, 0x8c, 13, 0x8d, 14, 0x8e, 15, 0x8f},
    };
    for (unsigned operation = 0;
            operation < sizeof(operations); operation++) {
        struct cpu_state cpu = {.pc = UINT64_C(0x2000)};
        for (unsigned lane = 0; lane < 16; lane++) {
            cpu.v[31].b[lane] = (byte_t) lane;
            cpu.v[27].b[lane] = (byte_t) (0x80 + lane);
            cpu.v[29].b[lane] = UINT8_MAX;
        }
        dword_t word = encode(true, 0, operations[operation], 27, 31, 29);
        if (operations[operation] == 3)
            assert(word == UINT32_C(0x4e1b3bfd));
        execute_instruction(&cpu, word);
        assert(memcmp(cpu.v[29].b, expected[operation], 16) == 0);
        assert(cpu.pc == UINT64_C(0x2004));
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
        {29, 31, 27},
    };
    for (unsigned q = 0; q < 2; q++) {
        for (unsigned size = 0; size < 4; size++) {
            if (q == 0 && size == 3)
                continue;
            for (unsigned operation = 1; operation < 8; operation++) {
                if (operation == 4)
                    continue;
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
                    union aarch64_vector_reg expected = reference_permute(
                            &before, q, (byte_t) size, (byte_t) operation,
                            rm, rn);
                    dword_t word = encode(q, (byte_t) size,
                            (byte_t) operation, rm, rn, rd);
                    if (q && size == 0 && operation == 3 &&
                            rd == 29 && rn == 31 && rm == 27)
                        assert(word == UINT32_C(0x4e1b3bfd));
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

int main(void) {
    test_llvm_vectors();
    test_encoding_space();
    test_fixed_bits();
    test_extract_llvm_vectors();
    test_extract_encoding_space();
    test_extract_boundaries();
    test_known_answers();
    test_execution_space();
    test_extract_known_answers();
    test_extract_execution_space();
    return 0;
}
