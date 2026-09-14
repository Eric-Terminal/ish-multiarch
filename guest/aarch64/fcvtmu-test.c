#include <assert.h>
#include <string.h>

#include "guest/aarch64/threaded.h"

#define PRODUCT_PC UINT64_C(0x0000600001051490)
#define PRODUCT_WORD UINT32_C(0x9e710381)

static const struct {
    dword_t word;
    byte_t source_width;
    byte_t destination_width;
} conversions[] = {
    {UINT32_C(0x1e310000), 32, 32},
    {UINT32_C(0x1e710000), 64, 32},
    {UINT32_C(0x9e310000), 32, 64},
    {UINT32_C(0x9e710000), 64, 64},
};

static struct cpu_state initial_cpu(qword_t source, byte_t rn,
        dword_t fpcr, dword_t fpsr) {
    struct cpu_state cpu = {
        .pc = PRODUCT_PC,
        .sp = UINT64_C(0x1122334455667788),
        .cycle = 123,
        .nzcv = UINT32_C(0xa0000000),
        .fpcr = fpcr,
        .fpsr = fpsr,
    };
    for (unsigned reg = 0; reg < 31; reg++)
        cpu.x[reg] = UINT64_C(0xfeedfacecafebeef) ^ reg;
    for (unsigned reg = 0; reg < 32; reg++) {
        cpu.v[reg].d[0] = UINT64_C(0x0123456789abcdef) ^ reg;
        cpu.v[reg].d[1] = UINT64_C(0xfedcba9876543210) ^ reg;
    }
    cpu.v[rn].d[0] = source;
    return cpu;
}

static void assert_conversion(unsigned conversion, byte_t rn, byte_t rd,
        qword_t source, dword_t fpcr, dword_t initial_fpsr,
        qword_t value, dword_t fpsr) {
    dword_t word = conversions[conversion].word | (dword_t) rn << 5 | rd;
    struct aarch64_decoded instruction = {0};
    assert(aarch64_decode(word, &instruction));
    assert(instruction.opcode == AARCH64_OP_FCVTMU_GENERAL);
    assert(instruction.width == conversions[conversion].source_width);
    assert(instruction.operands.fp_to_integer.destination_width ==
            conversions[conversion].destination_width);
    assert(instruction.operands.fp_to_integer.rn == rn);
    assert(instruction.operands.fp_to_integer.rd == rd);

    struct cpu_state initial = initial_cpu(source, rn, fpcr, initial_fpsr);
    struct cpu_state expected = initial;
    expected.pc += 4;
    expected.fpsr = fpsr;
    if (rd != 31)
        expected.x[rd] = value;

    struct cpu_state cpu = initial;
    struct aarch64_execute_result result =
            aarch64_execute(&cpu, NULL, &instruction);
    assert(result.stop == AARCH64_EXECUTE_RETIRED);
    assert(result.fault.kind == GUEST_MEMORY_FAULT_NONE);
    assert(memcmp(&cpu, &expected, sizeof(cpu)) == 0);

    // 相同 PC/opcode 分别走首次解码和缓存命中，输入由执行时的寄存器提供。
    struct aarch64_threaded_cache cache = {0};
    struct guest_tlb tlb = {0};
    for (unsigned iteration = 0; iteration < 2; iteration++) {
        cpu = initial;
        assert(aarch64_threaded_execute(&cache, &cpu, &tlb,
                cpu.pc, word, &result));
        assert(result.stop == AARCH64_EXECUTE_RETIRED);
        assert(result.fault.kind == GUEST_MEMORY_FAULT_NONE);
        assert(memcmp(&cpu, &expected, sizeof(cpu)) == 0);
    }
    assert(cache.stats.cache_misses == 1);
    assert(cache.stats.cache_hits == 1);
    assert(cache.stats.c_fallbacks == 2);
}

static void test_rounding_and_special_values(void) {
    static const struct {
        dword_t single;
        qword_t double_precision;
        qword_t value;
        dword_t exceptions;
    } cases[] = {
        {0x00000000, UINT64_C(0x0000000000000000), 0, 0},
        {0x80000000, UINT64_C(0x8000000000000000), 0, 0},
        {0x3f800000, UINT64_C(0x3ff0000000000000), 1, 0},
        {0x41f00000, UINT64_C(0x403e000000000000), 30, 0},
        {0x41f60000, UINT64_C(0x403ec00000000000), 30, AARCH64_FPSR_IXC},
        {0x3f400000, UINT64_C(0x3fe8000000000000), 0, AARCH64_FPSR_IXC},
        {0xbf400000, UINT64_C(0xbfe8000000000000), 0, AARCH64_FPSR_IOC},
        {0xbf800000, UINT64_C(0xbff0000000000000), 0, AARCH64_FPSR_IOC},
        {0xbfc00000, UINT64_C(0xbff8000000000000), 0, AARCH64_FPSR_IOC},
        {0xff800000, UINT64_C(0xfff0000000000000), 0, AARCH64_FPSR_IOC},
        {0x7fc12345, UINT64_C(0x7ff8000000000001), 0, AARCH64_FPSR_IOC},
        {0x7f812345, UINT64_C(0x7ff0000000000001), 0, AARCH64_FPSR_IOC},
        {0xffc12345, UINT64_C(0xfff8000000000001), 0, AARCH64_FPSR_IOC},
        {0xff812345, UINT64_C(0xfff0000000000001), 0, AARCH64_FPSR_IOC},
        {0x00800000, UINT64_C(0x0010000000000000), 0, AARCH64_FPSR_IXC},
        {0x80800000, UINT64_C(0x8010000000000000), 0, AARCH64_FPSR_IOC},
    };
    for (unsigned conversion = 0; conversion < 4; conversion++) {
        for (unsigned mode = 0; mode < 4; mode++) {
            dword_t fpcr = mode << AARCH64_FPCR_RMODE_SHIFT;
            for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
                qword_t source = conversions[conversion].source_width == 32 ?
                        UINT64_C(0xa5a5a5a500000000) | cases[i].single :
                        cases[i].double_precision;
                dword_t initial_fpsr = AARCH64_FPSR_QC | AARCH64_FPSR_OFC;
                assert_conversion(conversion, 28, 1, source, fpcr,
                        initial_fpsr, cases[i].value,
                        initial_fpsr | cases[i].exceptions);
                assert_conversion(conversion, 31, 31, source,
                        fpcr | AARCH64_FPCR_DN | AARCH64_FPCR_FZ,
                        AARCH64_FPSR_IXC, cases[i].value,
                        AARCH64_FPSR_IXC | cases[i].exceptions);
            }
        }
    }
}

static void test_unsigned_boundaries(void) {
    static const struct {
        unsigned conversion;
        qword_t source;
        qword_t value;
        dword_t exceptions;
    } cases[] = {
        {0, UINT32_C(0x4f7fffff), UINT32_C(0xffffff00), 0},
        {0, UINT32_C(0x4f800000), UINT32_MAX, AARCH64_FPSR_IOC},
        {0, UINT32_C(0x7f800000), UINT32_MAX, AARCH64_FPSR_IOC},
        {1, UINT64_C(0x41efffffffe00000), UINT32_MAX, 0},
        {1, UINT64_C(0x41effffffff80000), UINT32_MAX, AARCH64_FPSR_IXC},
        {1, UINT64_C(0x41f0000000000000), UINT32_MAX, AARCH64_FPSR_IOC},
        {1, UINT64_C(0x7ff0000000000000), UINT32_MAX, AARCH64_FPSR_IOC},
        {2, UINT32_C(0x5f7fffff), UINT64_C(0xffffff0000000000), 0},
        {2, UINT32_C(0x5f800000), UINT64_MAX, AARCH64_FPSR_IOC},
        {2, UINT32_C(0x7f800000), UINT64_MAX, AARCH64_FPSR_IOC},
        {3, UINT64_C(0x43efffffffffffff), UINT64_C(0xfffffffffffff800), 0},
        {3, UINT64_C(0x43f0000000000000), UINT64_MAX, AARCH64_FPSR_IOC},
        {3, UINT64_C(0x7ff0000000000000), UINT64_MAX, AARCH64_FPSR_IOC},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        assert_conversion(cases[i].conversion, 5, 5, cases[i].source,
                0, AARCH64_FPSR_DZC, cases[i].value,
                AARCH64_FPSR_DZC | cases[i].exceptions);
    }
}

static void test_subnormal_flush(void) {
    for (unsigned conversion = 0; conversion < 4; conversion++) {
        bool single = conversions[conversion].source_width == 32;
        qword_t sign = single ? UINT32_C(0x80000000) :
                UINT64_C(0x8000000000000000);
        qword_t largest = single ? UINT32_C(0x007fffff) :
                UINT64_C(0x000fffffffffffff);
        const qword_t sources[] = {1, largest, sign | 1, sign | largest};
        for (unsigned i = 0; i < 4; i++) {
            assert_conversion(conversion, 31, 0, sources[i], 0, 0, 0,
                    i < 2 ? AARCH64_FPSR_IXC : AARCH64_FPSR_IOC);
            assert_conversion(conversion, 31, 0, sources[i],
                    AARCH64_FPCR_FZ, 0, 0, AARCH64_FPSR_IDC);
        }
    }
}

static void test_product_cached_input_changes(void) {
    struct aarch64_threaded_cache cache = {0};
    struct guest_tlb tlb = {0};
    struct aarch64_execute_result result;
    const qword_t sources[] = {
        UINT64_C(0x403ec00000000000), UINT64_C(0xbfe8000000000000),
    };
    for (unsigned i = 0; i < 2; i++) {
        struct cpu_state cpu = initial_cpu(sources[i], 28, 0, 0);
        assert(aarch64_threaded_execute(&cache, &cpu, &tlb,
                PRODUCT_PC, PRODUCT_WORD, &result));
        assert(result.stop == AARCH64_EXECUTE_RETIRED);
        assert(cpu.pc == PRODUCT_PC + 4);
        assert(cpu.x[1] == (i == 0 ? 30 : 0));
        assert(cpu.fpsr == (i == 0 ? AARCH64_FPSR_IXC : AARCH64_FPSR_IOC));
    }
    assert(cache.stats.cache_misses == 1);
    assert(cache.stats.cache_hits == 1);
}

#if defined(__aarch64__)
// 以宿主 ARM 指令为独立判据；同一个汇编块保存并恢复浮点环境，
// 防止客户机的 FZ、舍入模式或异常标志影响测试进程自身。
static qword_t native_conversion(unsigned conversion, qword_t source,
        dword_t fpcr, dword_t initial_fpsr, dword_t *fpsr) {
    qword_t value, saved_fpcr, saved_fpsr, result_fpsr;
#define NATIVE_CONVERSION(instruction) \
    __asm__ volatile( \
        "mrs %[saved_fpcr], fpcr\n" \
        "mrs %[saved_fpsr], fpsr\n" \
        "msr fpcr, %[fpcr]\n" \
        "msr fpsr, %[initial_fpsr]\n" \
        "fmov d0, %[source]\n" \
        instruction "\n" \
        "mrs %[result_fpsr], fpsr\n" \
        "msr fpcr, %[saved_fpcr]\n" \
        "msr fpsr, %[saved_fpsr]\n" \
        : [value] "=&r" (value), [saved_fpcr] "=&r" (saved_fpcr), \
          [saved_fpsr] "=&r" (saved_fpsr), \
          [result_fpsr] "=&r" (result_fpsr) \
        : [source] "r" (source), [fpcr] "r" ((qword_t) fpcr), \
          [initial_fpsr] "r" ((qword_t) initial_fpsr) \
        : "v0", "memory")
    switch (conversion) {
        case 0: NATIVE_CONVERSION("fcvtmu %w[value], s0"); break;
        case 1: NATIVE_CONVERSION("fcvtmu %w[value], d0"); break;
        case 2: NATIVE_CONVERSION("fcvtmu %[value], s0"); break;
        case 3: NATIVE_CONVERSION("fcvtmu %[value], d0"); break;
        default: assert(false); return 0;
    }
#undef NATIVE_CONVERSION
    *fpsr = (dword_t) result_fpsr;
    return value;
}

static void test_native_differential(void) {
    qword_t seed = UINT64_C(0x9e710381);
    for (unsigned conversion = 0; conversion < 4; conversion++) {
        for (unsigned mode = 0; mode < 16; mode++) {
            dword_t fpcr = (mode & 3) << AARCH64_FPCR_RMODE_SHIFT;
            if (mode & 4)
                fpcr |= AARCH64_FPCR_FZ;
            if (mode & 8)
                fpcr |= AARCH64_FPCR_DN;
            for (unsigned i = 0; i < 256; i++) {
                seed ^= seed << 13;
                seed ^= seed >> 7;
                seed ^= seed << 17;
                dword_t fpsr;
                qword_t value = native_conversion(conversion, seed,
                        fpcr, AARCH64_FPSR_QC, &fpsr);
                assert_conversion(conversion, 28, 1, seed,
                        fpcr, AARCH64_FPSR_QC, value, fpsr);
            }
        }
    }
}
#endif

int main(void) {
    test_rounding_and_special_values();
    test_unsigned_boundaries();
    test_subnormal_flush();
    test_product_cached_input_changes();
#if defined(__aarch64__)
    test_native_differential();
#endif
    return 0;
}
