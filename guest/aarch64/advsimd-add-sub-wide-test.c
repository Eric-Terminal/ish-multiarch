#include <assert.h>

#include "guest/aarch64/decode.h"
#include "guest/aarch64/execute.h"

#define FIXED_MASK UINT32_C(0x9f20dc00)
#define FIXED_BITS UINT32_C(0x0e201000)

struct operation_case {
    enum aarch64_opcode opcode;
    bool upper;
    bool is_unsigned;
    bool subtract;
};

static const struct operation_case operations[] = {
    {AARCH64_OP_ADVSIMD_SADDW, false, false, false},
    {AARCH64_OP_ADVSIMD_SADDW2, true, false, false},
    {AARCH64_OP_ADVSIMD_UADDW, false, true, false},
    {AARCH64_OP_ADVSIMD_UADDW2, true, true, false},
    {AARCH64_OP_ADVSIMD_SSUBW, false, false, true},
    {AARCH64_OP_ADVSIMD_SSUBW2, true, false, true},
    {AARCH64_OP_ADVSIMD_USUBW, false, true, true},
    {AARCH64_OP_ADVSIMD_USUBW2, true, true, true},
};

static dword_t encode(unsigned operation, byte_t size,
        byte_t rd, byte_t rn, byte_t rm) {
    const struct operation_case *test = &operations[operation];
    return FIXED_BITS | (dword_t) test->upper << 30 |
            (dword_t) test->is_unsigned << 29 |
            (dword_t) size << 22 | (dword_t) test->subtract << 13 |
            (dword_t) rm << 16 | (dword_t) rn << 5 | rd;
}

static struct aarch64_decoded decode(dword_t word) {
    struct aarch64_decoded instruction = {0};
    assert(aarch64_decode(word, &instruction));
    return instruction;
}

static qword_t mask_for(byte_t size) {
    return size == 8 ? UINT64_MAX :
            (UINT64_C(1) << (size * 8)) - 1;
}

static qword_t read_element(const union aarch64_vector_reg *value,
        byte_t size, byte_t lane) {
    switch (size) {
        case 1: return value->b[lane];
        case 2: return value->h[lane];
        case 4: return value->s[lane];
        case 8: return value->d[lane];
        default: assert(false); return 0;
    }
}

static void write_element(union aarch64_vector_reg *value,
        byte_t size, byte_t lane, qword_t bits) {
    switch (size) {
        case 1: value->b[lane] = (byte_t) bits; break;
        case 2: value->h[lane] = (uint16_t) bits; break;
        case 4: value->s[lane] = (dword_t) bits; break;
        case 8: value->d[lane] = bits; break;
        default: assert(false);
    }
}

static void execute_word(struct cpu_state *cpu, dword_t word) {
    struct aarch64_decoded instruction = decode(word);
    struct aarch64_execute_result result =
            aarch64_execute(cpu, NULL, &instruction);
    assert(result.stop == AARCH64_EXECUTE_RETIRED);
}

static void test_decode_space(void) {
    unsigned decoded_count = 0;
    for (unsigned operation = 0;
            operation < sizeof(operations) / sizeof(operations[0]);
            operation++) {
        for (byte_t size = 0; size < 3; size++) {
            for (byte_t rd = 0; rd < 32; rd++) {
                for (byte_t rn = 0; rn < 32; rn++) {
                    for (byte_t rm = 0; rm < 32; rm++) {
                        dword_t word = encode(operation, size, rd, rn, rm);
                        struct aarch64_decoded instruction = decode(word);
                        assert(instruction.opcode == operations[operation].opcode);
                        assert(instruction.width == 128);
                        assert(instruction.operands.advsimd_three_same.rd == rd);
                        assert(instruction.operands.advsimd_three_same.rn == rn);
                        assert(instruction.operands.advsimd_three_same.rm == rm);
                        assert(instruction.operands.advsimd_three_same.element_size ==
                                (1U << size));
                        decoded_count++;
                    }
                }
            }
        }
    }
    assert(decoded_count == 786432);
    assert(FIXED_MASK == UINT32_C(0x9f20dc00));

    for (unsigned operation = 0;
            operation < sizeof(operations) / sizeof(operations[0]);
            operation++) {
        struct aarch64_decoded instruction;
        assert(!aarch64_decode(encode(operation, 3, 3, 5, 7),
                &instruction));
    }
}

static void test_execution(void) {
    for (unsigned operation = 0;
            operation < sizeof(operations) / sizeof(operations[0]);
            operation++) {
        for (byte_t size = 0; size < 3; size++) {
            byte_t source_size = (byte_t) (1U << size);
            byte_t destination_size = source_size * 2;
            byte_t lanes = 8 / source_size;
            struct cpu_state cpu = {
                .pc = UINT64_C(0x1000),
                .nzcv = UINT32_C(0xa0000000),
                .fpcr = UINT32_C(0x00400000),
                .fpsr = UINT32_C(0x08000001),
            };
            for (byte_t lane = 0; lane < lanes; lane++) {
                write_element(&cpu.v[5], destination_size, lane,
                        UINT64_C(0x123456789abcdef0) + lane);
                write_element(&cpu.v[6], source_size, lane,
                        UINT64_C(0x11) + lane);
                write_element(&cpu.v[6], source_size,
                        (byte_t) (lane + lanes),
                        mask_for(source_size) - lane);
            }
            union aarch64_vector_reg wide = cpu.v[5];
            union aarch64_vector_reg narrow = cpu.v[6];
            execute_word(&cpu, encode(operation, size, 7, 5, 6));

            for (byte_t lane = 0; lane < lanes; lane++) {
                qword_t left = read_element(
                        &wide, destination_size, lane);
                qword_t right = read_element(&narrow, source_size,
                        (byte_t) (lane +
                                (operations[operation].upper ? lanes : 0)));
                if (!operations[operation].is_unsigned &&
                        (right & (UINT64_C(1) <<
                                (source_size * 8 - 1))))
                    right |= ~mask_for(source_size);
                qword_t expected = operations[operation].subtract ?
                        left - right : left + right;
                assert(read_element(&cpu.v[7], destination_size, lane) ==
                        (expected & mask_for(destination_size)));
            }
            assert(cpu.pc == UINT64_C(0x1004));
            assert(cpu.nzcv == UINT32_C(0xa0000000));
            assert(cpu.fpcr == UINT32_C(0x00400000));
            assert(cpu.fpsr == UINT32_C(0x08000001));
        }
    }
}

static void test_product_alias(void) {
    struct cpu_state cpu = {.pc = UINT64_C(0x2000)};
    cpu.v[29].d[0] = UINT64_C(0x0000000200000001);
    cpu.v[29].d[1] = UINT64_C(0x0000000400000003);
    cpu.v[31].s[0] = UINT32_C(0x11111111);
    cpu.v[31].s[1] = UINT32_C(0x22222222);
    cpu.v[31].s[2] = UINT32_C(0xffffffff);
    cpu.v[31].s[3] = UINT32_C(0x80000000);

    execute_word(&cpu, UINT32_C(0x4ebf33bd));
    assert(cpu.v[29].d[0] == UINT64_C(0x0000000200000002));
    assert(cpu.v[29].d[1] == UINT64_C(0x0000000480000003));
    assert(cpu.pc == UINT64_C(0x2004));
}

int main(void) {
    test_decode_space();
    test_execution();
    test_product_alias();
    return 0;
}
