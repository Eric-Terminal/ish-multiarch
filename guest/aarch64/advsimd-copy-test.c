#include <assert.h>
#include <string.h>

#include "guest/aarch64/decode.h"
#include "guest/aarch64/execute.h"

struct decode_case {
    dword_t word;
    enum aarch64_opcode opcode;
    byte_t width;
    byte_t rd;
    byte_t rn;
    byte_t element_size;
    byte_t destination_index;
    byte_t source_index;
};

static dword_t encode_copy(bool q, bool op, byte_t imm5, byte_t imm4,
        byte_t rn, byte_t rd) {
    return UINT32_C(0x0e000400) |
            (q ? UINT32_C(1) << 30 : 0) |
            (op ? UINT32_C(1) << 29 : 0) |
            (dword_t) imm5 << 16 |
            (dword_t) imm4 << 11 |
            (dword_t) rn << 5 | rd;
}

static dword_t encode_scalar_copy(byte_t imm5, byte_t rn, byte_t rd) {
    return UINT32_C(0x5e000400) |
            (dword_t) imm5 << 16 |
            (dword_t) rn << 5 | rd;
}

static byte_t element_imm5(byte_t element_size, byte_t index) {
    byte_t shift = 0;
    while ((1U << shift) != element_size)
        shift++;
    return (byte_t) (((unsigned) index << (shift + 1)) |
            (1U << shift));
}

static void assert_decodes(const struct decode_case *expected) {
    struct aarch64_decoded instruction;
    assert(aarch64_decode(expected->word, &instruction));
    assert(instruction.opcode == expected->opcode);
    assert(instruction.width == expected->width);
    assert(instruction.operands.advsimd_copy.rd == expected->rd);
    assert(instruction.operands.advsimd_copy.rn == expected->rn);
    assert(instruction.operands.advsimd_copy.element_size ==
            expected->element_size);
    assert(instruction.operands.advsimd_copy.destination_index ==
            expected->destination_index);
    assert(instruction.operands.advsimd_copy.source_index ==
            expected->source_index);
}

static bool expected_valid(bool q, bool op, byte_t imm5, byte_t imm4) {
    if ((imm5 & 0xf) == 0)
        return false;
    byte_t shift = 0;
    while ((imm5 & (1U << shift)) == 0)
        shift++;
    byte_t size = (byte_t) (1U << shift);
    if (op)
        return q;
    if (imm4 == 0 || imm4 == 1)
        return q || size != 8;
    if (imm4 == 3)
        return q;
    if (imm4 == 5)
        return q ? size <= 4 : size <= 2;
    if (imm4 == 7)
        return q ? size == 8 : size <= 4;
    return false;
}

static int opcode_index(enum aarch64_opcode opcode) {
    switch (opcode) {
        case AARCH64_OP_ADVSIMD_DUP_ELEMENT: return 0;
        case AARCH64_OP_ADVSIMD_DUP_GENERAL: return 1;
        case AARCH64_OP_ADVSIMD_INS_GENERAL: return 2;
        case AARCH64_OP_ADVSIMD_INS_ELEMENT: return 3;
        case AARCH64_OP_ADVSIMD_SMOV: return 4;
        case AARCH64_OP_ADVSIMD_UMOV: return 5;
        default: return -1;
    }
}

static void test_decode(void) {
    static const struct decode_case cases[] = {
        {UINT32_C(0x5e1c07ef), AARCH64_OP_ADVSIMD_DUP_ELEMENT,
            32, 15, 31, 4, 3, 3},
        {UINT32_C(0x0e0f0462), AARCH64_OP_ADVSIMD_DUP_ELEMENT,
            64, 2, 3, 1, 7, 7},
        {UINT32_C(0x4e1c04a4), AARCH64_OP_ADVSIMD_DUP_ELEMENT,
            128, 4, 5, 4, 3, 3},
        {UINT32_C(0x4e1804e6), AARCH64_OP_ADVSIMD_DUP_ELEMENT,
            128, 6, 7, 8, 1, 1},
        {UINT32_C(0x4e020d28), AARCH64_OP_ADVSIMD_DUP_GENERAL,
            128, 8, 9, 2, 0, 0},
        {UINT32_C(0x4e080d6a), AARCH64_OP_ADVSIMD_DUP_GENERAL,
            128, 10, 11, 8, 0, 0},
        {UINT32_C(0x4e161dac), AARCH64_OP_ADVSIMD_INS_GENERAL,
            128, 12, 13, 2, 5, 5},
        {UINT32_C(0x4e181dee), AARCH64_OP_ADVSIMD_INS_GENERAL,
            128, 14, 15, 8, 1, 1},
        {UINT32_C(0x6e137630), AARCH64_OP_ADVSIMD_INS_ELEMENT,
            128, 16, 17, 1, 9, 14},
        {UINT32_C(0x6e142672), AARCH64_OP_ADVSIMD_INS_ELEMENT,
            128, 18, 19, 4, 2, 1},
        {UINT32_C(0x0e1f2eb4), AARCH64_OP_ADVSIMD_SMOV,
            32, 20, 21, 1, 15, 15},
        {UINT32_C(0x4e1c2ef6), AARCH64_OP_ADVSIMD_SMOV,
            64, 22, 23, 4, 3, 3},
        {UINT32_C(0x0e143f38), AARCH64_OP_ADVSIMD_UMOV,
            32, 24, 25, 4, 2, 2},
        {UINT32_C(0x4e183f7a), AARCH64_OP_ADVSIMD_UMOV,
            64, 26, 27, 8, 1, 1},
    };
    for (size_t i = 0; i < array_size(cases); i++)
        assert_decodes(&cases[i]);

    unsigned counts[6] = {0};
    for (byte_t q = 0; q < 2; q++) {
        for (byte_t op = 0; op < 2; op++) {
            for (byte_t imm5 = 0; imm5 < 32; imm5++) {
                for (byte_t imm4 = 0; imm4 < 16; imm4++) {
                    struct aarch64_decoded instruction;
                    bool decoded = aarch64_decode(
                            encode_copy(q, op, imm5, imm4, 1, 2),
                            &instruction);
                    assert(decoded == expected_valid(q, op, imm5, imm4));
                    if (decoded) {
                        int index = opcode_index(instruction.opcode);
                        assert(index >= 0);
                        counts[index]++;
                    }
                }
            }
        }
    }
    static const unsigned expected_counts[] = {58, 58, 30, 480, 52, 30};
    assert(memcmp(counts, expected_counts, sizeof(counts)) == 0);

    struct aarch64_decoded instruction;
    for (byte_t imm5 = 0; imm5 < 32; imm5++) {
        bool valid = (imm5 & 0xf) != 0;
        bool decoded = aarch64_decode(
                encode_scalar_copy(imm5, 31, 15), &instruction);
        assert(decoded == valid);
        if (!decoded)
            continue;
        byte_t size_shift = 0;
        while ((imm5 & (1U << size_shift)) == 0)
            size_shift++;
        byte_t element_size = (byte_t) (1U << size_shift);
        byte_t index = imm5 >> (size_shift + 1);
        assert(instruction.opcode == AARCH64_OP_ADVSIMD_DUP_ELEMENT);
        assert(instruction.width == element_size * 8);
        assert(instruction.operands.advsimd_copy.rd == 15);
        assert(instruction.operands.advsimd_copy.rn == 31);
        assert(instruction.operands.advsimd_copy.element_size ==
                element_size);
        assert(instruction.operands.advsimd_copy.source_index == index);
    }

    assert(aarch64_decode(encode_copy(true, false, 31, 1, 1, 2),
            &instruction));
    assert(instruction.opcode == AARCH64_OP_ADVSIMD_DUP_GENERAL);
    assert(aarch64_decode(encode_copy(true, true, 20, 7, 1, 2),
            &instruction));
    assert(instruction.operands.advsimd_copy.source_index == 1);

    static const dword_t invalid[] = {
        UINT32_C(0x4e000c20), UINT32_C(0x4e100c20),
        UINT32_C(0x0e080c20), UINT32_C(0x0e011c20),
        UINT32_C(0x2e010420), UINT32_C(0x0e042c20),
        UINT32_C(0x4e082c20), UINT32_C(0x0e083c20),
        UINT32_C(0x4e043c20), UINT32_C(0x4e011420),
        UINT32_C(0x5e1c03ef), UINT32_C(0x7e1c07ef),
    };
    for (size_t i = 0; i < array_size(invalid); i++)
        assert(!aarch64_decode(invalid[i], &instruction));
}

static void execute_word(struct cpu_state *cpu, dword_t word) {
    struct aarch64_decoded instruction;
    assert(aarch64_decode(word, &instruction));
    qword_t pc = cpu->pc;
    qword_t sp = cpu->sp;
    dword_t nzcv = cpu->nzcv;
    dword_t fpcr = cpu->fpcr;
    dword_t fpsr = cpu->fpsr;
    struct aarch64_exclusive_monitor exclusive = cpu->exclusive;
    struct aarch64_execute_result result =
            aarch64_execute(cpu, NULL, &instruction);
    assert(result.stop == AARCH64_EXECUTE_RETIRED);
    assert(cpu->pc == pc + 4);
    assert(cpu->sp == sp);
    assert(cpu->nzcv == nzcv);
    assert(cpu->fpcr == fpcr);
    assert(cpu->fpsr == fpsr);
    assert(memcmp(&cpu->exclusive, &exclusive, sizeof(exclusive)) == 0);
}

static struct cpu_state make_cpu(void) {
    return (struct cpu_state) {
        .pc = UINT64_C(0x1000),
        .sp = UINT64_C(0x123456789abcdef0),
        .nzcv = UINT32_C(0xa0000000),
        .fpcr = UINT32_C(0x11223344),
        .fpsr = UINT32_C(0x55667788),
        .exclusive = {
            .address = UINT64_C(0x4000),
            .mapping_epoch = 7,
            .write_epoch = 11,
            .size = 8,
            .valid = true,
        },
    };
}

static void test_dup(void) {
    struct cpu_state cpu = make_cpu();
    cpu.x[1] = 0xa5;
    execute_word(&cpu, UINT32_C(0x4e010c20));
    for (byte_t i = 0; i < 16; i++)
        assert(cpu.v[0].b[i] == 0xa5);

    cpu.v[0].q = ~(__uint128_t) 0;
    cpu.x[1] = 0x5a;
    execute_word(&cpu, encode_copy(false, false,
            element_imm5(1, 0), 1, 1, 0));
    for (byte_t i = 0; i < 8; i++)
        assert(cpu.v[0].b[i] == 0x5a);
    assert(cpu.v[0].d[1] == 0);

    cpu.v[3].s[0] = UINT32_C(0x11111111);
    cpu.v[3].s[1] = UINT32_C(0x22222222);
    cpu.v[3].s[2] = UINT32_C(0x33333333);
    cpu.v[3].s[3] = UINT32_C(0xabcdef01);
    execute_word(&cpu, encode_copy(true, false,
            element_imm5(4, 3), 0, 3, 3));
    for (byte_t i = 0; i < 4; i++)
        assert(cpu.v[3].s[i] == UINT32_C(0xabcdef01));

    cpu.v[31].q = ~(__uint128_t) 0;
    execute_word(&cpu, encode_copy(true, false,
            element_imm5(2, 0), 1, 31, 31));
    assert(cpu.v[31].q == 0);
}

static void test_scalar_dup(void) {
    static const struct {
        byte_t element_size;
        byte_t index;
        qword_t value;
    } cases[] = {
        {1, 15, UINT64_C(0xa5)},
        {2, 7, UINT64_C(0xb6c7)},
        {4, 3, UINT64_C(0xd8e9fa0b)},
        {8, 1, UINT64_C(0x123456789abcdef0)},
    };

    for (size_t i = 0; i < array_size(cases); i++) {
        struct cpu_state cpu = make_cpu();
        cpu.v[31].q = ~(__uint128_t) 0;
        if (cases[i].element_size == 1)
            cpu.v[31].b[cases[i].index] = (byte_t) cases[i].value;
        else if (cases[i].element_size == 2)
            cpu.v[31].h[cases[i].index] = (word_t) cases[i].value;
        else if (cases[i].element_size == 4)
            cpu.v[31].s[cases[i].index] = (dword_t) cases[i].value;
        else
            cpu.v[31].d[cases[i].index] = cases[i].value;
        union aarch64_vector_reg source = cpu.v[31];

        execute_word(&cpu, encode_scalar_copy(
                element_imm5(cases[i].element_size, cases[i].index),
                31, 15));
        assert(cpu.v[15].d[0] == cases[i].value);
        assert(cpu.v[15].d[1] == 0);
        assert(memcmp(&cpu.v[31], &source, sizeof(source)) == 0);
    }

    struct cpu_state aliased = make_cpu();
    aliased.v[31].d[0] = UINT64_C(0x1111222233334444);
    aliased.v[31].d[1] = UINT64_C(0x5555666677778888);
    execute_word(&aliased, encode_scalar_copy(
            element_imm5(4, 3), 31, 31));
    assert(aliased.v[31].d[0] == UINT64_C(0x0000000055556666));
    assert(aliased.v[31].d[1] == 0);
}

static void test_insert(void) {
    struct cpu_state cpu = make_cpu();
    for (byte_t i = 0; i < 16; i++)
        cpu.v[4].b[i] = i;
    byte_t original[16];
    memcpy(original, cpu.v[4].b, sizeof(original));
    execute_word(&cpu, encode_copy(true, true,
            element_imm5(1, 2), 12, 4, 4));
    assert(cpu.v[4].b[2] == 12);
    original[2] = 12;
    assert(memcmp(cpu.v[4].b, original, sizeof(original)) == 0);

    for (byte_t i = 0; i < 8; i++)
        cpu.v[5].h[i] = (word_t) (0x1000 + i);
    cpu.x[6] = UINT64_C(0xffffffffffff5678);
    word_t halfwords[8];
    memcpy(halfwords, cpu.v[5].h, sizeof(halfwords));
    execute_word(&cpu, encode_copy(true, false,
            element_imm5(2, 5), 3, 6, 5));
    halfwords[5] = UINT16_C(0x5678);
    assert(memcmp(cpu.v[5].h, halfwords, sizeof(halfwords)) == 0);
}

static void test_move_to_general(void) {
    struct cpu_state cpu = make_cpu();
    cpu.v[7].b[2] = 0x80;
    execute_word(&cpu, encode_copy(false, false,
            element_imm5(1, 2), 5, 7, 8));
    assert(cpu.x[8] == UINT64_C(0x00000000ffffff80));

    cpu.v[7].h[3] = UINT16_C(0x8001);
    execute_word(&cpu, encode_copy(false, false,
            element_imm5(2, 3), 5, 7, 9));
    assert(cpu.x[9] == UINT64_C(0x00000000ffff8001));

    cpu.v[7].s[1] = UINT32_C(0x80000001);
    execute_word(&cpu, encode_copy(true, false,
            element_imm5(4, 1), 5, 7, 10));
    assert(cpu.x[10] == UINT64_C(0xffffffff80000001));

    cpu.x[11] = UINT64_MAX;
    cpu.v[7].s[2] = UINT32_C(0xdeadbeef);
    execute_word(&cpu, encode_copy(false, false,
            element_imm5(4, 2), 7, 7, 11));
    assert(cpu.x[11] == UINT64_C(0x00000000deadbeef));

    cpu.v[0].d[0] = UINT64_C(0x8877665544332211);
    execute_word(&cpu, UINT32_C(0x4e083c01));
    assert(cpu.x[1] == UINT64_C(0x8877665544332211));

    cpu.v[31].d[1] = UINT64_C(0x0123456789abcdef);
    execute_word(&cpu, encode_copy(true, false,
            element_imm5(8, 1), 7, 31, 2));
    assert(cpu.x[2] == UINT64_C(0x0123456789abcdef));

    cpu.x[30] = UINT64_C(0x76543210fedcba98);
    execute_word(&cpu, encode_copy(true, false,
            element_imm5(8, 1), 7, 31, 31));
    assert(cpu.x[30] == UINT64_C(0x76543210fedcba98));
}

int main(void) {
    test_decode();
    test_dup();
    test_scalar_dup();
    test_insert();
    test_move_to_general();
    return 0;
}
