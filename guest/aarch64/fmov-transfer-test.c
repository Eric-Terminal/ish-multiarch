#include <assert.h>
#include <string.h>

#include "guest/aarch64/decode.h"
#include "guest/aarch64/execute.h"

#define FMOV_TRANSFER_FIXED_MASK UINT32_C(0xfffffc00)

struct transfer_case {
    dword_t bits;
    enum aarch64_opcode opcode;
    byte_t width;
};

static const struct transfer_case transfers[] = {
    {UINT32_C(0x1ee60000), AARCH64_OP_FMOV_GENERAL_FROM_SIMD, 16},
    {UINT32_C(0x1e260000), AARCH64_OP_FMOV_GENERAL_FROM_SIMD, 32},
    {UINT32_C(0x9e660000), AARCH64_OP_FMOV_GENERAL_FROM_SIMD, 64},
    {UINT32_C(0x9eae0000), AARCH64_OP_FMOV_GENERAL_FROM_SIMD_HIGH, 64},
    {UINT32_C(0x1ee70000), AARCH64_OP_FMOV_SIMD_FROM_GENERAL, 16},
    {UINT32_C(0x1e270000), AARCH64_OP_FMOV_SIMD_FROM_GENERAL, 32},
    {UINT32_C(0x9e670000), AARCH64_OP_FMOV_SIMD_FROM_GENERAL, 64},
    {UINT32_C(0x9eaf0000), AARCH64_OP_FMOV_SIMD_HIGH_FROM_GENERAL, 64},
    {UINT32_C(0x9ee60000), AARCH64_OP_FMOV_GENERAL_FROM_SIMD, 16},
    {UINT32_C(0x9ee70000), AARCH64_OP_FMOV_SIMD_FROM_GENERAL, 16},
};

static dword_t encode(unsigned transfer, byte_t rn, byte_t rd) {
    return transfers[transfer].bits | (dword_t) rn << 5 | rd;
}

static bool is_transfer_opcode(enum aarch64_opcode opcode) {
    return opcode >= AARCH64_OP_FMOV_GENERAL_FROM_SIMD &&
            opcode <= AARCH64_OP_FMOV_SIMD_HIGH_FROM_GENERAL;
}

static bool is_transfer_encoding(dword_t word) {
    dword_t bits = word & FMOV_TRANSFER_FIXED_MASK;
    for (unsigned transfer = 0;
            transfer < sizeof(transfers) / sizeof(transfers[0]); transfer++) {
        if (bits == transfers[transfer].bits)
            return true;
    }
    return false;
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

static void assert_decode(dword_t word, unsigned transfer,
        byte_t rd, byte_t rn) {
    struct aarch64_decoded instruction = decode(word);
    assert(instruction.opcode == transfers[transfer].opcode);
    assert(instruction.width == transfers[transfer].width);
    assert(instruction.operands.data_processing_1source.rd == rd);
    assert(instruction.operands.data_processing_1source.rn == rn);
}

static void test_llvm_vectors(void) {
    assert_decode(UINT32_C(0x1e260020), 1, 0, 1);
    assert_decode(UINT32_C(0x9e660062), 2, 2, 3);
    assert_decode(UINT32_C(0x1e2700a4), 5, 4, 5);
    assert_decode(UINT32_C(0x9e6700e6), 6, 6, 7);
    assert_decode(UINT32_C(0x9eae0128), 3, 8, 9);
    assert_decode(UINT32_C(0x9eaf016a), 7, 10, 11);
    assert_decode(UINT32_C(0x1ee601ac), 0, 12, 13);
    assert_decode(UINT32_C(0x1ee701ee), 4, 14, 15);
    assert_decode(UINT32_C(0x9ee60230), 8, 16, 17);
    assert_decode(UINT32_C(0x9ee70272), 9, 18, 19);
    assert_decode(UINT32_C(0x9e6603e5), 2, 5, 31);
}

static void test_encoding_space(void) {
    unsigned decoded_count = 0;
    for (unsigned transfer = 0;
            transfer < sizeof(transfers) / sizeof(transfers[0]); transfer++) {
        for (unsigned rn = 0; rn < 32; rn++) {
            for (unsigned rd = 0; rd < 32; rd++) {
                struct aarch64_decoded instruction;
                bool decoded = aarch64_decode(encode(
                        transfer, (byte_t) rn, (byte_t) rd), &instruction);
                assert(decoded);
                if (!decoded)
                    continue;
                decoded_count++;
                assert(instruction.opcode == transfers[transfer].opcode);
                assert(instruction.width == transfers[transfer].width);
                assert(instruction.operands.data_processing_1source.rd == rd);
                assert(instruction.operands.data_processing_1source.rn == rn);
            }
        }
    }
    assert(decoded_count == 10240);
}

static void test_fixed_bits(void) {
    for (unsigned transfer = 0;
            transfer < sizeof(transfers) / sizeof(transfers[0]); transfer++) {
        dword_t base = encode(transfer, 1, 0);
        for (unsigned bit = 10; bit < 32; bit++) {
            struct aarch64_decoded instruction;
            dword_t candidate = base ^ (UINT32_C(1) << bit);
            bool decoded = aarch64_decode(candidate, &instruction);
            bool transfer_decoded = decoded &&
                    is_transfer_opcode(instruction.opcode);
            assert(transfer_decoded == is_transfer_encoding(candidate));
        }
    }
    assert(FMOV_TRANSFER_FIXED_MASK == UINT32_C(0xfffffc00));
}

static void test_conversion_neighbors(void) {
    static const dword_t words[] = {
        UINT32_C(0x9e780020),
        UINT32_C(0x1e390062),
        UINT32_C(0x9e6200a4),
        UINT32_C(0x1e2300e6),
        UINT32_C(0x1e7e0128),
    };
    for (unsigned i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
        struct aarch64_decoded instruction;
        bool decoded = aarch64_decode(words[i], &instruction);
        assert(!decoded || !is_transfer_opcode(instruction.opcode));
    }
}

static void test_general_destinations(void) {
    static const qword_t expected[] = {
        UINT64_C(0x000000000000cdef),
        UINT64_C(0x0000000089abcdef),
        UINT64_C(0x0123456789abcdef),
        UINT64_C(0xfedcba9876543210),
        UINT64_C(0x000000000000cdef),
    };
    static const unsigned transfer_indices[] = {0, 1, 2, 3, 8};
    for (unsigned transfer = 0;
            transfer < sizeof(transfer_indices) /
                    sizeof(transfer_indices[0]); transfer++) {
        struct cpu_state cpu = {
            .pc = UINT64_C(0x1000),
            .sp = UINT64_C(0x1122334455667788),
            .nzcv = UINT32_C(0xa0000000),
            .fpcr = UINT32_C(0x01000000),
            .fpsr = UINT32_C(0x08000000),
        };
        cpu.x[5] = UINT64_MAX;
        cpu.v[31].d[0] = UINT64_C(0x0123456789abcdef);
        cpu.v[31].d[1] = UINT64_C(0xfedcba9876543210);
        unsigned index = transfer_indices[transfer];
        execute_instruction(&cpu, encode(index, 31, 5));
        assert(cpu.x[5] == expected[transfer]);
        assert(cpu.v[31].d[0] == UINT64_C(0x0123456789abcdef));
        assert(cpu.v[31].d[1] == UINT64_C(0xfedcba9876543210));
        assert(cpu.pc == UINT64_C(0x1004));
        assert(cpu.sp == UINT64_C(0x1122334455667788));
        assert(cpu.nzcv == UINT32_C(0xa0000000));
        assert(cpu.fpcr == UINT32_C(0x01000000));
        assert(cpu.fpsr == UINT32_C(0x08000000));
    }
}

static void test_simd_destinations(void) {
    static const unsigned transfer_indices[] = {4, 5, 6, 9};
    for (unsigned transfer = 0;
            transfer < sizeof(transfer_indices) /
                    sizeof(transfer_indices[0]); transfer++) {
        struct cpu_state cpu = {.pc = UINT64_C(0x2000)};
        cpu.x[7] = UINT64_C(0x0123456789abcdef);
        cpu.v[6].d[0] = UINT64_MAX;
        cpu.v[6].d[1] = UINT64_MAX;
        unsigned index = transfer_indices[transfer];
        execute_instruction(&cpu, encode(index, 7, 6));
        qword_t mask = transfers[index].width == 16 ? UINT16_MAX :
                transfers[index].width == 32 ? UINT32_MAX : UINT64_MAX;
        assert(cpu.v[6].d[0] == (cpu.x[7] & mask));
        assert(cpu.v[6].d[1] == 0);
        assert(cpu.x[7] == UINT64_C(0x0123456789abcdef));
        assert(cpu.pc == UINT64_C(0x2004));
    }

    struct cpu_state cpu = {.pc = UINT64_C(0x3000)};
    cpu.x[11] = UINT64_C(0x8877665544332211);
    cpu.v[10].d[0] = UINT64_C(0x0123456789abcdef);
    cpu.v[10].d[1] = UINT64_MAX;
    execute_instruction(&cpu, encode(7, 11, 10));
    assert(cpu.v[10].d[0] == UINT64_C(0x0123456789abcdef));
    assert(cpu.v[10].d[1] == UINT64_C(0x8877665544332211));
    assert(cpu.pc == UINT64_C(0x3004));
}

static void test_zero_register(void) {
    struct cpu_state cpu = {.pc = UINT64_C(0x4000)};
    cpu.v[31].d[0] = UINT64_C(0x0123456789abcdef);
    cpu.v[31].d[1] = UINT64_C(0xfedcba9876543210);
    execute_instruction(&cpu, encode(2, 31, 31));
    assert(cpu.v[31].d[0] == UINT64_C(0x0123456789abcdef));
    assert(cpu.v[31].d[1] == UINT64_C(0xfedcba9876543210));
    execute_instruction(&cpu, encode(6, 31, 31));
    assert(cpu.v[31].d[0] == 0);
    assert(cpu.v[31].d[1] == 0);
    assert(cpu.pc == UINT64_C(0x4008));

    cpu.v[10].d[0] = UINT64_C(0x0123456789abcdef);
    cpu.v[10].d[1] = UINT64_C(0xfedcba9876543210);
    execute_instruction(&cpu, encode(7, 31, 10));
    assert(cpu.v[10].d[0] == UINT64_C(0x0123456789abcdef));
    assert(cpu.v[10].d[1] == 0);
    cpu.v[10].d[1] = UINT64_C(0xfedcba9876543210);
    execute_instruction(&cpu, encode(3, 10, 31));
    assert(cpu.v[10].d[0] == UINT64_C(0x0123456789abcdef));
    assert(cpu.v[10].d[1] == UINT64_C(0xfedcba9876543210));
    assert(cpu.pc == UINT64_C(0x4010));
}

int main(void) {
    test_llvm_vectors();
    test_encoding_space();
    test_fixed_bits();
    test_conversion_neighbors();
    test_general_destinations();
    test_simd_destinations();
    test_zero_register();
    return 0;
}
