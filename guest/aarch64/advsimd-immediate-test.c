#include <assert.h>
#include <stdint.h>

#include "guest/aarch64/decode.h"
#include "guest/aarch64/execute.h"

static dword_t encode(bool q, bool op, byte_t cmode, bool o2,
        byte_t imm8, byte_t rd) {
    return UINT32_C(0x0f000400) |
            (dword_t) q << 30 |
            (dword_t) op << 29 |
            (dword_t) (imm8 & 0xe0) << 11 |
            (dword_t) cmode << 12 |
            (dword_t) o2 << 11 |
            (dword_t) (imm8 & 0x1f) << 5 |
            rd;
}

static dword_t encode_scalar_shl(byte_t shift, byte_t rn, byte_t rd) {
    return UINT32_C(0x5f405400) | (dword_t) shift << 16 |
            (dword_t) rn << 5 | rd;
}

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

static enum aarch64_opcode reference_opcode(bool op, byte_t cmode) {
    if (cmode == 14)
        return AARCH64_OP_ADVSIMD_MOVI;
    if ((cmode & 1) == 0 || cmode == 13)
        return op ? AARCH64_OP_ADVSIMD_MVNI : AARCH64_OP_ADVSIMD_MOVI;
    return op ? AARCH64_OP_ADVSIMD_BIC_IMMEDIATE :
            AARCH64_OP_ADVSIMD_ORR_IMMEDIATE;
}

static qword_t bytes_to_qword(const byte_t bytes[8]) {
    qword_t value = 0;
    for (unsigned byte = 0; byte < 8; byte++)
        value |= (qword_t) bytes[byte] << (byte * 8);
    return value;
}

static qword_t reference_immediate(bool op, byte_t cmode, byte_t imm8) {
    byte_t bytes[8] = {0};
    if (cmode < 8) {
        unsigned immediate_byte = cmode >> 1;
        bytes[immediate_byte] = imm8;
        bytes[4 + immediate_byte] = imm8;
    } else if (cmode < 12) {
        unsigned immediate_byte = (cmode >> 1) & 1;
        for (unsigned element = 0; element < 8; element += 2)
            bytes[element + immediate_byte] = imm8;
    } else if (cmode == 12) {
        for (unsigned element = 0; element < 8; element += 4) {
            bytes[element] = UINT8_MAX;
            bytes[element + 1] = imm8;
        }
    } else if (cmode == 13) {
        for (unsigned element = 0; element < 8; element += 4) {
            bytes[element] = UINT8_MAX;
            bytes[element + 1] = UINT8_MAX;
            bytes[element + 2] = imm8;
        }
    } else if (!op) {
        for (unsigned byte = 0; byte < 8; byte++)
            bytes[byte] = imm8;
    } else {
        for (unsigned byte = 0; byte < 8; byte++)
            bytes[byte] = imm8 & (UINT32_C(1) << byte) ? UINT8_MAX : 0;
    }
    return bytes_to_qword(bytes);
}

static void assert_decode(dword_t word, enum aarch64_opcode opcode,
        byte_t width, byte_t rd, qword_t immediate) {
    struct aarch64_decoded instruction = decode(word);
    assert(instruction.opcode == opcode);
    assert(instruction.width == width);
    assert(instruction.operands.advsimd_immediate.rd == rd);
    assert(instruction.operands.advsimd_immediate.immediate == immediate);
}

static void test_apple_vectors(void) {
    assert_decode(UINT32_C(0x4f00e400), AARCH64_OP_ADVSIMD_MOVI,
            128, 0, 0);
    assert_decode(UINT32_C(0x6f00e401), AARCH64_OP_ADVSIMD_MOVI,
            128, 1, 0);
    assert_decode(UINT32_C(0x6f05e542), AARCH64_OP_ADVSIMD_MOVI,
            128, 2, UINT64_C(0xff00ff00ff00ff00));
    assert_decode(UINT32_C(0x2f00e403), AARCH64_OP_ADVSIMD_MOVI,
            64, 3, 0);
    assert_decode(UINT32_C(0x0f05e4a4), AARCH64_OP_ADVSIMD_MOVI,
            64, 4, UINT64_C(0xa5a5a5a5a5a5a5a5));
    assert_decode(UINT32_C(0x0f00a645), AARCH64_OP_ADVSIMD_MOVI,
            64, 5, UINT64_C(0x1200120012001200));
    assert_decode(UINT32_C(0x4f016686), AARCH64_OP_ADVSIMD_MOVI,
            128, 6, UINT64_C(0x3400000034000000));
    assert_decode(UINT32_C(0x4f02c6c7), AARCH64_OP_ADVSIMD_MOVI,
            128, 7, UINT64_C(0x000056ff000056ff));
    assert_decode(UINT32_C(0x0f03d708), AARCH64_OP_ADVSIMD_MOVI,
            64, 8, UINT64_C(0x0078ffff0078ffff));
    assert_decode(UINT32_C(0x6f04a749), AARCH64_OP_ADVSIMD_MVNI,
            128, 9, UINT64_C(0x9a009a009a009a00));
    assert_decode(UINT32_C(0x2f05478a), AARCH64_OP_ADVSIMD_MVNI,
            64, 10, UINT64_C(0x00bc000000bc0000));
    assert_decode(UINT32_C(0x6f06d7cb), AARCH64_OP_ADVSIMD_MVNI,
            128, 11, UINT64_C(0x00deffff00deffff));
    assert_decode(UINT32_C(0x4f00b64c), AARCH64_OP_ADVSIMD_ORR_IMMEDIATE,
            128, 12, UINT64_C(0x1200120012001200));
    assert_decode(UINT32_C(0x4f01768d), AARCH64_OP_ADVSIMD_ORR_IMMEDIATE,
            128, 13, UINT64_C(0x3400000034000000));
    assert_decode(UINT32_C(0x2f0296ce), AARCH64_OP_ADVSIMD_BIC_IMMEDIATE,
            64, 14, UINT64_C(0x0056005600560056));
    assert_decode(UINT32_C(0x2f03370f), AARCH64_OP_ADVSIMD_BIC_IMMEDIATE,
            64, 15, UINT64_C(0x0000780000007800));
}

static void test_encoding_space(void) {
    unsigned decoded_count = 0;
    for (unsigned q = 0; q < 2; q++) {
        for (unsigned op = 0; op < 2; op++) {
            for (unsigned cmode = 0; cmode < 16; cmode++) {
                byte_t encoded_cmode = (byte_t) cmode;
                for (unsigned o2 = 0; o2 < 2; o2++) {
                    for (unsigned imm8 = 0; imm8 < 256; imm8++) {
                        byte_t encoded_imm8 = (byte_t) imm8;
                        for (unsigned rd_index = 0; rd_index < 2;
                                rd_index++) {
                            byte_t rd = rd_index == 0 ? 0 : 31;
                            struct aarch64_decoded instruction;
                            bool decoded = aarch64_decode(encode(q, op,
                                    encoded_cmode, o2, encoded_imm8, rd),
                                    &instruction);
                            bool expected = o2 == 0 && cmode < 15;
                            assert(decoded == expected);
                            if (!decoded)
                                continue;
                            decoded_count++;
                            assert(instruction.opcode ==
                                    reference_opcode(op, encoded_cmode));
                            assert(instruction.width == (q ? 128 : 64));
                            assert(instruction.operands.advsimd_immediate.rd ==
                                    rd);
                            assert(instruction.operands.advsimd_immediate.
                                    immediate == reference_immediate(
                                            op, encoded_cmode,
                                            encoded_imm8));
                        }
                    }
                }
            }
        }
    }
    assert(decoded_count == 30720);
}

static qword_t apply(enum aarch64_opcode opcode,
        qword_t old, qword_t immediate) {
    if (opcode == AARCH64_OP_ADVSIMD_MOVI)
        return immediate;
    if (opcode == AARCH64_OP_ADVSIMD_MVNI)
        return ~immediate;
    if (opcode == AARCH64_OP_ADVSIMD_ORR_IMMEDIATE)
        return old | immediate;
    return old & ~immediate;
}

static void test_execution_space(void) {
    for (unsigned q = 0; q < 2; q++) {
        for (unsigned op = 0; op < 2; op++) {
            for (unsigned cmode = 0; cmode < 15; cmode++) {
                byte_t encoded_cmode = (byte_t) cmode;
                enum aarch64_opcode opcode = reference_opcode(
                        op, encoded_cmode);
                for (unsigned imm8 = 0; imm8 < 256; imm8++) {
                    byte_t encoded_imm8 = (byte_t) imm8;
                    for (unsigned rd_index = 0; rd_index < 2; rd_index++) {
                        byte_t rd = rd_index == 0 ? 0 : 31;
                        qword_t old_low = UINT64_C(0x0123456789abcdef) ^
                                imm8;
                        qword_t old_high = UINT64_C(0xfedcba9876543210) ^
                                ((qword_t) imm8 << 32);
                        struct cpu_state cpu = {
                            .pc = UINT64_C(0x1000),
                            .sp = UINT64_C(0x1122334455667788),
                            .nzcv = UINT32_C(0xa0000000),
                            .fpcr = UINT32_C(0x01000000),
                            .fpsr = UINT32_C(0x08000000),
                        };
                        cpu.x[0] = UINT64_C(0x8877665544332211);
                        cpu.v[rd].d[0] = old_low;
                        cpu.v[rd].d[1] = old_high;
                        cpu.v[1].d[0] = UINT64_C(0x13579bdf2468ace0);
                        cpu.v[1].d[1] = UINT64_C(0x02468ace13579bdf);

                        execute_instruction(&cpu,
                                encode(q, op, encoded_cmode, false,
                                        encoded_imm8, rd));
                        qword_t immediate = reference_immediate(
                                op, encoded_cmode, encoded_imm8);
                        assert(cpu.v[rd].d[0] ==
                                apply(opcode, old_low, immediate));
                        assert(cpu.v[rd].d[1] == (q ?
                                apply(opcode, old_high, immediate) : 0));
                        assert(cpu.v[1].d[0] ==
                                UINT64_C(0x13579bdf2468ace0));
                        assert(cpu.v[1].d[1] ==
                                UINT64_C(0x02468ace13579bdf));
                        assert(cpu.x[0] == UINT64_C(0x8877665544332211));
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
}

static void test_special_forms(void) {
    struct cpu_state cpu = {.pc = UINT64_C(0x1800)};
    execute_instruction(&cpu, UINT32_C(0x6f00e400));
    assert(cpu.v[0].d[0] == 0);
    assert(cpu.v[0].d[1] == 0);

    cpu.v[0].d[0] = UINT64_C(0xff00ff00ff00ff00);
    cpu.v[0].d[1] = UINT64_C(0xff00ff00ff00ff00);
    execute_instruction(&cpu, encode(false, false, 9, false, 0x12, 0));
    assert(cpu.v[0].d[0] == UINT64_C(0xff12ff12ff12ff12));
    assert(cpu.v[0].d[1] == 0);

    execute_instruction(&cpu, encode(true, true, 14, false, 0xa5, 31));
    assert(cpu.v[31].d[0] == UINT64_C(0xff00ff0000ff00ff));
    assert(cpu.v[31].d[1] == UINT64_C(0xff00ff0000ff00ff));
    assert(cpu.pc == UINT64_C(0x180c));
}

static void assert_scalar_shl_decode(
        dword_t word, byte_t shift, byte_t rn, byte_t rd) {
    struct aarch64_decoded instruction = decode(word);
    assert(instruction.opcode == AARCH64_OP_ADVSIMD_SHL_SCALAR);
    assert(instruction.width == 64);
    assert(instruction.operands.advsimd_shift_immediate.rd == rd);
    assert(instruction.operands.advsimd_shift_immediate.rn == rn);
    assert(instruction.operands.advsimd_shift_immediate.shift == shift);
}

static void test_scalar_shl_decode(void) {
    assert_scalar_shl_decode(UINT32_C(0x5f405420), 0, 1, 0);
    assert_scalar_shl_decode(UINT32_C(0x5f415462), 1, 3, 2);
    assert_scalar_shl_decode(UINT32_C(0x5f5f54a4), 31, 5, 4);
    assert_scalar_shl_decode(UINT32_C(0x5f6054e6), 32, 7, 6);
    assert_scalar_shl_decode(UINT32_C(0x5f7e5528), 62, 9, 8);
    assert_scalar_shl_decode(UINT32_C(0x5f7f57fe), 63, 31, 30);
    assert_scalar_shl_decode(
            UINT32_C(0x5f7457ff), 52, 31, 31);
    unsigned count = 0;
    for (unsigned shift = 0; shift < 64; shift++) {
        for (unsigned rn = 0; rn < 32; rn++) {
            for (unsigned rd = 0; rd < 32; rd++) {
                assert_scalar_shl_decode(encode_scalar_shl(
                        (byte_t) shift, (byte_t) rn, (byte_t) rd),
                        (byte_t) shift, (byte_t) rn, (byte_t) rd);
                count++;
            }
        }
    }
    assert(count == 65536);

    unsigned rejected_count = 0;
    for (unsigned immediate = 0; immediate < 64; immediate++) {
        for (unsigned rn = 0; rn < 32; rn++) {
            for (unsigned rd = 0; rd < 32; rd++) {
                dword_t word = UINT32_C(0x5f005400) |
                        (dword_t) immediate << 16 |
                        (dword_t) rn << 5 | (dword_t) rd;
                struct aarch64_decoded instruction;
                assert(!aarch64_decode(word, &instruction));
                rejected_count++;
            }
        }
    }
    assert(rejected_count == 65536);

    const dword_t fixed_mask = UINT32_C(0xff80fc00);
    dword_t base = encode_scalar_shl(52, 31, 31);
    for (unsigned bit = 0; bit < 32; bit++) {
        if ((fixed_mask & (UINT32_C(1) << bit)) == 0)
            continue;
        struct aarch64_decoded instruction;
        bool decoded = aarch64_decode(
                base ^ (UINT32_C(1) << bit), &instruction);
        assert(!decoded || instruction.opcode !=
                AARCH64_OP_ADVSIMD_SHL_SCALAR);
    }

    static const dword_t neighbors[] = {
        UINT32_C(0x5f085420), // SHL B0, B1, #0 的保留标量编码。
        UINT32_C(0x5f105420), // SHL H0, H1, #0 的保留标量编码。
        UINT32_C(0x5f205420), // SHL S0, S1, #0 的保留标量编码。
        UINT32_C(0x4f405420), // 向量 SHL V0.2D, V1.2D, #0。
        UINT32_C(0x5f405020), // 操作字段不同的相邻标量编码。
        UINT32_C(0x7f405420), // U 字段不同的相邻标量编码。
    };
    for (unsigned i = 0; i < sizeof(neighbors) / sizeof(neighbors[0]); i++) {
        struct aarch64_decoded instruction;
        bool decoded = aarch64_decode(neighbors[i], &instruction);
        assert(!decoded || instruction.opcode !=
                AARCH64_OP_ADVSIMD_SHL_SCALAR);
    }
}

static void assert_scalar_shl_state(
        const struct cpu_state *cpu, qword_t expected, qword_t pc) {
    assert(cpu->v[2].d[0] == expected);
    assert(cpu->v[2].d[1] == 0);
    assert(cpu->x[0] == UINT64_C(0x8877665544332211));
    assert(cpu->pc == pc);
    assert(cpu->sp == UINT64_C(0x1122334455667788));
    assert(cpu->nzcv == UINT32_C(0xa0000000));
    assert(cpu->fpcr == UINT32_C(0x01000000));
    assert(cpu->fpsr == UINT32_C(0x08000000));
}

static qword_t reference_scalar_shl(qword_t source, byte_t shift) {
    for (byte_t bit = 0; bit < shift; bit++)
        source += source;
    return source;
}

static void test_scalar_shl_execution(void) {
    for (unsigned shift = 0; shift < 64; shift++) {
        qword_t source = UINT64_C(0x8123456789abcdef) ^ shift;
        struct cpu_state cpu = {
            .pc = UINT64_C(0x1000),
            .sp = UINT64_C(0x1122334455667788),
            .nzcv = UINT32_C(0xa0000000),
            .fpcr = UINT32_C(0x01000000),
            .fpsr = UINT32_C(0x08000000),
        };
        cpu.x[0] = UINT64_C(0x8877665544332211);
        cpu.v[1].d[0] = source;
        cpu.v[1].d[1] = UINT64_C(0x1020304050607080);
        cpu.v[2].d[0] = UINT64_MAX;
        cpu.v[2].d[1] = UINT64_MAX;
        union aarch64_vector_reg saved_source = cpu.v[1];

        execute_instruction(&cpu,
                encode_scalar_shl((byte_t) shift, 1, 2));
        assert_scalar_shl_state(
                &cpu, reference_scalar_shl(source, (byte_t) shift),
                UINT64_C(0x1004));
        assert(cpu.v[1].d[0] == saved_source.d[0]);
        assert(cpu.v[1].d[1] == saved_source.d[1]);

        cpu.v[2].d[0] = source;
        cpu.v[2].d[1] = UINT64_MAX;
        execute_instruction(&cpu,
                encode_scalar_shl((byte_t) shift, 2, 2));
        assert_scalar_shl_state(
                &cpu, reference_scalar_shl(source, (byte_t) shift),
                UINT64_C(0x1008));
    }
}

int main(void) {
    test_apple_vectors();
    test_encoding_space();
    test_execution_space();
    test_special_forms();
    test_scalar_shl_decode();
    test_scalar_shl_execution();
    return 0;
}
