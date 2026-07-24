#include <assert.h>
#include <string.h>

#include "guest/aarch64/decode.h"
#include "guest/aarch64/execute.h"

#define ADVSIMD_ADD_FIXED_MASK UINT32_C(0xbf20fc00)
#define ADVSIMD_ADD_FIXED_BITS UINT32_C(0x0e208400)
#define ADVSIMD_ADD_VARIABLE_MASK UINT32_C(0x40df03ff)
#define ADVSIMD_ADDV_FIXED_MASK UINT32_C(0xbf3ffc00)
#define ADVSIMD_ADDV_FIXED_BITS UINT32_C(0x0e31b800)
#define ADVSIMD_ADDV_VARIABLE_MASK UINT32_C(0x40c003ff)
#define ADVSIMD_ADDP_SCALAR_FIXED_MASK UINT32_C(0xfffffc00)
#define ADVSIMD_ADDP_SCALAR_FIXED_BITS UINT32_C(0x5ef1b800)
#define ADVSIMD_ADDP_SCALAR_VARIABLE_MASK UINT32_C(0x000003ff)

static dword_t encode(bool q, byte_t size, byte_t rm, byte_t rn, byte_t rd) {
    return ADVSIMD_ADD_FIXED_BITS |
            (dword_t) q << 30 |
            (dword_t) size << 22 |
            (dword_t) rm << 16 |
            (dword_t) rn << 5 |
            rd;
}

static dword_t encode_scalar_addp(byte_t rn, byte_t rd) {
    return ADVSIMD_ADDP_SCALAR_FIXED_BITS |
            (dword_t) rn << 5 | rd;
}

static dword_t encode_addv(bool q, byte_t size, byte_t rn, byte_t rd) {
    return ADVSIMD_ADDV_FIXED_BITS |
            (dword_t) q << 30 |
            (dword_t) size << 22 |
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

static void execute_instruction(struct cpu_state *cpu, dword_t word) {
    struct aarch64_decoded instruction = decode(word);
    struct aarch64_execute_result result =
            aarch64_execute(cpu, NULL, &instruction);
    assert(result.stop == AARCH64_EXECUTE_RETIRED);
}

static void assert_decode(dword_t word, byte_t width, byte_t element_size,
        byte_t rd, byte_t rn, byte_t rm) {
    struct aarch64_decoded instruction = decode(word);
    assert(instruction.opcode == AARCH64_OP_ADVSIMD_ADD);
    assert(instruction.width == width);
    assert(instruction.operands.advsimd_three_same.rd == rd);
    assert(instruction.operands.advsimd_three_same.rn == rn);
    assert(instruction.operands.advsimd_three_same.rm == rm);
    assert(instruction.operands.advsimd_three_same.element_size ==
            element_size);
}

static void test_llvm_vectors(void) {
    assert_decode(UINT32_C(0x0e258483), 64, 1, 3, 4, 5);
    assert_decode(UINT32_C(0x4e2884e6), 128, 1, 6, 7, 8);
    assert_decode(UINT32_C(0x0e6b8549), 64, 2, 9, 10, 11);
    assert_decode(UINT32_C(0x4e6e85ac), 128, 2, 12, 13, 14);
    assert_decode(UINT32_C(0x0eb1860f), 64, 4, 15, 16, 17);
    assert_decode(UINT32_C(0x4eb48672), 128, 4, 18, 19, 20);
    assert_decode(UINT32_C(0x4ef786d5), 128, 8, 21, 22, 23);
    assert_decode(UINT32_C(0x4eff879c), 128, 8, 28, 28, 31);
}

static void test_encoding_space(void) {
    assert((ADVSIMD_ADD_FIXED_MASK & ADVSIMD_ADD_VARIABLE_MASK) == 0);
    assert((ADVSIMD_ADD_FIXED_MASK | ADVSIMD_ADD_VARIABLE_MASK) ==
            UINT32_MAX);
    unsigned decoded_count = 0;
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
                        if (!decoded)
                            continue;
                        decoded_count++;
                        assert(instruction.opcode == AARCH64_OP_ADVSIMD_ADD);
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
}

static void test_fixed_bits(void) {
    dword_t base = encode(true, 3, 2, 1, 0);
    for (unsigned bit = 0; bit < 32; bit++) {
        if ((ADVSIMD_ADD_FIXED_MASK & (UINT32_C(1) << bit)) == 0)
            continue;
        struct aarch64_decoded instruction;
        bool decoded = aarch64_decode(base ^ (UINT32_C(1) << bit),
                &instruction);
        assert(!decoded || instruction.opcode != AARCH64_OP_ADVSIMD_ADD);
    }
}

static void assert_addv_decode(dword_t word, byte_t width,
        byte_t element_size, byte_t rd, byte_t rn) {
    struct aarch64_decoded instruction = decode(word);
    assert(instruction.opcode == AARCH64_OP_ADVSIMD_ADDV);
    assert(instruction.width == width);
    assert(instruction.operands.advsimd_across_lanes.rd == rd);
    assert(instruction.operands.advsimd_across_lanes.rn == rn);
    assert(instruction.operands.advsimd_across_lanes.element_size ==
            element_size);
}

static void test_addv_decode(void) {
    assert_addv_decode(UINT32_C(0x0e31b820), 64, 1, 0, 1);
    assert_addv_decode(UINT32_C(0x4e31b883), 128, 1, 3, 4);
    assert_addv_decode(UINT32_C(0x0e71b8c5), 64, 2, 5, 6);
    assert_addv_decode(UINT32_C(0x4e71b907), 128, 2, 7, 8);
    assert_addv_decode(UINT32_C(0x4eb1bbff), 128, 4, 31, 31);
    // GCC cc1 实际触发的 V31 自别名指令。
    assert_addv_decode(UINT32_C(0x4e71bbff), 128, 2, 31, 31);

    assert((ADVSIMD_ADDV_FIXED_MASK &
            ADVSIMD_ADDV_VARIABLE_MASK) == 0);
    assert((ADVSIMD_ADDV_FIXED_MASK |
            ADVSIMD_ADDV_VARIABLE_MASK) == UINT32_MAX);
    unsigned decoded_count = 0;
    unsigned reserved_count = 0;
    for (unsigned q = 0; q < 2; q++) {
        for (unsigned size = 0; size < 4; size++) {
            for (unsigned rn = 0; rn < 32; rn++) {
                for (unsigned rd = 0; rd < 32; rd++) {
                    struct aarch64_decoded instruction;
                    bool decoded = aarch64_decode(encode_addv(q,
                            (byte_t) size, (byte_t) rn, (byte_t) rd),
                            &instruction);
                    bool expected = size < 2 || (q != 0 && size == 2);
                    assert(decoded == expected);
                    if (!expected) {
                        reserved_count++;
                        continue;
                    }
                    decoded_count++;
                    assert(instruction.opcode == AARCH64_OP_ADVSIMD_ADDV);
                    assert(instruction.width == (q ? 128 : 64));
                    assert(instruction.operands.advsimd_across_lanes.rd ==
                            rd);
                    assert(instruction.operands.advsimd_across_lanes.rn ==
                            rn);
                    assert(instruction.operands.advsimd_across_lanes.
                            element_size == (1U << size));
                }
            }
        }
    }
    assert(decoded_count == 5120);
    assert(reserved_count == 3072);

    dword_t base = encode_addv(true, 1, 31, 31);
    for (unsigned bit = 0; bit < 32; bit++) {
        if ((ADVSIMD_ADDV_FIXED_MASK & (UINT32_C(1) << bit)) == 0)
            continue;
        struct aarch64_decoded instruction;
        bool decoded = aarch64_decode(base ^ (UINT32_C(1) << bit),
                &instruction);
        assert(!decoded || instruction.opcode != AARCH64_OP_ADVSIMD_ADDV);
    }

    static const dword_t neighbors[] = {
        UINT32_C(0x0e303820), // SADDLV。
        UINT32_C(0x2e303820), // UADDLV。
        UINT32_C(0x0e30a820), // SMAXV。
        UINT32_C(0x2e30a820), // UMAXV。
        UINT32_C(0x0e31a820), // SMINV。
        UINT32_C(0x2e31a820), // UMINV。
        UINT32_C(0x2e31b820), // bit 29 保留编码。
        UINT32_C(0x5ef1b820), // 标量整数 ADDP。
        UINT32_C(0x7e70d820), // 标量浮点 FADDP。
    };
    for (unsigned index = 0;
            index < sizeof(neighbors) / sizeof(neighbors[0]); index++) {
        struct aarch64_decoded instruction;
        bool decoded = aarch64_decode(neighbors[index], &instruction);
        assert(!decoded || instruction.opcode != AARCH64_OP_ADVSIMD_ADDV);
    }
}

static void assert_scalar_addp_decode(
        dword_t word, byte_t rd, byte_t rn) {
    struct aarch64_decoded instruction = decode(word);
    assert(instruction.opcode == AARCH64_OP_ADVSIMD_ADDP_SCALAR);
    assert(instruction.width == 64);
    assert(instruction.operands.advsimd_unary.rd == rd);
    assert(instruction.operands.advsimd_unary.rn == rn);
}

static void test_scalar_addp_decode(void) {
    assert_scalar_addp_decode(UINT32_C(0x5ef1b820), 0, 1);
    assert_scalar_addp_decode(UINT32_C(0x5ef1bbbf), 31, 29);
    assert_scalar_addp_decode(UINT32_C(0x5ef1bbff), 31, 31);

    assert((ADVSIMD_ADDP_SCALAR_FIXED_MASK &
            ADVSIMD_ADDP_SCALAR_VARIABLE_MASK) == 0);
    assert((ADVSIMD_ADDP_SCALAR_FIXED_MASK |
            ADVSIMD_ADDP_SCALAR_VARIABLE_MASK) == UINT32_MAX);
    unsigned decoded_count = 0;
    for (unsigned rn = 0; rn < 32; rn++) {
        for (unsigned rd = 0; rd < 32; rd++) {
            assert_scalar_addp_decode(
                    encode_scalar_addp((byte_t) rn, (byte_t) rd),
                    (byte_t) rd, (byte_t) rn);
            decoded_count++;
        }
    }
    assert(decoded_count == 1024);

    const dword_t base = encode_scalar_addp(29, 31);
    for (unsigned bit = 0; bit < 32; bit++) {
        if ((ADVSIMD_ADDP_SCALAR_FIXED_MASK &
                (UINT32_C(1) << bit)) == 0)
            continue;
        struct aarch64_decoded instruction;
        bool decoded = aarch64_decode(
                base ^ (UINT32_C(1) << bit), &instruction);
        assert(!decoded ||
                instruction.opcode != AARCH64_OP_ADVSIMD_ADDP_SCALAR);
    }

    // 标量整数 ADDP 只有 2D arrangement。
    for (unsigned size = 0; size < 3; size++) {
        dword_t word = (base & ~UINT32_C(0x00c00000)) |
                (dword_t) size << 22;
        struct aarch64_decoded instruction;
        bool decoded = aarch64_decode(word, &instruction);
        assert(!decoded ||
                instruction.opcode != AARCH64_OP_ADVSIMD_ADDP_SCALAR);
    }

    static const dword_t neighbors[] = {
        UINT32_C(0x4ee2bc20), // 三操作数向量 ADDP。
        UINT32_C(0x4eb1b820), // 向量横向 ADDV。
        UINT32_C(0x7e30d820), // 标量浮点 FADDP S。
        UINT32_C(0x7e70d820), // 标量浮点 FADDP D。
        UINT32_C(0x7e70f820), // 标量浮点 FMAXP D。
        UINT32_C(0x7ef0f820), // 标量浮点 FMINP D。
    };
    for (unsigned index = 0;
            index < sizeof(neighbors) / sizeof(neighbors[0]); index++) {
        struct aarch64_decoded instruction;
        bool decoded = aarch64_decode(neighbors[index], &instruction);
        assert(!decoded ||
                instruction.opcode != AARCH64_OP_ADVSIMD_ADDP_SCALAR);
    }
}

static union aarch64_vector_reg reference_add(
        union aarch64_vector_reg left, union aarch64_vector_reg right,
        byte_t width, byte_t element_size) {
    union aarch64_vector_reg result = {0};
    unsigned bytes = width / 8;
    if (element_size == 1) {
        for (unsigned lane = 0; lane < bytes; lane++)
            result.b[lane] = (byte_t) (left.b[lane] + right.b[lane]);
    } else if (element_size == 2) {
        for (unsigned lane = 0; lane < bytes / 2; lane++)
            result.h[lane] = (word_t) (left.h[lane] + right.h[lane]);
    } else if (element_size == 4) {
        for (unsigned lane = 0; lane < bytes / 4; lane++)
            result.s[lane] = left.s[lane] + right.s[lane];
    } else {
        for (unsigned lane = 0; lane < bytes / 8; lane++)
            result.d[lane] = left.d[lane] + right.d[lane];
    }
    return result;
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
        {1, 1, 1},
        {31, 30, 29},
        {28, 28, 31},
    };
    for (unsigned q = 0; q < 2; q++) {
        for (unsigned size = 0; size < 4; size++) {
            if (q == 0 && size == 3)
                continue;
            byte_t width = q ? 128 : 64;
            byte_t element_size = (byte_t) (1U << size);
            for (unsigned form = 0; form <
                    sizeof(registers) / sizeof(registers[0]); form++) {
                struct cpu_state cpu = {
                    .pc = UINT64_C(0x1000),
                    .sp = UINT64_C(0x1122334455667788),
                    .nzcv = UINT32_C(0xa0000000),
                    .fpcr = UINT32_C(0x01000000),
                    .fpsr = UINT32_C(0x08000000),
                };
                union aarch64_vector_reg left = {
                    .d = {UINT64_C(0xff00fffffffffffe),
                            UINT64_C(0x7fffffffffffffff)},
                };
                union aarch64_vector_reg right = {
                    .d = {UINT64_C(0x0101000100000005),
                            UINT64_C(0x8000000000000001)},
                };
                const byte_t rd = registers[form].rd;
                const byte_t rn = registers[form].rn;
                const byte_t rm = registers[form].rm;
                if (rn == rm)
                    right = left;
                cpu.v[rn] = left;
                cpu.v[rm] = right;
                cpu.v[27].d[0] = UINT64_C(0x13579bdf2468ace0);
                cpu.v[27].d[1] = UINT64_C(0x02468ace13579bdf);

                union aarch64_vector_reg expected = reference_add(
                        left, right, width, element_size);
                execute_instruction(&cpu, encode(q, (byte_t) size,
                        rm, rn, rd));
                assert(memcmp(&cpu.v[rd], &expected, sizeof(expected)) == 0);
                if (rd != 27) {
                    assert(cpu.v[27].d[0] ==
                            UINT64_C(0x13579bdf2468ace0));
                    assert(cpu.v[27].d[1] ==
                            UINT64_C(0x02468ace13579bdf));
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

static void assert_scalar_addp_execution(byte_t rn, byte_t rd,
        qword_t left, qword_t right) {
    struct cpu_state cpu = {
        .cycle = UINT64_C(0x123456789abcdef0),
        .sp = UINT64_C(0x1122334455667788),
        .pc = UINT64_C(0x2000),
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
    cpu.x[0] = UINT64_C(0xfedcba9876543210);
    for (unsigned reg = 0; reg < 32; reg++) {
        cpu.v[reg].d[0] = UINT64_C(0x0102030405060708) +
                reg * UINT64_C(0x1111111111111111);
        cpu.v[reg].d[1] = UINT64_C(0xf0e0d0c0b0a09080) ^
                (reg * UINT64_C(0x0101010101010101));
    }
    if (rd != rn)
        cpu.v[rd].q = ~(__uint128_t) 0;
    cpu.v[rn].d[0] = left;
    cpu.v[rn].d[1] = right;

    struct cpu_state expected = cpu;
    expected.v[rd] = (union aarch64_vector_reg) {
        .d = {left + right, 0},
    };
    expected.pc += 4;

    execute_instruction(&cpu, encode_scalar_addp(rn, rd));
    assert(memcmp(&cpu, &expected, sizeof(cpu)) == 0);
}

static void test_scalar_addp_execution(void) {
    for (unsigned rn = 0; rn < 32; rn++) {
        for (unsigned rd = 0; rd < 32; rd++) {
            qword_t left = UINT64_MAX -
                    rn * UINT64_C(0x0102030405060708);
            qword_t right = UINT64_C(0x8000000000000001) +
                    rd * UINT64_C(0x0001000100010001);
            assert_scalar_addp_execution(
                    (byte_t) rn, (byte_t) rd, left, right);
        }
    }

    static const qword_t boundaries[][2] = {
        {0, 0},
        {1, 2},
        {UINT64_MAX, 1},
        {UINT64_C(0x8000000000000000),
                UINT64_C(0x8000000000000000)},
        {UINT64_C(0x7fffffffffffffff),
                UINT64_C(0x7fffffffffffffff)},
    };
    static const byte_t registers[][2] = {
        {1, 2},
        {2, 2},
        {29, 31},
        {31, 29},
        {31, 31},
    };
    for (unsigned value = 0;
            value < sizeof(boundaries) / sizeof(boundaries[0]); value++) {
        for (unsigned form = 0;
                form < sizeof(registers) / sizeof(registers[0]); form++) {
            assert_scalar_addp_execution(
                    registers[form][0], registers[form][1],
                    boundaries[value][0], boundaries[value][1]);
        }
    }

    assert_scalar_addp_execution(
            29, 31, UINT64_C(0x7f7f7f7f7f7e7f7f),
            UINT64_C(0x7f7f7f7f7f7f7f7f));
}

static union aarch64_vector_reg reference_addv(
        union aarch64_vector_reg source, byte_t width,
        byte_t element_size) {
    unsigned lanes = width / (element_size * 8);
    qword_t sum = 0;
    for (unsigned lane = 0; lane < lanes; lane++) {
        if (element_size == 1)
            sum += source.b[lane];
        else if (element_size == 2)
            sum += source.h[lane];
        else
            sum += source.s[lane];
    }

    union aarch64_vector_reg result = {0};
    if (element_size == 1)
        result.b[0] = (byte_t) sum;
    else if (element_size == 2)
        result.h[0] = (word_t) sum;
    else
        result.s[0] = (dword_t) sum;
    return result;
}

static void assert_addv_execution(bool q, byte_t size,
        byte_t rn, byte_t rd, union aarch64_vector_reg source) {
    struct cpu_state cpu = {
        .cycle = UINT64_C(0x123456789abcdef0),
        .sp = UINT64_C(0x1122334455667788),
        .pc = UINT64_C(0x3000),
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
    cpu.x[0] = UINT64_C(0xfedcba9876543210);
    for (unsigned reg = 0; reg < 32; reg++) {
        cpu.v[reg].d[0] = UINT64_C(0x0102030405060708) +
                reg * UINT64_C(0x1111111111111111);
        cpu.v[reg].d[1] = UINT64_C(0xf0e0d0c0b0a09080) ^
                (reg * UINT64_C(0x0101010101010101));
    }
    if (rd != rn)
        cpu.v[rd].q = ~(__uint128_t) 0;
    cpu.v[rn] = source;

    byte_t width = q ? 128 : 64;
    byte_t element_size = (byte_t) (1U << size);
    struct cpu_state expected = cpu;
    expected.v[rd] = reference_addv(source, width, element_size);
    expected.pc += 4;

    execute_instruction(&cpu, encode_addv(q, size, rn, rd));
    assert(memcmp(&cpu, &expected, sizeof(cpu)) == 0);
}

static void test_addv_execution(void) {
    const union aarch64_vector_reg source = {
        .d = {
            UINT64_C(0x80017fff0001ffff),
            UINT64_C(0x678aaaaaedcc1234),
        },
    };
    for (unsigned q = 0; q < 2; q++) {
        for (unsigned size = 0; size < 3; size++) {
            if (!q && size == 2)
                continue;
            for (unsigned rn = 0; rn < 32; rn++) {
                for (unsigned rd = 0; rd < 32; rd++) {
                    assert_addv_execution(q, (byte_t) size,
                            (byte_t) rn, (byte_t) rd, source);
                }
            }
        }
    }

    // 8H 自别名覆盖全部 lane、模 16 位回绕和标量高位清零。
    assert_addv_execution(true, 1, 31, 31,
            (union aarch64_vector_reg) {
                .d = {
                    UINT64_C(0x80017fff0001ffff),
                    UINT64_C(0x678aaaaaedcc1234),
                },
            });
}

int main(void) {
    test_llvm_vectors();
    test_encoding_space();
    test_fixed_bits();
    test_addv_decode();
    test_scalar_addp_decode();
    test_execution_space();
    test_scalar_addp_execution();
    test_addv_execution();
    return 0;
}
