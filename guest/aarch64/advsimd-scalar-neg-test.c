#include <assert.h>
#include <string.h>

#include "guest/aarch64/decode.h"
#include "guest/aarch64/execute.h"

#define SCALAR_NEG_FIXED_MASK UINT32_C(0xfffffc00)
#define SCALAR_NEG_FIXED_BITS UINT32_C(0x7ee0b800)
#define SCALAR_NEG_VARIABLE_MASK UINT32_C(0x000003ff)
#define SCALAR_NEG_FAMILY_BITS UINT32_C(0x7e20b800)

static dword_t encode_neg(byte_t rn, byte_t rd) {
    return SCALAR_NEG_FIXED_BITS | (dword_t) rn << 5 | rd;
}

static dword_t encode_family(byte_t size, byte_t rn, byte_t rd) {
    return SCALAR_NEG_FAMILY_BITS |
            (dword_t) size << 22 |
            (dword_t) rn << 5 | rd;
}

static bool is_scalar_neg(const struct aarch64_decoded *instruction) {
    return instruction->opcode == AARCH64_OP_ADVSIMD_NEG &&
            instruction->width == 64 &&
            instruction->operands.advsimd_unary.element_size == 8;
}

static struct aarch64_decoded decode(dword_t word) {
    struct aarch64_decoded instruction = {0};
    bool decoded = aarch64_decode(word, &instruction);
    assert(decoded);
    use(decoded);
    return instruction;
}

static void assert_decode(dword_t word, byte_t rn, byte_t rd) {
    struct aarch64_decoded instruction = decode(word);
    assert(is_scalar_neg(&instruction));
    assert(instruction.operands.advsimd_unary.rn == rn);
    assert(instruction.operands.advsimd_unary.rd == rd);
}

static void test_llvm_vectors(void) {
    assert_decode(UINT32_C(0x7ee0b820), 1, 0);
    assert_decode(UINT32_C(0x7ee0b8e6), 7, 6);
    assert_decode(UINT32_C(0x7ee0bbbe), 29, 30);
    // GCC cc1 的 GIMPLE evrp pass 实际触发字。
    assert_decode(UINT32_C(0x7ee0bbff), 31, 31);
}

static void test_encoding_space(void) {
    assert((SCALAR_NEG_FIXED_MASK & SCALAR_NEG_VARIABLE_MASK) == 0);
    assert((SCALAR_NEG_FIXED_MASK | SCALAR_NEG_VARIABLE_MASK) ==
            UINT32_MAX);

    unsigned legal_count = 0;
    unsigned reserved_count = 0;
    for (unsigned size = 0; size < 4; size++) {
        for (unsigned rn = 0; rn < 32; rn++) {
            for (unsigned rd = 0; rd < 32; rd++) {
                struct aarch64_decoded instruction;
                bool decoded = aarch64_decode(encode_family(
                        (byte_t) size, (byte_t) rn, (byte_t) rd),
                        &instruction);
                if (size != 3) {
                    assert(!decoded);
                    reserved_count++;
                    continue;
                }
                assert(decoded);
                assert(is_scalar_neg(&instruction));
                assert(instruction.operands.advsimd_unary.rn == rn);
                assert(instruction.operands.advsimd_unary.rd == rd);
                legal_count++;
            }
        }
    }
    assert(legal_count == 1024);
    assert(reserved_count == 3072);

    dword_t base = encode_neg(31, 31);
    for (unsigned bit = 0; bit < 32; bit++) {
        if ((SCALAR_NEG_FIXED_MASK & (UINT32_C(1) << bit)) == 0)
            continue;
        struct aarch64_decoded instruction;
        bool decoded = aarch64_decode(
                base ^ (UINT32_C(1) << bit), &instruction);
        assert(!decoded || !is_scalar_neg(&instruction));
    }

    static const dword_t neighbors[] = {
        UINT32_C(0x5ee0bbff), // 标量 ABS D。
        UINT32_C(0x5ee07bff), // 标量 SQABS D。
        UINT32_C(0x7ee07bff), // 标量 SQNEG D。
        UINT32_C(0x7ee08bff), // 标量 CMGE D, #0。
        UINT32_C(0x6ee0bbff), // 向量 NEG V.2D。
        UINT32_C(0x7eff87ff), // 标量 SUB D。
        UINT32_C(0x1e6143ff), // 标量浮点 FNEG D。
    };
    for (unsigned index = 0;
            index < sizeof(neighbors) / sizeof(neighbors[0]); index++) {
        struct aarch64_decoded instruction;
        bool decoded = aarch64_decode(neighbors[index], &instruction);
        assert(!decoded || !is_scalar_neg(&instruction));
    }
}

static void execute_instruction(struct cpu_state *cpu, dword_t word) {
    struct aarch64_decoded instruction = decode(word);
    struct aarch64_execute_result result =
            aarch64_execute(cpu, NULL, &instruction);
    assert(result.stop == AARCH64_EXECUTE_RETIRED);
}

static void assert_execution(byte_t rn, byte_t rd,
        qword_t source_low, qword_t expected_low,
        qword_t source_high) {
    struct cpu_state cpu = {
        .cycle = UINT64_C(0x123456789abcdef0),
        .sp = UINT64_C(0x1122334455667788),
        .pc = UINT64_C(0x4000),
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
    if (rd != rn) {
        cpu.v[rd].d[0] = UINT64_MAX;
        cpu.v[rd].d[1] = UINT64_MAX;
    }
    cpu.v[rn].d[0] = source_low;
    cpu.v[rn].d[1] = source_high;

    struct cpu_state expected = cpu;
    expected.v[rd] = (union aarch64_vector_reg) {
        .d = {expected_low, 0},
    };
    expected.pc += 4;

    execute_instruction(&cpu, encode_neg(rn, rd));
    assert(memcmp(&cpu, &expected, sizeof(cpu)) == 0);
}

static void test_execution(void) {
    for (unsigned rn = 0; rn < 32; rn++) {
        for (unsigned rd = 0; rd < 32; rd++) {
            assert_execution((byte_t) rn, (byte_t) rd,
                    UINT64_C(0x0123456789abcdef),
                    UINT64_C(0xfedcba9876543211),
                    UINT64_C(0xfedcba9876543210));
        }
    }

    static const struct {
        qword_t input;
        qword_t expected;
    } boundaries[] = {
        {UINT64_C(0), UINT64_C(0)},
        {UINT64_C(1), UINT64_MAX},
        {UINT64_MAX, UINT64_C(1)},
        {UINT64_C(0x7fffffffffffffff),
                UINT64_C(0x8000000000000001)},
        {UINT64_C(0x8000000000000000),
                UINT64_C(0x8000000000000000)},
        {UINT64_C(0x55aa55aa55aa55aa),
                UINT64_C(0xaa55aa55aa55aa56)},
    };
    for (unsigned index = 0;
            index < sizeof(boundaries) / sizeof(boundaries[0]); index++) {
        assert_execution(31, 31,
                boundaries[index].input, boundaries[index].expected,
                UINT64_C(0xaaaaaaaaaaaaaaaa));
        assert_execution(30, 31,
                boundaries[index].input, boundaries[index].expected,
                UINT64_C(0x5555555555555555));
    }
}

int main(void) {
    test_llvm_vectors();
    test_encoding_space();
    test_execution();
    return 0;
}
