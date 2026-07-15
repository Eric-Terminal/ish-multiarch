#include <stddef.h>
#include <stdint.h>

#include "aarch64-backend-config.h"
#include "guest/aarch64/threaded.h"

#ifndef EXPECTED_APPLE_WORD_BYTES
#error "必须声明 Apple 宿主 ABI 的预期字宽"
#endif

#ifndef EXPECTED_AARCH64_THREADED_DEFAULT
#error "必须声明 AArch64 auto 后端的预期选择"
#endif

typedef void (*apple_expected_threaded_handler)(
        struct cpu_state *cpu, struct guest_tlb *tlb,
        const struct aarch64_decoded *instruction,
        struct aarch64_execute_result *result);

_Static_assert(__builtin_types_compatible_p(aarch64_threaded_handler,
                apple_expected_threaded_handler),
        "threaded handler 必须保留带类型的执行接口");
_Static_assert(__builtin_types_compatible_p(
                __typeof__(((struct aarch64_threaded_cache_entry *) 0)->handler),
                aarch64_threaded_handler),
        "threaded 缓存项必须保存带类型的 handler");
_Static_assert(sizeof(aarch64_threaded_handler) ==
                EXPECTED_APPLE_WORD_BYTES,
        "threaded handler 指针宽度与 Apple 切片不一致");
_Static_assert(sizeof(((struct aarch64_threaded_cache_entry *) 0)->handler) ==
                EXPECTED_APPLE_WORD_BYTES,
        "threaded 缓存项的函数指针宽度与 Apple 切片不一致");
_Static_assert(sizeof(struct aarch64_threaded_cache_entry) == 64,
        "threaded 缓存项必须保持为 64 字节");
_Static_assert(ISH_AARCH64_BACKEND_THREADED_DEFAULT ==
                EXPECTED_AARCH64_THREADED_DEFAULT,
        "编译器观察到的 AArch64 默认后端与门禁预期不一致");
_Static_assert(sizeof(void *) == EXPECTED_APPLE_WORD_BYTES,
        "Apple 数据指针宽度与目标切片不一致");
_Static_assert(sizeof(uintptr_t) == EXPECTED_APPLE_WORD_BYTES,
        "Apple uintptr_t 宽度与目标切片不一致");
_Static_assert(sizeof(long) == EXPECTED_APPLE_WORD_BYTES,
        "Apple long 宽度与目标切片不一致");
_Static_assert(sizeof(size_t) == EXPECTED_APPLE_WORD_BYTES,
        "Apple size_t 宽度与目标切片不一致");

#if EXPECTED_APPLE_WORD_BYTES == 4
#ifndef __ILP32__
#error "watchOS arm64_32 threaded 缓存必须使用 ILP32 ABI"
#endif
#ifdef __LP64__
#error "watchOS arm64_32 threaded 缓存不得使用 LP64 ABI"
#endif
_Static_assert(UINTPTR_MAX == UINT32_MAX,
        "ILP32 threaded 函数指针必须容纳于 32 位 uintptr_t");
#elif EXPECTED_APPLE_WORD_BYTES == 8
#ifdef __ILP32__
#error "64 位 Apple threaded 缓存不得使用 ILP32 ABI"
#endif
#ifndef __LP64__
#error "64 位 Apple threaded 缓存必须使用 LP64 ABI"
#endif
_Static_assert(UINTPTR_MAX == UINT64_MAX,
        "LP64 threaded 函数指针必须容纳于 64 位 uintptr_t");
#else
#error "Apple AArch64 后端门禁只支持 4 或 8 字节宿主字宽"
#endif
