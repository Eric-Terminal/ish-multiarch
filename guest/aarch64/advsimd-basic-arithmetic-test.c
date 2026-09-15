#include "guest/aarch64/fp-common-test.h"

static const struct {
    dword_t bits;
    enum aarch64_opcode opcode;
    byte_t width, element_size;
} layouts[] = {
    {0x0e209c00, AARCH64_OP_ADVSIMD_MUL, 64, 1},
    {0x4e209c00, AARCH64_OP_ADVSIMD_MUL, 128, 1},
    {0x0e609c00, AARCH64_OP_ADVSIMD_MUL, 64, 2},
    {0x4e609c00, AARCH64_OP_ADVSIMD_MUL, 128, 2},
    {0x0ea09c00, AARCH64_OP_ADVSIMD_MUL, 64, 4},
    {0x4ea09c00, AARCH64_OP_ADVSIMD_MUL, 128, 4},
    {0x0e20d400, AARCH64_OP_ADVSIMD_FADD, 64, 4},
    {0x4e20d400, AARCH64_OP_ADVSIMD_FADD, 128, 4},
    {0x4e60d400, AARCH64_OP_ADVSIMD_FADD, 128, 8},
    {0x0ea0d400, AARCH64_OP_ADVSIMD_FSUB, 64, 4},
    {0x4ea0d400, AARCH64_OP_ADVSIMD_FSUB, 128, 4},
    {0x4ee0d400, AARCH64_OP_ADVSIMD_FSUB, 128, 8},
    {0x2e20dc00, AARCH64_OP_ADVSIMD_FMUL, 64, 4},
    {0x6e20dc00, AARCH64_OP_ADVSIMD_FMUL, 128, 4},
    {0x6e60dc00, AARCH64_OP_ADVSIMD_FMUL, 128, 8},
};

static void test_decoding(void) {
    for (unsigned layout = 0; layout < 15; layout++) {
        for (unsigned rn = 0; rn < 32; rn++) {
            for (unsigned rm = 0; rm < 32; rm++) {
                for (unsigned rd = 0; rd < 32; rd++) {
                    struct aarch64_decoded decoded;
                    assert(aarch64_decode(layouts[layout].bits | rm << 16 | rn << 5 | rd,
                            &decoded));
                    assert(decoded.opcode == layouts[layout].opcode);
                    assert(decoded.width == layouts[layout].width);
                    assert(decoded.operands.advsimd_three_same.element_size == layouts[layout].element_size);
                    assert(decoded.operands.advsimd_three_same.rn == rn);
                    assert(decoded.operands.advsimd_three_same.rm == rm);
                    assert(decoded.operands.advsimd_three_same.rd == rd);
                }
            }
        }
    }
    // MUL 没有 64 位整数通道，向量浮点也没有 1D 排列；FP16/按元素另属编码族。
    const dword_t rejected[] = {
        0x0ee29c20, 0x4ee29c20, 0x0e62d420, 0x0ee2d420, 0x2e62dc20,
        0x0e421420, 0x0ec21420, 0x2e421c20, 0x4f829020, 0x4f829820,
    };
    for (unsigned i = 0; i < sizeof(rejected) / sizeof(rejected[0]); i++) {
        struct aarch64_decoded decoded;
        assert(!aarch64_decode(rejected[i], &decoded));
    }
}

static void test_fixed_results(void) {
    for (unsigned layout = 0; layout < 15; layout++) {
        unsigned size = layouts[layout].element_size;
        for (unsigned alias = 0; alias < 3; alias++) {
            struct cpu_state cpu = fp_test_cpu();
            unsigned rd = alias == 0 ? 2 : alias == 1 ? 31 : 1;
            union aarch64_vector_reg result = {0};
            for (unsigned lane = 0; lane < layouts[layout].width / (size * 8); lane++) {
                qword_t left, right, expected;
                if (layout < 6) {
                    left = UINT64_MAX;
                    right = lane + 2;
                    expected = 0 - right;
                } else {
                    // 1.5 与 -2 的精确运算，独立检查通道顺序和无异常路径。
                    left = size == 8 ? UINT64_C(0x3ff8000000000000) : UINT32_C(0x3fc00000);
                    right = size == 8 ? UINT64_C(0xc000000000000000) : UINT32_C(0xc0000000);
                    expected = layout < 9 ? (size == 8 ? UINT64_C(0xbfe0000000000000) : UINT32_C(0xbf000000)) :
                            layout < 12 ? (size == 8 ? UINT64_C(0x400c000000000000) : UINT32_C(0x40600000)) :
                            (size == 8 ? UINT64_C(0xc008000000000000) : UINT32_C(0xc0400000));
                }
                memcpy((byte_t *) &cpu.v[31] + lane * size, &left, size);
                memcpy((byte_t *) &cpu.v[1] + lane * size, &right, size);
                memcpy((byte_t *) &result + lane * size, &expected, size);
            }
            struct cpu_state expected = cpu;
            expected.pc += 4;
            expected.v[rd] = result;
            fp_test_instruction(layouts[layout].bits | 1 << 16 | 31 << 5 | rd, &cpu, &expected);
        }
    }
}

#if defined(__aarch64__)
static struct fp_native_result native_operation(unsigned layout,
        union aarch64_vector_reg left, union aarch64_vector_reg right,
        dword_t fpcr, dword_t initial_fpsr) {
    struct fp_native_result native = {0};
#define VECTOR_NATIVE(index, mnemonic, arrangement) \
    case index: FP_NATIVE_EXECUTE(mnemonic " v2." arrangement ", v0." arrangement ", v1." arrangement); break
    switch (layout) {
        VECTOR_NATIVE(0, "mul", "8b"); VECTOR_NATIVE(1, "mul", "16b");
        VECTOR_NATIVE(2, "mul", "4h"); VECTOR_NATIVE(3, "mul", "8h");
        VECTOR_NATIVE(4, "mul", "2s"); VECTOR_NATIVE(5, "mul", "4s");
        VECTOR_NATIVE(6, "fadd", "2s"); VECTOR_NATIVE(7, "fadd", "4s");
        VECTOR_NATIVE(8, "fadd", "2d"); VECTOR_NATIVE(9, "fsub", "2s");
        VECTOR_NATIVE(10, "fsub", "4s"); VECTOR_NATIVE(11, "fsub", "2d");
        VECTOR_NATIVE(12, "fmul", "2s"); VECTOR_NATIVE(13, "fmul", "4s");
        VECTOR_NATIVE(14, "fmul", "2d");
        default: assert(false);
    }
#undef VECTOR_NATIVE
    return native;
}

static void test_native_differential(void) {
    static const struct { dword_t single; qword_t double_precision; } values[] = {
        {0, 0}, {0x80000000, UINT64_C(0x8000000000000000)},
        {1, 1}, {0x80000001, UINT64_C(0x8000000000000001)},
        {0x007fffff, UINT64_C(0x000fffffffffffff)},
        {0x00800000, UINT64_C(0x0010000000000000)},
        {0x3f800000, UINT64_C(0x3ff0000000000000)},
        {0xbf800000, UINT64_C(0xbff0000000000000)},
        {0x33800000, UINT64_C(0x3ca0000000000000)},
        {0x7f7fffff, UINT64_C(0x7fefffffffffffff)},
        {0x7f800000, UINT64_C(0x7ff0000000000000)},
        {0xff800000, UINT64_C(0xfff0000000000000)},
        {0x7fc00123, UINT64_C(0x7ff8000000000123)},
        {0xffc00456, UINT64_C(0xfff8000000000456)},
        {0x7f800789, UINT64_C(0x7ff0000000000789)},
        {0xff800abc, UINT64_C(0xfff0000000000abc)},
    };
    qword_t seed = UINT64_C(0x135c794ab0ed628f);
    unsigned count = 0;
    for (unsigned config = 0; config < 16; config++) {
        for (unsigned layout = 0; layout < 15; layout++) {
            for (unsigned sample = 0; sample < 256 + 256; sample++) {
                struct cpu_state cpu = fp_test_cpu();
                cpu.fpcr = (config & 3) << AARCH64_FPCR_RMODE_SHIFT |
                        (config & 4 ? AARCH64_FPCR_FZ : 0) |
                        (config & 8 ? AARCH64_FPCR_DN : 0);
                cpu.fpsr |= sample & 1 ? AARCH64_FPSR_IOC : AARCH64_FPSR_DZC;
                unsigned size = layouts[layout].element_size;
                for (unsigned lane = 0; lane < 16 / size; lane++) {
                    unsigned li = (sample / 16 + lane) % 16;
                    unsigned ri = (sample % 16 + lane * 3) % 16;
                    qword_t left = sample < 256 ? (size == 8 ?
                            values[li].double_precision : values[li].single) : fp_test_random(&seed);
                    qword_t right = sample < 256 ? (size == 8 ?
                            values[ri].double_precision : values[ri].single) : fp_test_random(&seed);
                    memcpy((byte_t *) &cpu.v[31] + lane * size, &left, size);
                    memcpy((byte_t *) &cpu.v[1] + lane * size, &right, size);
                }
                struct fp_native_result native = native_operation(layout,
                        cpu.v[31], cpu.v[1], cpu.fpcr, cpu.fpsr);
                unsigned rd = sample % 3 == 0 ? 31 : sample % 3 == 1 ? 1 : 2;
                struct cpu_state expected = cpu;
                expected.pc += 4;
                expected.v[rd] = native.vector;
                expected.fpsr = native.fpsr;
                fp_test_instruction(layouts[layout].bits | 1 << 16 | 31 << 5 | rd, &cpu, &expected);
                count++;
            }
        }
    }
    printf("NEON 基础算术：%u 组原生 AArch64 对照通过\n", count);
}
#endif

int main(void) {
    test_decoding();
    test_fixed_results();
#if defined(__aarch64__)
    test_native_differential();
#endif
    return 0;
}
