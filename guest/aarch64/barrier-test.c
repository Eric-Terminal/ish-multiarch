#include <assert.h>
#include <string.h>

#include "guest/aarch64/decode.h"
#include "guest/aarch64/execute.h"

struct barrier_case {
    dword_t base;
    enum aarch64_opcode opcode;
};

static const struct barrier_case barrier_cases[] = {
    {UINT32_C(0xd503305f), AARCH64_OP_CLREX},
    {UINT32_C(0xd503309f), AARCH64_OP_DSB},
    {UINT32_C(0xd50330bf), AARCH64_OP_DMB},
    {UINT32_C(0xd50330df), AARCH64_OP_ISB},
};

static struct aarch64_decoded decode(dword_t word) {
    struct aarch64_decoded instruction;
    assert(aarch64_decode(word, &instruction));
    return instruction;
}

static void test_decode(void) {
    unsigned decoded = 0;
    for (size_t i = 0; i < array_size(barrier_cases); i++) {
        for (unsigned crm = 0; crm < 16; crm++) {
            struct aarch64_decoded instruction = decode(
                    barrier_cases[i].base | (dword_t) crm << 8);
            assert(instruction.opcode == barrier_cases[i].opcode);
            assert(instruction.width == 64);
            decoded++;
        }
    }
    assert(decoded == 64);

    assert(decode(UINT32_C(0xd5033f5f)).opcode == AARCH64_OP_CLREX);
    assert(decode(UINT32_C(0xd503305f)).opcode == AARCH64_OP_CLREX);
    assert(decode(UINT32_C(0xd503375f)).opcode == AARCH64_OP_CLREX);
    assert(decode(UINT32_C(0xd5033fbf)).opcode == AARCH64_OP_DMB);
    assert(decode(UINT32_C(0xd5033bbf)).opcode == AARCH64_OP_DMB);
    assert(decode(UINT32_C(0xd5033f9f)).opcode == AARCH64_OP_DSB);
    assert(decode(UINT32_C(0xd5033fdf)).opcode == AARCH64_OP_ISB);
}

static void assert_architectural_state(const struct cpu_state *cpu,
        const struct cpu_state *before) {
    assert(memcmp(cpu->x, before->x, sizeof(cpu->x)) == 0);
    assert(cpu->sp == before->sp);
    assert(cpu->nzcv == before->nzcv);
    assert(memcmp(cpu->v, before->v, sizeof(cpu->v)) == 0);
    assert(cpu->fpcr == before->fpcr);
    assert(cpu->fpsr == before->fpsr);
    assert(cpu->tpidr_el0 == before->tpidr_el0);
}

static void test_execution(void) {
    for (size_t i = 1; i < array_size(barrier_cases); i++) {
        for (unsigned crm = 0; crm < 16; crm++) {
            struct cpu_state cpu = {
                .pc = UINT64_C(0x1000),
                .sp = UINT64_C(0x1122334455667788),
                .nzcv = UINT32_C(0xa0000000),
                .fpcr = UINT32_C(0x01000000),
                .fpsr = UINT32_C(0x08000000),
                .tpidr_el0 = UINT64_C(0x8877665544332211),
            };
            cpu.x[0] = UINT64_C(0x0123456789abcdef);
            cpu.v[0].d[0] = UINT64_C(0x13579bdf2468ace0);
            cpu.v[0].d[1] = UINT64_C(0x02468ace13579bdf);
            aarch64_set_exclusive(&cpu, UINT64_C(0x2000), 8,
                    UINT64_C(0xaabbccddeeff0011), 0, NULL, 7, 11);
            struct cpu_state before = cpu;
            struct aarch64_decoded instruction = decode(
                    barrier_cases[i].base | (dword_t) crm << 8);
            struct aarch64_execute_result result =
                    aarch64_execute(&cpu, NULL, &instruction);
            assert(result.stop == AARCH64_EXECUTE_RETIRED);
            assert(cpu.pc == UINT64_C(0x1004));
            assert_architectural_state(&cpu, &before);
            assert(memcmp(&cpu.exclusive, &before.exclusive,
                    sizeof(cpu.exclusive)) == 0);
        }
    }
}

static void test_monitor_clearing(void) {
    struct cpu_state cpu = {.pc = UINT64_C(0x1800)};
    aarch64_set_exclusive(&cpu, UINT64_C(0x3000), 4,
            UINT32_C(0x12345678), 0, NULL, 9, 13);
    struct aarch64_decoded clrex = decode(UINT32_C(0xd5033f5f));
    struct aarch64_execute_result result =
            aarch64_execute(&cpu, NULL, &clrex);
    assert(result.stop == AARCH64_EXECUTE_RETIRED);
    assert(!cpu.exclusive.valid);
    assert(cpu.pc == UINT64_C(0x1804));

    aarch64_set_exclusive(&cpu, UINT64_C(0x4000), 8,
            UINT64_C(0xabcdef0123456789), 0, NULL, 10, 17);
    struct aarch64_decoded svc = decode(UINT32_C(0xd4000001));
    result = aarch64_execute(&cpu, NULL, &svc);
    assert(result.stop == AARCH64_EXECUTE_SYSCALL);
    assert(!cpu.exclusive.valid);
    assert(cpu.pc == UINT64_C(0x1808));
}

int main(void) {
    test_decode();
    test_execution();
    test_monitor_clearing();
    return 0;
}
