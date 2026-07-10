#include <assert.h>
#include <string.h>

#include "guest/aarch64/decode.h"
#include "guest/aarch64/execute.h"

struct decode_case {
    dword_t word;
    enum aarch64_opcode opcode;
    byte_t width;
    byte_t rn;
    byte_t operand;
    byte_t condition;
    byte_t nzcv;
    bool immediate;
};

static dword_t encode_conditional_compare(bool is_64, bool compare,
        bool immediate, byte_t operand, byte_t condition,
        byte_t rn, byte_t nzcv) {
    return UINT32_C(0x3a400000) |
            (is_64 ? UINT32_C(1) << 31 : 0) |
            (compare ? UINT32_C(1) << 30 : 0) |
            (dword_t) operand << 16 |
            (dword_t) condition << 12 |
            (immediate ? UINT32_C(1) << 11 : 0) |
            (dword_t) rn << 5 | nzcv;
}

static void assert_decodes(const struct decode_case *expected) {
    struct aarch64_decoded instruction;
    assert(aarch64_decode(expected->word, &instruction));
    assert(instruction.opcode == expected->opcode);
    assert(instruction.width == expected->width);
    assert(instruction.operands.conditional_compare.rn == expected->rn);
    assert(instruction.operands.conditional_compare.operand ==
            expected->operand);
    assert(instruction.operands.conditional_compare.condition ==
            expected->condition);
    assert(instruction.operands.conditional_compare.nzcv == expected->nzcv);
    assert(instruction.operands.conditional_compare.immediate ==
            expected->immediate);
}

static void execute_word(struct cpu_state *cpu, dword_t word) {
    struct aarch64_decoded instruction;
    assert(aarch64_decode(word, &instruction));
    struct aarch64_execute_result result =
            aarch64_execute(cpu, NULL, &instruction);
    assert(result.stop == AARCH64_EXECUTE_RETIRED);
    assert(result.fault.kind == GUEST_MEMORY_FAULT_NONE);
}

static void test_decode(void) {
    static const struct decode_case cases[] = {
        {UINT32_C(0x7a4a8820), AARCH64_OP_CCMP, 32, 1, 10, 8, 0, true},
        {UINT32_C(0xfa5fd86f), AARCH64_OP_CCMP, 64, 3, 31, 13, 15, true},
        {UINT32_C(0x3a4008a9), AARCH64_OP_CCMN, 32, 5, 0, 0, 9, true},
        {UINT32_C(0xba51e8e6), AARCH64_OP_CCMN, 64, 7, 17, 14, 6, true},
        {UINT32_C(0x7a428020), AARCH64_OP_CCMP, 32, 1, 2, 8, 0, false},
        {UINT32_C(0xfa44d06f), AARCH64_OP_CCMP, 64, 3, 4, 13, 15, false},
        {UINT32_C(0x3a4600a9), AARCH64_OP_CCMN, 32, 5, 6, 0, 9, false},
        {UINT32_C(0xba48e0e6), AARCH64_OP_CCMN, 64, 7, 8, 14, 6, false},
    };
    for (size_t i = 0; i < array_size(cases); i++)
        assert_decodes(&cases[i]);

    for (byte_t is_64 = 0; is_64 < 2; is_64++) {
        for (byte_t compare = 0; compare < 2; compare++) {
            for (byte_t immediate = 0; immediate < 2; immediate++) {
                for (byte_t condition = 0; condition < 16; condition++) {
                    struct decode_case expected = {
                        .word = encode_conditional_compare(is_64, compare,
                                immediate, 7, condition, 9, 5),
                        .opcode = compare ? AARCH64_OP_CCMP : AARCH64_OP_CCMN,
                        .width = is_64 ? 64 : 32,
                        .rn = 9,
                        .operand = 7,
                        .condition = condition,
                        .nzcv = 5,
                        .immediate = immediate,
                    };
                    assert_decodes(&expected);
                }
            }
        }
    }

    static const dword_t reserved[] = {
        UINT32_C(0x7a4a8c20),
        UINT32_C(0x7a4a8830),
        UINT32_C(0x7a428420),
        UINT32_C(0x7a428030),
    };
    struct aarch64_decoded instruction;
    for (size_t i = 0; i < array_size(reserved); i++)
        assert(!aarch64_decode(reserved[i], &instruction));
}

static void assert_flags(dword_t word, qword_t left, qword_t right,
        dword_t expected) {
    struct cpu_state cpu = {
        .pc = UINT64_C(0x1000),
        .nzcv = UINT32_C(1) << 30,
    };
    cpu.x[1] = left;
    cpu.x[2] = right;
    qword_t registers[31];
    memcpy(registers, cpu.x, sizeof(registers));
    execute_word(&cpu, word);
    assert(cpu.pc == UINT64_C(0x1004));
    assert(cpu.nzcv == expected);
    assert(memcmp(registers, cpu.x, sizeof(registers)) == 0);
}

static void test_true_condition(void) {
    assert_flags(encode_conditional_compare(false, true, true,
            1, 14, 1, 0), UINT32_C(0x80000000), 0,
            UINT32_C(0x30000000));
    assert_flags(encode_conditional_compare(true, true, false,
            2, 14, 1, 0), 0, 1, UINT32_C(0x80000000));
    assert_flags(encode_conditional_compare(false, false, false,
            2, 14, 1, 0), UINT32_MAX, 1, UINT32_C(0x60000000));
    assert_flags(encode_conditional_compare(true, false, true,
            1, 14, 1, 0), INT64_MAX, 0, UINT32_C(0x90000000));
}

static void test_fallback_flags(void) {
    for (byte_t is_64 = 0; is_64 < 2; is_64++) {
        for (byte_t compare = 0; compare < 2; compare++) {
            for (byte_t immediate = 0; immediate < 2; immediate++) {
                for (byte_t flags = 0; flags < 16; flags++) {
                    struct cpu_state cpu = {
                        .pc = UINT64_C(0x2000),
                        .sp = UINT64_C(0xabcdef0123456789),
                    };
                    for (byte_t reg = 0; reg < 31; reg++)
                        cpu.x[reg] = UINT64_C(0x100000000) + reg;
                    qword_t registers[31];
                    memcpy(registers, cpu.x, sizeof(registers));
                    execute_word(&cpu, encode_conditional_compare(
                            is_64, compare, immediate, 2, 0, 1, flags));
                    assert(cpu.pc == UINT64_C(0x2004));
                    assert(cpu.nzcv == (dword_t) flags << 28);
                    assert(cpu.sp == UINT64_C(0xabcdef0123456789));
                    assert(memcmp(registers, cpu.x, sizeof(registers)) == 0);
                }
            }
        }
    }
}

static void test_zero_register_and_unconditional(void) {
    for (byte_t compare = 0; compare < 2; compare++) {
        struct cpu_state cpu = {
            .pc = UINT64_C(0x3000),
            .sp = UINT64_C(0x123456789abcdef0),
        };
        execute_word(&cpu, encode_conditional_compare(
                true, compare, false, 31, 14, 31, 0));
        assert(cpu.nzcv == (compare ?
                UINT32_C(0x60000000) : UINT32_C(0x40000000)));
        assert(cpu.sp == UINT64_C(0x123456789abcdef0));
    }

    for (byte_t condition = 14; condition < 16; condition++) {
        struct cpu_state cpu = {
            .pc = UINT64_C(0x4000),
            .nzcv = UINT32_C(0xf0000000),
        };
        execute_word(&cpu, encode_conditional_compare(
                true, false, true, 1, condition, 31, 15));
        assert(cpu.nzcv == 0);
    }
}

int main(void) {
    test_decode();
    test_true_condition();
    test_fallback_flags();
    test_zero_register_and_unconditional();
    return 0;
}
