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

static void assert_decode(dword_t word, byte_t width,
        byte_t rd, byte_t rn, byte_t rm, byte_t lsb) {
    struct aarch64_decoded instruction = decode(word);
    assert(instruction.opcode == AARCH64_OP_EXTR);
    assert(instruction.width == width);
    assert(instruction.operands.extract.rd == rd);
    assert(instruction.operands.extract.rn == rn);
    assert(instruction.operands.extract.rm == rm);
    assert(instruction.operands.extract.lsb == lsb);
}

static void test_decode(void) {
    assert_decode(UINT32_C(0x13820020), 32, 0, 1, 2, 0);
    assert_decode(UINT32_C(0x13857c83), 32, 3, 4, 5, 31);
    assert_decode(UINT32_C(0x93c800e6), 64, 6, 7, 8, 0);
    assert_decode(UINT32_C(0x93cbfd49), 64, 9, 10, 11, 63);
    assert_decode(UINT32_C(0x138d01ac), 32, 12, 13, 13, 0);
    assert_decode(UINT32_C(0x138f45ee), 32, 14, 15, 15, 17);
    assert_decode(UINT32_C(0x93d10230), 64, 16, 17, 17, 0);
    assert_decode(UINT32_C(0x93d39672), 64, 18, 19, 19, 37);
    assert_decode(UINT32_C(0x139f7fff), 32, 31, 31, 31, 31);
    assert_decode(UINT32_C(0x93dfffff), 64, 31, 31, 31, 63);

    unsigned valid = 0;
    for (unsigned sf = 0; sf < 2; sf++) {
        for (unsigned n = 0; n < 2; n++) {
            for (unsigned lsb = 0; lsb < 64; lsb++) {
                dword_t word = UINT32_C(0x13820020) |
                        (dword_t) sf << 31 |
                        (dword_t) n << 22 |
                        (dword_t) lsb << 10;
                bool expected = n == sf && (sf || lsb < 32);
                struct aarch64_decoded instruction;
                bool decoded = aarch64_decode(word, &instruction);
                assert(decoded == expected);
                if (decoded)
                    valid++;
            }
        }
    }
    assert(valid == 96);
}

static qword_t extract_oracle(qword_t high, qword_t low,
        unsigned width, unsigned lsb) {
    qword_t mask = width == 64 ? UINT64_MAX : UINT32_MAX;
    __uint128_t concatenated = (__uint128_t) (high & mask) << width |
            (low & mask);
    return (qword_t) (concatenated >> lsb) & mask;
}

static void test_all_shifts(void) {
    static const qword_t values[] = {
        0,
        UINT64_MAX,
        1,
        UINT64_C(0x8000000000000000),
        UINT64_C(0xaaaaaaaaaaaaaaaa),
        UINT64_C(0x0123456789abcdef),
    };
    for (unsigned sf = 0; sf < 2; sf++) {
        unsigned width = sf ? 64 : 32;
        for (unsigned lsb = 0; lsb < width; lsb++) {
            for (size_t i = 0; i < array_size(values); i++) {
                qword_t high = values[i];
                qword_t low = values[array_size(values) - 1 - i];
                struct cpu_state cpu = {
                    .pc = UINT64_C(0x1000),
                    .sp = UINT64_C(0xfedcba9876543210),
                    .nzcv = UINT32_C(0xa0000000),
                    .x[0] = UINT64_MAX,
                    .x[1] = high,
                    .x[2] = low,
                };
                dword_t word = UINT32_C(0x13820020) |
                        (dword_t) sf << 31 |
                        (dword_t) sf << 22 |
                        (dword_t) lsb << 10;
                execute_instruction(&cpu, word);
                assert(cpu.x[0] == extract_oracle(
                        high, low, width, lsb));
                assert(cpu.pc == UINT64_C(0x1004));
                assert(cpu.sp == UINT64_C(0xfedcba9876543210));
                assert(cpu.nzcv == UINT32_C(0xa0000000));
            }
        }
    }
}

static void test_register_boundaries(void) {
    struct cpu_state cpu = {
        .pc = UINT64_C(0x1800),
        .sp = UINT64_C(0x123456789abcdef0),
        .nzcv = UINT32_C(0x60000000),
    };
    cpu.x[1] = UINT64_C(0x1122334455667788);
    execute_instruction(&cpu, UINT32_C(0x93c123e0));
    assert(cpu.x[0] == UINT64_C(0x0011223344556677));

    cpu.x[1] = UINT64_C(0x8877665544332211);
    execute_instruction(&cpu, UINT32_C(0x93df2020));
    assert(cpu.x[0] == UINT64_C(0x1100000000000000));

    cpu.x[1] = UINT64_C(0x0123456789abcdef);
    cpu.x[2] = UINT64_C(0xfedcba9876543210);
    execute_instruction(&cpu, UINT32_C(0x93c22021));
    assert(cpu.x[1] == UINT64_C(0xeffedcba98765432));

    cpu.x[1] = UINT64_C(0x0123456789abcdef);
    cpu.x[2] = UINT64_C(0xfedcba9876543210);
    execute_instruction(&cpu, UINT32_C(0x93c22022));
    assert(cpu.x[2] == UINT64_C(0xeffedcba98765432));

    cpu.x[0] = UINT64_C(0x13579bdf2468ace0);
    execute_instruction(&cpu, UINT32_C(0x93c2203f));
    assert(cpu.x[0] == UINT64_C(0x13579bdf2468ace0));
    assert(cpu.pc == UINT64_C(0x1814));
    assert(cpu.sp == UINT64_C(0x123456789abcdef0));
    assert(cpu.nzcv == UINT32_C(0x60000000));
}

int main(void) {
    test_decode();
    test_all_shifts();
    test_register_boundaries();
    return 0;
}
