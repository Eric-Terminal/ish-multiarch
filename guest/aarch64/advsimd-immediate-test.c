#include <assert.h>
#include <stdint.h>
#include <string.h>

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

static dword_t encode_scalar_ushr(byte_t shift, byte_t rn, byte_t rd) {
    assert(shift >= 1 && shift <= 64);
    return UINT32_C(0x7f000400) |
            (dword_t) (128 - shift) << 16 |
            (dword_t) rn << 5 | rd;
}

static dword_t encode_vector_sshr(bool q, byte_t element_size,
        byte_t shift, byte_t rn, byte_t rd) {
    byte_t element_bits = (byte_t) (element_size * 8);
    assert(element_size == 1 || element_size == 2 ||
            element_size == 4 || element_size == 8);
    assert(q || element_size != 8);
    assert(shift >= 1 && shift <= element_bits);
    return UINT32_C(0x0f000400) |
            (dword_t) q << 30 |
            (dword_t) (element_bits * 2 - shift) << 16 |
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
    assert(instruction.operands.advsimd_shift_immediate.element_size == 8);
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

static void assert_scalar_ushr_decode(
        dword_t word, byte_t shift, byte_t rn, byte_t rd) {
    struct aarch64_decoded instruction = decode(word);
    assert(instruction.opcode == AARCH64_OP_ADVSIMD_USHR_SCALAR);
    assert(instruction.width == 64);
    assert(instruction.operands.advsimd_shift_immediate.rd == rd);
    assert(instruction.operands.advsimd_shift_immediate.rn == rn);
    assert(instruction.operands.advsimd_shift_immediate.element_size == 8);
    assert(instruction.operands.advsimd_shift_immediate.shift == shift);
}

static void test_scalar_ushr_decode(void) {
    assert_scalar_ushr_decode(
            UINT32_C(0x7f5907fc), 39, 31, 28);
    assert_scalar_ushr_decode(UINT32_C(0x7f7f0420), 1, 1, 0);
    assert_scalar_ushr_decode(UINT32_C(0x7f400462), 64, 3, 2);

    unsigned count = 0;
    for (unsigned shift = 1; shift <= 64; shift++) {
        for (unsigned rn = 0; rn < 32; rn++) {
            for (unsigned rd = 0; rd < 32; rd++) {
                assert_scalar_ushr_decode(encode_scalar_ushr(
                        (byte_t) shift, (byte_t) rn, (byte_t) rd),
                        (byte_t) shift, (byte_t) rn, (byte_t) rd);
                count++;
            }
        }
    }
    assert(count == 65536);

    for (unsigned immediate = 0; immediate < 64; immediate++) {
        struct aarch64_decoded instruction;
        dword_t word = UINT32_C(0x7f000400) |
                (dword_t) immediate << 16;
        assert(!aarch64_decode(word, &instruction));
    }

    const dword_t fixed_mask = UINT32_C(0xffc0fc00);
    const dword_t base = UINT32_C(0x7f5907fc);
    for (unsigned bit = 0; bit < 32; bit++) {
        if ((fixed_mask & (UINT32_C(1) << bit)) == 0)
            continue;
        struct aarch64_decoded instruction;
        bool decoded = aarch64_decode(
                base ^ (UINT32_C(1) << bit), &instruction);
        assert(!decoded || instruction.opcode !=
                AARCH64_OP_ADVSIMD_USHR_SCALAR);
    }

    static const dword_t neighbors[] = {
        UINT32_C(0x5f5907fc), // SSHR D28, D31, #39。
        UINT32_C(0x7f5917fc), // USRA D28, D31, #39。
        UINT32_C(0x5f5917fc), // SSRA D28, D31, #39。
        UINT32_C(0x7f5947fc), // SRI D28, D31, #39。
        UINT32_C(0x5f6757fc), // SHL D28, D31, #39。
        UINT32_C(0x6f5907fc), // USHR V28.2D, V31.2D, #39。
    };
    for (unsigned index = 0; index < array_size(neighbors); index++) {
        struct aarch64_decoded instruction;
        bool decoded = aarch64_decode(neighbors[index], &instruction);
        assert(!decoded || instruction.opcode !=
                AARCH64_OP_ADVSIMD_USHR_SCALAR);
    }
}

static void test_scalar_ushr_execution(void) {
    for (unsigned shift = 1; shift <= 64; shift++) {
        qword_t source = UINT64_C(0x8123456789abcdef) ^ shift;
        struct cpu_state cpu = {
            .pc = UINT64_C(0x2000),
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
                encode_scalar_ushr((byte_t) shift, 1, 2));
        assert_scalar_shl_state(&cpu,
                shift == 64 ? 0 : source >> shift,
                UINT64_C(0x2004));
        assert(cpu.v[1].d[0] == saved_source.d[0]);
        assert(cpu.v[1].d[1] == saved_source.d[1]);

        cpu.v[2].d[0] = source;
        cpu.v[2].d[1] = UINT64_MAX;
        execute_instruction(&cpu,
                encode_scalar_ushr((byte_t) shift, 2, 2));
        assert_scalar_shl_state(&cpu,
                shift == 64 ? 0 : source >> shift,
                UINT64_C(0x2008));
    }
}

static void assert_vector_sshr_decode(dword_t word, bool q,
        byte_t element_size, byte_t shift, byte_t rn, byte_t rd) {
    struct aarch64_decoded instruction = decode(word);
    assert(instruction.opcode == AARCH64_OP_ADVSIMD_SSHR);
    assert(instruction.width == (q ? 128 : 64));
    assert(instruction.operands.advsimd_shift_immediate.rd == rd);
    assert(instruction.operands.advsimd_shift_immediate.rn == rn);
    assert(instruction.operands.advsimd_shift_immediate.element_size ==
            element_size);
    assert(instruction.operands.advsimd_shift_immediate.shift == shift);
}

static void test_vector_sshr_decode(void) {
    assert_vector_sshr_decode(
            UINT32_C(0x0f3807fe), false, 4, 8, 31, 30);
    assert_vector_sshr_decode(
            UINT32_C(0x0f0f0420), false, 1, 1, 1, 0);
    assert_vector_sshr_decode(
            UINT32_C(0x4f080462), true, 1, 8, 3, 2);
    assert_vector_sshr_decode(
            UINT32_C(0x0f1f04a4), false, 2, 1, 5, 4);
    assert_vector_sshr_decode(
            UINT32_C(0x4f20056a), true, 4, 32, 11, 10);
    assert_vector_sshr_decode(
            UINT32_C(0x4f4005ee), true, 8, 64, 15, 14);

    static const byte_t element_sizes[] = {1, 2, 4, 8};
    unsigned legal = 0;
    for (unsigned q = 0; q < 2; q++) {
        for (unsigned size_index = 0;
                size_index < array_size(element_sizes); size_index++) {
            byte_t element_size = element_sizes[size_index];
            if (!q && element_size == 8)
                continue;
            for (unsigned shift = 1;
                    shift <= element_size * 8; shift++) {
                for (unsigned rn = 0; rn < 32; rn++) {
                    for (unsigned rd = 0; rd < 32; rd++) {
                        assert_vector_sshr_decode(encode_vector_sshr(
                                q != 0, element_size, (byte_t) shift,
                                (byte_t) rn, (byte_t) rd), q != 0,
                                element_size, (byte_t) shift,
                                (byte_t) rn, (byte_t) rd);
                        legal++;
                    }
                }
            }
        }
    }
    assert(legal == 180224);

    for (unsigned q = 0; q < 2; q++) {
        for (unsigned immediate = 0; immediate < 8; immediate++) {
            for (unsigned rn = 0; rn < 32; rn++) {
                for (unsigned rd = 0; rd < 32; rd++) {
                    dword_t word = UINT32_C(0x0f000400) |
                            (dword_t) q << 30 |
                            (dword_t) immediate << 16 |
                            (dword_t) rn << 5 | (dword_t) rd;
                    struct aarch64_decoded instruction = decode(word);
                    assert(instruction.opcode != AARCH64_OP_ADVSIMD_SSHR);
                }
            }
        }
    }

    for (unsigned immediate = 64; immediate < 128; immediate++) {
        for (unsigned rn = 0; rn < 32; rn++) {
            for (unsigned rd = 0; rd < 32; rd++) {
                dword_t word = UINT32_C(0x0f000400) |
                        (dword_t) immediate << 16 |
                        (dword_t) rn << 5 | (dword_t) rd;
                struct aarch64_decoded instruction;
                assert(!aarch64_decode(word, &instruction));
            }
        }
    }

    const dword_t fixed_mask = UINT32_C(0xbf80fc00);
    const dword_t product = UINT32_C(0x0f3807fe);
    for (unsigned bit = 0; bit < 32; bit++) {
        if ((fixed_mask & (UINT32_C(1) << bit)) == 0)
            continue;
        struct aarch64_decoded instruction;
        bool decoded = aarch64_decode(
                product ^ (UINT32_C(1) << bit), &instruction);
        assert(!decoded || instruction.opcode != AARCH64_OP_ADVSIMD_SSHR);
    }

    static const dword_t neighbors[] = {
        UINT32_C(0x5f7807fe), // 标量 SSHR D30, D31, #8。
        UINT32_C(0x2f3807fe), // 向量 USHR。
        UINT32_C(0x0f3817fe), // 向量 SSRA。
        UINT32_C(0x0f3827fe), // 向量 SRSHR。
        UINT32_C(0x0f3837fe), // 向量 SRSRA。
        UINT32_C(0x2f3847fe), // 向量 SRI。
        UINT32_C(0x0f3857fe), // 向量 SHL。
        UINT32_C(0x0f38a7fe), // 向量 SSHLL。
    };
    for (unsigned index = 0; index < array_size(neighbors); index++) {
        struct aarch64_decoded instruction;
        bool decoded = aarch64_decode(neighbors[index], &instruction);
        assert(!decoded || instruction.opcode != AARCH64_OP_ADVSIMD_SSHR);
    }
}

static qword_t vector_lane_mask(byte_t element_size) {
    return element_size == 8 ? UINT64_MAX :
            (UINT64_C(1) << (element_size * 8)) - 1;
}

static qword_t read_vector_lane(const union aarch64_vector_reg *reg,
        byte_t element_size, byte_t lane) {
    if (element_size == 1)
        return reg->b[lane];
    if (element_size == 2)
        return reg->h[lane];
    if (element_size == 4)
        return reg->s[lane];
    return reg->d[lane];
}

static void write_vector_lane(union aarch64_vector_reg *reg,
        byte_t element_size, byte_t lane, qword_t value) {
    if (element_size == 1)
        reg->b[lane] = (byte_t) value;
    else if (element_size == 2)
        reg->h[lane] = (word_t) value;
    else if (element_size == 4)
        reg->s[lane] = (dword_t) value;
    else
        reg->d[lane] = value;
}

static qword_t reference_vector_sshr(
        qword_t value, byte_t element_size, byte_t shift) {
    qword_t mask = vector_lane_mask(element_size);
    qword_t sign = UINT64_C(1) << (element_size * 8 - 1);
    value &= mask;
    // 逐位补入原符号位，避免依赖宿主对负有符号右移的实现选择。
    for (byte_t bit = 0; bit < shift; bit++)
        value = (value >> 1) | (value & sign);
    return value & mask;
}

static union aarch64_vector_reg make_vector_sshr_source(
        byte_t element_size, byte_t shift) {
    qword_t mask = vector_lane_mask(element_size);
    qword_t sign = UINT64_C(1) << (element_size * 8 - 1);
    qword_t patterns[] = {
        0,
        1,
        sign - 1,
        sign,
        sign + 1,
        mask - 1,
        mask,
        mask ^ (mask >> 1),
    };
    union aarch64_vector_reg source = {0};
    for (byte_t lane = 0; lane < 16 / element_size; lane++) {
        write_vector_lane(&source, element_size, lane,
                patterns[(lane + shift) % array_size(patterns)]);
    }
    return source;
}

static union aarch64_vector_reg expected_vector_sshr(
        const union aarch64_vector_reg *source, bool q,
        byte_t element_size, byte_t shift) {
    union aarch64_vector_reg result = {0};
    byte_t lanes = (byte_t) ((q ? 16 : 8) / element_size);
    for (byte_t lane = 0; lane < lanes; lane++) {
        write_vector_lane(&result, element_size, lane,
                reference_vector_sshr(read_vector_lane(
                        source, element_size, lane),
                        element_size, shift));
    }
    return result;
}

static void test_vector_sshr_execution(void) {
    static const byte_t element_sizes[] = {1, 2, 4, 8};
    for (unsigned q = 0; q < 2; q++) {
        for (unsigned size_index = 0;
                size_index < array_size(element_sizes); size_index++) {
            byte_t element_size = element_sizes[size_index];
            if (!q && element_size == 8)
                continue;
            for (unsigned shift = 1;
                    shift <= element_size * 8; shift++) {
                union aarch64_vector_reg source =
                        make_vector_sshr_source(
                                element_size, (byte_t) shift);
                union aarch64_vector_reg result =
                        expected_vector_sshr(&source, q != 0,
                                element_size, (byte_t) shift);
                struct cpu_state cpu = {
                    .cycle = UINT64_C(0x123456789abcdef0),
                    .sp = UINT64_C(0x1122334455667788),
                    .pc = UINT64_C(0x3000),
                    .nzcv = UINT32_C(0xa0000000),
                    .fpcr = UINT32_C(0x01000000),
                    .fpsr = UINT32_C(0x08000010),
                    .tpidr_el0 = UINT64_C(0x8877665544332211),
                    .segfault_addr = UINT64_C(0x1020304050607080),
                    .segfault_was_write = true,
                    .trapno = UINT32_C(0x12345678),
                    .single_step = true,
                    ._poked = true,
                };
                cpu.x[0] = UINT64_C(0xfedcba9876543210);
                cpu.v[1] = source;
                cpu.v[2].q = ~(__uint128_t) 0;
                struct cpu_state expected = cpu;
                expected.v[2] = result;
                expected.pc += 4;

                execute_instruction(&cpu, encode_vector_sshr(
                        q != 0, element_size, (byte_t) shift, 1, 2));
                assert(memcmp(&cpu, &expected, sizeof(cpu)) == 0);

                cpu = expected;
                cpu.pc = UINT64_C(0x3800);
                cpu.v[2] = source;
                expected = cpu;
                expected.v[2] = result;
                expected.pc += 4;
                execute_instruction(&cpu, encode_vector_sshr(
                        q != 0, element_size, (byte_t) shift, 2, 2));
                assert(memcmp(&cpu, &expected, sizeof(cpu)) == 0);
            }
        }
    }

    struct cpu_state product = {.pc = UINT64_C(0x4000)};
    product.v[31].d[0] = UINT64_C(1);
    product.v[31].d[1] = UINT64_C(0x0123456789abcdef);
    product.v[30].d[0] = UINT64_C(4);
    product.v[30].d[1] = UINT64_C(1);
    execute_instruction(&product, UINT32_C(0x0f3807fe));
    assert(product.v[30].d[0] == 0);
    assert(product.v[30].d[1] == 0);
    assert(product.v[31].d[0] == UINT64_C(1));
    assert(product.v[31].d[1] == UINT64_C(0x0123456789abcdef));
    assert(product.pc == UINT64_C(0x4004));

    struct cpu_state full_width = {.pc = UINT64_C(0x4800)};
    full_width.v[31].d[0] = UINT64_C(0x8000000000000000);
    full_width.v[31].d[1] = UINT64_MAX;
    execute_instruction(&full_width,
            encode_vector_sshr(true, 8, 64, 31, 31));
    assert(full_width.v[31].d[0] == UINT64_MAX);
    assert(full_width.v[31].d[1] == UINT64_MAX);
    assert(full_width.pc == UINT64_C(0x4804));
}

int main(void) {
    test_apple_vectors();
    test_encoding_space();
    test_execution_space();
    test_special_forms();
    test_scalar_shl_decode();
    test_scalar_shl_execution();
    test_scalar_ushr_decode();
    test_scalar_ushr_execution();
    test_vector_sshr_decode();
    test_vector_sshr_execution();
    return 0;
}
