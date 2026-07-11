#include <assert.h>
#include <string.h>

#include "guest/aarch64/decode.h"
#include "guest/aarch64/execute.h"

#define ADVSIMD_TABLE_FIXED_MASK UINT32_C(0xbfe08c00)
#define ADVSIMD_TABLE_FIXED_BITS UINT32_C(0x0e000000)
#define ADVSIMD_TABLE_VARIABLE_MASK UINT32_C(0x401f73ff)

static dword_t encode(bool q, bool extend, byte_t table_count,
        byte_t rm, byte_t rn, byte_t rd) {
    return ADVSIMD_TABLE_FIXED_BITS |
            (dword_t) q << 30 |
            (dword_t) rm << 16 |
            (dword_t) (table_count - 1) << 13 |
            (dword_t) extend << 12 |
            (dword_t) rn << 5 |
            rd;
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

static void assert_decode(dword_t word, enum aarch64_opcode opcode,
        byte_t width, byte_t table_count, byte_t rd, byte_t rn, byte_t rm) {
    struct aarch64_decoded instruction = decode(word);
    assert(instruction.opcode == opcode);
    assert(instruction.width == width);
    assert(instruction.operands.advsimd_table.rd == rd);
    assert(instruction.operands.advsimd_table.rn == rn);
    assert(instruction.operands.advsimd_table.rm == rm);
    assert(instruction.operands.advsimd_table.table_count == table_count);
}

static void test_llvm_vectors(void) {
    assert_decode(UINT32_C(0x0e020020), AARCH64_OP_ADVSIMD_TBL,
            64, 1, 0, 1, 2);
    assert_decode(UINT32_C(0x4e050083), AARCH64_OP_ADVSIMD_TBL,
            128, 1, 3, 4, 5);
    assert_decode(UINT32_C(0x0e0920e6), AARCH64_OP_ADVSIMD_TBL,
            64, 2, 6, 7, 9);
    assert_decode(UINT32_C(0x4e0d216a), AARCH64_OP_ADVSIMD_TBL,
            128, 2, 10, 11, 13);
    assert_decode(UINT32_C(0x0e1241ee), AARCH64_OP_ADVSIMD_TBL,
            64, 3, 14, 15, 18);
    assert_decode(UINT32_C(0x4e174293), AARCH64_OP_ADVSIMD_TBL,
            128, 3, 19, 20, 23);
    assert_decode(UINT32_C(0x0e1d6338), AARCH64_OP_ADVSIMD_TBL,
            64, 4, 24, 25, 29);
    assert_decode(UINT32_C(0x4e1b639e), AARCH64_OP_ADVSIMD_TBL,
            128, 4, 30, 28, 27);
    assert_decode(UINT32_C(0x0e021020), AARCH64_OP_ADVSIMD_TBX,
            64, 1, 0, 1, 2);
    assert_decode(UINT32_C(0x4e087083), AARCH64_OP_ADVSIMD_TBX,
            128, 4, 3, 4, 8);
    assert_decode(UINT32_C(0x4e1703ff), AARCH64_OP_ADVSIMD_TBL,
            128, 1, 31, 31, 23);
}

static void test_encoding_space(void) {
    assert((ADVSIMD_TABLE_FIXED_MASK & ADVSIMD_TABLE_VARIABLE_MASK) == 0);
    assert((ADVSIMD_TABLE_FIXED_MASK | ADVSIMD_TABLE_VARIABLE_MASK) ==
            UINT32_MAX);
    unsigned decoded_count = 0;
    for (unsigned q = 0; q < 2; q++) {
        for (unsigned extend = 0; extend < 2; extend++) {
            for (unsigned table_count = 1; table_count <= 4; table_count++) {
                for (unsigned rm = 0; rm < 32; rm++) {
                    for (unsigned rn = 0; rn < 32; rn++) {
                        for (unsigned rd = 0; rd < 32; rd++) {
                            struct aarch64_decoded instruction;
                            bool decoded = aarch64_decode(encode(q, extend,
                                    (byte_t) table_count, (byte_t) rm,
                                    (byte_t) rn, (byte_t) rd), &instruction);
                            assert(decoded);
                            if (!decoded)
                                continue;
                            decoded_count++;
                            assert(instruction.opcode == (extend ?
                                    AARCH64_OP_ADVSIMD_TBX :
                                    AARCH64_OP_ADVSIMD_TBL));
                            assert(instruction.width == (q ? 128 : 64));
                            assert(instruction.operands.advsimd_table.rd ==
                                    rd);
                            assert(instruction.operands.advsimd_table.rn ==
                                    rn);
                            assert(instruction.operands.advsimd_table.rm ==
                                    rm);
                            assert(instruction.operands.advsimd_table.
                                    table_count == table_count);
                        }
                    }
                }
            }
        }
    }
    assert(decoded_count == 524288);
}

static void test_fixed_bits(void) {
    dword_t base = encode(true, false, 4, 2, 1, 0);
    for (unsigned bit = 0; bit < 32; bit++) {
        if ((ADVSIMD_TABLE_FIXED_MASK & (UINT32_C(1) << bit)) == 0)
            continue;
        struct aarch64_decoded instruction;
        bool decoded = aarch64_decode(base ^ (UINT32_C(1) << bit),
                &instruction);
        assert(!decoded || (instruction.opcode != AARCH64_OP_ADVSIMD_TBL &&
                instruction.opcode != AARCH64_OP_ADVSIMD_TBX));
    }
}

static union aarch64_vector_reg reference_lookup(
        const struct cpu_state *before, bool q, bool extend,
        byte_t table_count, byte_t rm, byte_t rn, byte_t rd) {
    union aarch64_vector_reg result = {0};
    unsigned lanes = q ? 16 : 8;
    unsigned table_size = table_count * 16;
    for (unsigned lane = 0; lane < lanes; lane++) {
        byte_t index = before->v[rm].b[lane];
        if (index < table_size) {
            byte_t source = (byte_t) ((rn + index / 16) & 0x1f);
            result.b[lane] = before->v[source].b[index % 16];
        } else if (extend) {
            result.b[lane] = before->v[rd].b[lane];
        }
    }
    return result;
}

static void fill_registers(struct cpu_state *cpu) {
    for (unsigned reg = 0; reg < 32; reg++) {
        for (unsigned byte = 0; byte < 16; byte++)
            cpu->v[reg].b[byte] = (byte_t) (reg * 17 + byte * 13);
    }
}

static void set_indices(union aarch64_vector_reg *reg, byte_t table_size) {
    static const byte_t boundary_offsets[] = {0, 1, 15, 16};
    for (unsigned lane = 0; lane < 16; lane++) {
        if (lane < sizeof(boundary_offsets))
            reg->b[lane] = boundary_offsets[lane];
        else if (lane == 4)
            reg->b[lane] = (byte_t) (table_size - 1);
        else if (lane == 5)
            reg->b[lane] = table_size;
        else if (lane == 6)
            reg->b[lane] = UINT8_MAX;
        else
            reg->b[lane] = (byte_t) (lane * 7);
    }
}

static void test_execution_space(void) {
    static const struct {
        byte_t rd;
        byte_t rn;
        byte_t rm;
    } registers[] = {
        {0, 1, 10},
        {31, 31, 23},
        {2, 30, 2},
        {4, 29, 31},
        {31, 31, 31},
    };
    for (unsigned q = 0; q < 2; q++) {
        for (unsigned extend = 0; extend < 2; extend++) {
            for (unsigned table_count = 1; table_count <= 4; table_count++) {
                for (unsigned form = 0; form <
                        sizeof(registers) / sizeof(registers[0]); form++) {
                    const byte_t rd = registers[form].rd;
                    const byte_t rn = registers[form].rn;
                    const byte_t rm = registers[form].rm;
                    struct cpu_state cpu = {
                        .pc = UINT64_C(0x1000),
                        .sp = UINT64_C(0x1122334455667788),
                        .nzcv = UINT32_C(0xa0000000),
                        .fpcr = UINT32_C(0x01000000),
                        .fpsr = UINT32_C(0x08000000),
                    };
                    fill_registers(&cpu);
                    set_indices(&cpu.v[rm], (byte_t) (table_count * 16));
                    struct cpu_state before = cpu;
                    union aarch64_vector_reg expected = reference_lookup(
                            &before, q, extend, (byte_t) table_count,
                            rm, rn, rd);

                    dword_t word = encode(q, extend,
                            (byte_t) table_count, rm, rn, rd);
                    if (q && !extend && table_count == 1 && form == 1)
                        assert(word == UINT32_C(0x4e1703ff));
                    execute_instruction(&cpu, word);
                    assert(memcmp(&cpu.v[rd], &expected,
                            sizeof(expected)) == 0);
                    for (unsigned reg = 0; reg < 32; reg++) {
                        if (reg != rd)
                            assert(memcmp(&cpu.v[reg], &before.v[reg],
                                    sizeof(cpu.v[reg])) == 0);
                    }
                    assert(cpu.pc == UINT64_C(0x1004));
                    assert(cpu.sp == UINT64_C(0x1122334455667788));
                    assert(cpu.nzcv == UINT32_C(0xa0000000));
                    assert(cpu.fpcr == UINT32_C(0x01000000));
                    assert(cpu.fpsr == UINT32_C(0x08000000));
                }
            }
        }
    }
}

int main(void) {
    test_llvm_vectors();
    test_encoding_space();
    test_fixed_bits();
    test_execution_space();
    return 0;
}
