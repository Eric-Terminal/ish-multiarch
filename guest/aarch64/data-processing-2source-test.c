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
        byte_t width, byte_t rd, byte_t rn, byte_t rm) {
    struct aarch64_decoded instruction = decode(word);
    assert(instruction.opcode == opcode);
    assert(instruction.width == width);
    assert(instruction.operands.data_processing_2source.rd == rd);
    assert(instruction.operands.data_processing_2source.rn == rn);
    assert(instruction.operands.data_processing_2source.rm == rm);
}

static void test_decode(void) {
    assert_decode(UINT32_C(0x1ac20820), AARCH64_OP_UDIV,
            32, 0, 1, 2);
    assert_decode(UINT32_C(0x9ac50883), AARCH64_OP_UDIV,
            64, 3, 4, 5);
    assert_decode(UINT32_C(0x1ac80ce6), AARCH64_OP_SDIV,
            32, 6, 7, 8);
    assert_decode(UINT32_C(0x9acb0d49), AARCH64_OP_SDIV,
            64, 9, 10, 11);
    assert_decode(UINT32_C(0x1ace21ac), AARCH64_OP_LSLV,
            32, 12, 13, 14);
    assert_decode(UINT32_C(0x9ad1220f), AARCH64_OP_LSLV,
            64, 15, 16, 17);
    assert_decode(UINT32_C(0x1ad42672), AARCH64_OP_LSRV,
            32, 18, 19, 20);
    assert_decode(UINT32_C(0x9ad726d5), AARCH64_OP_LSRV,
            64, 21, 22, 23);
    assert_decode(UINT32_C(0x1ada2b38), AARCH64_OP_ASRV,
            32, 24, 25, 26);
    assert_decode(UINT32_C(0x9add2b9b), AARCH64_OP_ASRV,
            64, 27, 28, 29);
    assert_decode(UINT32_C(0x1ac02ffe), AARCH64_OP_RORV,
            32, 30, 31, 0);
    assert_decode(UINT32_C(0x9ac22c3f), AARCH64_OP_RORV,
            64, 31, 1, 2);
}

static bool supported_operation(unsigned operation) {
    return operation == 2 || operation == 3 ||
            (operation >= 8 && operation <= 11);
}

static void test_encoding_space(void) {
    unsigned valid[2] = {0};
    for (unsigned is_64 = 0; is_64 < 2; is_64++) {
        for (unsigned operation = 0; operation < 64; operation++) {
            dword_t word = UINT32_C(0x1ac00000) |
                    (dword_t) is_64 << 31 |
                    (dword_t) operation << 10 |
                    UINT32_C(0x00020020);
            struct aarch64_decoded instruction;
            bool decoded = aarch64_decode(word, &instruction);
            assert(decoded == supported_operation(operation));
            if (decoded)
                valid[is_64]++;
        }
    }
    assert(valid[0] == 6);
    assert(valid[1] == 6);

    struct aarch64_decoded instruction;
    assert(!aarch64_decode(UINT32_C(0x3ac20820), &instruction));
    assert(!aarch64_decode(UINT32_C(0x1ac00020), &instruction));
}

static void test_unsigned_divide(void) {
    struct cpu_state cpu = {
        .pc = UINT64_C(0x1000),
        .sp = UINT64_C(0xfedcba9876543210),
        .nzcv = UINT32_C(0xb0000000),
    };
    cpu.x[1] = UINT64_C(0xffffffff00000064);
    cpu.x[2] = 7;
    execute_instruction(&cpu, UINT32_C(0x1ac20820));
    assert(cpu.x[0] == 14);

    cpu.x[4] = UINT64_MAX;
    cpu.x[5] = 3;
    execute_instruction(&cpu, UINT32_C(0x9ac50883));
    assert(cpu.x[3] == UINT64_C(0x5555555555555555));

    cpu.x[0] = UINT64_MAX;
    cpu.x[1] = 42;
    execute_instruction(&cpu, UINT32_C(0x9adf0820));
    assert(cpu.x[0] == 0);

    cpu.x[0] = UINT64_MAX;
    cpu.x[1] = 9;
    execute_instruction(&cpu, UINT32_C(0x9ac10be0));
    assert(cpu.x[0] == 0);

    cpu.x[0] = UINT64_C(0x13579bdf2468ace0);
    cpu.x[1] = 81;
    cpu.x[2] = 9;
    execute_instruction(&cpu, UINT32_C(0x9ac2083f));
    assert(cpu.x[0] == UINT64_C(0x13579bdf2468ace0));
    assert(cpu.pc == UINT64_C(0x1014));
    assert(cpu.sp == UINT64_C(0xfedcba9876543210));
    assert(cpu.nzcv == UINT32_C(0xb0000000));
}

static void test_signed_divide(void) {
    struct cpu_state cpu = {
        .pc = UINT64_C(0x1800),
        .nzcv = UINT32_C(0x60000000),
    };
    cpu.x[7] = UINT32_C(0xffffff9c);
    cpu.x[8] = 7;
    execute_instruction(&cpu, UINT32_C(0x1ac80ce6));
    assert(cpu.x[6] == UINT32_C(0xfffffff2));

    cpu.x[10] = 100;
    cpu.x[11] = UINT64_C(0xfffffffffffffff9);
    execute_instruction(&cpu, UINT32_C(0x9acb0d49));
    assert(cpu.x[9] == UINT64_C(0xfffffffffffffff2));

    cpu.x[1] = UINT64_C(0xfffffffffffffff9);
    cpu.x[2] = 3;
    execute_instruction(&cpu, UINT32_C(0x9ac20c20));
    assert(cpu.x[0] == UINT64_C(0xfffffffffffffffe));

    cpu.x[1] = 7;
    cpu.x[2] = UINT64_C(0xfffffffffffffffd);
    execute_instruction(&cpu, UINT32_C(0x9ac20c20));
    assert(cpu.x[0] == UINT64_C(0xfffffffffffffffe));

    cpu.x[1] = UINT64_C(0xfffffffffffffff9);
    cpu.x[2] = UINT64_C(0xfffffffffffffffd);
    execute_instruction(&cpu, UINT32_C(0x9ac20c20));
    assert(cpu.x[0] == 2);

    cpu.x[1] = UINT64_C(0x8000000000000000);
    cpu.x[2] = UINT64_MAX;
    execute_instruction(&cpu, UINT32_C(0x9ac20c20));
    assert(cpu.x[0] == UINT64_C(0x8000000000000000));

    cpu.x[1] = UINT64_C(0xffffffff80000000);
    cpu.x[2] = UINT64_MAX;
    execute_instruction(&cpu, UINT32_C(0x1ac20c20));
    assert(cpu.x[0] == UINT32_C(0x80000000));

    cpu.x[1] = UINT64_C(0x8000000000000000);
    cpu.x[2] = UINT64_C(0x8000000000000000);
    execute_instruction(&cpu, UINT32_C(0x9ac20c20));
    assert(cpu.x[0] == 1);

    cpu.x[1] = UINT64_C(0x8000000000000000);
    cpu.x[2] = 0;
    execute_instruction(&cpu, UINT32_C(0x9ac20c20));
    assert(cpu.x[0] == 0);
    assert(cpu.pc == UINT64_C(0x1824));
    assert(cpu.nzcv == UINT32_C(0x60000000));
}

static void test_variable_shifts(void) {
    struct cpu_state cpu = {
        .pc = UINT64_C(0x2000),
        .sp = UINT64_C(0x123456789abcdef0),
        .nzcv = UINT32_C(0xf0000000),
    };
    cpu.x[13] = UINT64_C(0xffffffff80000001);
    cpu.x[14] = 33;
    execute_instruction(&cpu, UINT32_C(0x1ace21ac));
    assert(cpu.x[12] == 2);

    cpu.x[16] = 1;
    cpu.x[17] = 63;
    execute_instruction(&cpu, UINT32_C(0x9ad1220f));
    assert(cpu.x[15] == UINT64_C(0x8000000000000000));

    cpu.x[19] = UINT64_C(0xffffffff80000000);
    cpu.x[20] = 36;
    execute_instruction(&cpu, UINT32_C(0x1ad42672));
    assert(cpu.x[18] == UINT32_C(0x08000000));

    cpu.x[22] = UINT64_C(0x8000000000000000);
    cpu.x[23] = 65;
    execute_instruction(&cpu, UINT32_C(0x9ad726d5));
    assert(cpu.x[21] == UINT64_C(0x4000000000000000));

    cpu.x[25] = UINT64_C(0xffffffff80000001);
    cpu.x[26] = 63;
    execute_instruction(&cpu, UINT32_C(0x1ada2b38));
    assert(cpu.x[24] == UINT32_MAX);

    cpu.x[28] = UINT64_C(0x8000000000000000);
    cpu.x[29] = 127;
    execute_instruction(&cpu, UINT32_C(0x9add2b9b));
    assert(cpu.x[27] == UINT64_MAX);

    cpu.x[0] = 8;
    cpu.x[1] = UINT64_C(0xffffffff12345678);
    execute_instruction(&cpu, UINT32_C(0x1ac02c20));
    assert(cpu.x[0] == UINT32_C(0x78123456));

    cpu.x[1] = UINT64_C(0x0123456789abcdef);
    cpu.x[2] = 8;
    execute_instruction(&cpu, UINT32_C(0x9ac22c20));
    assert(cpu.x[0] == UINT64_C(0xef0123456789abcd));

    cpu.x[2] = 64;
    execute_instruction(&cpu, UINT32_C(0x9ac22c20));
    assert(cpu.x[0] == UINT64_C(0x0123456789abcdef));

    cpu.x[0] = UINT64_MAX;
    execute_instruction(&cpu, UINT32_C(0x1ac02ffe));
    assert(cpu.x[30] == 0);

    cpu.x[0] = UINT64_C(0x2468ace013579bdf);
    execute_instruction(&cpu, UINT32_C(0x9ac22c3f));
    assert(cpu.x[0] == UINT64_C(0x2468ace013579bdf));
    assert(cpu.pc == UINT64_C(0x202c));
    assert(cpu.sp == UINT64_C(0x123456789abcdef0));
    assert(cpu.nzcv == UINT32_C(0xf0000000));
}

int main(void) {
    test_decode();
    test_encoding_space();
    test_unsigned_divide();
    test_signed_divide();
    test_variable_shifts();
    return 0;
}
