#include <assert.h>
#include <string.h>

#include "guest/aarch64/decode.h"
#include "guest/aarch64/execute.h"

#define ADVSIMD_COMPARE_FIXED_MASK UINT32_C(0x9f200000)
#define ADVSIMD_COMPARE_FIXED_BITS UINT32_C(0x0e200000)
#define ADVSIMD_COMPARE_VARIABLE_MASK UINT32_C(0x60dfffff)
#define ADVSIMD_CMGE_ZERO_SCALAR_FIXED_MASK UINT32_C(0xfffffc00)
#define ADVSIMD_CMGE_ZERO_SCALAR_FIXED_BITS UINT32_C(0x7ee08800)
#define ADVSIMD_CMGE_ZERO_SCALAR_VARIABLE_MASK UINT32_C(0x000003ff)
#define ADVSIMD_CMGE_ZERO_SCALAR_FAMILY_BITS UINT32_C(0x7e208800)

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

static dword_t encode_scalar_cmge_zero(
        byte_t size, byte_t rn, byte_t rd) {
    return ADVSIMD_CMGE_ZERO_SCALAR_FAMILY_BITS |
            (dword_t) size << 22 |
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

static void assert_scalar_cmge_zero_decode(
        dword_t word, byte_t rd, byte_t rn) {
    struct aarch64_decoded instruction = decode(word);
    assert(instruction.opcode == AARCH64_OP_ADVSIMD_CMGE_ZERO_SCALAR);
    assert(instruction.width == 64);
    assert(instruction.operands.advsimd_unary.rd == rd);
    assert(instruction.operands.advsimd_unary.rn == rn);
    assert(instruction.operands.advsimd_unary.element_size == 8);
}

static void test_scalar_cmge_zero_decode(void) {
    assert_scalar_cmge_zero_decode(UINT32_C(0x7ee08820), 0, 1);
    assert_scalar_cmge_zero_decode(UINT32_C(0x7ee08883), 3, 4);
    assert_scalar_cmge_zero_decode(UINT32_C(0x7ee08bdd), 29, 30);
    // GCC cc1 实际触发的 V31 自别名指令。
    assert_scalar_cmge_zero_decode(UINT32_C(0x7ee08bff), 31, 31);

    assert((ADVSIMD_CMGE_ZERO_SCALAR_FIXED_MASK &
            ADVSIMD_CMGE_ZERO_SCALAR_VARIABLE_MASK) == 0);
    assert((ADVSIMD_CMGE_ZERO_SCALAR_FIXED_MASK |
            ADVSIMD_CMGE_ZERO_SCALAR_VARIABLE_MASK) == UINT32_MAX);

    unsigned decoded_count = 0;
    unsigned reserved_count = 0;
    for (unsigned size = 0; size < 4; size++) {
        for (unsigned rn = 0; rn < 32; rn++) {
            for (unsigned rd = 0; rd < 32; rd++) {
                dword_t word = encode_scalar_cmge_zero(
                        (byte_t) size, (byte_t) rn, (byte_t) rd);
                struct aarch64_decoded instruction;
                bool decoded = aarch64_decode(word, &instruction);
                bool expected = size == 3;
                assert(decoded == expected);
                if (!decoded) {
                    reserved_count++;
                    continue;
                }
                decoded_count++;
                assert(instruction.opcode ==
                        AARCH64_OP_ADVSIMD_CMGE_ZERO_SCALAR);
                assert(instruction.width == 64);
                assert(instruction.operands.advsimd_unary.rd == rd);
                assert(instruction.operands.advsimd_unary.rn == rn);
                assert(instruction.operands.advsimd_unary.element_size == 8);
            }
        }
    }
    assert(decoded_count == 1024);
    assert(reserved_count == 3072);

    const dword_t base = encode_scalar_cmge_zero(3, 31, 31);
    for (unsigned bit = 0; bit < 32; bit++) {
        if ((ADVSIMD_CMGE_ZERO_SCALAR_FIXED_MASK &
                (UINT32_C(1) << bit)) == 0)
            continue;
        struct aarch64_decoded instruction;
        bool decoded = aarch64_decode(
                base ^ (UINT32_C(1) << bit), &instruction);
        assert(!decoded ||
                instruction.opcode !=
                AARCH64_OP_ADVSIMD_CMGE_ZERO_SCALAR);
    }

    static const dword_t neighbors[] = {
        UINT32_C(0x5ee08820), // 标量 CMGT #0。
        UINT32_C(0x7ee09820), // 标量 CMLE #0。
        UINT32_C(0x5ee09820), // 标量 CMEQ #0。
        UINT32_C(0x5ee0a820), // 标量 CMLT #0。
        UINT32_C(0x5ee23c20), // 三操作数标量 CMGE。
        UINT32_C(0x6ee08820), // 向量 CMGE #0。
        UINT32_C(0x4ee23c20), // 三操作数向量 CMGE。
        UINT32_C(0x7ee0c820), // 标量浮点 FCMGE #0.0。
        UINT32_C(0x7e62e420), // 三操作数标量浮点 FCMGE。
        UINT32_C(0x6ee0c820), // 向量浮点 FCMGE #0.0。
    };
    for (unsigned index = 0;
            index < sizeof(neighbors) / sizeof(neighbors[0]); index++) {
        struct aarch64_decoded instruction;
        bool decoded = aarch64_decode(neighbors[index], &instruction);
        assert(!decoded ||
                instruction.opcode !=
                AARCH64_OP_ADVSIMD_CMGE_ZERO_SCALAR);
    }
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

static void assert_scalar_cmge_zero_execution(
        byte_t rn, byte_t rd, qword_t source) {
    struct cpu_state cpu = {
        .cycle = UINT64_C(0x123456789abcdef0),
        .sp = UINT64_C(0x1122334455667788),
        .pc = UINT64_C(0x1800),
        .nzcv = UINT32_C(0xa0000000),
        .fpcr = UINT32_C(0x00c00000),
        .fpsr = UINT32_C(0x0800001f),
        .tpidr_el0 = UINT64_C(0x8877665544332211),
        .segfault_addr = UINT64_C(0x1020304050607080),
        .segfault_was_write = true,
        .trapno = UINT32_C(0x12345678),
        .single_step = true,
        ._poked = true,
    };
    for (unsigned reg = 0; reg < 31; reg++)
        cpu.x[reg] = UINT64_C(0x0102030405060708) +
                reg * UINT64_C(0x1111111111111111);
    for (unsigned reg = 0; reg < 32; reg++) {
        cpu.v[reg].d[0] = UINT64_C(0x1020304050607080) ^
                (reg * UINT64_C(0x0101010101010101));
        cpu.v[reg].d[1] = UINT64_C(0x8877665544332211) +
                reg * UINT64_C(0x0010001000100010);
    }

    cpu.v[rd].q = ~(__uint128_t) 0;
    cpu.v[rn] = (union aarch64_vector_reg) {
        .d = {source, UINT64_C(0xa5a5a5a5a5a5a5a5)},
    };

    struct cpu_state expected = cpu;
    expected.v[rd] = (union aarch64_vector_reg) {
        .d = {
            (source & (UINT64_C(1) << 63)) == 0 ? UINT64_MAX : 0,
            0,
        },
    };
    expected.pc += 4;

    execute_instruction(&cpu, encode_scalar_cmge_zero(3, rn, rd));
    assert(memcmp(&cpu, &expected, sizeof(cpu)) == 0);
}

static void test_scalar_cmge_zero_execution(void) {
    for (unsigned rn = 0; rn < 32; rn++) {
        for (unsigned rd = 0; rd < 32; rd++) {
            qword_t source = rn < 16 ?
                    UINT64_C(0x7fffffffffffffff) - rn :
                    UINT64_C(0x8000000000000000) + rn;
            assert_scalar_cmge_zero_execution(
                    (byte_t) rn, (byte_t) rd, source);
        }
    }

    static const qword_t boundaries[] = {
        0,
        1,
        UINT64_C(0x7ffffffffffffffe),
        UINT64_C(0x7fffffffffffffff),
        UINT64_C(0x8000000000000000),
        UINT64_C(0x8000000000000001),
        UINT64_MAX,
        UINT64_C(0x00004488cd11569e),
    };
    static const byte_t registers[][2] = {
        {1, 0},
        {1, 1},
        {30, 29},
        {31, 30},
        {31, 31},
    };
    for (unsigned value = 0;
            value < sizeof(boundaries) / sizeof(boundaries[0]); value++) {
        for (unsigned form = 0;
                form < sizeof(registers) / sizeof(registers[0]); form++) {
            assert_scalar_cmge_zero_execution(
                    registers[form][0], registers[form][1],
                    boundaries[value]);
        }
    }
}

int main(void) {
    test_llvm_vectors();
    test_scalar_cmge_zero_decode();
    test_encoding_space();
    test_fixed_and_opcode_bits();
    test_known_answers();
    test_execution_space();
    test_scalar_cmge_zero_execution();
    return 0;
}
