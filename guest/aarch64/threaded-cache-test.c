#include <assert.h>
#include <string.h>

#include "guest/aarch64/threaded.h"

#define FIRST_PC UINT64_C(0x0000456789abc100)
#define COLLISION_STRIDE (AARCH64_THREADED_CACHE_SIZE * UINT64_C(4))
#define NOP UINT32_C(0xd503201f)
#define ADD_X0 UINT32_C(0x91000400)

static void test_conflicting_hot_code(void) {
    struct aarch64_threaded_cache cache = {0};
    struct cpu_state cpu = {0};
    struct guest_tlb tlb = {0};
    struct aarch64_execute_result result;
    for (unsigned iteration = 0; iteration < 100; iteration++) {
        for (unsigned way = 0; way < AARCH64_THREADED_CACHE_WAYS; way++) {
            cpu.pc = FIRST_PC + way * COLLISION_STRIDE;
            assert(aarch64_threaded_execute(&cache, &cpu, &tlb,
                    cpu.pc, ADD_X0, &result));
            assert(result.stop == AARCH64_EXECUTE_RETIRED);
        }
    }
    assert(cpu.x[0] == 100 * AARCH64_THREADED_CACHE_WAYS);
    assert(cache.stats.cache_misses == AARCH64_THREADED_CACHE_WAYS);
    assert(cache.stats.cache_hits == 99 * AARCH64_THREADED_CACHE_WAYS);

    // 同一 PC 改写后应原位重解码，不挤掉同槽位的其他热指令。
    cpu.pc = FIRST_PC;
    assert(aarch64_threaded_execute(&cache, &cpu, &tlb,
            cpu.pc, NOP, &result));
    qword_t misses = cache.stats.cache_misses;
    for (unsigned way = 0; way < AARCH64_THREADED_CACHE_WAYS; way++) {
        cpu.pc = FIRST_PC + way * COLLISION_STRIDE;
        assert(aarch64_threaded_execute(&cache, &cpu, &tlb,
                cpu.pc, way == 0 ? NOP : ADD_X0, &result));
    }
    assert(cache.stats.cache_misses == misses);
}

static void test_replacement_and_undefined_rewrite(void) {
    struct aarch64_threaded_cache cache = {0};
    struct cpu_state cpu = {0};
    struct guest_tlb tlb = {0};
    struct aarch64_execute_result result;
    for (unsigned way = 0; way <= AARCH64_THREADED_CACHE_WAYS; way++) {
        cpu.pc = FIRST_PC + way * COLLISION_STRIDE;
        assert(aarch64_threaded_execute(&cache, &cpu, &tlb,
                cpu.pc, NOP, &result));
    }
    qword_t misses = cache.stats.cache_misses;
    // 第五个冲突项只淘汰一个槽位，剩下三项仍可命中。
    for (unsigned way = 1; way < AARCH64_THREADED_CACHE_WAYS; way++) {
        cpu.pc = FIRST_PC + way * COLLISION_STRIDE;
        assert(aarch64_threaded_execute(&cache, &cpu, &tlb,
                cpu.pc, NOP, &result));
    }
    assert(cache.stats.cache_misses == misses);
    cpu.pc = FIRST_PC;
    assert(aarch64_threaded_execute(&cache, &cpu, &tlb,
            cpu.pc, NOP, &result));
    assert(cache.stats.cache_misses == misses + 1);

    cpu.pc = FIRST_PC;
    struct cpu_state before = cpu;
    assert(!aarch64_threaded_execute(&cache, &cpu, &tlb,
            cpu.pc, 0, &result));
    assert(memcmp(&before, &cpu, sizeof(cpu)) == 0);
    assert(aarch64_threaded_execute(&cache, &cpu, &tlb,
            cpu.pc, ADD_X0, &result));
    assert(cpu.x[0] == before.x[0] + 1);
}

int main(void) {
    test_conflicting_hot_code();
    test_replacement_and_undefined_rewrite();
    return 0;
}
