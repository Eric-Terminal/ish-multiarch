#include <assert.h>
#include <string.h>

#include "guest/aarch64/decode.h"
#include "guest/aarch64/execute.h"

#define SCALAR_SSHR_FIXED_MASK UINT32_C(0xffc0fc00)
#define SCALAR_SSHR_FIXED_BITS UINT32_C(0x5f400400)
#define SCALAR_SSHR_VARIABLE_MASK UINT32_C(0x003f03ff)
#define SCALAR_SSHR_FAMILY_BITS UINT32_C(0x5f000400)

static dword_t encode_sshr(
        byte_t shift, byte_t rn, byte_t rd) {
    assert(shift >= 1 && shift <= 64);
    return SCALAR_SSHR_FAMILY_BITS |
            (dword_t) (128 - shift) << 16 |
            (dword_t) rn << 5 | rd;
}

static bool is_scalar_sshr(
        const struct aarch64_decoded *instruction) {
    return instruction->opcode == AARCH64_OP_ADVSIMD_SSHR &&
            instruction->width == 64 &&
            instruction->operands.advsimd_shift_immediate.
                    element_size == 8;
}

static struct aarch64_decoded decode(dword_t word) {
    struct aarch64_decoded instruction = {0};
    assert(aarch64_decode(word, &instruction));
    return instruction;
}

static void assert_decode(dword_t word,
        byte_t shift, byte_t rn, byte_t rd) {
    struct aarch64_decoded instruction = decode(word);
    assert(is_scalar_sshr(&instruction));
    assert(instruction.operands.advsimd_shift_immediate.shift == shift);
    assert(instruction.operands.advsimd_shift_immediate.rn == rn);
    assert(instruction.operands.advsimd_shift_immediate.rd == rd);
}

static void test_llvm_vectors(void) {
    assert_decode(UINT32_C(0x5f7f0420), 1, 1, 0);
    assert_decode(UINT32_C(0x5f5907fc), 39, 31, 28);
    assert_decode(UINT32_C(0x5f400462), 64, 3, 2);
    // GCC cc1 的 RTL expand pass 实际触发字。
    assert_decode(UINT32_C(0x5f7d07fd), 3, 31, 29);
}

static void test_encoding_space(void) {
    assert((SCALAR_SSHR_FIXED_MASK &
            SCALAR_SSHR_VARIABLE_MASK) == 0);
    assert((SCALAR_SSHR_FIXED_MASK |
            SCALAR_SSHR_VARIABLE_MASK) == UINT32_MAX);

    unsigned legal_count = 0;
    unsigned reserved_count = 0;
    for (unsigned immediate = 0; immediate < 128; immediate++) {
        for (unsigned rn = 0; rn < 32; rn++) {
            for (unsigned rd = 0; rd < 32; rd++) {
                dword_t word = SCALAR_SSHR_FAMILY_BITS |
                        (dword_t) immediate << 16 |
                        (dword_t) rn << 5 | rd;
                struct aarch64_decoded instruction;
                bool decoded = aarch64_decode(word, &instruction);
                if (immediate < 64) {
                    assert(!decoded);
                    reserved_count++;
                    continue;
                }
                assert(decoded);
                assert(is_scalar_sshr(&instruction));
                assert(instruction.operands.advsimd_shift_immediate.
                        shift == 128 - immediate);
                assert(instruction.operands.advsimd_shift_immediate.rn ==
                        rn);
                assert(instruction.operands.advsimd_shift_immediate.rd ==
                        rd);
                legal_count++;
            }
        }
    }
    assert(legal_count == 65536);
    assert(reserved_count == 65536);

    dword_t base = encode_sshr(3, 31, 29);
    for (unsigned bit = 0; bit < 32; bit++) {
        if ((SCALAR_SSHR_FIXED_MASK &
                (UINT32_C(1) << bit)) == 0)
            continue;
        struct aarch64_decoded instruction;
        bool decoded = aarch64_decode(
                base ^ (UINT32_C(1) << bit), &instruction);
        assert(!decoded || !is_scalar_sshr(&instruction));
    }

    static const dword_t neighbors[] = {
        UINT32_C(0x7f7d07fd), // 标量 USHR。
        UINT32_C(0x4f7d07fd), // 向量 SSHR。
        UINT32_C(0x5f7d17fd), // 标量 SSRA。
        UINT32_C(0x5f7d27fd), // 标量 SRSHR。
        UINT32_C(0x5f7d37fd), // 标量 SRSRA。
        UINT32_C(0x7f7d47fd), // 标量 SRI。
        UINT32_C(0x5f4357fd), // 标量 SHL。
        UINT32_C(0x5f7da7fd), // 保留的相邻操作字段。
    };
    for (unsigned index = 0;
            index < sizeof(neighbors) / sizeof(neighbors[0]); index++) {
        struct aarch64_decoded instruction;
        bool decoded = aarch64_decode(neighbors[index], &instruction);
        assert(!decoded || !is_scalar_sshr(&instruction));
    }
}

static qword_t reference_sshr(qword_t value, byte_t shift) {
    qword_t sign = value & UINT64_C(0x8000000000000000);
    for (byte_t bit = 0; bit < shift; bit++)
        value = (value >> 1) | sign;
    return value;
}

static void execute_instruction(struct cpu_state *cpu, dword_t word) {
    struct aarch64_decoded instruction = decode(word);
    struct aarch64_execute_result result =
            aarch64_execute(cpu, NULL, &instruction);
    assert(result.stop == AARCH64_EXECUTE_RETIRED);
}

static void assert_execution(byte_t shift, byte_t rn, byte_t rd,
        qword_t source_low, qword_t source_high,
        qword_t expected_low) {
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
    if (rd != rn)
        cpu.v[rd].q = ~(__uint128_t) 0;
    cpu.v[rn].d[0] = source_low;
    cpu.v[rn].d[1] = source_high;

    struct cpu_state expected = cpu;
    expected.v[rd] = (union aarch64_vector_reg) {
        .d = {expected_low, 0},
    };
    expected.pc += 4;

    execute_instruction(&cpu, encode_sshr(shift, rn, rd));
    assert(memcmp(&cpu, &expected, sizeof(cpu)) == 0);
}

static void test_execution_space(void) {
    for (unsigned shift = 1; shift <= 64; shift++) {
        for (unsigned rn = 0; rn < 32; rn++) {
            for (unsigned rd = 0; rd < 32; rd++) {
                qword_t source = UINT64_C(0x0123456789abcdef) ^
                        (qword_t) shift << 48 ^
                        (qword_t) rn << 32 ^ rd;
                if ((shift + rn + rd) & 1)
                    source |= UINT64_C(0x8000000000000000);
                assert_execution((byte_t) shift,
                        (byte_t) rn, (byte_t) rd, source,
                        UINT64_C(0xa5a5a5a5a5a5a5a5),
                        reference_sshr(source, (byte_t) shift));
            }
        }
    }
}

static void test_boundaries(void) {
    static const struct {
        byte_t shift;
        qword_t input;
        qword_t expected;
    } cases[] = {
        {1, UINT64_C(0x8000000000000000),
                UINT64_C(0xc000000000000000)},
        {1, UINT64_C(0x7fffffffffffffff),
                UINT64_C(0x3fffffffffffffff)},
        {3, UINT64_C(0x0000000000000040), UINT64_C(8)},
        {3, UINT64_C(0xbbaa9988ffeeddcd),
                UINT64_C(0xf77553311ffddbb9)},
        {63, UINT64_C(0x8000000000000000), UINT64_MAX},
        {63, UINT64_C(0x7fffffffffffffff), UINT64_C(0)},
        {64, UINT64_C(0x8000000000000000), UINT64_MAX},
        {64, UINT64_MAX, UINT64_MAX},
        {64, UINT64_C(0), UINT64_C(0)},
        {64, UINT64_C(1), UINT64_C(0)},
    };
    for (unsigned index = 0;
            index < sizeof(cases) / sizeof(cases[0]); index++) {
        assert_execution(cases[index].shift, 31, 29,
                cases[index].input,
                UINT64_C(0x1122334455667788),
                cases[index].expected);
        assert_execution(cases[index].shift, 31, 31,
                cases[index].input,
                UINT64_C(0x8877665544332211),
                cases[index].expected);
    }
}

int main(void) {
    test_llvm_vectors();
    test_encoding_space();
    test_execution_space();
    test_boundaries();
    return 0;
}
