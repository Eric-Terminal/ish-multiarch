#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#include "guest/aarch64/decode.h"
#include "guest/aarch64/execute.h"

static dword_t encode(bool upper, byte_t source_size,
        byte_t rn, byte_t rd) {
    byte_t size = source_size == 2 ? 0 : source_size == 4 ? 1 : 2;
    assert(source_size == 2 || source_size == 4 || source_size == 8);
    return UINT32_C(0x0e212800) | (dword_t) upper << 30 |
            (dword_t) size << 22 | (dword_t) rn << 5 | rd;
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

static bool is_narrow_opcode(enum aarch64_opcode opcode) {
    return opcode == AARCH64_OP_ADVSIMD_XTN ||
            opcode == AARCH64_OP_ADVSIMD_XTN2;
}

static void assert_decode(dword_t word, bool upper,
        byte_t source_size, byte_t rn, byte_t rd) {
    struct aarch64_decoded instruction = decode(word);
    assert(instruction.opcode == (upper ? AARCH64_OP_ADVSIMD_XTN2 :
            AARCH64_OP_ADVSIMD_XTN));
    assert(instruction.width == (upper ? 128 : 64));
    assert(instruction.operands.advsimd_narrow.rd == rd);
    assert(instruction.operands.advsimd_narrow.rn == rn);
    assert(instruction.operands.advsimd_narrow.source_element_size ==
            source_size);
}

static void test_decode_and_neighbors(void) {
    assert_decode(UINT32_C(0x0e212820), false, 2, 1, 0);
    assert_decode(UINT32_C(0x4e212862), true, 2, 3, 2);
    assert_decode(UINT32_C(0x0e6128a4), false, 4, 5, 4);
    assert_decode(UINT32_C(0x4e6128e6), true, 4, 7, 6);
    assert_decode(UINT32_C(0x0ea12928), false, 8, 9, 8);
    assert_decode(UINT32_C(0x4ea1296a), true, 8, 11, 10);
    assert_decode(UINT32_C(0x0e612b9c), false, 4, 28, 28);

    static const byte_t source_sizes[] = {2, 4, 8};
    unsigned legal = 0;
    for (unsigned upper = 0; upper < 2; upper++) {
        for (unsigned size = 0; size < array_size(source_sizes); size++) {
            for (unsigned rn = 0; rn < 32; rn++) {
                for (unsigned rd = 0; rd < 32; rd++) {
                    assert_decode(encode(upper != 0, source_sizes[size],
                            (byte_t) rn, (byte_t) rd), upper != 0,
                            source_sizes[size], (byte_t) rn, (byte_t) rd);
                    legal++;
                }
            }
        }
    }
    assert(legal == 6144);

    for (unsigned upper = 0; upper < 2; upper++) {
        struct aarch64_decoded instruction;
        dword_t reserved = UINT32_C(0x0ee12800) |
                (dword_t) upper << 30;
        assert(!aarch64_decode(reserved, &instruction));
    }

    static const dword_t neighbors[] = {
        UINT32_C(0x0e214820), // SQXTN V0.8B, V1.8H。
        UINT32_C(0x2e214820), // UQXTN V0.8B, V1.8H。
        UINT32_C(0x0f0f8420), // SHRN V0.8B, V1.8H, #1。
    };
    for (unsigned index = 0; index < array_size(neighbors); index++) {
        struct aarch64_decoded instruction;
        bool decoded = aarch64_decode(neighbors[index], &instruction);
        assert(!decoded || !is_narrow_opcode(instruction.opcode));
    }

    const dword_t fixed_mask = UINT32_C(0xbf3ffc00);
    const dword_t base = UINT32_C(0x0e612b9c);
    for (unsigned bit = 0; bit < 32; bit++) {
        if ((fixed_mask & (UINT32_C(1) << bit)) == 0)
            continue;
        struct aarch64_decoded instruction;
        bool decoded = aarch64_decode(
                base ^ (UINT32_C(1) << bit), &instruction);
        assert(!decoded || !is_narrow_opcode(instruction.opcode));
    }
}

static qword_t read_lane(const union aarch64_vector_reg *reg,
        byte_t element_size, byte_t lane) {
    if (element_size == 1)
        return reg->b[lane];
    if (element_size == 2)
        return reg->h[lane];
    if (element_size == 4)
        return reg->s[lane];
    return reg->d[lane];
}

static void write_lane(union aarch64_vector_reg *reg,
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

static union aarch64_vector_reg make_source(byte_t source_size) {
    union aarch64_vector_reg source = {0};
    byte_t lanes = 16 / source_size;
    unsigned destination_bits = source_size * 4;
    qword_t low_mask = (UINT64_C(1) << destination_bits) - 1;
    for (byte_t lane = 0; lane < lanes; lane++) {
        qword_t low = (UINT64_C(0x76543210) + lane * 0x11111111) &
                low_mask;
        qword_t high = (UINT64_C(0xa5) + lane) << destination_bits;
        write_lane(&source, source_size, lane, high | low);
    }
    return source;
}

static union aarch64_vector_reg expected_result(
        const union aarch64_vector_reg *source,
        const union aarch64_vector_reg *old_destination,
        byte_t source_size, bool upper) {
    byte_t destination_size = source_size / 2;
    byte_t lanes = 16 / source_size;
    union aarch64_vector_reg result = {0};
    if (upper)
        result.d[0] = old_destination->d[0];
    for (byte_t lane = 0; lane < lanes; lane++)
        write_lane(&result, destination_size,
                (byte_t) (lane + (upper ? lanes : 0)),
                read_lane(source, source_size, lane));
    return result;
}

static void assert_stable_state(const struct cpu_state *cpu, qword_t pc) {
    assert(cpu->pc == pc);
    assert(cpu->x[0] == UINT64_C(0x0123456789abcdef));
    assert(cpu->sp == UINT64_C(0xfedcba9876543210));
    assert(cpu->nzcv == UINT32_C(0xa0000000));
    assert(cpu->fpcr == UINT32_C(0x01000000));
    assert(cpu->fpsr == UINT32_C(0x08000000));
}

static void test_execution_space(void) {
    static const byte_t source_sizes[] = {2, 4, 8};
    for (unsigned upper = 0; upper < 2; upper++) {
        for (unsigned index = 0; index < array_size(source_sizes); index++) {
            byte_t source_size = source_sizes[index];
            union aarch64_vector_reg source = make_source(source_size);
            union aarch64_vector_reg old_destination = {
                .d = {UINT64_C(0x0123456789abcdef), UINT64_MAX},
            };
            union aarch64_vector_reg expected = expected_result(
                    &source, &old_destination,
                    source_size, upper != 0);
            struct cpu_state cpu = {
                .pc = UINT64_C(0x1000),
                .sp = UINT64_C(0xfedcba9876543210),
                .nzcv = UINT32_C(0xa0000000),
                .fpcr = UINT32_C(0x01000000),
                .fpsr = UINT32_C(0x08000000),
            };
            cpu.x[0] = UINT64_C(0x0123456789abcdef);
            cpu.v[1] = source;
            cpu.v[2] = old_destination;
            execute_instruction(&cpu,
                    encode(upper != 0, source_size, 1, 2));
            assert(cpu.v[2].d[0] == expected.d[0]);
            assert(cpu.v[2].d[1] == expected.d[1]);
            assert(cpu.v[1].d[0] == source.d[0]);
            assert(cpu.v[1].d[1] == source.d[1]);
            assert_stable_state(&cpu, UINT64_C(0x1004));

            cpu.pc = UINT64_C(0x1800);
            cpu.v[2] = source;
            expected = expected_result(
                    &source, &source, source_size, upper != 0);
            execute_instruction(&cpu,
                    encode(upper != 0, source_size, 2, 2));
            assert(cpu.v[2].d[0] == expected.d[0]);
            assert(cpu.v[2].d[1] == expected.d[1]);
            assert_stable_state(&cpu, UINT64_C(0x1804));
        }
    }
}

int main(void) {
    test_decode_and_neighbors();
    test_execution_space();
    return 0;
}
