#include <assert.h>
#include <stdint.h>

#include "guest/aarch64/decode.h"
#include "guest/aarch64/execute.h"

static dword_t encode(bool is_64, unsigned operation,
        byte_t rd, byte_t rn) {
    return UINT32_C(0x5ac00000) |
            (dword_t) is_64 << 31 |
            (dword_t) operation << 10 |
            (dword_t) rn << 5 |
            rd;
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

static void assert_decode(dword_t word, enum aarch64_opcode opcode,
        byte_t width, byte_t rd, byte_t rn) {
    struct aarch64_decoded instruction = decode(word);
    assert(instruction.opcode == opcode);
    assert(instruction.width == width);
    assert(instruction.operands.data_processing_1source.rd == rd);
    assert(instruction.operands.data_processing_1source.rn == rn);
}

static void test_decode(void) {
    assert_decode(UINT32_C(0x5ac00020), AARCH64_OP_RBIT, 32, 0, 1);
    assert_decode(UINT32_C(0xdac00062), AARCH64_OP_RBIT, 64, 2, 3);
    assert_decode(UINT32_C(0x5ac004a4), AARCH64_OP_REV16, 32, 4, 5);
    assert_decode(UINT32_C(0xdac004e6), AARCH64_OP_REV16, 64, 6, 7);
    assert_decode(UINT32_C(0x5ac00928), AARCH64_OP_REV32, 32, 8, 9);
    assert_decode(UINT32_C(0xdac0096a), AARCH64_OP_REV32, 64, 10, 11);
    assert_decode(UINT32_C(0xdac00dac), AARCH64_OP_REV64, 64, 12, 13);
    assert_decode(UINT32_C(0xdac00dee), AARCH64_OP_REV64, 64, 14, 15);
    assert_decode(UINT32_C(0x5ac01230), AARCH64_OP_CLZ, 32, 16, 17);
    assert_decode(UINT32_C(0xdac01272), AARCH64_OP_CLZ, 64, 18, 19);
    assert_decode(UINT32_C(0x5ac016b4), AARCH64_OP_CLS, 32, 20, 21);
    assert_decode(UINT32_C(0xdac016f6), AARCH64_OP_CLS, 64, 22, 23);
}

static bool supported_operation(bool is_64, unsigned operation) {
    return operation <= 5 && (is_64 || operation != 3);
}

static void test_encoding_space(void) {
    unsigned valid[2] = {0};
    for (unsigned is_64 = 0; is_64 < 2; is_64++) {
        for (unsigned operation = 0; operation < 64; operation++) {
            struct aarch64_decoded instruction;
            bool decoded = aarch64_decode(encode(is_64, operation, 0, 1),
                    &instruction);
            assert(decoded == supported_operation(is_64, operation));
            if (decoded)
                valid[is_64]++;
        }
    }
    assert(valid[0] == 5);
    assert(valid[1] == 6);

    struct aarch64_decoded instruction;
    assert(!aarch64_decode(UINT32_C(0x1ac00020), &instruction));
    assert(!aarch64_decode(UINT32_C(0x5ae00020), &instruction));
}

static qword_t width_mask(unsigned width) {
    return width == 64 ? UINT64_MAX : UINT32_MAX;
}

static qword_t reference_rbit(qword_t value, unsigned width) {
    bool bits[64];
    for (unsigned bit = 0; bit < width; bit++)
        bits[bit] = (value >> bit) & 1;
    qword_t result = 0;
    for (unsigned bit = 0; bit < width; bit++) {
        if (bits[bit])
            result |= UINT64_C(1) << (width - 1 - bit);
    }
    return result;
}

static qword_t reference_reverse_bytes(qword_t value, unsigned width,
        unsigned element_width) {
    byte_t source[8] = {0};
    byte_t destination[8] = {0};
    unsigned byte_count = width / 8;
    unsigned element_bytes = element_width / 8;
    for (unsigned byte = 0; byte < byte_count; byte++)
        source[byte] = (byte_t) (value >> (byte * 8));
    for (unsigned element = 0; element < byte_count;
            element += element_bytes) {
        for (unsigned byte = 0; byte < element_bytes; byte++) {
            destination[element + byte] =
                    source[element + element_bytes - 1 - byte];
        }
    }
    qword_t result = 0;
    for (unsigned byte = 0; byte < byte_count; byte++)
        result |= (qword_t) destination[byte] << (byte * 8);
    return result;
}

static qword_t reference_clz(qword_t value, unsigned width) {
    unsigned count = 0;
    for (unsigned bit = width; bit > 0; bit--) {
        if ((value >> (bit - 1)) & 1)
            break;
        count++;
    }
    return count;
}

static qword_t reference_cls(qword_t value, unsigned width) {
    bool sign = (value >> (width - 1)) & 1;
    unsigned count = 0;
    for (unsigned bit = width - 1; bit > 0; bit--) {
        if (((value >> (bit - 1)) & 1) != sign)
            break;
        count++;
    }
    return count;
}

static qword_t reference_result(unsigned operation, qword_t value,
        unsigned width) {
    value &= width_mask(width);
    switch (operation) {
        case 0:
            return reference_rbit(value, width);
        case 1:
            return reference_reverse_bytes(value, width, 16);
        case 2:
            return reference_reverse_bytes(value, width, 32);
        case 3:
            return reference_reverse_bytes(value, width, 64);
        case 4:
            return reference_clz(value, width);
        default:
            return reference_cls(value, width);
    }
}

static void test_execution(void) {
    static const qword_t values[] = {
        0,
        UINT64_MAX,
        1,
        UINT64_C(0x8000000000000000),
        UINT64_C(0x7fffffffffffffff),
        UINT64_C(0x0123456789abcdef),
        UINT64_C(0xaaaaaaaaaaaaaaaa),
    };
    for (unsigned is_64 = 0; is_64 < 2; is_64++) {
        unsigned width = is_64 ? 64 : 32;
        for (unsigned operation = 0; operation <= 5; operation++) {
            if (!supported_operation(is_64, operation))
                continue;
            for (size_t i = 0; i < array_size(values); i++) {
                struct cpu_state cpu = {
                    .pc = UINT64_C(0x1000),
                    .sp = UINT64_C(0xfedcba9876543210),
                    .nzcv = UINT32_C(0xa0000000),
                    .fpcr = UINT32_C(0x01000000),
                    .fpsr = UINT32_C(0x08000000),
                };
                cpu.x[0] = UINT64_MAX;
                cpu.x[1] = values[i];
                execute_instruction(&cpu,
                        encode(is_64, operation, 0, 1));
                assert(cpu.x[0] == reference_result(
                        operation, values[i], width));
                assert(cpu.pc == UINT64_C(0x1004));
                assert(cpu.sp == UINT64_C(0xfedcba9876543210));
                assert(cpu.nzcv == UINT32_C(0xa0000000));
                assert(cpu.fpcr == UINT32_C(0x01000000));
                assert(cpu.fpsr == UINT32_C(0x08000000));
            }
        }
    }
}

static void test_known_results(void) {
    struct cpu_state cpu = {.pc = UINT64_C(0x1800)};
    cpu.x[1] = UINT64_C(0x0123456789abcdef);

    execute_instruction(&cpu, encode(true, 0, 0, 1));
    assert(cpu.x[0] == UINT64_C(0xf7b3d591e6a2c480));
    execute_instruction(&cpu, encode(true, 1, 0, 1));
    assert(cpu.x[0] == UINT64_C(0x23016745ab89efcd));
    execute_instruction(&cpu, encode(true, 2, 0, 1));
    assert(cpu.x[0] == UINT64_C(0x67452301efcdab89));
    execute_instruction(&cpu, encode(true, 3, 0, 1));
    assert(cpu.x[0] == UINT64_C(0xefcdab8967452301));

    execute_instruction(&cpu, encode(false, 0, 0, 1));
    assert(cpu.x[0] == UINT32_C(0xf7b3d591));
    execute_instruction(&cpu, encode(false, 1, 0, 1));
    assert(cpu.x[0] == UINT32_C(0xab89efcd));
    execute_instruction(&cpu, encode(false, 2, 0, 1));
    assert(cpu.x[0] == UINT32_C(0xefcdab89));
}

static void test_leading_counts(void) {
    for (unsigned is_64 = 0; is_64 < 2; is_64++) {
        unsigned width = is_64 ? 64 : 32;
        qword_t mask = width_mask(width);
        for (unsigned bit = 0; bit < width; bit++) {
            struct cpu_state cpu = {.pc = UINT64_C(0x1c00)};
            cpu.x[1] = UINT64_C(1) << bit;
            execute_instruction(&cpu, encode(is_64, 4, 0, 1));
            assert(cpu.x[0] == width - 1 - bit);

            if (bit == width - 1)
                continue;
            execute_instruction(&cpu, encode(is_64, 5, 0, 1));
            assert(cpu.x[0] == width - 2 - bit);

            cpu.x[1] = mask ^ (UINT64_C(1) << bit);
            execute_instruction(&cpu, encode(is_64, 5, 0, 1));
            assert(cpu.x[0] == width - 2 - bit);
        }

        struct cpu_state cpu = {.pc = UINT64_C(0x1d00)};
        execute_instruction(&cpu, encode(is_64, 5, 0, 1));
        assert(cpu.x[0] == width - 1);
        cpu.x[1] = mask;
        execute_instruction(&cpu, encode(is_64, 5, 0, 1));
        assert(cpu.x[0] == width - 1);
    }
}

static void test_boundaries(void) {
    struct cpu_state cpu = {
        .pc = UINT64_C(0x2000),
        .sp = UINT64_C(0x123456789abcdef0),
        .nzcv = UINT32_C(0x60000000),
    };
    execute_instruction(&cpu, encode(false, 4, 0, 31));
    assert(cpu.x[0] == 32);

    cpu.x[1] = UINT64_C(0x0123456789abcdef);
    execute_instruction(&cpu, encode(true, 0, 1, 1));
    assert(cpu.x[1] == UINT64_C(0xf7b3d591e6a2c480));

    cpu.x[0] = UINT64_C(0x13579bdf2468ace0);
    execute_instruction(&cpu, encode(true, 3, 31, 1));
    assert(cpu.x[0] == UINT64_C(0x13579bdf2468ace0));

    cpu.x[1] = UINT64_C(0xffffffff89abcdef);
    execute_instruction(&cpu, encode(false, 1, 0, 1));
    assert(cpu.x[0] == UINT32_C(0xab89efcd));
    assert(cpu.pc == UINT64_C(0x2010));
    assert(cpu.sp == UINT64_C(0x123456789abcdef0));
    assert(cpu.nzcv == UINT32_C(0x60000000));
}

int main(void) {
    test_decode();
    test_encoding_space();
    test_execution();
    test_known_results();
    test_leading_counts();
    test_boundaries();
    return 0;
}
