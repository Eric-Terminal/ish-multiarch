#ifndef GUEST_TARGET_TYPES_H
#define GUEST_TARGET_TYPES_H

#include "guest/selection.h"

#if defined(ISH_GUEST_I386)
typedef dword_t guest_addr_t;
typedef dword_t guest_ulong_t;
typedef sdword_t guest_long_t;
typedef dword_t guest_time_t;
typedef dword_t guest_clock_t;
#define GUEST_ABI_BITS 32
#define GUEST_VIRTUAL_ADDRESS_BITS 32
#elif defined(ISH_GUEST_AARCH64)
typedef qword_t guest_addr_t;
typedef qword_t guest_ulong_t;
typedef sqword_t guest_long_t;
typedef qword_t guest_time_t;
typedef qword_t guest_clock_t;
#define GUEST_ABI_BITS 64
#define GUEST_VIRTUAL_ADDRESS_BITS 48
#endif

_Static_assert(sizeof(guest_addr_t) * 8 == GUEST_ABI_BITS,
        "guest 地址类型与 ABI 宽度不一致");
_Static_assert(sizeof(guest_time_t) == sizeof(guest_long_t),
        "guest time_t 必须与 guest long 等宽");

#endif
