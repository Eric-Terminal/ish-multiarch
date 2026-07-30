#ifndef ISH_APPLE_DEFINES_H
#define ISH_APPLE_DEFINES_H

#include <stdint.h>

#include "iSHAppleLinuxErrno.h"

#define ISH_APPLE_ABI_VERSION UINT32_C(1)

#if defined(__GNUC__)
#define ISH_APPLE_API __attribute__((visibility("default")))
#else
#define ISH_APPLE_API
#endif

#if defined(__clang__) && __has_feature(nullability)
#define ISH_APPLE_NULLABLE _Nullable
#define ISH_APPLE_NONNULL _Nonnull
#else
#define ISH_APPLE_NULLABLE
#define ISH_APPLE_NONNULL
#endif

#ifdef __cplusplus
#define ISH_APPLE_EXTERN_C_BEGIN extern "C" {
#define ISH_APPLE_EXTERN_C_END }
#else
#define ISH_APPLE_EXTERN_C_BEGIN
#define ISH_APPLE_EXTERN_C_END
#endif

#endif
