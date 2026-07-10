#include <assert.h>
#include <stdint.h>

#include "guest/aarch64/decode.h"
#include "guest/aarch64/execute.h"

static struct aarch64_decoded decode(dword_t word) {
    struct aarch64_decoded instruction;
    assert(aarch64_decode(word, &instruction));
    return instruction;
}

static void execute_instruction(struct cpu_state *cpu,
        const struct aarch64_decoded *instruction) {
    struct aarch64_execute_result result =
            aarch64_execute(cpu, NULL, instruction);
    assert(result.stop == AARCH64_EXECUTE_RETIRED);
}

static void test_add_sub(void) {
    struct cpu_state cpu = {.pc = 0x1000};
    cpu.x[1] = 10;
    struct aarch64_decoded instruction = decode(UINT32_C(0x91048c20));
    execute_instruction(&cpu, &instruction);
    assert(cpu.x[0] == 10 + 0x123);
    assert(cpu.pc == 0x1004);

    cpu.x[3] = 9;
    instruction = decode(UINT32_C(0xd1001462));
    execute_instruction(&cpu, &instruction);
    assert(cpu.x[2] == 4);

    cpu.x[1] = UINT64_MAX;
    instruction = decode(UINT32_C(0xb1000420));
    execute_instruction(&cpu, &instruction);
    assert(cpu.x[0] == 0);
    assert(cpu.nzcv == UINT32_C(0x60000000));

    cpu.x[1] = UINT64_C(0x8000000000000000);
    instruction = decode(UINT32_C(0xf1000420));
    execute_instruction(&cpu, &instruction);
    assert(cpu.x[0] == UINT64_C(0x7fffffffffffffff));
    assert(cpu.nzcv == UINT32_C(0x30000000));

    cpu.sp = 0x8000;
    instruction = decode(UINT32_C(0x910083ff));
    execute_instruction(&cpu, &instruction);
    assert(cpu.sp == 0x8020);
}

static void test_add_sub_shifted(void) {
    struct cpu_state cpu = {.pc = 0x1800};
    cpu.x[1] = 10;
    cpu.x[2] = 20;
    struct aarch64_decoded instruction = decode(UINT32_C(0x8b020020));
    assert(instruction.opcode == AARCH64_OP_ADD_SHIFTED_REGISTER);
    execute_instruction(&cpu, &instruction);
    assert(cpu.x[0] == 30);

    cpu.x[3] = UINT64_MAX;
    cpu.x[4] = 1;
    cpu.x[5] = 2;
    instruction = decode(UINT32_C(0x0b051c83));
    assert(instruction.operands.add_sub_shifted.shift == 7);
    execute_instruction(&cpu, &instruction);
    assert(cpu.x[3] == 257);

    cpu.x[6] = 5;
    cpu.x[7] = 7;
    instruction = decode(UINT32_C(0xeb0700df));
    execute_instruction(&cpu, &instruction);
    assert(cpu.nzcv == UINT32_C(0x80000000));

    cpu.x[9] = UINT32_C(0x80000000);
    instruction = decode(UINT32_C(0x4b897fe8));
    assert(instruction.operands.add_sub_shifted.shift_type ==
            AARCH64_SHIFT_ASR);
    execute_instruction(&cpu, &instruction);
    assert(cpu.x[8] == 1);
}

static void test_logical_shifted(void) {
    struct cpu_state cpu = {.pc = 0x1c00};
    cpu.x[11] = UINT64_C(0xffff);
    cpu.x[12] = 3;
    struct aarch64_decoded instruction = decode(UINT32_C(0x8a0c156a));
    assert(instruction.opcode == AARCH64_OP_AND_SHIFTED_REGISTER);
    execute_instruction(&cpu, &instruction);
    assert(cpu.x[10] == 0x60);

    cpu.x[14] = UINT64_C(0x123456789abcdef0);
    instruction = decode(UINT32_C(0xaa0e03ed));
    execute_instruction(&cpu, &instruction);
    assert(cpu.x[13] == cpu.x[14]);

    cpu.x[15] = UINT64_MAX;
    cpu.x[16] = 0;
    cpu.x[17] = 1;
    instruction = decode(UINT32_C(0x4ad12e0f));
    assert(instruction.operands.logical_shifted.shift_type ==
            AARCH64_SHIFT_ROR);
    execute_instruction(&cpu, &instruction);
    assert(cpu.x[15] == UINT32_C(0x00200000));

    cpu.x[18] = UINT64_C(0x8000000000000000);
    cpu.x[19] = UINT64_C(0x8000000000000000);
    instruction = decode(UINT32_C(0xea13025f));
    execute_instruction(&cpu, &instruction);
    assert(cpu.nzcv == UINT32_C(0x80000000));

    cpu.x[21] = UINT64_C(0xffff);
    cpu.x[22] = 8;
    instruction = decode(UINT32_C(0x8a760eb4));
    assert(instruction.operands.logical_shifted.invert);
    execute_instruction(&cpu, &instruction);
    assert(cpu.x[20] == UINT64_C(0xfffe));

    cpu.x[2] = UINT64_C(0xfedcba9876543210);
    instruction = decode(UINT32_C(0xaac203e0));
    execute_instruction(&cpu, &instruction);
    assert(cpu.x[0] == cpu.x[2]);
}

static void assert_logical_immediate(dword_t word,
        enum aarch64_opcode opcode, byte_t width, qword_t immediate) {
    struct aarch64_decoded instruction = decode(word);
    assert(instruction.opcode == opcode);
    assert(instruction.width == width);
    assert(instruction.operands.logical_immediate.immediate == immediate);
}

static void test_logical_immediate(void) {
    assert_logical_immediate(UINT32_C(0x1200f020),
            AARCH64_OP_AND_IMMEDIATE, 32, UINT32_C(0x55555555));
    assert_logical_immediate(UINT32_C(0x3201e062),
            AARCH64_OP_ORR_IMMEDIATE, 32, UINT32_C(0x88888888));
    assert_logical_immediate(UINT32_C(0xd2003d6a),
            AARCH64_OP_EOR_IMMEDIATE, 64,
            UINT64_C(0x0000ffff0000ffff));
    assert_logical_immediate(UINT32_C(0xf241059f),
            AARCH64_OP_ANDS_IMMEDIATE, 64,
            UINT64_C(0x8000000000000001));
    assert_logical_immediate(UINT32_C(0xf2009cc5),
            AARCH64_OP_ANDS_IMMEDIATE, 64,
            UINT64_C(0x00ff00ff00ff00ff));
    assert_logical_immediate(UINT32_C(0xd201f083),
            AARCH64_OP_EOR_IMMEDIATE, 64,
            UINT64_C(0xaaaaaaaaaaaaaaaa));
    assert_logical_immediate(UINT32_C(0xd23ff083),
            AARCH64_OP_EOR_IMMEDIATE, 64,
            UINT64_C(0xaaaaaaaaaaaaaaaa));

    struct cpu_state cpu = {.pc = 0x2400};
    struct aarch64_decoded instruction = decode(UINT32_C(0xb24003e0));
    execute_instruction(&cpu, &instruction);
    assert(cpu.x[0] == 1);

    cpu.x[2] = UINT64_MAX;
    instruction = decode(UINT32_C(0x92410441));
    execute_instruction(&cpu, &instruction);
    assert(cpu.x[1] == UINT64_C(0x8000000000000001));

    cpu.x[4] = 0;
    instruction = decode(UINT32_C(0xd201f083));
    execute_instruction(&cpu, &instruction);
    assert(cpu.x[3] == UINT64_C(0xaaaaaaaaaaaaaaaa));

    cpu.x[7] = UINT64_MAX;
    instruction = decode(UINT32_C(0x320107e7));
    execute_instruction(&cpu, &instruction);
    assert(cpu.x[7] == UINT32_C(0x80000001));

    cpu.x[6] = UINT64_MAX;
    cpu.nzcv = UINT32_C(0xf0000000);
    instruction = decode(UINT32_C(0xf2009cc5));
    execute_instruction(&cpu, &instruction);
    assert(cpu.x[5] == UINT64_C(0x00ff00ff00ff00ff));
    assert(cpu.nzcv == 0);

    cpu.sp = UINT64_C(0xabcdef0000000000);
    cpu.x[15] = UINT64_C(0x1200);
    instruction = decode(UINT32_C(0xb2401dff));
    execute_instruction(&cpu, &instruction);
    assert(cpu.sp == UINT64_C(0x12ff));

    cpu.x[16] = UINT64_MAX;
    instruction = decode(UINT32_C(0x121c6e1f));
    execute_instruction(&cpu, &instruction);
    assert(cpu.sp == UINT32_C(0xfffffff0));

    cpu.sp = UINT64_C(0x123456789abcdef0);
    cpu.x[12] = UINT64_C(0x8000000000000000);
    cpu.nzcv = UINT32_C(0xf0000000);
    instruction = decode(UINT32_C(0xf241059f));
    execute_instruction(&cpu, &instruction);
    assert(cpu.sp == UINT64_C(0x123456789abcdef0));
    assert(cpu.nzcv == UINT32_C(0x80000000));
}

static void test_logical_immediate_encoding_space(void) {
    unsigned valid[2] = {0};
    for (unsigned is_64 = 0; is_64 < 2; is_64++) {
        for (unsigned n = 0; n < 2; n++) {
            for (unsigned rotation = 0; rotation < 64; rotation++) {
                for (unsigned ones = 0; ones < 64; ones++) {
                    dword_t word = UINT32_C(0x12000000) |
                            (dword_t) is_64 << 31 |
                            (dword_t) n << 22 |
                            (dword_t) rotation << 16 |
                            (dword_t) ones << 10;
                    struct aarch64_decoded instruction;
                    if (!aarch64_decode(word, &instruction))
                        continue;
                    valid[is_64]++;
                    qword_t immediate =
                            instruction.operands.logical_immediate.immediate;
                    assert(immediate != 0);
                    assert(immediate != (is_64 ? UINT64_MAX : UINT32_MAX));
                    if (!is_64)
                        assert((immediate >> 32) == 0);
                }
            }
        }
    }
    assert(valid[0] == 3648);
    assert(valid[1] == 7680);
}

static void assert_bitfield_move(dword_t word,
        enum aarch64_opcode opcode, byte_t width, byte_t rd, byte_t rn,
        byte_t immr, byte_t imms, qword_t write_mask, qword_t top_mask) {
    struct aarch64_decoded instruction = decode(word);
    assert(instruction.opcode == opcode);
    assert(instruction.width == width);
    assert(instruction.operands.bitfield_move.rd == rd);
    assert(instruction.operands.bitfield_move.rn == rn);
    assert(instruction.operands.bitfield_move.immr == immr);
    assert(instruction.operands.bitfield_move.imms == imms);
    assert(instruction.operands.bitfield_move.write_mask == write_mask);
    assert(instruction.operands.bitfield_move.top_mask == top_mask);
}

static void test_bitfield_move_decode(void) {
    assert_bitfield_move(UINT32_C(0xd348fc83), AARCH64_OP_UBFM,
            64, 3, 4, 8, 63, UINT64_MAX,
            UINT64_C(0x00ffffffffffffff));
    assert_bitfield_move(UINT32_C(0x13077cc5), AARCH64_OP_SBFM,
            32, 5, 6, 7, 31, UINT32_MAX, UINT32_C(0x01ffffff));
    assert_bitfield_move(UINT32_C(0xd373c820), AARCH64_OP_UBFM,
            64, 0, 1, 51, 50, UINT64_C(0xffffffffffffe000),
            UINT64_MAX);
    assert_bitfield_move(UINT32_C(0xb3744dac), AARCH64_OP_BFM,
            64, 12, 13, 52, 19, UINT64_C(0x00000000fffff000),
            UINT32_MAX);
    assert_bitfield_move(UINT32_C(0x33073dee), AARCH64_OP_BFM,
            32, 14, 15, 7, 15, UINT32_C(0xfe0001ff),
            UINT32_C(0x000001ff));

    assert_bitfield_move(UINT32_C(0xd340fc20), AARCH64_OP_UBFM,
            64, 0, 1, 0, 63, UINT64_MAX, UINT64_MAX);
    assert_bitfield_move(UINT32_C(0xd37ffc20), AARCH64_OP_UBFM,
            64, 0, 1, 63, 63, UINT64_MAX, 1);
    assert_bitfield_move(UINT32_C(0xd3483c20), AARCH64_OP_UBFM,
            64, 0, 1, 8, 15, UINT64_C(0xff000000000000ff),
            UINT64_C(0xff));
    assert_bitfield_move(UINT32_C(0xd3781c20), AARCH64_OP_UBFM,
            64, 0, 1, 56, 7, UINT64_C(0xff00), UINT64_C(0xffff));
    assert_bitfield_move(UINT32_C(0xd378dc20), AARCH64_OP_UBFM,
            64, 0, 1, 56, 55, UINT64_C(0xffffffffffffff00),
            UINT64_MAX);
}

static qword_t bitfield_mask_oracle(unsigned width, unsigned immr,
        unsigned imms, bool top) {
    unsigned levels = width - 1;
    unsigned d = ((unsigned) imms + width - immr) & levels;
    qword_t result = 0;
    for (unsigned bit = 0; bit < width; bit++) {
        bool set = top ? bit <= d :
                ((bit + immr) & levels) <= imms;
        if (set)
            result |= UINT64_C(1) << bit;
    }
    return result;
}

static void test_bitfield_move_encoding_space(void) {
    unsigned valid[2][3] = {{0}};
    static const enum aarch64_opcode opcodes[] = {
        AARCH64_OP_SBFM,
        AARCH64_OP_BFM,
        AARCH64_OP_UBFM,
    };
    for (unsigned is_64 = 0; is_64 < 2; is_64++) {
        for (unsigned n = 0; n < 2; n++) {
            for (unsigned operation = 0; operation < 4; operation++) {
                for (unsigned immr = 0; immr < 64; immr++) {
                    for (unsigned imms = 0; imms < 64; imms++) {
                        dword_t word = UINT32_C(0x13000000) |
                                (dword_t) is_64 << 31 |
                                (dword_t) operation << 29 |
                                (dword_t) n << 22 |
                                (dword_t) immr << 16 |
                                (dword_t) imms << 10;
                        bool expected = operation < 3 && n == is_64 &&
                                (is_64 || (immr < 32 && imms < 32));
                        struct aarch64_decoded instruction;
                        bool decoded = aarch64_decode(word, &instruction);
                        assert(decoded == expected);
                        if (!decoded)
                            continue;

                        valid[is_64][operation]++;
                        byte_t width = is_64 ? 64 : 32;
                        assert(instruction.opcode == opcodes[operation]);
                        assert(instruction.width == width);
                        assert(instruction.operands.bitfield_move.write_mask ==
                                bitfield_mask_oracle(width, immr, imms, false));
                        assert(instruction.operands.bitfield_move.top_mask ==
                                bitfield_mask_oracle(width, immr, imms, true));
                    }
                }
            }
        }
    }
    for (unsigned operation = 0; operation < 3; operation++) {
        assert(valid[0][operation] == 1024);
        assert(valid[1][operation] == 4096);
    }
}

static void test_bitfield_move_execute(void) {
    struct cpu_state cpu = {
        .pc = UINT64_C(0x2c00),
        .sp = UINT64_C(0xfedcba9876543210),
        .nzcv = UINT32_C(0xa0000000),
    };
    cpu.x[4] = UINT64_C(0x123456789abcdef0);
    struct aarch64_decoded instruction = decode(UINT32_C(0xd348fc83));
    execute_instruction(&cpu, &instruction);
    assert(cpu.x[3] == UINT64_C(0x00123456789abcde));

    cpu.x[6] = UINT64_C(0xffffffff80000080);
    instruction = decode(UINT32_C(0x13077cc5));
    execute_instruction(&cpu, &instruction);
    assert(cpu.x[5] == UINT32_C(0xff000001));

    cpu.x[1] = UINT64_C(0x123);
    instruction = decode(UINT32_C(0xd373c820));
    execute_instruction(&cpu, &instruction);
    assert(cpu.x[0] == UINT64_C(0x246000));

    cpu.x[3] = UINT64_C(0xffffffffffffffa5);
    instruction = decode(UINT32_C(0x53001c62));
    execute_instruction(&cpu, &instruction);
    assert(cpu.x[2] == UINT64_C(0xa5));

    cpu.x[5] = UINT64_C(0xffffffffffffbeef);
    instruction = decode(UINT32_C(0x53003ca4));
    execute_instruction(&cpu, &instruction);
    assert(cpu.x[4] == UINT64_C(0xbeef));

    cpu.x[7] = UINT64_C(0x80);
    instruction = decode(UINT32_C(0x93401ce6));
    execute_instruction(&cpu, &instruction);
    assert(cpu.x[6] == UINT64_C(0xffffffffffffff80));

    cpu.x[9] = UINT64_C(0xffffffff00008001);
    instruction = decode(UINT32_C(0x13003d28));
    execute_instruction(&cpu, &instruction);
    assert(cpu.x[8] == UINT32_C(0xffff8001));

    cpu.x[11] = UINT64_C(0x80000001);
    instruction = decode(UINT32_C(0x93407d6a));
    execute_instruction(&cpu, &instruction);
    assert(cpu.x[10] == UINT64_C(0xffffffff80000001));

    cpu.x[12] = UINT64_C(0xffff000000000fff);
    cpu.x[13] = UINT64_C(0xabcde);
    instruction = decode(UINT32_C(0xb3744dac));
    execute_instruction(&cpu, &instruction);
    assert(cpu.x[12] == UINT64_C(0xffff0000abcdefff));

    cpu.x[14] = UINT64_C(0xffffffffaaaaaa00);
    cpu.x[15] = UINT64_C(0xff80);
    instruction = decode(UINT32_C(0x33073dee));
    execute_instruction(&cpu, &instruction);
    assert(cpu.x[14] == UINT32_C(0xaaaaabff));

    cpu.x[17] = UINT64_C(0x80000200);
    instruction = decode(UINT32_C(0x93497e30));
    execute_instruction(&cpu, &instruction);
    assert(cpu.x[16] == UINT64_C(0xffffffffffc00001));

    cpu.x[19] = UINT64_C(0xffe0);
    instruction = decode(UINT32_C(0x53053e72));
    execute_instruction(&cpu, &instruction);
    assert(cpu.x[18] == UINT64_C(0x7ff));

    cpu.x[12] = UINT64_C(0xffff0000000abcde);
    instruction = decode(UINT32_C(0xb3744d8c));
    execute_instruction(&cpu, &instruction);
    assert(cpu.x[12] == UINT64_C(0xffff0000abcdecde));

    cpu.x[0] = UINT64_MAX;
    instruction = decode(UINT32_C(0xd340ffe0));
    execute_instruction(&cpu, &instruction);
    assert(cpu.x[0] == 0);

    cpu.x[0] = UINT64_C(0x13579bdf2468ace0);
    cpu.x[1] = UINT64_MAX;
    instruction = decode(UINT32_C(0xd340fc3f));
    execute_instruction(&cpu, &instruction);
    assert(cpu.x[0] == UINT64_C(0x13579bdf2468ace0));
    assert(cpu.sp == UINT64_C(0xfedcba9876543210));
    assert(cpu.nzcv == UINT32_C(0xa0000000));
    assert(cpu.pc == UINT64_C(0x2c3c));
}

static void test_move_wide(void) {
    struct cpu_state cpu = {.pc = 0x2000};
    struct aarch64_decoded instruction = decode(UINT32_C(0xd2a24680));
    execute_instruction(&cpu, &instruction);
    assert(cpu.x[0] == UINT64_C(0x12340000));

    instruction = decode(UINT32_C(0xf2cacf00));
    execute_instruction(&cpu, &instruction);
    assert(cpu.x[0] == UINT64_C(0x567812340000));

    instruction = decode(UINT32_C(0x12800001));
    execute_instruction(&cpu, &instruction);
    assert(cpu.x[1] == UINT32_MAX);
}

static void test_pc_relative(void) {
    struct cpu_state cpu = {.pc = 0x44};
    struct aarch64_decoded instruction = decode(UINT32_C(0x10000047));
    assert(instruction.opcode == AARCH64_OP_ADR);
    assert(instruction.operands.pc_relative.displacement == 8);
    execute_instruction(&cpu, &instruction);
    assert(cpu.x[7] == 0x4c);

    cpu.pc = UINT64_C(0x10123);
    instruction = decode(UINT32_C(0x90000028));
    assert(instruction.opcode == AARCH64_OP_ADRP);
    assert(instruction.operands.pc_relative.displacement == 4 * 4096);
    execute_instruction(&cpu, &instruction);
    assert(cpu.x[8] == UINT64_C(0x14000));

    cpu.pc = UINT64_C(0x12345);
    instruction = decode(UINT32_C(0xd0ffffe9));
    assert(instruction.operands.pc_relative.displacement == -2 * 4096);
    execute_instruction(&cpu, &instruction);
    assert(cpu.x[9] == UINT64_C(0x10000));

    cpu.pc = UINT64_C(0x2000);
    instruction = decode(UINT32_C(0x10ffffea));
    assert(instruction.operands.pc_relative.displacement == -4);
    execute_instruction(&cpu, &instruction);
    assert(cpu.x[10] == UINT64_C(0x1ffc));

    qword_t old_x0 = cpu.x[0];
    instruction = decode(UINT32_C(0x1000005f));
    assert(instruction.operands.pc_relative.rd == 31);
    execute_instruction(&cpu, &instruction);
    assert(cpu.x[0] == old_x0);
    assert(cpu.pc == UINT64_C(0x2008));

    cpu.pc = UINT64_MAX - 3;
    instruction = decode(UINT32_C(0x10000047));
    execute_instruction(&cpu, &instruction);
    assert(cpu.x[7] == 4);
    assert(cpu.pc == 0);

    cpu.pc = UINT64_C(0x123);
    instruction = decode(UINT32_C(0xf0ffffe0));
    execute_instruction(&cpu, &instruction);
    assert(cpu.x[0] == UINT64_C(0xfffffffffffff000));
}

static void assert_conditional_select(dword_t word,
        enum aarch64_opcode opcode, byte_t width, byte_t rd, byte_t rn,
        byte_t rm, byte_t condition) {
    struct aarch64_decoded instruction = decode(word);
    assert(instruction.opcode == opcode);
    assert(instruction.width == width);
    assert(instruction.operands.conditional_select.rd == rd);
    assert(instruction.operands.conditional_select.rn == rn);
    assert(instruction.operands.conditional_select.rm == rm);
    assert(instruction.operands.conditional_select.condition == condition);
}

static void test_conditional_select(void) {
    assert_conditional_select(UINT32_C(0x9a9702d5), AARCH64_OP_CSEL,
            64, 21, 22, 23, 0);
    assert_conditional_select(UINT32_C(0x1a9a1738), AARCH64_OP_CSINC,
            32, 24, 25, 26, 1);
    assert_conditional_select(UINT32_C(0xda9da39b), AARCH64_OP_CSINV,
            64, 27, 28, 29, 10);
    assert_conditional_select(UINT32_C(0xda82b420), AARCH64_OP_CSNEG,
            64, 0, 1, 2, 11);

    struct cpu_state cpu = {
        .pc = UINT64_C(0x2800),
        .nzcv = UINT32_C(0x40000000),
        .x[22] = 10,
        .x[23] = 20,
    };
    struct aarch64_decoded instruction = decode(UINT32_C(0x9a9702d5));
    execute_instruction(&cpu, &instruction);
    assert(cpu.x[21] == 10);
    assert(cpu.nzcv == UINT32_C(0x40000000));
    cpu.nzcv = 0;
    execute_instruction(&cpu, &instruction);
    assert(cpu.x[21] == 20);
    assert(cpu.nzcv == 0);

    cpu.nzcv = UINT32_C(0x40000000);
    cpu.x[26] = UINT32_MAX;
    instruction = decode(UINT32_C(0x1a9a1738));
    execute_instruction(&cpu, &instruction);
    assert(cpu.x[24] == 0);
    assert(cpu.nzcv == UINT32_C(0x40000000));

    cpu.nzcv = UINT32_C(0xf0000000);
    cpu.x[25] = UINT64_C(0xffffffff12345678);
    instruction = decode(UINT32_C(0x1a9ae738));
    execute_instruction(&cpu, &instruction);
    assert(cpu.x[24] == UINT32_C(0x12345678));
    assert(cpu.nzcv == UINT32_C(0xf0000000));

    cpu.nzcv = 0;
    cpu.x[8] = UINT64_MAX;
    instruction = decode(UINT32_C(0x9a8824e6));
    execute_instruction(&cpu, &instruction);
    assert(cpu.x[6] == 0);
    assert(cpu.nzcv == 0);

    cpu.nzcv = UINT32_C(0x80000000);
    cpu.x[29] = UINT64_C(0x00ff00ff00ff00ff);
    instruction = decode(UINT32_C(0xda9da39b));
    execute_instruction(&cpu, &instruction);
    assert(cpu.x[27] == UINT64_C(0xff00ff00ff00ff00));
    assert(cpu.nzcv == UINT32_C(0x80000000));

    cpu.nzcv = 0;
    cpu.x[28] = 42;
    execute_instruction(&cpu, &instruction);
    assert(cpu.x[27] == 42);

    cpu.nzcv = UINT32_C(0x80000000);
    cpu.x[15] = UINT64_MAX;
    cpu.x[17] = UINT64_C(0xffffffff00000000);
    instruction = decode(UINT32_C(0x5a91520f));
    execute_instruction(&cpu, &instruction);
    assert(cpu.x[15] == UINT32_MAX);
    assert(cpu.nzcv == UINT32_C(0x80000000));

    cpu.nzcv = 0;
    cpu.x[2] = UINT64_C(0x8000000000000000);
    instruction = decode(UINT32_C(0xda82b420));
    execute_instruction(&cpu, &instruction);
    assert(cpu.x[0] == UINT64_C(0x8000000000000000));
    assert(cpu.nzcv == 0);

    cpu.nzcv = UINT32_C(0x50000000);
    cpu.x[1] = UINT64_C(0x123456789abcdef0);
    cpu.x[2] = 5;
    instruction = decode(UINT32_C(0xda82f420));
    execute_instruction(&cpu, &instruction);
    assert(cpu.x[0] == UINT64_C(0x123456789abcdef0));
    assert(cpu.nzcv == UINT32_C(0x50000000));

    cpu.nzcv = UINT32_C(0x10000000);
    cpu.x[23] = UINT64_C(0xffffffff80000000);
    instruction = decode(UINT32_C(0x5a9776d5));
    execute_instruction(&cpu, &instruction);
    assert(cpu.x[21] == UINT32_C(0x80000000));
    assert(cpu.nzcv == UINT32_C(0x10000000));

    cpu.nzcv = UINT32_C(0x40000000);
    cpu.sp = UINT64_C(0xfedcba9876543210);
    instruction = decode(UINT32_C(0x1a9f17e3));
    execute_instruction(&cpu, &instruction);
    assert(cpu.x[3] == 1);
    assert(cpu.sp == UINT64_C(0xfedcba9876543210));
    assert(cpu.nzcv == UINT32_C(0x40000000));

    qword_t old_x0 = cpu.x[0];
    instruction = decode(UINT32_C(0x9a82003f));
    execute_instruction(&cpu, &instruction);
    assert(cpu.x[0] == old_x0);
    assert(cpu.sp == UINT64_C(0xfedcba9876543210));
}

static void test_branches(void) {
    struct cpu_state cpu = {.pc = 0x3000};
    struct aarch64_decoded instruction = decode(UINT32_C(0x14000002));
    execute_instruction(&cpu, &instruction);
    assert(cpu.pc == 0x3008);

    cpu.pc = 0x3000;
    instruction = decode(UINT32_C(0x17ffffff));
    execute_instruction(&cpu, &instruction);
    assert(cpu.pc == 0x2ffc);

    cpu.pc = 0x4000;
    instruction = decode(UINT32_C(0x94000002));
    execute_instruction(&cpu, &instruction);
    assert(cpu.pc == 0x4008);
    assert(cpu.x[30] == 0x4004);

    cpu.x[3] = 0x5000;
    instruction = decode(UINT32_C(0xd61f0060));
    execute_instruction(&cpu, &instruction);
    assert(cpu.pc == 0x5000);

    cpu.pc = 0x5100;
    cpu.x[4] = 0x5200;
    instruction = decode(UINT32_C(0xd63f0080));
    execute_instruction(&cpu, &instruction);
    assert(cpu.pc == 0x5200);
    assert(cpu.x[30] == 0x5104);

    cpu.x[30] = 0x6000;
    instruction = decode(UINT32_C(0xd65f03c0));
    execute_instruction(&cpu, &instruction);
    assert(cpu.pc == 0x6000);
}

static void test_conditional_branches(void) {
    struct cpu_state cpu = {.pc = 0x7000, .nzcv = UINT32_C(0x40000000)};
    struct aarch64_decoded instruction = decode(UINT32_C(0x540000c0));
    assert(instruction.opcode == AARCH64_OP_B_CONDITIONAL);
    execute_instruction(&cpu, &instruction);
    assert(cpu.pc == 0x7018);

    cpu.pc = 0x7000;
    cpu.nzcv = 0;
    execute_instruction(&cpu, &instruction);
    assert(cpu.pc == 0x7004);

    cpu.pc = 0x7100;
    cpu.nzcv = UINT32_C(0x40000000);
    instruction = decode(UINT32_C(0x54ffff80));
    execute_instruction(&cpu, &instruction);
    assert(cpu.pc == 0x70f0);

    cpu.pc = 0x7200;
    cpu.x[3] = UINT64_C(0xffffffff00000000);
    instruction = decode(UINT32_C(0x34000083));
    assert(instruction.width == 32);
    execute_instruction(&cpu, &instruction);
    assert(cpu.pc == 0x7210);

    cpu.pc = 0x7300;
    cpu.x[4] = 1;
    instruction = decode(UINT32_C(0xb5000084));
    execute_instruction(&cpu, &instruction);
    assert(cpu.pc == 0x7310);

    cpu.pc = 0x7400;
    cpu.x[5] = 0;
    instruction = decode(UINT32_C(0x36380045));
    assert(instruction.operands.test_branch.bit == 7);
    execute_instruction(&cpu, &instruction);
    assert(cpu.pc == 0x7408);

    cpu.pc = 0x7500;
    cpu.x[6] = UINT64_C(1) << 42;
    instruction = decode(UINT32_C(0xb7500046));
    assert(instruction.operands.test_branch.bit == 42);
    execute_instruction(&cpu, &instruction);
    assert(cpu.pc == 0x7508);
}

static void assert_load_store(dword_t word, enum aarch64_opcode opcode,
        byte_t size, byte_t rn, byte_t rt, int64_t offset) {
    struct aarch64_decoded instruction = decode(word);
    assert(instruction.opcode == opcode);
    assert(instruction.width == (size == 8 ? 64 : 32));
    assert(instruction.operands.load_store.size == size);
    assert(instruction.operands.load_store.rn == rn);
    assert(instruction.operands.load_store.rt == rt);
    assert(instruction.operands.load_store.offset == offset);
    assert(!instruction.operands.load_store.signed_load);
}

static void test_load_store_decode(void) {
    assert_load_store(UINT32_C(0xf9400020),
            AARCH64_OP_LOAD_IMM12, 8, 1, 0, 0);
    assert_load_store(UINT32_C(0xf9000020),
            AARCH64_OP_STORE_IMM12, 8, 1, 0, 0);
    assert_load_store(UINT32_C(0xb9400c62),
            AARCH64_OP_LOAD_IMM12, 4, 3, 2, 12);
    assert_load_store(UINT32_C(0xb9000c62),
            AARCH64_OP_STORE_IMM12, 4, 3, 2, 12);
    assert_load_store(UINT32_C(0x39401ca4),
            AARCH64_OP_LOAD_IMM12, 1, 5, 4, 7);
    assert_load_store(UINT32_C(0x39001ca4),
            AARCH64_OP_STORE_IMM12, 1, 5, 4, 7);
    assert_load_store(UINT32_C(0x79400ce6),
            AARCH64_OP_LOAD_IMM12, 2, 7, 6, 6);
    assert_load_store(UINT32_C(0x79000ce6),
            AARCH64_OP_STORE_IMM12, 2, 7, 6, 6);
    assert_load_store(UINT32_C(0xf9400be9),
            AARCH64_OP_LOAD_IMM12, 8, 31, 9, 16);

    struct aarch64_decoded instruction = decode(UINT32_C(0xf85f8020));
    assert(instruction.opcode == AARCH64_OP_LOAD_IMM9);
    assert(instruction.operands.load_store.offset == -8);
    assert(instruction.operands.load_store.address_mode ==
            AARCH64_ADDRESS_OFFSET);
    instruction = decode(UINT32_C(0xb8007062));
    assert(instruction.opcode == AARCH64_OP_STORE_IMM9);
    assert(instruction.operands.load_store.offset == 7);
    instruction = decode(UINT32_C(0xf84084a4));
    assert(instruction.operands.load_store.address_mode ==
            AARCH64_ADDRESS_POST_INDEX);
    assert(instruction.operands.load_store.offset == 8);
    instruction = decode(UINT32_C(0xb81fcce6));
    assert(instruction.operands.load_store.address_mode ==
            AARCH64_ADDRESS_PRE_INDEX);
    assert(instruction.operands.load_store.offset == -4);
}

static void assert_signed_load(dword_t word, enum aarch64_opcode opcode,
        byte_t size, byte_t width, byte_t rn, byte_t rt, int64_t offset,
        enum aarch64_address_mode address_mode) {
    struct aarch64_decoded instruction = decode(word);
    assert(instruction.opcode == opcode);
    assert(instruction.width == width);
    assert(instruction.operands.load_store.size == size);
    assert(instruction.operands.load_store.rn == rn);
    assert(instruction.operands.load_store.rt == rt);
    assert(instruction.operands.load_store.offset == offset);
    assert(instruction.operands.load_store.address_mode == address_mode);
    assert(instruction.operands.load_store.signed_load);
}

static void test_signed_load_decode(void) {
    assert_signed_load(UINT32_C(0x39800d49), AARCH64_OP_LOAD_IMM12,
            1, 64, 10, 9, 3, AARCH64_ADDRESS_OFFSET);
    assert_signed_load(UINT32_C(0x39c0156a), AARCH64_OP_LOAD_IMM12,
            1, 32, 11, 10, 5, AARCH64_ADDRESS_OFFSET);
    assert_signed_load(UINT32_C(0x79c0098b), AARCH64_OP_LOAD_IMM12,
            2, 32, 12, 11, 4, AARCH64_ADDRESS_OFFSET);
    assert_signed_load(UINT32_C(0x79800dac), AARCH64_OP_LOAD_IMM12,
            2, 64, 13, 12, 6, AARCH64_ADDRESS_OFFSET);
    assert_signed_load(UINT32_C(0xb98009cd), AARCH64_OP_LOAD_IMM12,
            4, 64, 14, 13, 8, AARCH64_ADDRESS_OFFSET);
    assert_signed_load(UINT32_C(0x389ff020), AARCH64_OP_LOAD_IMM9,
            1, 64, 1, 0, -1, AARCH64_ADDRESS_OFFSET);
    assert_signed_load(UINT32_C(0x78c03062), AARCH64_OP_LOAD_IMM9,
            2, 32, 3, 2, 3, AARCH64_ADDRESS_OFFSET);
    assert_signed_load(UINT32_C(0xb88044a4), AARCH64_OP_LOAD_IMM9,
            4, 64, 5, 4, 4, AARCH64_ADDRESS_POST_INDEX);
    assert_signed_load(UINT32_C(0x38dfece6), AARCH64_OP_LOAD_IMM9,
            1, 32, 7, 6, -2, AARCH64_ADDRESS_PRE_INDEX);
    assert_signed_load(UINT32_C(0x389fffff), AARCH64_OP_LOAD_IMM9,
            1, 64, 31, 31, -1, AARCH64_ADDRESS_PRE_INDEX);
}

static void assert_load_store_pair(dword_t word, enum aarch64_opcode opcode,
        byte_t size, byte_t rn, byte_t rt, byte_t rt2, int64_t offset,
        enum aarch64_address_mode address_mode) {
    struct aarch64_decoded instruction = decode(word);
    assert(instruction.opcode == opcode);
    assert(instruction.width == size * 8);
    assert(instruction.operands.load_store_pair.rn == rn);
    assert(instruction.operands.load_store_pair.rt == rt);
    assert(instruction.operands.load_store_pair.rt2 == rt2);
    assert(instruction.operands.load_store_pair.offset == offset);
    assert(instruction.operands.load_store_pair.address_mode == address_mode);
}

static void test_load_store_pair_decode(void) {
    assert_load_store_pair(UINT32_C(0xa9bf7bfd), AARCH64_OP_STORE_PAIR,
            8, 31, 29, 30, -16, AARCH64_ADDRESS_PRE_INDEX);
    assert_load_store_pair(UINT32_C(0xa8c17bfd), AARCH64_OP_LOAD_PAIR,
            8, 31, 29, 30, 16, AARCH64_ADDRESS_POST_INDEX);
    assert_load_store_pair(UINT32_C(0x29010440), AARCH64_OP_STORE_PAIR,
            4, 2, 0, 1, 8, AARCH64_ADDRESS_OFFSET);
    assert_load_store_pair(UINT32_C(0xa94190a3), AARCH64_OP_LOAD_PAIR,
            8, 5, 3, 4, 24, AARCH64_ADDRESS_OFFSET);
    assert_load_store_pair(UINT32_C(0xa9400400), AARCH64_OP_LOAD_PAIR,
            8, 0, 0, 1, 0, AARCH64_ADDRESS_OFFSET);
}

static void test_svc_decode(void) {
    struct aarch64_decoded instruction = decode(UINT32_C(0xd4000001));
    assert(instruction.opcode == AARCH64_OP_SVC);
    assert(instruction.operands.exception.immediate == 0);

    instruction = decode(UINT32_C(0xd4000541));
    assert(instruction.opcode == AARCH64_OP_SVC);
    assert(instruction.operands.exception.immediate == 42);
}

int main(void) {
    struct cpu_state cpu = {.pc = 0x1000};
    struct aarch64_decoded nop = decode(UINT32_C(0xd503201f));
    execute_instruction(&cpu, &nop);
    assert(cpu.pc == 0x1004);

    test_add_sub();
    test_add_sub_shifted();
    test_logical_shifted();
    test_logical_immediate();
    test_logical_immediate_encoding_space();
    test_bitfield_move_decode();
    test_bitfield_move_encoding_space();
    test_bitfield_move_execute();
    test_move_wide();
    test_pc_relative();
    test_conditional_select();
    test_branches();
    test_conditional_branches();
    test_load_store_decode();
    test_signed_load_decode();
    test_load_store_pair_decode();
    test_svc_decode();

    struct aarch64_decoded invalid;
    assert(!aarch64_decode(UINT32_C(0x32800000), &invalid));
    assert(!aarch64_decode(UINT32_C(0xd61f03e0), &invalid));
    assert(!aarch64_decode(UINT32_C(0x7d800000), &invalid));
    assert(!aarch64_decode(UINT32_C(0xd4000002), &invalid));
    assert(!aarch64_decode(UINT32_C(0x0b058083), &invalid));
    assert(!aarch64_decode(UINT32_C(0x8bc20020), &invalid));
    assert(!aarch64_decode(UINT32_C(0x0a058083), &invalid));
    assert(!aarch64_decode(UINT32_C(0xf85f8820), &invalid));
    assert(!aarch64_decode(UINT32_C(0xf84084a5), &invalid));
    assert(!aarch64_decode(UINT32_C(0xb81fcce7), &invalid));
    assert(!aarch64_decode(UINT32_C(0x38dfece7), &invalid));
    assert(!aarch64_decode(UINT32_C(0x389ff820), &invalid));
    assert(!aarch64_decode(UINT32_C(0xb9c00020), &invalid));
    assert(!aarch64_decode(UINT32_C(0xf9800400), &invalid));
    assert(!aarch64_decode(UINT32_C(0xf89f8000), &invalid));
    assert(!aarch64_decode(UINT32_C(0x12400020), &invalid));
    assert(!aarch64_decode(UINT32_C(0x9200f820), &invalid));
    assert(!aarch64_decode(UINT32_C(0x9200fc20), &invalid));
    assert(!aarch64_decode(UINT32_C(0x12007c20), &invalid));
    assert(!aarch64_decode(UINT32_C(0x9240fc20), &invalid));
    assert(!aarch64_decode(UINT32_C(0x73000020), &invalid));
    assert(!aarch64_decode(UINT32_C(0x13400020), &invalid));
    assert(!aarch64_decode(UINT32_C(0x93000020), &invalid));
    assert(!aarch64_decode(UINT32_C(0x13200020), &invalid));
    assert(!aarch64_decode(UINT32_C(0x13008020), &invalid));
    assert(!aarch64_decode(UINT32_C(0xba9702d5), &invalid));
    assert(!aarch64_decode(UINT32_C(0x9a970ad5), &invalid));
    assert(!aarch64_decode(UINT32_C(0xa8410440), &invalid));
    assert(!aarch64_decode(UINT32_C(0x69000440), &invalid));
    assert(!aarch64_decode(UINT32_C(0x69410440), &invalid));
    assert(!aarch64_decode(UINT32_C(0xe9000440), &invalid));
    assert(!aarch64_decode(UINT32_C(0xe9400440), &invalid));
    assert(!aarch64_decode(UINT32_C(0xed410440), &invalid));
    assert(!aarch64_decode(UINT32_C(0xa8c10400), &invalid));
    assert(!aarch64_decode(UINT32_C(0xa9400040), &invalid));
    assert(!aarch64_decode(UINT32_C(0xa8810400), &invalid));
    return 0;
}
