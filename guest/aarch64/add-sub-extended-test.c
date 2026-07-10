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
        byte_t width, byte_t rd, byte_t rn, byte_t rm,
        enum aarch64_extend_type extend_type, byte_t shift) {
    struct aarch64_decoded instruction = decode(word);
    assert(instruction.opcode == opcode);
    assert(instruction.width == width);
    assert(instruction.operands.add_sub_extended.rd == rd);
    assert(instruction.operands.add_sub_extended.rn == rn);
    assert(instruction.operands.add_sub_extended.rm == rm);
    assert(instruction.operands.add_sub_extended.extend_type == extend_type);
    assert(instruction.operands.add_sub_extended.shift == shift);
}

static void test_decode(void) {
    assert_decode(UINT32_C(0x8b220020), AARCH64_OP_ADD_EXTENDED_REGISTER,
            64, 0, 1, 2, AARCH64_EXTEND_UXTB, 0);
    assert_decode(UINT32_C(0x8b2d87ec), AARCH64_OP_ADD_EXTENDED_REGISTER,
            64, 12, 31, 13, AARCH64_EXTEND_SXTB, 1);
    assert_decode(UINT32_C(0x8b2faddf), AARCH64_OP_ADD_EXTENDED_REGISTER,
            64, 31, 14, 15, AARCH64_EXTEND_SXTH, 3);
    assert_decode(UINT32_C(0x8b32d230), AARCH64_OP_ADD_EXTENDED_REGISTER,
            64, 16, 17, 18, AARCH64_EXTEND_SXTW, 4);
    assert_decode(UINT32_C(0x8b35e693), AARCH64_OP_ADD_EXTENDED_REGISTER,
            64, 19, 20, 21, AARCH64_EXTEND_SXTX, 1);
    assert_decode(UINT32_C(0x0b221020), AARCH64_OP_ADD_EXTENDED_REGISTER,
            32, 0, 1, 2, AARCH64_EXTEND_UXTB, 4);
    assert_decode(UINT32_C(0x0b24247f), AARCH64_OP_ADD_EXTENDED_REGISTER,
            32, 31, 3, 4, AARCH64_EXTEND_UXTH, 1);
    assert_decode(UINT32_C(0x0b264be5), AARCH64_OP_ADD_EXTENDED_REGISTER,
            32, 5, 31, 6, AARCH64_EXTEND_UXTW, 2);
    assert_decode(UINT32_C(0xab37cbf6), AARCH64_OP_ADDS_EXTENDED_REGISTER,
            64, 22, 31, 23, AARCH64_EXTEND_SXTW, 2);
    assert_decode(UINT32_C(0xab3f73ff), AARCH64_OP_ADDS_EXTENDED_REGISTER,
            64, 31, 31, 31, AARCH64_EXTEND_UXTX, 4);
    assert_decode(UINT32_C(0xcb384fff), AARCH64_OP_SUB_EXTENDED_REGISTER,
            64, 31, 31, 24, AARCH64_EXTEND_UXTW, 3);
    assert_decode(UINT32_C(0x6b39a7ff), AARCH64_OP_SUBS_EXTENDED_REGISTER,
            32, 31, 31, 25, AARCH64_EXTEND_SXTH, 1);
    assert_decode(UINT32_C(0xeb221020), AARCH64_OP_SUBS_EXTENDED_REGISTER,
            64, 0, 1, 2, AARCH64_EXTEND_UXTB, 4);
    assert_decode(UINT32_C(0x4b25c883), AARCH64_OP_SUB_EXTENDED_REGISTER,
            32, 3, 4, 5, AARCH64_EXTEND_SXTW, 2);
    assert_decode(UINT32_C(0x2b2787e6), AARCH64_OP_ADDS_EXTENDED_REGISTER,
            32, 6, 31, 7, AARCH64_EXTEND_SXTB, 1);
}

static void test_encoding_space(void) {
    static const enum aarch64_opcode opcodes[] = {
        AARCH64_OP_ADD_EXTENDED_REGISTER,
        AARCH64_OP_ADDS_EXTENDED_REGISTER,
        AARCH64_OP_SUB_EXTENDED_REGISTER,
        AARCH64_OP_SUBS_EXTENDED_REGISTER,
    };
    unsigned valid = 0;
    for (unsigned is_64 = 0; is_64 < 2; is_64++) {
        for (unsigned operation = 0; operation < 4; operation++) {
            for (unsigned option = 0; option < 8; option++) {
                for (unsigned shift = 0; shift < 8; shift++) {
                    dword_t word = UINT32_C(0x0b200020) |
                            (dword_t) is_64 << 31 |
                            (dword_t) operation << 29 |
                            UINT32_C(2) << 16 |
                            (dword_t) option << 13 |
                            (dword_t) shift << 10;
                    struct aarch64_decoded instruction;
                    bool decoded = aarch64_decode(word, &instruction);
                    assert(decoded == (shift <= 4));
                    if (!decoded)
                        continue;
                    valid++;
                    assert(instruction.opcode == opcodes[operation]);
                    assert(instruction.width == (is_64 ? 64 : 32));
                    assert(instruction.operands.add_sub_extended.extend_type ==
                            (enum aarch64_extend_type) option);
                    assert(instruction.operands.add_sub_extended.shift == shift);
                }
            }
        }
    }
    assert(valid == 320);

    struct aarch64_decoded instruction;
    assert(!aarch64_decode(UINT32_C(0x8b221420), &instruction));
    assert(!aarch64_decode(UINT32_C(0x8b221820), &instruction));
    assert(!aarch64_decode(UINT32_C(0x8b221c20), &instruction));
}

static void test_extension_semantics(void) {
    static const qword_t expected[] = {
        UINT64_C(0x0000000000000100),
        UINT64_C(0x0000000000010100),
        UINT64_C(0x0000000100010100),
        UINT64_C(0x0000000100010100),
        UINT64_C(0xffffffffffffff00),
        UINT64_C(0xffffffffffff0100),
        UINT64_C(0xffffffff00010100),
        UINT64_C(0x0000000100010100),
    };
    struct cpu_state cpu = {
        .pc = UINT64_C(0x1000),
        .nzcv = UINT32_C(0xa0000000),
    };
    for (unsigned option = 0; option < 8; option++) {
        cpu.x[1] = 0;
        cpu.x[2] = UINT64_C(0x8000000080008080);
        dword_t word = UINT32_C(0x8b220420) |
                (dword_t) option << 13;
        execute_instruction(&cpu, word);
        assert(cpu.x[0] == expected[option]);
    }

    cpu.x[0] = UINT64_MAX;
    cpu.x[1] = 0;
    cpu.x[2] = UINT64_C(0x8000000100000001);
    execute_instruction(&cpu, UINT32_C(0x0b226020));
    assert(cpu.x[0] == 1);
    cpu.x[0] = UINT64_MAX;
    execute_instruction(&cpu, UINT32_C(0x0b22e020));
    assert(cpu.x[0] == 1);
    assert(cpu.pc == UINT64_C(0x1028));
    assert(cpu.nzcv == UINT32_C(0xa0000000));
}

static void test_flags_and_stack_pointer(void) {
    struct cpu_state cpu = {
        .pc = UINT64_C(0x1800),
        .sp = UINT64_C(0x7fffffffffffffff),
        .nzcv = UINT32_C(0x50000000),
    };
    cpu.x[23] = 1;
    execute_instruction(&cpu, UINT32_C(0xab37cbf6));
    assert(cpu.x[22] == UINT64_C(0x8000000000000003));
    assert(cpu.nzcv == UINT32_C(0x90000000));
    assert(cpu.sp == UINT64_C(0x7fffffffffffffff));

    cpu.x[15] = UINT64_C(0xffffffff80000000);
    cpu.x[16] = UINT64_C(0xffffffffffff2000);
    execute_instruction(&cpu, UINT32_C(0x6b3029ee));
    assert(cpu.x[14] == UINT32_C(0x7fff8000));
    assert(cpu.nzcv == UINT32_C(0x30000000));

    cpu.sp = UINT64_MAX;
    execute_instruction(&cpu, UINT32_C(0xab3f73ff));
    assert(cpu.sp == UINT64_MAX);
    assert(cpu.nzcv == UINT32_C(0x80000000));

    cpu.sp = UINT64_C(0x1000);
    cpu.x[24] = 1;
    cpu.nzcv = UINT32_C(0x60000000);
    execute_instruction(&cpu, UINT32_C(0xcb384fff));
    assert(cpu.sp == UINT64_C(0xff8));
    assert(cpu.nzcv == UINT32_C(0x60000000));

    cpu.x[3] = UINT64_C(0xffffffff);
    cpu.x[4] = 1;
    execute_instruction(&cpu, UINT32_C(0x0b24247f));
    assert(cpu.sp == 1);
    assert(cpu.nzcv == UINT32_C(0x60000000));

    cpu.sp = UINT64_C(0xffffffff00000010);
    cpu.x[6] = 2;
    execute_instruction(&cpu, UINT32_C(0x0b264be5));
    assert(cpu.x[5] == UINT64_C(0x18));
    assert(cpu.sp == UINT64_C(0xffffffff00000010));
    assert(cpu.pc == UINT64_C(0x1818));
}

int main(void) {
    test_decode();
    test_encoding_space();
    test_extension_semantics();
    test_flags_and_stack_pointer();
    return 0;
}
