#ifndef GUEST_AARCH64_FP_COMMON_TEST_H
#define GUEST_AARCH64_FP_COMMON_TEST_H

#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "guest/aarch64/threaded.h"

static inline struct cpu_state fp_test_cpu(void) {
    struct cpu_state cpu = {
        .pc = UINT64_C(0x600001051490), .sp = UINT64_C(0x1122334455667788),
        .nzcv = UINT32_C(0xa0000000), .fpsr = AARCH64_FPSR_QC,
        .tpidr_el0 = UINT64_C(0x1020304050607080), .cycle = 123,
    };
    for (unsigned i = 0; i < 31; i++)
        cpu.x[i] = UINT64_C(0xabcdef9876543210) ^ i;
    for (unsigned i = 0; i < 32; i++) {
        cpu.v[i].d[0] = UINT64_C(0x1122334455667788) ^ i;
        cpu.v[i].d[1] = UINT64_C(0x8877665544332211) ^ i;
    }
    return cpu;
}

static inline void fp_test_instruction(dword_t word,
        const struct cpu_state *initial, const struct cpu_state *expected) {
    struct aarch64_decoded instruction;
    assert(aarch64_decode(word, &instruction));
    struct guest_tlb tlb = {0};
    struct aarch64_threaded_cache cache = {0};
    // 同一预期分别约束 C、threaded 首次解码和命中缓存，不互相充当判据。
    for (unsigned backend = 0; backend < 3; backend++) {
        struct cpu_state cpu = *initial;
        struct aarch64_execute_result result;
        if (backend == 0)
            result = aarch64_execute(&cpu, &tlb, &instruction);
        else
            assert(aarch64_threaded_execute(&cache, &cpu, &tlb, cpu.pc, word, &result));
        assert(result.stop == AARCH64_EXECUTE_RETIRED);
        assert(result.fault.kind == GUEST_MEMORY_FAULT_NONE);
        if (memcmp(&cpu, expected, sizeof(cpu)) != 0) {
            fprintf(stderr, "指令 0x%08" PRIx32 " 后端 %u 状态不一致，FPCR=%08" PRIx32
                    "，FPSR=%08" PRIx32 "，预期=%08" PRIx32 "\n",
                    word, backend, initial->fpcr, cpu.fpsr, expected->fpsr);
            for (unsigned reg = 0; reg < 32; reg++) {
                if (memcmp(&cpu.v[reg], &expected->v[reg], sizeof(cpu.v[reg])) != 0)
                    fprintf(stderr, "V%u=%016" PRIx64 "%016" PRIx64
                            "，预期=%016" PRIx64 "%016" PRIx64 "\n", reg,
                            cpu.v[reg].d[1], cpu.v[reg].d[0],
                            expected->v[reg].d[1], expected->v[reg].d[0]);
                if (reg < 31 && cpu.x[reg] != expected->x[reg])
                    fprintf(stderr, "X%u=%016" PRIx64 "，预期=%016" PRIx64 "\n",
                            reg, cpu.x[reg], expected->x[reg]);
            }
            assert(false);
        }
    }
    assert(cache.stats.cache_misses == 1 && cache.stats.cache_hits == 1);
}

static inline qword_t fp_test_random(qword_t *seed) {
    *seed ^= *seed << 13;
    *seed ^= *seed >> 7;
    *seed ^= *seed << 17;
    return *seed;
}

#if defined(__aarch64__)
struct fp_native_result {
    union aarch64_vector_reg vector;
    qword_t integer;
    dword_t fpsr;
};

// 必须在同一个汇编块中恢复宿主环境；标量结果的高位也交由硬件给出。
#define FP_NATIVE_EXECUTE(instruction) do { \
    qword_t saved_fpcr, saved_fpsr, result_fpsr, integer; \
    __asm__ volatile( \
        "mrs %[saved_fpcr], fpcr\n" \
        "mrs %[saved_fpsr], fpsr\n" \
        "msr fpcr, %[fpcr]\n" \
        "msr fpsr, %[initial_fpsr]\n" \
        "ldr q0, [%[left]]\n" \
        "ldr q1, [%[right]]\n" \
        "movi v2.16b, #0xa5\n" \
        "mov %[integer], xzr\n" \
        instruction "\n" \
        "str q2, [%[output]]\n" \
        "mrs %[result_fpsr], fpsr\n" \
        "msr fpcr, %[saved_fpcr]\n" \
        "msr fpsr, %[saved_fpsr]\n" \
        : [saved_fpcr] "=&r" (saved_fpcr), [saved_fpsr] "=&r" (saved_fpsr), \
          [result_fpsr] "=&r" (result_fpsr), [integer] "=&r" (integer) \
        : [fpcr] "r" ((qword_t) fpcr), [initial_fpsr] "r" ((qword_t) initial_fpsr), \
          [left] "r" (&left), [right] "r" (&right), [output] "r" (&native.vector) \
        : "v0", "v1", "v2", "memory"); \
    native.fpsr = (dword_t) result_fpsr; \
    native.integer = integer; \
} while (0)
#endif

#endif
