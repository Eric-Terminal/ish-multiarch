#ifndef GUEST_AARCH64_THREADED_PROFILE_H
#define GUEST_AARCH64_THREADED_PROFILE_H

#ifndef ISH_AARCH64_THREADED_PROFILE
#define ISH_AARCH64_THREADED_PROFILE 0
#endif

#if ISH_AARCH64_THREADED_PROFILE
#include "guest/aarch64/decode.h"

struct aarch64_threaded_cache;

struct aarch64_threaded_profile_snapshot {
    qword_t cache_hits;
    qword_t cache_misses;
    qword_t fast_dispatches;
    qword_t c_fallbacks;
    qword_t undefined_dispatches;
    qword_t fallback_by_opcode[AARCH64_OP_COUNT];
    dword_t representative_word_by_opcode[AARCH64_OP_COUNT];
};

// 仅供没有并发执行或合并的测试边界清空全局聚合。
void aarch64_threaded_profile_reset_for_test(void);
void aarch64_threaded_profile_merge(
        const struct aarch64_threaded_cache *cache);
void aarch64_threaded_profile_snapshot(
        struct aarch64_threaded_profile_snapshot *snapshot);
void aarch64_threaded_profile_write_fd(int fd);
#endif

#endif
