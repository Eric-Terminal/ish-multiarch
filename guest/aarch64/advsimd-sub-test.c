#include <assert.h>
#include <string.h>

#include "guest/aarch64/decode.h"
#include "guest/aarch64/execute.h"

#define ADVSIMD_SUB_FIXED_MASK UINT32_C(0xbf20fc00)
#define ADVSIMD_SUB_FIXED_BITS UINT32_C(0x2e208400)
#define ADVSIMD_SUB_VARIABLE_MASK UINT32_C(0x40df03ff)

static dword_t encode(
        bool q, byte_t size, byte_t rm, byte_t rn, byte_t rd) {
    return ADVSIMD_SUB_FIXED_BITS |
            (dword_t) q << 30 |
            (dword_t) size << 22 |
            (dword_t) rm << 16 |
            (dword_t) rn << 5 |
            rd;
}

static struct aarch64_decoded decode(dword_t word) {
    struct aarch64_decoded instruction = {0};
    bool decoded = aarch64_decode(word, &instruction);
    assert(decoded);
    use(decoded);
    return instruction;
}

static bool is_vector_sub(const struct aarch64_decoded *instruction) {
    return instruction->opcode == AARCH64_OP_ADVSIMD_SUB &&
            !(instruction->width == 64 &&
            instruction->operands.advsimd_three_same.element_size == 8);
}

static void assert_decode(dword_t word, byte_t width, byte_t element_size,
        byte_t rd, byte_t rn, byte_t rm) {
    struct aarch64_decoded instruction = decode(word);
    assert(is_vector_sub(&instruction));
    assert(instruction.width == width);
    assert(instruction.operands.advsimd_three_same.rd == rd);
    assert(instruction.operands.advsimd_three_same.rn == rn);
    assert(instruction.operands.advsimd_three_same.rm == rm);
    assert(instruction.operands.advsimd_three_same.element_size ==
            element_size);
}

static void execute_instruction(struct cpu_state *cpu, dword_t word) {
    struct aarch64_decoded instruction = decode(word);
    struct aarch64_execute_result result =
            aarch64_execute(cpu, NULL, &instruction);
    assert(result.stop == AARCH64_EXECUTE_RETIRED);
}

static void test_llvm_vectors(void) {
    assert_decode(UINT32_C(0x2e258483), 64, 1, 3, 4, 5);
    assert_decode(UINT32_C(0x6e2884e6), 128, 1, 6, 7, 8);
    assert_decode(UINT32_C(0x2e6b8549), 64, 2, 9, 10, 11);
    assert_decode(UINT32_C(0x6e6e85ac), 128, 2, 12, 13, 14);
    assert_decode(UINT32_C(0x2eb1860f), 64, 4, 15, 16, 17);
    assert_decode(UINT32_C(0x6eb48672), 128, 4, 18, 19, 20);
    assert_decode(UINT32_C(0x6ef786d5), 128, 8, 21, 22, 23);
    // GCC cc1 实际触发的目标与右源别名指令。
    assert_decode(UINT32_C(0x6efe87fe), 128, 8, 30, 31, 30);
}

static void test_encoding_space(void) {
    assert((ADVSIMD_SUB_FIXED_MASK & ADVSIMD_SUB_VARIABLE_MASK) == 0);
    assert((ADVSIMD_SUB_FIXED_MASK | ADVSIMD_SUB_VARIABLE_MASK) ==
            UINT32_MAX);

    unsigned decoded_count = 0;
    unsigned reserved_count = 0;
    for (unsigned q = 0; q < 2; q++) {
        for (unsigned size = 0; size < 4; size++) {
            for (unsigned rm = 0; rm < 32; rm++) {
                for (unsigned rn = 0; rn < 32; rn++) {
                    for (unsigned rd = 0; rd < 32; rd++) {
                        struct aarch64_decoded instruction;
                        bool decoded = aarch64_decode(encode(q,
                                (byte_t) size, (byte_t) rm, (byte_t) rn,
                                (byte_t) rd), &instruction);
                        bool expected = q != 0 || size != 3;
                        assert(decoded == expected);
                        if (!decoded) {
                            reserved_count++;
                            continue;
                        }
                        decoded_count++;
                        assert(is_vector_sub(&instruction));
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
    assert(decoded_count == 229376);
    assert(reserved_count == 32768);
}

static void test_fixed_bits_and_neighbors(void) {
    const dword_t base = encode(true, 3, 30, 31, 30);
    for (unsigned bit = 0; bit < 32; bit++) {
        if ((ADVSIMD_SUB_FIXED_MASK & (UINT32_C(1) << bit)) == 0)
            continue;
        struct aarch64_decoded instruction;
        bool decoded = aarch64_decode(
                base ^ (UINT32_C(1) << bit), &instruction);
        assert(!decoded || !is_vector_sub(&instruction));
    }

    static const dword_t neighbors[] = {
        UINT32_C(0x0e228420), // 向量 ADD。
        UINT32_C(0x7ee28420), // 标量 SUB。
        UINT32_C(0x0e222c20), // 向量 SQSUB。
        UINT32_C(0x2e222c20), // 向量 UQSUB。
        UINT32_C(0x0e222420), // 向量 SHSUB。
        UINT32_C(0x2e222420), // 向量 UHSUB。
        UINT32_C(0x0e222020), // 向量 SSUBL。
        UINT32_C(0x2e222020), // 向量 USUBL。
        UINT32_C(0x0ea2d420), // 向量浮点 FSUB 2S。
        UINT32_C(0x4ee2d420), // 向量浮点 FSUB 2D。
        UINT32_C(0x1e623820), // 标量浮点 FSUB D。
        UINT32_C(0xcb020020), // 通用寄存器 SUB。
        UINT32_C(0x2e228c20), // 向量 CMEQ。
    };
    for (unsigned index = 0;
            index < sizeof(neighbors) / sizeof(neighbors[0]); index++) {
        struct aarch64_decoded instruction;
        bool decoded = aarch64_decode(neighbors[index], &instruction);
        assert(!decoded || !is_vector_sub(&instruction));
    }
}

static union aarch64_vector_reg reference_sub(
        union aarch64_vector_reg left, union aarch64_vector_reg right,
        byte_t width, byte_t element_size) {
    union aarch64_vector_reg result = {0};
    unsigned bytes = width / 8;
    if (element_size == 1) {
        for (unsigned lane = 0; lane < bytes; lane++)
            result.b[lane] = (byte_t) (
                    (qword_t) left.b[lane] - right.b[lane]);
    } else if (element_size == 2) {
        for (unsigned lane = 0; lane < bytes / 2; lane++)
            result.h[lane] = (word_t) (
                    (qword_t) left.h[lane] - right.h[lane]);
    } else if (element_size == 4) {
        for (unsigned lane = 0; lane < bytes / 4; lane++)
            result.s[lane] = (dword_t) (
                    (qword_t) left.s[lane] - right.s[lane]);
    } else {
        for (unsigned lane = 0; lane < bytes / 8; lane++)
            result.d[lane] = left.d[lane] - right.d[lane];
    }
    return result;
}

static void assert_execution(bool q, byte_t size,
        byte_t rm, byte_t rn, byte_t rd,
        union aarch64_vector_reg left,
        union aarch64_vector_reg right) {
    struct cpu_state cpu = {
        .cycle = UINT64_C(0x123456789abcdef0),
        .sp = UINT64_C(0x1122334455667788),
        .pc = UINT64_C(0x1800),
        .nzcv = UINT32_C(0xa0000000),
        .fpcr = UINT32_C(0x01000000),
        .fpsr = UINT32_C(0x08000010),
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
    if (rn == rm)
        right = left;
    cpu.v[rn] = left;
    cpu.v[rm] = right;

    byte_t width = q ? 128 : 64;
    byte_t element_size = (byte_t) (1U << size);
    union aarch64_vector_reg actual_left = cpu.v[rn];
    union aarch64_vector_reg actual_right = cpu.v[rm];
    struct cpu_state expected = cpu;
    expected.v[rd] = reference_sub(
            actual_left, actual_right, width, element_size);
    expected.pc += 4;

    execute_instruction(&cpu, encode(q, size, rm, rn, rd));
    assert(memcmp(&cpu, &expected, sizeof(cpu)) == 0);
}

static void test_execution_space(void) {
    const union aarch64_vector_reg left = {
        .d = {
            UINT64_C(0x000180007fff00ff),
            UINT64_C(0x80000000ffffffff),
        },
    };
    const union aarch64_vector_reg right = {
        .d = {
            UINT64_C(0x0101000180000100),
            UINT64_C(0x7fffffff00000001),
        },
    };
    for (unsigned q = 0; q < 2; q++) {
        for (unsigned size = 0; size < 4; size++) {
            if (q == 0 && size == 3)
                continue;
            for (unsigned rm = 0; rm < 32; rm++) {
                for (unsigned rn = 0; rn < 32; rn++) {
                    for (unsigned rd = 0; rd < 32; rd++) {
                        assert_execution(q, (byte_t) size,
                                (byte_t) rm, (byte_t) rn, (byte_t) rd,
                                left, right);
                    }
                }
            }
        }
    }
}

static void test_boundaries_and_product(void) {
    static const union aarch64_vector_reg boundaries[][2] = {
        {
            {.d = {0, 0}},
            {.d = {UINT64_MAX, UINT64_MAX}},
        },
        {
            {.d = {
                UINT64_C(0x8000000080008080),
                UINT64_C(0x7fffffff7fff7f7f),
            }},
            {.d = {
                UINT64_C(0x0000000100010101),
                UINT64_C(0x8000000080008080),
            }},
        },
        {
            {.d = {
                UINT64_C(0x00004488cd11569e),
                0,
            }},
            {.d = {
                UINT64_C(0x00004488cd115599),
                UINT64_C(0x022266aaef3377bb),
            }},
        },
    };
    static const byte_t registers[][3] = {
        {2, 1, 0},
        {2, 1, 1},
        {2, 1, 2},
        {1, 1, 2},
        {31, 31, 31},
        {30, 31, 30},
    };

    for (unsigned q = 0; q < 2; q++) {
        for (unsigned size = 0; size < 4; size++) {
            if (q == 0 && size == 3)
                continue;
            for (unsigned value = 0;
                    value < sizeof(boundaries) / sizeof(boundaries[0]);
                    value++) {
                for (unsigned form = 0;
                        form < sizeof(registers) / sizeof(registers[0]);
                        form++) {
                    assert_execution(q, (byte_t) size,
                            registers[form][0],
                            registers[form][1],
                            registers[form][2],
                            boundaries[value][0],
                            boundaries[value][1]);
                }
            }
        }
    }

    union aarch64_vector_reg product = reference_sub(
            boundaries[2][0], boundaries[2][1], 128, 8);
    assert(product.d[0] == UINT64_C(0x0000000000000105));
    assert(product.d[1] == UINT64_C(0xfddd995510cc8845));
}

int main(void) {
    test_llvm_vectors();
    test_encoding_space();
    test_fixed_bits_and_neighbors();
    test_execution_space();
    test_boundaries_and_product();
    return 0;
}
