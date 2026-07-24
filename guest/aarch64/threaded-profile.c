#include "guest/aarch64/threaded-profile.h"

#if ISH_AARCH64_THREADED_PROFILE
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdatomic.h>

#include "guest/aarch64/backend.h"
#include "guest/aarch64/threaded.h"

static atomic_flag profile_lock = ATOMIC_FLAG_INIT;
static struct aarch64_threaded_profile_snapshot aggregate;

static void profile_lock_acquire(void) {
    while (atomic_flag_test_and_set_explicit(
            &profile_lock, memory_order_acquire)) {
    }
}

static void profile_lock_release(void) {
    atomic_flag_clear_explicit(&profile_lock, memory_order_release);
}

void aarch64_threaded_profile_reset_for_test(void) {
    profile_lock_acquire();
    aggregate = (struct aarch64_threaded_profile_snapshot) {0};
    profile_lock_release();
}

void aarch64_threaded_profile_merge(
        const struct aarch64_threaded_cache *cache) {
    assert(cache != NULL);
    profile_lock_acquire();
    aggregate.cache_hits += cache->stats.cache_hits;
    aggregate.cache_misses += cache->stats.cache_misses;
    aggregate.fast_dispatches += cache->stats.fast_dispatches;
    aggregate.c_fallbacks += cache->stats.c_fallbacks;
    aggregate.undefined_dispatches +=
            cache->profile.undefined_dispatches;
    for (enum aarch64_opcode opcode = 0;
            opcode < AARCH64_OP_COUNT; opcode++) {
        qword_t count = cache->profile.fallback_by_opcode[opcode];
        if (count == 0)
            continue;
        if (aggregate.fallback_by_opcode[opcode] == 0) {
            aggregate.representative_word_by_opcode[opcode] =
                    cache->profile.representative_word_by_opcode[opcode];
        }
        aggregate.fallback_by_opcode[opcode] += count;
    }
    profile_lock_release();
}

void aarch64_threaded_profile_snapshot(
        struct aarch64_threaded_profile_snapshot *snapshot) {
    assert(snapshot != NULL);
    profile_lock_acquire();
    *snapshot = aggregate;
    profile_lock_release();
}

void aarch64_threaded_profile_write_fd(int fd) {
    assert(fd >= 0);
    struct aarch64_threaded_profile_snapshot snapshot;
    aarch64_threaded_profile_snapshot(&snapshot);
    const char *backend =
            aarch64_backend_default() == AARCH64_BACKEND_THREADED ?
            "threaded" : "c";

    dprintf(fd, "AARCH64_THREADED_PROFILE\tversion\t1\n");
    dprintf(fd, "AARCH64_THREADED_PROFILE\tbackend\t%s\n", backend);
    dprintf(fd,
            "AARCH64_THREADED_PROFILE\ttotals"
            "\tcache_hits\t%" PRIu64
            "\tcache_misses\t%" PRIu64
            "\tfast_dispatches\t%" PRIu64
            "\tc_fallbacks\t%" PRIu64
            "\tundefined_dispatches\t%" PRIu64 "\n",
            snapshot.cache_hits,
            snapshot.cache_misses,
            snapshot.fast_dispatches,
            snapshot.c_fallbacks,
            snapshot.undefined_dispatches);
    for (enum aarch64_opcode opcode = 0;
            opcode < AARCH64_OP_COUNT; opcode++) {
        if (snapshot.fallback_by_opcode[opcode] == 0)
            continue;
        dprintf(fd,
                "AARCH64_THREADED_PROFILE\topcode\t%u"
                "\tcount\t%" PRIu64
                "\trepresentative_word\t0x%08" PRIx32 "\n",
                (unsigned) opcode,
                snapshot.fallback_by_opcode[opcode],
                snapshot.representative_word_by_opcode[opcode]);
    }
}
#endif
