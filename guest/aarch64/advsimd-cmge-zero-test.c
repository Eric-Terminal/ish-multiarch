#include <assert.h>
#include <string.h>

#include "guest/aarch64/threaded.h"

static dword_t encode(bool q, byte_t size, byte_t rn, byte_t rd) {
    return UINT32_C(0x2e208800) | (dword_t) q << 30 |
            (dword_t) size << 22 | (dword_t) rn << 5 | rd;
}

static void test_decode(void) {
    for (unsigned q = 0; q < 2; q++) {
        for (unsigned size = 0; size < 4; size++) {
            for (unsigned rn = 0; rn < 32; rn++) {
                for (unsigned rd = 0; rd < 32; rd++) {
                    struct aarch64_decoded instruction;
                    bool decoded = aarch64_decode(encode(q, size, rn, rd),
                            &instruction);
                    assert(decoded == (q != 0 || size != 3));
                    if (!decoded)
                        continue;
                    assert(instruction.opcode ==
                            AARCH64_OP_ADVSIMD_CMGE_ZERO_VECTOR);
                    assert(instruction.width == (q ? 128 : 64));
                    assert(instruction.operands.advsimd_unary.element_size ==
                            (1U << size));
                    assert(instruction.operands.advsimd_unary.rn == rn);
                    assert(instruction.operands.advsimd_unary.rd == rd);
                }
            }
        }
    }

    // 固定编码位翻转后不能误入本指令族，包括相邻的 CMGT 与标量 CMGE。
    for (unsigned bit = 0; bit < 32; bit++) {
        if ((UINT32_C(0xbf3ffc00) & (UINT32_C(1) << bit)) == 0)
            continue;
        struct aarch64_decoded instruction;
        bool decoded = aarch64_decode(
                UINT32_C(0x6ee08820) ^ (UINT32_C(1) << bit), &instruction);
        assert(!decoded || instruction.opcode !=
                AARCH64_OP_ADVSIMD_CMGE_ZERO_VECTOR);
        use(decoded);
    }
}

static void check_execution(dword_t word, byte_t element_size,
        byte_t width, byte_t rn, byte_t rd, unsigned phase) {
    struct cpu_state initial;
    memset(&initial, 0, sizeof(initial));
    initial.pc = UINT64_C(0x0000555500813d84);
    initial.nzcv = UINT32_C(0xa0000000);
    initial.fpsr = UINT32_C(0x0800001f);
    initial.fpcr = UINT32_C(0x00c00000);
    initial.sp = UINT64_C(0x1234567890);
    initial.cycle = 17;
    for (unsigned reg = 0; reg < 31; reg++)
        initial.x[reg] = UINT64_C(0x8877665544332211) + reg;
    memset(initial.v, 0xa5, sizeof(initial.v));

    qword_t sign = UINT64_C(1) << (element_size * 8 - 1);
    qword_t mask = element_size == 8 ? UINT64_MAX : (sign << 1) - 1;
    const qword_t values[] = {0, 1, sign - 1, sign, sign + 1, mask};
    const bool nonnegative[] = {true, true, true, false, false, false};
    union aarch64_vector_reg expected_vector = {0};
    byte_t lanes = (byte_t) (width / (element_size * 8));
    for (unsigned lane = 0; lane < lanes; lane++) {
        unsigned sample = (lane + phase) % array_size(values);
        for (unsigned byte = 0; byte < element_size; byte++) {
            unsigned offset = lane * element_size + byte;
            initial.v[rn].b[offset] = (byte_t) (values[sample] >> (byte * 8));
            expected_vector.b[offset] = nonnegative[sample] ? 0xff : 0;
        }
    }
    struct cpu_state expected = initial;
    expected.v[rd] = expected_vector;
    expected.pc += 4;

    struct aarch64_decoded instruction;
    bool decoded = aarch64_decode(word, &instruction);
    assert(decoded);
    use(decoded);
    struct cpu_state cpu = initial;
    struct aarch64_execute_result result =
            aarch64_execute(&cpu, NULL, &instruction);
    assert(result.stop == AARCH64_EXECUTE_RETIRED);
    assert(memcmp(&cpu, &expected, sizeof(cpu)) == 0);

    // 覆盖报告使用的 threaded 后端，以及再次执行同一 PC 时的缓存命中。
    struct aarch64_threaded_cache cache = {0};
    struct guest_tlb tlb = {0};
    for (unsigned iteration = 0; iteration < 2; iteration++) {
        cpu = initial;
        bool executed = aarch64_threaded_execute(&cache, &cpu, &tlb,
                cpu.pc, word, &result);
        assert(executed && result.stop == AARCH64_EXECUTE_RETIRED);
        assert(memcmp(&cpu, &expected, sizeof(cpu)) == 0);
        use(executed);
    }
    assert(cache.stats.cache_misses == 1 && cache.stats.cache_hits == 1);
}

int main(void) {
    test_decode();
    for (unsigned phase = 0; phase < 6; phase++) {
        // #152/#153 原始 opcode；源目标同为 V0，Q=0 必须清除高 64 位。
        check_execution(UINT32_C(0x2e208800), 1, 64, 0, 0, phase);
        for (unsigned q = 0; q < 2; q++) {
            for (unsigned size = 0; size < 4; size++) {
                if (!q && size == 3)
                    continue;
                for (unsigned alias = 0; alias < 2; alias++) {
                    byte_t rn = alias ? 31 : 7;
                    check_execution(encode(q, size, rn, 31),
                            1U << size, q ? 128 : 64, rn, 31, phase);
                }
            }
        }
    }
    return 0;
}
