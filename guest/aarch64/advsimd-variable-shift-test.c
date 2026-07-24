#include <assert.h>
#include <string.h>

#include "guest/aarch64/decode.h"
#include "guest/aarch64/execute.h"

#define ADVSIMD_VARIABLE_SHIFT_FIXED_MASK UINT32_C(0x9f20fc00)
#define ADVSIMD_VARIABLE_SHIFT_FIXED_BITS UINT32_C(0x0e204400)
#define ADVSIMD_VARIABLE_SHIFT_VARIABLE_MASK UINT32_C(0x60df03ff)

static dword_t encode(bool q, bool u, byte_t size,
        byte_t rm, byte_t rn, byte_t rd) {
    return ADVSIMD_VARIABLE_SHIFT_FIXED_BITS |
            (dword_t) q << 30 |
            (dword_t) u << 29 |
            (dword_t) size << 22 |
            (dword_t) rm << 16 |
            (dword_t) rn << 5 |
            rd;
}

static enum aarch64_opcode opcode_for(bool u) {
    return u ? AARCH64_OP_ADVSIMD_USHL : AARCH64_OP_ADVSIMD_SSHL;
}

static bool is_variable_shift_opcode(enum aarch64_opcode opcode) {
    return opcode == AARCH64_OP_ADVSIMD_SSHL ||
            opcode == AARCH64_OP_ADVSIMD_USHL;
}

static struct aarch64_decoded decode(dword_t word) {
    struct aarch64_decoded instruction = {0};
    bool decoded = aarch64_decode(word, &instruction);
    assert(decoded);
    use(decoded);
    return instruction;
}

static void assert_decode(dword_t word, enum aarch64_opcode opcode,
        byte_t width, byte_t element_size,
        byte_t rd, byte_t rn, byte_t rm) {
    struct aarch64_decoded instruction = decode(word);
    assert(instruction.opcode == opcode);
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
    // 由本地 Apple LLVM 汇编器生成，覆盖 SSHL/USHL 的全部 arrangement。
    assert_decode(UINT32_C(0x0e224420), AARCH64_OP_ADVSIMD_SSHL,
            64, 1, 0, 1, 2);
    assert_decode(UINT32_C(0x4e224420), AARCH64_OP_ADVSIMD_SSHL,
            128, 1, 0, 1, 2);
    assert_decode(UINT32_C(0x0e624420), AARCH64_OP_ADVSIMD_SSHL,
            64, 2, 0, 1, 2);
    assert_decode(UINT32_C(0x4e624420), AARCH64_OP_ADVSIMD_SSHL,
            128, 2, 0, 1, 2);
    assert_decode(UINT32_C(0x0ea24420), AARCH64_OP_ADVSIMD_SSHL,
            64, 4, 0, 1, 2);
    assert_decode(UINT32_C(0x4ea24420), AARCH64_OP_ADVSIMD_SSHL,
            128, 4, 0, 1, 2);
    assert_decode(UINT32_C(0x4ee24420), AARCH64_OP_ADVSIMD_SSHL,
            128, 8, 0, 1, 2);

    assert_decode(UINT32_C(0x2e224420), AARCH64_OP_ADVSIMD_USHL,
            64, 1, 0, 1, 2);
    assert_decode(UINT32_C(0x6e224420), AARCH64_OP_ADVSIMD_USHL,
            128, 1, 0, 1, 2);
    assert_decode(UINT32_C(0x2e624420), AARCH64_OP_ADVSIMD_USHL,
            64, 2, 0, 1, 2);
    assert_decode(UINT32_C(0x6e624420), AARCH64_OP_ADVSIMD_USHL,
            128, 2, 0, 1, 2);
    assert_decode(UINT32_C(0x2ea24420), AARCH64_OP_ADVSIMD_USHL,
            64, 4, 0, 1, 2);
    assert_decode(UINT32_C(0x6ea24420), AARCH64_OP_ADVSIMD_USHL,
            128, 4, 0, 1, 2);
    assert_decode(UINT32_C(0x6ee24420), AARCH64_OP_ADVSIMD_USHL,
            128, 8, 0, 1, 2);

    // Watch 产品门禁中真实触发的 V31 自别名形式。
    assert_decode(UINT32_C(0x6e7847ff), AARCH64_OP_ADVSIMD_USHL,
            128, 2, 31, 31, 24);
}

static void test_encoding_space(void) {
    assert((ADVSIMD_VARIABLE_SHIFT_FIXED_MASK &
            ADVSIMD_VARIABLE_SHIFT_VARIABLE_MASK) == 0);
    assert((ADVSIMD_VARIABLE_SHIFT_FIXED_MASK |
            ADVSIMD_VARIABLE_SHIFT_VARIABLE_MASK) == UINT32_MAX);

    unsigned decoded_count = 0;
    unsigned rejected_count = 0;
    for (unsigned q = 0; q < 2; q++) {
        for (unsigned u = 0; u < 2; u++) {
            for (unsigned size = 0; size < 4; size++) {
                for (unsigned rm = 0; rm < 32; rm++) {
                    for (unsigned rn = 0; rn < 32; rn++) {
                        for (unsigned rd = 0; rd < 32; rd++) {
                            struct aarch64_decoded instruction = {0};
                            bool decoded = aarch64_decode(encode(q, u,
                                    (byte_t) size, (byte_t) rm,
                                    (byte_t) rn, (byte_t) rd),
                                    &instruction);
                            bool expected = q != 0 || size != 3;
                            assert(decoded == expected);
                            if (!decoded) {
                                rejected_count++;
                                continue;
                            }
                            decoded_count++;
                            assert(instruction.opcode == opcode_for(u));
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
    assert(decoded_count == 458752);
    assert(rejected_count == 65536);
}

static void assert_not_variable_shift(dword_t word) {
    struct aarch64_decoded instruction = {0};
    bool decoded = aarch64_decode(word, &instruction);
    assert(!decoded || !is_variable_shift_opcode(instruction.opcode));
}

static void test_encoding_boundaries(void) {
    dword_t base = encode(true, true, 1, 2, 1, 0);
    unsigned flipped_count = 0;
    for (unsigned bit = 0; bit < 32; bit++) {
        if ((ADVSIMD_VARIABLE_SHIFT_FIXED_MASK &
                (UINT32_C(1) << bit)) == 0)
            continue;
        assert_not_variable_shift(base ^ (UINT32_C(1) << bit));
        flipped_count++;
    }
    assert(flipped_count == 13);

    static const dword_t neighbors[] = {
        // 饱和、舍入以及饱和舍入族拥有独立语义和状态副作用。
        UINT32_C(0x4e624c20), // SQSHL V0.8H, V1.8H, V2.8H
        UINT32_C(0x6e624c20), // UQSHL V0.8H, V1.8H, V2.8H
        UINT32_C(0x4e625420), // SRSHL V0.8H, V1.8H, V2.8H
        UINT32_C(0x6e625420), // URSHL V0.8H, V1.8H, V2.8H
        UINT32_C(0x4e625c20), // SQRSHL V0.8H, V1.8H, V2.8H
        UINT32_C(0x6e625c20), // UQRSHL V0.8H, V1.8H, V2.8H
        // bit 28 选择标量编码空间，不能混入向量 .2D arrangement。
        UINT32_C(0x5ee24420), // SSHL D0, D1, D2
        UINT32_C(0x7ee24420), // USHL D0, D1, D2
    };
    for (unsigned i = 0; i < sizeof(neighbors) / sizeof(neighbors[0]); i++)
        assert_not_variable_shift(neighbors[i]);

    struct aarch64_decoded instruction = {0};
    assert(!aarch64_decode(UINT32_C(0x0ee24420), &instruction));
    assert(!aarch64_decode(UINT32_C(0x2ee24420), &instruction));
}

static qword_t element_mask(byte_t element_bits) {
    return element_bits == 64 ? UINT64_MAX :
            (UINT64_C(1) << element_bits) - 1;
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

static qword_t reference_lane(qword_t value, qword_t raw_shift,
        byte_t element_bits, bool u) {
    qword_t mask = element_mask(element_bits);
    value &= mask;
    byte_t encoded_shift = (byte_t) raw_shift;
    int shift = encoded_shift < 128 ?
            encoded_shift : (int) encoded_shift - 256;
    if (shift >= 0) {
        if (shift >= element_bits)
            return 0;
        return (value << shift) & mask;
    }

    unsigned magnitude = (unsigned) -shift;
    bool negative = (value &
            (UINT64_C(1) << (element_bits - 1))) != 0;
    if (magnitude >= element_bits)
        return !u && negative ? mask : 0;

    qword_t result = value >> magnitude;
    if (!u && negative)
        result |= mask << (element_bits - magnitude);
    return result & mask;
}

static union aarch64_vector_reg reference_result(
        const struct cpu_state *before, bool q, bool u, byte_t size,
        byte_t rn, byte_t rm) {
    byte_t element_size = (byte_t) (1U << size);
    byte_t element_bits = (byte_t) (element_size * 8);
    byte_t lanes = (byte_t) ((q ? 128 : 64) / element_bits);
    union aarch64_vector_reg result = {0};
    for (byte_t lane = 0; lane < lanes; lane++) {
        qword_t value = read_lane(&before->v[rn], element_size, lane);
        qword_t shift = read_lane(&before->v[rm], element_size, lane);
        write_lane(&result, element_size, lane,
                reference_lane(value, shift, element_bits, u));
    }
    return result;
}

static void initialize_cpu(struct cpu_state *cpu) {
    memset(cpu, 0, sizeof(*cpu));
    cpu->cycle = UINT64_C(0x1020304050607080);
    for (unsigned reg = 0; reg < 31; reg++)
        cpu->x[reg] = UINT64_C(0x1111111111111111) * (reg + 1);
    cpu->sp = UINT64_C(0x8877665544332210);
    cpu->pc = UINT64_C(0x1000);
    cpu->nzcv = UINT32_C(0xa0000000);
    for (unsigned reg = 0; reg < 32; reg++) {
        for (unsigned byte = 0; byte < 16; byte++)
            cpu->v[reg].b[byte] = (byte_t) (reg * 19 + byte * 7);
    }
    cpu->fpcr = UINT32_C(0x01c00000);
    cpu->fpsr = UINT32_C(0x08000011);
    cpu->tpidr_el0 = UINT64_C(0x123456789abcdef0);
    cpu->exclusive.address = UINT64_C(0x2000);
    cpu->exclusive.value_low = UINT64_C(0x0102030405060708);
    cpu->exclusive.value_high = UINT64_C(0x1112131415161718);
    cpu->exclusive.mapping_epoch = 17;
    cpu->exclusive.write_epoch = 23;
    cpu->exclusive.sync_identity = 29;
    cpu->exclusive.size = 16;
    cpu->exclusive.pair = true;
    cpu->exclusive.valid = true;
    cpu->segfault_addr = UINT64_C(0x3000);
    cpu->segfault_was_write = true;
    cpu->trapno = UINT32_C(0x55aa);
    cpu->single_step = true;
    cpu->_poked = true;
}

static void load_source(union aarch64_vector_reg *reg,
        bool q, byte_t size) {
    static const qword_t patterns[] = {
        UINT64_C(0x0000000000000000),
        UINT64_C(0x0000000000000001),
        UINT64_C(0x8000000000000000),
        UINT64_C(0x8000000000000003),
        UINT64_MAX,
        UINT64_C(0x55aa55aa55aa55aa),
        UINT64_C(0xaa55aa55aa55aa55),
    };
    byte_t element_size = (byte_t) (1U << size);
    byte_t element_bits = (byte_t) (element_size * 8);
    byte_t lanes = (byte_t) ((q ? 128 : 64) / element_bits);
    qword_t mask = element_mask(element_bits);
    qword_t sign = UINT64_C(1) << (element_bits - 1);
    for (byte_t lane = 0; lane < lanes; lane++) {
        qword_t value = patterns[lane %
                (sizeof(patterns) / sizeof(patterns[0]))] & mask;
        if (lane == 2)
            value = sign;
        else if (lane == 3)
            value = sign | 3;
        write_lane(reg, element_size, lane, value);
    }
}

static void load_shifts(union aarch64_vector_reg *reg,
        bool q, byte_t size, int shift) {
    byte_t element_size = (byte_t) (1U << size);
    byte_t element_bits = (byte_t) (element_size * 8);
    byte_t lanes = (byte_t) ((q ? 128 : 64) / element_bits);
    qword_t mask = element_mask(element_bits);
    qword_t high_noise = UINT64_C(0xa55aa55aa55aa500) &
            (mask & ~UINT64_C(0xff));
    if (element_size > 1)
        assert(high_noise != 0);
    for (byte_t lane = 0; lane < lanes; lane++)
        write_lane(reg, element_size, lane,
                high_noise | (byte_t) shift);
}

static void test_signed_right_shift_difference(void) {
    static const byte_t source[] = {
        0x80, 0x7f, 0xff, 0x01, 0x80, 0x01, 0x81, 0x7f,
    };
    static const byte_t shifts[] = {
        0xff, 0xff, 0xf8, 0xf8, 0xf7, 0xf7, 0xf9, 0xf9,
    };
    static const byte_t signed_expected[] = {
        0xc0, 0x3f, 0xff, 0x00, 0xff, 0x00, 0xff, 0x00,
    };
    static const byte_t unsigned_expected[] = {
        0x40, 0x3f, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,
    };
    struct cpu_state signed_cpu;
    initialize_cpu(&signed_cpu);
    memcpy(signed_cpu.v[1].b, source, sizeof(source));
    memcpy(signed_cpu.v[2].b, shifts, sizeof(shifts));
    struct cpu_state unsigned_cpu = signed_cpu;

    execute_instruction(&signed_cpu, encode(false, false, 0, 2, 1, 0));
    execute_instruction(&unsigned_cpu, encode(false, true, 0, 2, 1, 0));
    assert(memcmp(signed_cpu.v[0].b,
            signed_expected, sizeof(signed_expected)) == 0);
    assert(memcmp(unsigned_cpu.v[0].b,
            unsigned_expected, sizeof(unsigned_expected)) == 0);
    for (unsigned byte = 8; byte < 16; byte++) {
        assert(signed_cpu.v[0].b[byte] == 0);
        assert(unsigned_cpu.v[0].b[byte] == 0);
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
        {31, 31, 24},
        {31, 24, 31},
        {24, 31, 31},
        {31, 30, 29},
    };
    for (unsigned q = 0; q < 2; q++) {
        for (unsigned size = 0; size < 4; size++) {
            if (q == 0 && size == 3)
                continue;
            int bits = 8 << size;
            const int shifts[] = {
                0,
                1, -1,
                bits - 1, -(bits - 1),
                bits, -bits,
                bits + 1, -(bits + 1),
                127, -128,
            };
            for (unsigned u = 0; u < 2; u++) {
                for (unsigned shift_index = 0; shift_index <
                        sizeof(shifts) / sizeof(shifts[0]); shift_index++) {
                    for (unsigned form = 0; form <
                            sizeof(registers) / sizeof(registers[0]);
                            form++) {
                        byte_t rd = registers[form].rd;
                        byte_t rn = registers[form].rn;
                        byte_t rm = registers[form].rm;
                        struct cpu_state cpu;
                        initialize_cpu(&cpu);
                        load_source(&cpu.v[rn], q, (byte_t) size);
                        load_shifts(&cpu.v[rm], q, (byte_t) size,
                                shifts[shift_index]);
                        struct cpu_state before = cpu;
                        union aarch64_vector_reg expected_vector =
                                reference_result(&before, q, u,
                                        (byte_t) size, rn, rm);
                        dword_t word = encode(q, u, (byte_t) size,
                                rm, rn, rd);
                        if (q && u && size == 1 &&
                                rd == 31 && rn == 31 && rm == 24)
                            assert(word == UINT32_C(0x6e7847ff));

                        execute_instruction(&cpu, word);
                        struct cpu_state expected = before;
                        expected.pc += 4;
                        expected.v[rd] = expected_vector;
                        assert(memcmp(&cpu, &expected, sizeof(cpu)) == 0);
                        if (!q) {
                            for (unsigned byte = 8; byte < 16; byte++)
                                assert(cpu.v[rd].b[byte] == 0);
                        }
                    }
                }
            }
        }
    }
}

int main(void) {
    test_llvm_vectors();
    test_encoding_space();
    test_encoding_boundaries();
    test_signed_right_shift_difference();
    test_execution_space();
    return 0;
}
