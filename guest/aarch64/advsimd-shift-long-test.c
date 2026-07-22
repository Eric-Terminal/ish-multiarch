#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#include "guest/aarch64/decode.h"
#include "guest/aarch64/execute.h"

static dword_t encode(bool is_unsigned, bool upper,
        byte_t element_size, byte_t shift, byte_t rn, byte_t rd) {
    byte_t immediate = (byte_t) (element_size * 8 + shift);
    return UINT32_C(0x0f00a400) |
            (dword_t) upper << 30 |
            (dword_t) is_unsigned << 29 |
            (dword_t) immediate << 16 |
            (dword_t) rn << 5 | rd;
}

static struct aarch64_decoded decode(dword_t word) {
    struct aarch64_decoded instruction;
    assert(aarch64_decode(word, &instruction));
    return instruction;
}

static void execute_instruction(struct cpu_state *cpu, dword_t word) {
    struct aarch64_decoded instruction = decode(word);
    struct aarch64_execute_result result =
            aarch64_execute(cpu, NULL, &instruction);
    assert(result.stop == AARCH64_EXECUTE_RETIRED);
}

static bool is_shift_long_opcode(enum aarch64_opcode opcode) {
    return opcode == AARCH64_OP_ADVSIMD_SSHLL ||
            opcode == AARCH64_OP_ADVSIMD_SSHLL2 ||
            opcode == AARCH64_OP_ADVSIMD_USHLL ||
            opcode == AARCH64_OP_ADVSIMD_USHLL2;
}

static void assert_decode(dword_t word, bool is_unsigned, bool upper,
        byte_t element_size, byte_t shift, byte_t rn, byte_t rd) {
    struct aarch64_decoded instruction = decode(word);
    enum aarch64_opcode expected = is_unsigned ?
            (upper ? AARCH64_OP_ADVSIMD_USHLL2 :
                    AARCH64_OP_ADVSIMD_USHLL) :
            (upper ? AARCH64_OP_ADVSIMD_SSHLL2 :
                    AARCH64_OP_ADVSIMD_SSHLL);
    assert(instruction.opcode == expected);
    assert(instruction.width == 128);
    assert(instruction.operands.advsimd_shift_long.rd == rd);
    assert(instruction.operands.advsimd_shift_long.rn == rn);
    assert(instruction.operands.advsimd_shift_long.element_size ==
            element_size);
    assert(instruction.operands.advsimd_shift_long.shift == shift);
}

static void test_decode_and_neighbors(void) {
    assert_decode(UINT32_C(0x0f08a420), false, false, 1, 0, 1, 0);
    assert_decode(UINT32_C(0x4f0fa4e6), false, true, 1, 7, 7, 6);
    assert_decode(UINT32_C(0x0f10a528), false, false, 2, 0, 9, 8);
    assert_decode(UINT32_C(0x4f1fa5ee), false, true, 2, 15, 15, 14);
    assert_decode(UINT32_C(0x0f20a630), false, false, 4, 0, 17, 16);
    assert_decode(UINT32_C(0x4f3fa6f6), false, true, 4, 31, 23, 22);
    assert_decode(UINT32_C(0x0f20a5ef), false, false, 4, 0, 15, 15);
    assert_decode(UINT32_C(0x2f20a7ff), true, false, 4, 0, 31, 31);

    static const byte_t element_sizes[] = {1, 2, 4};
    unsigned legal = 0;
    for (unsigned is_unsigned = 0; is_unsigned < 2; is_unsigned++) {
        for (unsigned upper = 0; upper < 2; upper++) {
            for (unsigned size_index = 0;
                    size_index < sizeof(element_sizes) /
                            sizeof(element_sizes[0]); size_index++) {
                byte_t element_size = element_sizes[size_index];
                for (unsigned shift = 0;
                        shift < element_size * 8; shift++) {
                    for (unsigned rn = 0; rn < 32; rn++) {
                        for (unsigned rd = 0; rd < 32; rd++) {
                            assert_decode(encode(is_unsigned != 0,
                                    upper != 0, element_size,
                                    (byte_t) shift, (byte_t) rn,
                                    (byte_t) rd), is_unsigned != 0,
                                    upper != 0, element_size,
                                    (byte_t) shift, (byte_t) rn,
                                    (byte_t) rd);
                            legal++;
                        }
                    }
                }
            }
        }
    }
    assert(legal == 229376);

    unsigned overlap = 0;
    for (unsigned is_unsigned = 0; is_unsigned < 2; is_unsigned++) {
        for (unsigned upper = 0; upper < 2; upper++) {
            for (unsigned immediate = 0; immediate < 8; immediate++) {
                for (unsigned rn = 0; rn < 32; rn++) {
                    for (unsigned rd = 0; rd < 32; rd++) {
                        dword_t word = UINT32_C(0x0f00a400) |
                                (dword_t) upper << 30 |
                                (dword_t) is_unsigned << 29 |
                                (dword_t) immediate << 16 |
                                (dword_t) rn << 5 | (dword_t) rd;
                        struct aarch64_decoded instruction;
                        assert(aarch64_decode(word, &instruction));
                        assert(instruction.opcode == (is_unsigned ?
                                AARCH64_OP_ADVSIMD_MVNI :
                                AARCH64_OP_ADVSIMD_MOVI));
                        assert(instruction.width == (upper ? 128 : 64));
                        overlap++;
                    }
                }
            }
        }
    }
    assert(overlap == 32768);

    unsigned reserved = 0;
    for (unsigned is_unsigned = 0; is_unsigned < 2; is_unsigned++) {
        for (unsigned upper = 0; upper < 2; upper++) {
            for (unsigned immediate = 64; immediate < 128; immediate++) {
                for (unsigned rn = 0; rn < 32; rn++) {
                    for (unsigned rd = 0; rd < 32; rd++) {
                        dword_t word = UINT32_C(0x0f00a400) |
                                (dword_t) upper << 30 |
                                (dword_t) is_unsigned << 29 |
                                (dword_t) immediate << 16 |
                                (dword_t) rn << 5 | (dword_t) rd;
                        struct aarch64_decoded instruction;
                        assert(!aarch64_decode(word, &instruction));
                        reserved++;
                    }
                }
            }
        }
    }
    assert(reserved == 262144);

    const dword_t fixed_mask = UINT32_C(0x9f80fc00);
    const dword_t base = UINT32_C(0x0f20a5ef);
    for (unsigned bit = 0; bit < 32; bit++) {
        if ((fixed_mask & (UINT32_C(1) << bit)) == 0)
            continue;
        struct aarch64_decoded instruction;
        bool decoded = aarch64_decode(
                base ^ (UINT32_C(1) << bit), &instruction);
        assert(!decoded || !is_shift_long_opcode(instruction.opcode));
    }
}

static qword_t lane_mask(byte_t element_size) {
    return element_size == 8 ? UINT64_MAX :
            (UINT64_C(1) << (element_size * 8)) - 1;
}

static qword_t read_lane(const union aarch64_vector_reg *reg,
        byte_t element_size, byte_t lane) {
    if (element_size == 1)
        return reg->b[lane];
    if (element_size == 2)
        return reg->h[lane];
    if (element_size == 4)
        return reg->s[lane];
    return reg->d[lane];
}

static void write_lane(union aarch64_vector_reg *reg,
        byte_t element_size, byte_t lane, qword_t value) {
    if (element_size == 1)
        reg->b[lane] = (byte_t) value;
    else if (element_size == 2)
        reg->h[lane] = (word_t) value;
    else if (element_size == 4)
        reg->s[lane] = (dword_t) value;
    else
        reg->d[lane] = value;
}

// 用有符号数学乘法构造预言机，避免复刻执行器的位扩展路径。
static qword_t reference_lane(
        qword_t value, bool is_unsigned,
        byte_t source_size, byte_t shift) {
    unsigned source_bits = source_size * 8;
    qword_t source_mask = lane_mask(source_size);
    qword_t source_sign = UINT64_C(1) << (source_bits - 1);
    __int128 signed_value = (__int128) (value & source_mask);
    if (!is_unsigned && (value & source_sign))
        signed_value -= (__int128) 1 << source_bits;
    signed_value *= (__int128) 1 << shift;
    return (qword_t) signed_value & lane_mask(source_size * 2);
}

static union aarch64_vector_reg make_source(
        byte_t element_size, byte_t shift) {
    qword_t mask = lane_mask(element_size);
    qword_t sign = UINT64_C(1) << (element_size * 8 - 1);
    qword_t patterns[] = {
        0,
        1,
        2,
        sign - 1,
        sign,
        sign + 1,
        mask - 1,
        mask,
    };
    union aarch64_vector_reg source = {0};
    byte_t lanes = 16 / element_size;
    for (byte_t lane = 0; lane < lanes; lane++)
        write_lane(&source, element_size, lane,
                patterns[(lane + shift) % 8]);
    return source;
}

static union aarch64_vector_reg expected_result(
        const union aarch64_vector_reg *source,
        bool is_unsigned, bool upper,
        byte_t source_size, byte_t shift) {
    byte_t lanes = 8 / source_size;
    byte_t source_offset = upper ? lanes : 0;
    union aarch64_vector_reg result = {0};
    for (byte_t lane = 0; lane < lanes; lane++) {
        qword_t value = read_lane(source, source_size,
                (byte_t) (source_offset + lane));
        write_lane(&result, source_size * 2, lane,
                reference_lane(value, is_unsigned,
                        source_size, shift));
    }
    return result;
}

static void assert_stable_state(const struct cpu_state *cpu, qword_t pc) {
    assert(cpu->pc == pc);
    assert(cpu->x[0] == UINT64_C(0x0123456789abcdef));
    assert(cpu->sp == UINT64_C(0xfedcba9876543210));
    assert(cpu->nzcv == UINT32_C(0xa0000000));
    assert(cpu->fpcr == UINT32_C(0x01000000));
    assert(cpu->fpsr == UINT32_C(0x08000000));
}

static void test_execution_space(void) {
    static const byte_t element_sizes[] = {1, 2, 4};
    for (unsigned is_unsigned = 0; is_unsigned < 2; is_unsigned++) {
        for (unsigned upper = 0; upper < 2; upper++) {
            for (unsigned size_index = 0;
                    size_index < sizeof(element_sizes) /
                            sizeof(element_sizes[0]); size_index++) {
                byte_t element_size = element_sizes[size_index];
                for (unsigned shift = 0;
                        shift < element_size * 8; shift++) {
                    union aarch64_vector_reg source =
                            make_source(element_size, (byte_t) shift);
                    union aarch64_vector_reg expected = expected_result(
                            &source, is_unsigned != 0, upper != 0,
                            element_size, (byte_t) shift);
                    struct cpu_state cpu = {
                        .pc = UINT64_C(0x1000),
                        .sp = UINT64_C(0xfedcba9876543210),
                        .nzcv = UINT32_C(0xa0000000),
                        .fpcr = UINT32_C(0x01000000),
                        .fpsr = UINT32_C(0x08000000),
                    };
                    cpu.x[0] = UINT64_C(0x0123456789abcdef);
                    cpu.v[1] = source;
                    cpu.v[2].d[0] = UINT64_MAX;
                    cpu.v[2].d[1] = UINT64_MAX;
                    execute_instruction(&cpu, encode(is_unsigned != 0,
                            upper != 0, element_size,
                            (byte_t) shift, 1, 2));
                    assert(cpu.v[2].d[0] == expected.d[0]);
                    assert(cpu.v[2].d[1] == expected.d[1]);
                    assert(cpu.v[1].d[0] == source.d[0]);
                    assert(cpu.v[1].d[1] == source.d[1]);
                    assert_stable_state(&cpu, UINT64_C(0x1004));

                    cpu.pc = UINT64_C(0x1800);
                    cpu.v[2] = source;
                    execute_instruction(&cpu, encode(is_unsigned != 0,
                            upper != 0, element_size,
                            (byte_t) shift, 2, 2));
                    assert(cpu.v[2].d[0] == expected.d[0]);
                    assert(cpu.v[2].d[1] == expected.d[1]);
                    assert_stable_state(&cpu, UINT64_C(0x1804));
                }
            }
        }
    }
}

static void test_real_path(void) {
    struct cpu_state cpu = {
        .pc = UINT64_C(0x2000),
        .nzcv = UINT32_C(0xa0000000),
    };
    cpu.v[15].s[0] = UINT32_C(0x80000000);
    cpu.v[15].s[1] = UINT32_C(0x7fffffff);
    cpu.v[15].s[2] = UINT32_C(0xffffffff);
    cpu.v[15].s[3] = 1;
    execute_instruction(&cpu, UINT32_C(0x0f20a5ef));
    assert(cpu.v[15].d[0] == UINT64_C(0xffffffff80000000));
    assert(cpu.v[15].d[1] == UINT64_C(0x000000007fffffff));
    assert(cpu.nzcv == UINT32_C(0xa0000000));
    assert(cpu.pc == UINT64_C(0x2004));

    cpu.pc = UINT64_C(0x2800);
    cpu.v[31].s[0] = UINT32_C(0x80000000);
    cpu.v[31].s[1] = UINT32_C(0xffffffff);
    execute_instruction(&cpu, UINT32_C(0x2f20a7ff));
    assert(cpu.v[31].d[0] == UINT64_C(0x0000000080000000));
    assert(cpu.v[31].d[1] == UINT64_C(0x00000000ffffffff));
    assert(cpu.nzcv == UINT32_C(0xa0000000));
    assert(cpu.pc == UINT64_C(0x2804));
}

int main(void) {
    test_decode_and_neighbors();
    test_execution_space();
    test_real_path();
    return 0;
}
