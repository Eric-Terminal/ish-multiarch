#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "guest/aarch64/decode.h"
#include "guest/aarch64/execute.h"

#define VECTOR_SHL UINT32_C(0x0f005400)

static dword_t encode_vector_shl(bool q, byte_t element_size,
        byte_t shift, byte_t rn, byte_t rd) {
    byte_t element_bits = (byte_t) (element_size * 8);
    assert(element_size == 1 || element_size == 2 ||
            element_size == 4 || element_size == 8);
    assert(q || element_size != 8);
    assert(shift < element_bits);
    return VECTOR_SHL | (dword_t) q << 30 |
            (dword_t) (element_bits + shift) << 16 |
            (dword_t) rn << 5 | rd;
}

static struct aarch64_decoded decode(dword_t word) {
    struct aarch64_decoded instruction = {0};
    assert(aarch64_decode(word, &instruction));
    return instruction;
}

static void assert_decode(dword_t word, bool q, byte_t element_size,
        byte_t shift, byte_t rn, byte_t rd) {
    struct aarch64_decoded instruction = decode(word);
    assert(instruction.opcode == AARCH64_OP_ADVSIMD_SHL);
    assert(instruction.width == (q ? 128 : 64));
    assert(instruction.operands.advsimd_shift_immediate.rd == rd);
    assert(instruction.operands.advsimd_shift_immediate.rn == rn);
    assert(instruction.operands.advsimd_shift_immediate.element_size ==
            element_size);
    assert(instruction.operands.advsimd_shift_immediate.shift == shift);
}

static void test_known_encodings(void) {
    static const struct {
        dword_t word;
        bool q;
        byte_t element_size;
        byte_t shift;
        byte_t rn;
        byte_t rd;
    } cases[] = {
        {UINT32_C(0x0f085420), false, 1, 0, 1, 0},
        {UINT32_C(0x4f0f5462), true, 1, 7, 3, 2},
        {UINT32_C(0x0f1054a4), false, 2, 0, 5, 4},
        {UINT32_C(0x4f1f54e6), true, 2, 15, 7, 6},
        {UINT32_C(0x0f205528), false, 4, 0, 9, 8},
        {UINT32_C(0x4f3f556a), true, 4, 31, 11, 10},
        {UINT32_C(0x4f4055ac), true, 8, 0, 13, 12},
        {UINT32_C(0x4f4357ff), true, 8, 3, 31, 31},
        {UINT32_C(0x4f7f57be), true, 8, 63, 29, 30},
    };
    for (unsigned index = 0; index < array_size(cases); index++) {
        assert_decode(cases[index].word, cases[index].q,
                cases[index].element_size, cases[index].shift,
                cases[index].rn, cases[index].rd);
    }
}

static void test_encoding_space(void) {
    static const byte_t element_sizes[] = {1, 2, 4, 8};
    unsigned legal = 0;
    for (unsigned q = 0; q < 2; q++) {
        for (unsigned size_index = 0;
                size_index < array_size(element_sizes); size_index++) {
            byte_t element_size = element_sizes[size_index];
            if (!q && element_size == 8)
                continue;
            for (unsigned shift = 0; shift < element_size * 8; shift++) {
                for (unsigned rn = 0; rn < 32; rn++) {
                    for (unsigned rd = 0; rd < 32; rd++) {
                        assert_decode(encode_vector_shl(q != 0,
                                element_size, (byte_t) shift,
                                (byte_t) rn, (byte_t) rd), q != 0,
                                element_size, (byte_t) shift,
                                (byte_t) rn, (byte_t) rd);
                        legal++;
                    }
                }
            }
        }
    }
    assert(legal == 180224);

    unsigned reserved = 0;
    for (unsigned immediate = 64; immediate < 128; immediate++) {
        for (unsigned rn = 0; rn < 32; rn++) {
            for (unsigned rd = 0; rd < 32; rd++) {
                dword_t word = VECTOR_SHL |
                        (dword_t) immediate << 16 |
                        (dword_t) rn << 5 | (dword_t) rd;
                struct aarch64_decoded instruction;
                assert(!aarch64_decode(word, &instruction));
                reserved++;
            }
        }
    }
    assert(reserved == 65536);

    unsigned modified_immediate = 0;
    for (unsigned q = 0; q < 2; q++) {
        for (unsigned immediate = 0; immediate < 8; immediate++) {
            for (unsigned rn = 0; rn < 32; rn++) {
                for (unsigned rd = 0; rd < 32; rd++) {
                    dword_t word = VECTOR_SHL |
                            (dword_t) q << 30 |
                            (dword_t) immediate << 16 |
                            (dword_t) rn << 5 | (dword_t) rd;
                    struct aarch64_decoded instruction = decode(word);
                    assert(instruction.opcode ==
                            AARCH64_OP_ADVSIMD_ORR_IMMEDIATE);
                    modified_immediate++;
                }
            }
        }
    }
    assert(modified_immediate == 16384);
}

static void test_fixed_bits_and_neighbors(void) {
    const dword_t fixed_mask = UINT32_C(0xbf80fc00);
    const dword_t product = UINT32_C(0x4f4357ff);
    unsigned fixed_bits = 0;
    for (unsigned bit = 0; bit < 32; bit++) {
        if ((fixed_mask & (UINT32_C(1) << bit)) == 0)
            continue;
        struct aarch64_decoded instruction;
        bool decoded = aarch64_decode(
                product ^ (UINT32_C(1) << bit), &instruction);
        assert(!decoded || instruction.opcode != AARCH64_OP_ADVSIMD_SHL);
        fixed_bits++;
    }
    assert(fixed_bits == 14);

    static const dword_t neighbors[] = {
        UINT32_C(0x4f4307ff), // 向量 SSHR。
        UINT32_C(0x6f4307ff), // 向量 USHR。
        UINT32_C(0x6f4347ff), // 向量 SRI。
        UINT32_C(0x6f4357ff), // 向量 SLI。
        UINT32_C(0x5f4357ff), // 标量 SHL。
        UINT32_C(0x7f4357ff), // 标量 SLI。
        UINT32_C(0x4efe47ff), // 向量 SSHL。
        UINT32_C(0x6efe47ff), // 向量 USHL。
        UINT32_C(0x0f23a7ff), // 向量 SSHLL。
        UINT32_C(0x0f4357ff), // 保留的 Q=0、64 位元素。
    };
    for (unsigned index = 0; index < array_size(neighbors); index++) {
        struct aarch64_decoded instruction;
        bool decoded = aarch64_decode(neighbors[index], &instruction);
        assert(!decoded || instruction.opcode != AARCH64_OP_ADVSIMD_SHL);
    }
}

static union aarch64_vector_reg reference_shift(
        union aarch64_vector_reg source, bool q,
        byte_t element_size, byte_t shift) {
    union aarch64_vector_reg result = {0};
    byte_t lanes = (byte_t) ((q ? 16 : 8) / element_size);
    for (byte_t lane = 0; lane < lanes; lane++) {
        if (element_size == 1)
            result.b[lane] = (byte_t)
                    ((qword_t) source.b[lane] << shift);
        else if (element_size == 2)
            result.h[lane] = (word_t)
                    ((qword_t) source.h[lane] << shift);
        else if (element_size == 4)
            result.s[lane] = (dword_t)
                    ((qword_t) source.s[lane] << shift);
        else
            result.d[lane] = source.d[lane] << shift;
    }
    return result;
}

static struct cpu_state initial_cpu(void) {
    struct cpu_state cpu = {
        .cycle = UINT64_C(0x1020304050607080),
        .sp = UINT64_C(0x1122334455667788),
        .pc = UINT64_C(0x2000),
        .nzcv = UINT32_C(0xa0000000),
        .fpcr = AARCH64_FPCR_AHP | AARCH64_FPCR_DN |
                AARCH64_FPCR_FZ | AARCH64_FPCR_RMODE_MASK,
        .fpsr = AARCH64_FPSR_WRITE_MASK,
        .tpidr_el0 = UINT64_C(0x8877665544332211),
        .segfault_addr = UINT64_C(0x123456789abcdef0),
        .segfault_was_write = true,
        .trapno = UINT32_C(0x13572468),
        .single_step = true,
        ._poked = true,
    };
    for (unsigned reg = 0; reg < 31; reg++)
        cpu.x[reg] = UINT64_C(0x0102030405060708) ^ reg;
    for (unsigned reg = 0; reg < 32; reg++) {
        cpu.v[reg].d[0] = UINT64_C(0x81fe7f0100ff55aa) ^
                (UINT64_C(0x0101010101010101) * reg);
        cpu.v[reg].d[1] = UINT64_C(0x80000001ffff7f00) ^
                (UINT64_C(0x1010101010101010) * reg);
    }
    return cpu;
}

static void assert_execution(bool q, byte_t element_size,
        byte_t shift, byte_t rn, byte_t rd) {
    struct cpu_state cpu = initial_cpu();
    union aarch64_vector_reg source = cpu.v[rn];
    if (rd != rn)
        cpu.v[rd].q = ~(__uint128_t) 0;
    struct cpu_state expected = cpu;
    expected.v[rd] = reference_shift(
            source, q, element_size, shift);
    expected.pc += 4;

    struct aarch64_decoded instruction = decode(encode_vector_shl(
            q, element_size, shift, rn, rd));
    struct aarch64_execute_result result =
            aarch64_execute(&cpu, NULL, &instruction);
    assert(result.stop == AARCH64_EXECUTE_RETIRED);
    assert(result.fault.kind == GUEST_MEMORY_FAULT_NONE);
    assert(memcmp(&cpu, &expected, sizeof(cpu)) == 0);
}

static void test_execution_space(void) {
    static const byte_t element_sizes[] = {1, 2, 4, 8};
    static const byte_t registers[][2] = {
        {1, 2},
        {2, 2},
        {31, 30},
        {30, 31},
        {31, 31},
    };
    unsigned executions = 0;
    for (unsigned q = 0; q < 2; q++) {
        for (unsigned size_index = 0;
                size_index < array_size(element_sizes); size_index++) {
            byte_t element_size = element_sizes[size_index];
            if (!q && element_size == 8)
                continue;
            for (unsigned shift = 0; shift < element_size * 8; shift++) {
                for (unsigned pair = 0;
                        pair < array_size(registers); pair++) {
                    assert_execution(q != 0, element_size,
                            (byte_t) shift, registers[pair][0],
                            registers[pair][1]);
                    executions++;
                }
            }
        }
    }
    assert(executions == 880);
}

static void test_product_word(void) {
    struct cpu_state cpu = initial_cpu();
    cpu.v[31].d[0] = UINT64_MAX;
    cpu.v[31].d[1] = UINT64_C(0x8000000000000001);
    struct cpu_state expected = cpu;
    expected.v[31].d[0] = UINT64_C(0xfffffffffffffff8);
    expected.v[31].d[1] = UINT64_C(0x0000000000000008);
    expected.pc += 4;

    struct aarch64_decoded instruction =
            decode(UINT32_C(0x4f4357ff));
    struct aarch64_execute_result result =
            aarch64_execute(&cpu, NULL, &instruction);
    assert(result.stop == AARCH64_EXECUTE_RETIRED);
    assert(memcmp(&cpu, &expected, sizeof(cpu)) == 0);
}

int main(void) {
    test_known_encodings();
    test_encoding_space();
    test_fixed_bits_and_neighbors();
    test_execution_space();
    test_product_word();
    return 0;
}
