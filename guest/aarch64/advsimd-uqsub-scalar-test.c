#include <assert.h>
#include <string.h>

#include "guest/aarch64/decode.h"
#include "guest/aarch64/execute.h"

#define UQSUB_SCALAR_FIXED_MASK UINT32_C(0xff20fc00)
#define UQSUB_SCALAR_FIXED_BITS UINT32_C(0x7e202c00)
#define UQSUB_SCALAR_VARIABLE_MASK UINT32_C(0x00df03ff)

#define BUSYBOX_UQSUB_H30 UINT32_C(0x7e7f2fde)
#define BUSYBOX_UQSUB_H28 UINT32_C(0x7e7d2f9c)

static dword_t encode(
        byte_t size, byte_t rm, byte_t rn, byte_t rd) {
    return UQSUB_SCALAR_FIXED_BITS |
            (dword_t) size << 22 |
            (dword_t) rm << 16 |
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

static bool is_scalar_uqsub(const struct aarch64_decoded *instruction) {
    return instruction->opcode == AARCH64_OP_ADVSIMD_UQSUB_SCALAR;
}

static void assert_decode(dword_t word, byte_t width, byte_t element_size,
        byte_t rd, byte_t rn, byte_t rm) {
    struct aarch64_decoded instruction = decode(word);
    assert(is_scalar_uqsub(&instruction));
    assert(instruction.width == width);
    assert(instruction.operands.advsimd_three_same.element_size ==
            element_size);
    assert(instruction.operands.advsimd_three_same.rd == rd);
    assert(instruction.operands.advsimd_three_same.rn == rn);
    assert(instruction.operands.advsimd_three_same.rm == rm);
}

static void test_busybox_words(void) {
    assert_decode(BUSYBOX_UQSUB_H30, 16, 2, 30, 30, 31);
    assert_decode(BUSYBOX_UQSUB_H28, 16, 2, 28, 28, 29);
}

static void test_encoding_space(void) {
    assert((UQSUB_SCALAR_FIXED_MASK &
            UQSUB_SCALAR_VARIABLE_MASK) == 0);
    assert((UQSUB_SCALAR_FIXED_MASK |
            UQSUB_SCALAR_VARIABLE_MASK) == UINT32_MAX);

    unsigned decoded_count = 0;
    for (unsigned size = 0; size < 4; size++) {
        for (unsigned rm = 0; rm < 32; rm++) {
            for (unsigned rn = 0; rn < 32; rn++) {
                for (unsigned rd = 0; rd < 32; rd++) {
                    struct aarch64_decoded instruction = decode(encode(
                            (byte_t) size, (byte_t) rm,
                            (byte_t) rn, (byte_t) rd));
                    assert(is_scalar_uqsub(&instruction));
                    assert(instruction.width == (8U << size));
                    assert(instruction.operands.advsimd_three_same.
                            element_size == (1U << size));
                    assert(instruction.operands.advsimd_three_same.rd == rd);
                    assert(instruction.operands.advsimd_three_same.rn == rn);
                    assert(instruction.operands.advsimd_three_same.rm == rm);
                    decoded_count++;
                }
            }
        }
    }
    assert(decoded_count == 4 * 32 * 32 * 32);
}

static void test_fixed_bits_and_neighbors(void) {
    dword_t base = encode(3, 31, 30, 29);
    for (unsigned bit = 0; bit < 32; bit++) {
        if ((UQSUB_SCALAR_FIXED_MASK & (UINT32_C(1) << bit)) == 0)
            continue;
        struct aarch64_decoded instruction;
        bool decoded = aarch64_decode(
                base ^ (UINT32_C(1) << bit), &instruction);
        assert(!decoded || !is_scalar_uqsub(&instruction));
    }

    static const dword_t neighbors[] = {
        UINT32_C(0x5e622c20), // 标量 SQSUB H。
        UINT32_C(0x2e622c20), // 向量 UQSUB 4H。
        UINT32_C(0x6e622c20), // 向量 UQSUB 8H。
        UINT32_C(0x7ee28420), // 标量 SUB D。
    };
    for (unsigned index = 0;
            index < sizeof(neighbors) / sizeof(neighbors[0]); index++) {
        struct aarch64_decoded instruction;
        bool decoded = aarch64_decode(neighbors[index], &instruction);
        assert(!decoded || !is_scalar_uqsub(&instruction));
    }
}

static qword_t read_scalar(
        const union aarch64_vector_reg *value, byte_t element_size) {
    switch (element_size) {
        case 1:
            return value->b[0];
        case 2:
            return value->h[0];
        case 4:
            return value->s[0];
        case 8:
            return value->d[0];
    }
    assert(false);
    return 0;
}

static void write_scalar(union aarch64_vector_reg *value,
        byte_t element_size, qword_t scalar) {
    switch (element_size) {
        case 1:
            value->b[0] = (byte_t) scalar;
            return;
        case 2:
            value->h[0] = (word_t) scalar;
            return;
        case 4:
            value->s[0] = (dword_t) scalar;
            return;
        case 8:
            value->d[0] = scalar;
            return;
    }
    assert(false);
}

static union aarch64_vector_reg scalar_source(
        byte_t element_size, qword_t scalar, qword_t salt) {
    union aarch64_vector_reg value = {
        .d = {
            UINT64_C(0xa5a5a5a5a5a5a5a5) ^ salt,
            UINT64_C(0x5a5a5a5a5a5a5a5a) ^ (salt << 1),
        },
    };
    write_scalar(&value, element_size, scalar);
    return value;
}

static void assert_execution(dword_t word, byte_t element_size,
        union aarch64_vector_reg left,
        union aarch64_vector_reg right,
        dword_t initial_fpsr, bool expect_saturation) {
    struct aarch64_decoded instruction = decode(word);
    assert(is_scalar_uqsub(&instruction));
    assert(instruction.operands.advsimd_three_same.element_size ==
            element_size);
    byte_t rd = instruction.operands.advsimd_three_same.rd;
    byte_t rn = instruction.operands.advsimd_three_same.rn;
    byte_t rm = instruction.operands.advsimd_three_same.rm;

    struct cpu_state cpu = {
        .cycle = UINT64_C(0x123456789abcdef0),
        .sp = UINT64_C(0x1122334455667788),
        .pc = UINT64_C(0x1800),
        .nzcv = UINT32_C(0xa0000000),
        .fpcr = AARCH64_FPCR_FZ,
        .fpsr = initial_fpsr,
        .tpidr_el0 = UINT64_C(0x8877665544332211),
        .segfault_addr = UINT64_C(0x1020304050607080),
        .segfault_was_write = true,
        .trapno = UINT32_C(0x12345678),
        .single_step = true,
        ._poked = true,
    };
    for (unsigned reg = 0; reg < 31; reg++)
        cpu.x[reg] = UINT64_C(0x0102030405060708) +
                reg * UINT64_C(0x1111111111111111);
    for (unsigned reg = 0; reg < 32; reg++) {
        cpu.v[reg].d[0] = UINT64_C(0x1020304050607080) ^
                (reg * UINT64_C(0x0101010101010101));
        cpu.v[reg].d[1] = UINT64_C(0x8877665544332211) +
                reg * UINT64_C(0x0010001000100010);
    }

    cpu.v[rn] = left;
    if (rm != rn)
        cpu.v[rm] = right;

    qword_t actual_left = read_scalar(&cpu.v[rn], element_size);
    qword_t actual_right = read_scalar(&cpu.v[rm], element_size);
    bool saturated = actual_left < actual_right;
    assert(saturated == expect_saturation);

    struct cpu_state expected = cpu;
    expected.v[rd] = (union aarch64_vector_reg) {0};
    write_scalar(&expected.v[rd], element_size,
            saturated ? 0 : actual_left - actual_right);
    if (saturated)
        expected.fpsr |= AARCH64_FPSR_QC;
    expected.pc += 4;

    struct aarch64_execute_result result =
            aarch64_execute(&cpu, NULL, &instruction);
    assert(result.stop == AARCH64_EXECUTE_RETIRED);
    assert(memcmp(&cpu, &expected, sizeof(cpu)) == 0);
}

static void test_execution_boundaries_and_aliases(void) {
    // BusyBox 的两条指令都让目标与左源别名。
    assert_execution(BUSYBOX_UQSUB_H30, 2,
            scalar_source(2, UINT16_C(0x7fff), 1),
            scalar_source(2, UINT16_C(0x8000), 2),
            AARCH64_FPSR_IXC, true);
    assert_execution(BUSYBOX_UQSUB_H28, 2,
            scalar_source(2, UINT16_C(0x9001), 3),
            scalar_source(2, UINT16_C(0x8000), 4),
            AARCH64_FPSR_QC | AARCH64_FPSR_DZC, false);

    // B 覆盖目标与右源别名，S 覆盖相等源，D 覆盖非饱和大数减法。
    assert_execution(encode(0, 4, 5, 4), 1,
            scalar_source(1, 0, 5),
            scalar_source(1, 1, 6),
            0, true);
    assert_execution(encode(2, 7, 7, 6), 4,
            scalar_source(4, UINT32_C(0x89abcdef), 7),
            scalar_source(4, UINT32_C(0x12345678), 8),
            AARCH64_FPSR_OFC, false);
    assert_execution(encode(3, 10, 9, 9), 8,
            scalar_source(8, UINT64_MAX, 9),
            scalar_source(8, 1, 10),
            AARCH64_FPSR_IDC, false);
}

int main(void) {
    test_busybox_words();
    test_encoding_space();
    test_fixed_bits_and_neighbors();
    test_execution_boundaries_and_aliases();
    return 0;
}
