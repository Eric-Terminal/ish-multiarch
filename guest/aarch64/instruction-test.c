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

static void assert_load_store(dword_t word, enum aarch64_opcode opcode,
        byte_t size, byte_t rn, byte_t rt, qword_t offset) {
    struct aarch64_decoded instruction = decode(word);
    assert(instruction.opcode == opcode);
    assert(instruction.operands.load_store.size == size);
    assert(instruction.operands.load_store.rn == rn);
    assert(instruction.operands.load_store.rt == rt);
    assert(instruction.operands.load_store.offset == offset);
}

static void test_load_store_decode(void) {
    assert_load_store(UINT32_C(0xf9400020),
            AARCH64_OP_LOAD_UNSIGNED_IMMEDIATE, 8, 1, 0, 0);
    assert_load_store(UINT32_C(0xf9000020),
            AARCH64_OP_STORE_UNSIGNED_IMMEDIATE, 8, 1, 0, 0);
    assert_load_store(UINT32_C(0xb9400c62),
            AARCH64_OP_LOAD_UNSIGNED_IMMEDIATE, 4, 3, 2, 12);
    assert_load_store(UINT32_C(0xb9000c62),
            AARCH64_OP_STORE_UNSIGNED_IMMEDIATE, 4, 3, 2, 12);
    assert_load_store(UINT32_C(0x39401ca4),
            AARCH64_OP_LOAD_UNSIGNED_IMMEDIATE, 1, 5, 4, 7);
    assert_load_store(UINT32_C(0x39001ca4),
            AARCH64_OP_STORE_UNSIGNED_IMMEDIATE, 1, 5, 4, 7);
    assert_load_store(UINT32_C(0x79400ce6),
            AARCH64_OP_LOAD_UNSIGNED_IMMEDIATE, 2, 7, 6, 6);
    assert_load_store(UINT32_C(0x79000ce6),
            AARCH64_OP_STORE_UNSIGNED_IMMEDIATE, 2, 7, 6, 6);
    assert_load_store(UINT32_C(0xf9400be9),
            AARCH64_OP_LOAD_UNSIGNED_IMMEDIATE, 8, 31, 9, 16);
}

int main(void) {
    struct cpu_state cpu = {.pc = 0x1000};
    struct aarch64_decoded nop = decode(UINT32_C(0xd503201f));
    execute_instruction(&cpu, &nop);
    assert(cpu.pc == 0x1004);

    test_add_sub();
    test_move_wide();
    test_branches();
    test_load_store_decode();

    struct aarch64_decoded invalid;
    assert(!aarch64_decode(UINT32_C(0x32800000), &invalid));
    assert(!aarch64_decode(UINT32_C(0xd61f03e0), &invalid));
    assert(!aarch64_decode(UINT32_C(0x3d400000), &invalid));
    assert(!aarch64_decode(UINT32_C(0x39800000), &invalid));
    return 0;
}
