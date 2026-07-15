#include <assert.h>

#include "guest/aarch64/threaded.h"

static void execute_c_fallback(struct cpu_state *cpu,
        struct guest_tlb *tlb,
        const struct aarch64_decoded *instruction,
        struct aarch64_execute_result *result) {
    *result = aarch64_execute(cpu, tlb, instruction);
}

static unsigned cache_index(guest_addr_t pc) {
    return (unsigned) ((pc >> 2) &
            (AARCH64_THREADED_CACHE_SIZE - 1));
}

bool aarch64_threaded_execute(struct aarch64_threaded_cache *cache,
        struct cpu_state *cpu, struct guest_tlb *tlb,
        guest_addr_t pc, dword_t word,
        struct aarch64_execute_result *result) {
    assert(cache != NULL);
    assert(cpu != NULL);
    assert(tlb != NULL);
    assert(result != NULL);
    assert((pc & 3) == 0);

    struct aarch64_threaded_cache_entry *entry =
            &cache->entries[cache_index(pc)];
    if (entry->valid && entry->pc == pc && entry->word == word) {
        cache->stats.cache_hits++;
    } else {
        cache->stats.cache_misses++;
        entry->valid = false;
        entry->pc = pc;
        entry->word = word;
        entry->handler = NULL;
        entry->c_fallback = false;

        struct aarch64_decoded decoded;
        if (aarch64_decode(word, &decoded)) {
            entry->decoded = decoded;
            entry->handler = execute_c_fallback;
            entry->c_fallback = true;
        }
        // valid 最后置位，保证命中的 token 已经完整初始化。
        entry->valid = true;
    }

    if (entry->handler == NULL)
        return false;
    *result = (struct aarch64_execute_result) {
        .stop = AARCH64_EXECUTE_RETIRED,
        .fault = {.kind = GUEST_MEMORY_FAULT_NONE},
    };
    if (entry->c_fallback)
        cache->stats.c_fallbacks++;
    entry->handler(cpu, tlb, &entry->decoded, result);
    return true;
}
