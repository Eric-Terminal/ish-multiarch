#include <assert.h>
#include <stdint.h>

#include "guest/aarch64/decode.h"
#include "guest/aarch64/execute.h"

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

static void assert_decode(dword_t word, enum aarch64_opcode opcode,
        byte_t width, byte_t rd, byte_t rn, byte_t rm, byte_t ra) {
    struct aarch64_decoded instruction = decode(word);
    assert(instruction.opcode == opcode);
    assert(instruction.width == width);
    assert(instruction.operands.data_processing_3source.rd == rd);
    assert(instruction.operands.data_processing_3source.rn == rn);
    assert(instruction.operands.data_processing_3source.rm == rm);
    assert(instruction.operands.data_processing_3source.ra == ra);
}

static void test_decode(void) {
    assert_decode(UINT32_C(0x1b020c20), AARCH64_OP_MADD,
            32, 0, 1, 2, 3);
    assert_decode(UINT32_C(0x9b061ca4), AARCH64_OP_MADD,
            64, 4, 5, 6, 7);
    assert_decode(UINT32_C(0x1b16deb4), AARCH64_OP_MSUB,
            32, 20, 21, 22, 23);
    assert_decode(UINT32_C(0x9b1aef38), AARCH64_OP_MSUB,
            64, 24, 25, 26, 27);
    assert_decode(UINT32_C(0x9b2824e6), AARCH64_OP_SMADDL,
            64, 6, 7, 8, 9);
    assert_decode(UINT32_C(0x9b2cb56a), AARCH64_OP_SMSUBL,
            64, 10, 11, 12, 13);
    assert_decode(UINT32_C(0x9bb045ee), AARCH64_OP_UMADDL,
            64, 14, 15, 16, 17);
    assert_decode(UINT32_C(0x9bb4d672), AARCH64_OP_UMSUBL,
            64, 18, 19, 20, 21);

    assert_decode(UINT32_C(0x1b107dee), AARCH64_OP_MADD,
            32, 14, 15, 16, 31);
    assert_decode(UINT32_C(0x9b13fe51), AARCH64_OP_MSUB,
            64, 17, 18, 19, 31);
    assert_decode(UINT32_C(0x9b387ef6), AARCH64_OP_SMADDL,
            64, 22, 23, 24, 31);
    assert_decode(UINT32_C(0x9ba2fc20), AARCH64_OP_UMSUBL,
            64, 0, 1, 2, 31);
}

static bool expected_encoding(unsigned sf, unsigned op54,
        unsigned op31) {
    return op54 == 0 && (op31 == 0 ||
            (sf == 1 && (op31 == 1 || op31 == 5)));
}

static void test_encoding_space(void) {
    unsigned valid = 0;
    for (unsigned sf = 0; sf < 2; sf++) {
        for (unsigned op54 = 0; op54 < 4; op54++) {
            for (unsigned op31 = 0; op31 < 8; op31++) {
                for (unsigned subtract = 0; subtract < 2; subtract++) {
                    for (unsigned ra = 0; ra < 32; ra++) {
                        dword_t word = UINT32_C(0x1b020020) |
                                (dword_t) sf << 31 |
                                (dword_t) op54 << 29 |
                                (dword_t) op31 << 21 |
                                (dword_t) subtract << 15 |
                                (dword_t) ra << 10;
                        struct aarch64_decoded instruction;
                        bool decoded = aarch64_decode(word, &instruction);
                        bool expected = expected_encoding(sf, op54, op31);
                        assert(decoded == expected);
                        if (decoded)
                            valid++;
                    }
                }
            }
        }
    }
    assert(valid == 256);
}

static void test_basic_multiply_add(void) {
    struct cpu_state cpu = {
        .pc = UINT64_C(0x1000),
        .sp = UINT64_C(0xfedcba9876543210),
        .nzcv = UINT32_C(0xa0000000),
    };
    cpu.x[0] = UINT64_MAX;
    cpu.x[1] = UINT64_MAX;
    cpu.x[2] = 2;
    cpu.x[3] = 3;
    execute_instruction(&cpu, UINT32_C(0x1b020c20));
    assert(cpu.x[0] == 1);

    cpu.x[5] = UINT64_MAX;
    cpu.x[6] = 2;
    cpu.x[7] = 3;
    execute_instruction(&cpu, UINT32_C(0x9b061ca4));
    assert(cpu.x[4] == 1);

    cpu.x[21] = 2;
    cpu.x[22] = 3;
    cpu.x[23] = 1;
    execute_instruction(&cpu, UINT32_C(0x1b16deb4));
    assert(cpu.x[20] == UINT32_C(0xfffffffb));

    cpu.x[25] = 2;
    cpu.x[26] = 3;
    cpu.x[27] = 1;
    execute_instruction(&cpu, UINT32_C(0x9b1aef38));
    assert(cpu.x[24] == UINT64_C(0xfffffffffffffffb));

    cpu.x[0] = 3;
    cpu.x[1] = 4;
    cpu.x[2] = 5;
    execute_instruction(&cpu, UINT32_C(0x9b020020));
    assert(cpu.x[0] == 23);

    cpu.x[0] = UINT64_C(0x13579bdf2468ace0);
    cpu.x[11] = 6;
    cpu.x[12] = 7;
    cpu.x[13] = 8;
    execute_instruction(&cpu, UINT32_C(0x9b0c357f));
    assert(cpu.x[0] == UINT64_C(0x13579bdf2468ace0));
    assert(cpu.pc == UINT64_C(0x1018));
    assert(cpu.sp == UINT64_C(0xfedcba9876543210));
    assert(cpu.nzcv == UINT32_C(0xa0000000));
}

static void test_long_multiply_add(void) {
    struct cpu_state cpu = {
        .pc = UINT64_C(0x1800),
        .sp = UINT64_C(0x123456789abcdef0),
        .nzcv = UINT32_C(0x60000000),
    };
    cpu.x[7] = UINT64_C(0xffffffff80000000);
    cpu.x[8] = UINT64_MAX;
    cpu.x[9] = UINT64_MAX;
    execute_instruction(&cpu, UINT32_C(0x9b2824e6));
    assert(cpu.x[6] == UINT64_C(0x000000007fffffff));

    cpu.x[11] = 1;
    cpu.x[12] = 1;
    cpu.x[13] = UINT64_C(0x8000000000000000);
    execute_instruction(&cpu, UINT32_C(0x9b2cb56a));
    assert(cpu.x[10] == UINT64_C(0x7fffffffffffffff));

    cpu.x[15] = UINT64_MAX;
    cpu.x[16] = UINT64_MAX;
    cpu.x[17] = UINT64_MAX;
    execute_instruction(&cpu, UINT32_C(0x9bb045ee));
    assert(cpu.x[14] == UINT64_C(0xfffffffe00000000));

    cpu.x[19] = UINT64_MAX;
    cpu.x[20] = UINT64_MAX;
    cpu.x[21] = 0;
    execute_instruction(&cpu, UINT32_C(0x9bb4d672));
    assert(cpu.x[18] == UINT64_C(0x00000001ffffffff));

    cpu.x[23] = UINT32_C(0xfffffffe);
    cpu.x[24] = 3;
    execute_instruction(&cpu, UINT32_C(0x9b387ef6));
    assert(cpu.x[22] == UINT64_C(0xfffffffffffffffa));

    cpu.x[0] = UINT64_C(0x0f1e2d3c4b5a6978);
    cpu.x[6] = 9;
    cpu.x[7] = 10;
    execute_instruction(&cpu, UINT32_C(0x9ba77cdf));
    assert(cpu.x[0] == UINT64_C(0x0f1e2d3c4b5a6978));
    assert(cpu.pc == UINT64_C(0x1818));
    assert(cpu.sp == UINT64_C(0x123456789abcdef0));
    assert(cpu.nzcv == UINT32_C(0x60000000));
}

int main(void) {
    test_decode();
    test_encoding_space();
    test_basic_multiply_add();
    test_long_multiply_add();
    return 0;
}
