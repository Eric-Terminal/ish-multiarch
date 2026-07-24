#ifndef GUEST_AARCH64_THREADED_H
#define GUEST_AARCH64_THREADED_H

#include "guest/aarch64/execute.h"

#ifndef ISH_AARCH64_THREADED_PROFILE
#define ISH_AARCH64_THREADED_PROFILE 0
#endif

#define AARCH64_THREADED_CACHE_BITS 6
#define AARCH64_THREADED_CACHE_SIZE \
    (1U << AARCH64_THREADED_CACHE_BITS)

typedef void (*aarch64_threaded_handler)(
        struct cpu_state *cpu, struct guest_tlb *tlb,
        const struct aarch64_decoded *instruction,
        struct aarch64_execute_result *result);

struct aarch64_threaded_cache_entry {
    guest_addr_t pc;
    dword_t word;
    bool valid;
    bool c_fallback;
    aarch64_threaded_handler handler;
    struct aarch64_decoded decoded;
};

struct aarch64_threaded_stats {
    qword_t cache_hits;
    qword_t cache_misses;
    qword_t fast_dispatches;
    qword_t c_fallbacks;
};

#if ISH_AARCH64_THREADED_PROFILE
struct aarch64_threaded_local_profile {
    qword_t undefined_dispatches;
    qword_t fallback_by_opcode[AARCH64_OP_COUNT];
    dword_t representative_word_by_opcode[AARCH64_OP_COUNT];
};
#endif

struct aarch64_threaded_cache {
    struct aarch64_threaded_cache_entry
            entries[AARCH64_THREADED_CACHE_SIZE];
    struct aarch64_threaded_stats stats;
#if ISH_AARCH64_THREADED_PROFILE
    struct aarch64_threaded_local_profile profile;
#endif
};

_Static_assert(sizeof(struct aarch64_threaded_cache_entry) == 64,
        "AArch64 threaded 缓存项必须保持为 64 字节");
_Static_assert((AARCH64_THREADED_CACHE_SIZE &
        (AARCH64_THREADED_CACHE_SIZE - 1)) == 0,
        "AArch64 threaded 缓存容量必须为二次幂");

bool aarch64_threaded_execute(struct aarch64_threaded_cache *cache,
        struct cpu_state *cpu, struct guest_tlb *tlb,
        guest_addr_t pc, dword_t word,
        struct aarch64_execute_result *result);

#endif
