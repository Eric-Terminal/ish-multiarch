#include <assert.h>
#include <string.h>

#include "guest/aarch64/decode.h"
#include "guest/aarch64/execute.h"

#define ADVSIMD_ADD_FIXED_MASK UINT32_C(0xbf20fc00)
#define ADVSIMD_ADD_FIXED_BITS UINT32_C(0x0e208400)
#define ADVSIMD_ADD_VARIABLE_MASK UINT32_C(0x40df03ff)

static dword_t encode(bool q, byte_t size, byte_t rm, byte_t rn, byte_t rd) {
    return ADVSIMD_ADD_FIXED_BITS |
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

int main(void) {
    test_llvm_vectors();
    test_encoding_space();
    test_fixed_bits();
    test_execution_space();
    return 0;
}
