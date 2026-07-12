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

static void test_decode(void) {
    for (unsigned rt = 0; rt < 32; rt++) {
        struct aarch64_decoded instruction =
                decode(UINT32_C(0xd53b00e0) | (dword_t) rt);
        assert(instruction.opcode == AARCH64_OP_MRS_DCZID_EL0);
        assert(instruction.width == 64);
        assert(instruction.operands.system_register.rt == rt);

        assert(!aarch64_decode(UINT32_C(0xd51b00e0) | (dword_t) rt,
                &instruction));

        instruction =
                decode(UINT32_C(0xd53bd040) | (dword_t) rt);
        assert(instruction.opcode == AARCH64_OP_MRS_TPIDR_EL0);
        assert(instruction.width == 64);
        assert(instruction.operands.system_register.rt == rt);

        instruction = decode(UINT32_C(0xd51bd040) | (dword_t) rt);
        assert(instruction.opcode == AARCH64_OP_MSR_TPIDR_EL0);
        assert(instruction.width == 64);
        assert(instruction.operands.system_register.rt == rt);

        instruction = decode(UINT32_C(0xd53b4400) | (dword_t) rt);
        assert(instruction.opcode == AARCH64_OP_MRS_FPCR);
        assert(instruction.width == 64);
        assert(instruction.operands.system_register.rt == rt);

        instruction = decode(UINT32_C(0xd51b4400) | (dword_t) rt);
        assert(instruction.opcode == AARCH64_OP_MSR_FPCR);
        assert(instruction.width == 64);
        assert(instruction.operands.system_register.rt == rt);

        instruction = decode(UINT32_C(0xd53b4420) | (dword_t) rt);
        assert(instruction.opcode == AARCH64_OP_MRS_FPSR);
        assert(instruction.width == 64);
        assert(instruction.operands.system_register.rt == rt);

        instruction = decode(UINT32_C(0xd51b4420) | (dword_t) rt);
        assert(instruction.opcode == AARCH64_OP_MSR_FPSR);
        assert(instruction.width == 64);
        assert(instruction.operands.system_register.rt == rt);
    }

    static const dword_t unsupported[] = {
        UINT32_C(0xd53b00c0), UINT32_C(0xd53b01e0),
        UINT32_C(0xd53b10e0), UINT32_C(0xd53a00e0),
        UINT32_C(0xd53300e0),
        UINT32_C(0xd53bd060), UINT32_C(0xd51bd060),
        UINT32_C(0xd53bd0a0), UINT32_C(0xd51bd0a0),
        UINT32_C(0xd53bd140), UINT32_C(0xd53bc040),
        UINT32_C(0xd53ad040), UINT32_C(0xd533d040),
        UINT32_C(0xd53b43e0), UINT32_C(0xd51b43e0),
        UINT32_C(0xd53b4440), UINT32_C(0xd51b4440),
    };
    for (size_t i = 0; i < array_size(unsupported); i++) {
        struct aarch64_decoded instruction;
        assert(!aarch64_decode(unsupported[i], &instruction));
    }
}

static void test_execute(void) {
    struct cpu_state cpu = {
        .pc = UINT64_C(0x1000),
        .sp = UINT64_C(0xfedcba9876543210),
        .nzcv = UINT32_C(0xb0000000),
        .fpcr = UINT32_MAX,
        .fpsr = UINT32_MAX,
        .tpidr_el0 = UINT64_C(0x123456789abcdef0),
    };
    cpu.x[2] = UINT64_MAX;
    execute_instruction(&cpu, UINT32_C(0xd53b00e2));
    assert(cpu.x[2] == UINT64_C(0x10));
    assert(cpu.tpidr_el0 == UINT64_C(0x123456789abcdef0));

    cpu.x[30] = UINT64_C(0x7766554433221100);
    execute_instruction(&cpu, UINT32_C(0xd53b00ff));
    assert(cpu.x[30] == UINT64_C(0x7766554433221100));

    execute_instruction(&cpu, UINT32_C(0xd53b4403));
    assert(cpu.x[3] == AARCH64_FPCR_WRITE_MASK);

    execute_instruction(&cpu, UINT32_C(0xd53b4424));
    assert(cpu.x[4] == AARCH64_FPSR_WRITE_MASK);

    cpu.x[30] = UINT64_C(0x1020304050607080);
    execute_instruction(&cpu, UINT32_C(0xd53b441f));
    execute_instruction(&cpu, UINT32_C(0xd53b443f));
    assert(cpu.x[30] == UINT64_C(0x1020304050607080));
    assert(cpu.fpcr == UINT32_MAX);
    assert(cpu.fpsr == UINT32_MAX);

    cpu.x[5] = UINT64_MAX;
    execute_instruction(&cpu, UINT32_C(0xd51b4405));
    assert(cpu.fpcr == AARCH64_FPCR_WRITE_MASK);
    assert(cpu.fpsr == UINT32_MAX);
    assert(cpu.x[5] == UINT64_MAX);

    cpu.x[6] = UINT64_MAX;
    execute_instruction(&cpu, UINT32_C(0xd51b4426));
    assert(cpu.fpcr == AARCH64_FPCR_WRITE_MASK);
    assert(cpu.fpsr == AARCH64_FPSR_WRITE_MASK);
    assert(cpu.nzcv == UINT32_C(0xb0000000));
    assert(cpu.x[6] == UINT64_MAX);

    execute_instruction(&cpu, UINT32_C(0xd51b441f));
    execute_instruction(&cpu, UINT32_C(0xd51b443f));
    assert(cpu.fpcr == 0);
    assert(cpu.fpsr == 0);

    execute_instruction(&cpu, UINT32_C(0xd53bd040));
    assert(cpu.x[0] == UINT64_C(0x123456789abcdef0));

    cpu.x[30] = UINT64_MAX;
    execute_instruction(&cpu, UINT32_C(0xd53bd05e));
    assert(cpu.x[30] == UINT64_C(0x123456789abcdef0));

    cpu.x[0] = UINT64_C(0x0f0e0d0c0b0a0908);
    execute_instruction(&cpu, UINT32_C(0xd53bd05f));
    assert(cpu.x[0] == UINT64_C(0x0f0e0d0c0b0a0908));

    cpu.x[1] = UINT64_C(0x8877665544332211);
    execute_instruction(&cpu, UINT32_C(0xd51bd041));
    assert(cpu.tpidr_el0 == UINT64_C(0x8877665544332211));
    assert(cpu.x[1] == UINT64_C(0x8877665544332211));

    execute_instruction(&cpu, UINT32_C(0xd51bd05f));
    assert(cpu.tpidr_el0 == 0);
    assert(cpu.pc == UINT64_C(0x103c));
    assert(cpu.sp == UINT64_C(0xfedcba9876543210));
    assert(cpu.nzcv == UINT32_C(0xb0000000));
}

int main(void) {
    test_decode();
    test_execute();
    return 0;
}
