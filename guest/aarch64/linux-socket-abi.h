#ifndef GUEST_AARCH64_LINUX_SOCKET_ABI_H
#define GUEST_AARCH64_LINUX_SOCKET_ABI_H

#include "misc.h"

struct aarch64_linux_user_msghdr {
    qword_t name;
    sdword_t name_length;
    dword_t name_padding;
    qword_t vectors;
    qword_t vector_count;
    qword_t control;
    qword_t control_length;
    dword_t flags;
    dword_t flags_padding;
} __attribute__((packed, aligned(8)));

_Static_assert(sizeof(struct aarch64_linux_user_msghdr) == 56 &&
        _Alignof(struct aarch64_linux_user_msghdr) == 8 &&
        __builtin_offsetof(struct aarch64_linux_user_msghdr, name) == 0 &&
        __builtin_offsetof(struct aarch64_linux_user_msghdr,
                name_length) == 8 &&
        __builtin_offsetof(struct aarch64_linux_user_msghdr,
                vectors) == 16 &&
        __builtin_offsetof(struct aarch64_linux_user_msghdr,
                vector_count) == 24 &&
        __builtin_offsetof(struct aarch64_linux_user_msghdr,
                control) == 32 &&
        __builtin_offsetof(struct aarch64_linux_user_msghdr,
                control_length) == 40 &&
        __builtin_offsetof(struct aarch64_linux_user_msghdr, flags) == 48,
        "AArch64 Linux msghdr ABI 必须固定为 56 字节 LP64 wire");

struct aarch64_linux_cmsghdr {
    qword_t length;
    sdword_t level;
    sdword_t type;
    byte_t data[];
} __attribute__((packed, aligned(8)));

_Static_assert(sizeof(struct aarch64_linux_cmsghdr) == 16 &&
        _Alignof(struct aarch64_linux_cmsghdr) == 8 &&
        __builtin_offsetof(struct aarch64_linux_cmsghdr, length) == 0 &&
        __builtin_offsetof(struct aarch64_linux_cmsghdr, level) == 8 &&
        __builtin_offsetof(struct aarch64_linux_cmsghdr, type) == 12 &&
        __builtin_offsetof(struct aarch64_linux_cmsghdr, data) == 16,
        "AArch64 Linux cmsghdr ABI 必须使用 64 位长度和 8 字节对齐");

#define AARCH64_LINUX_CMSG_ALIGNMENT UINT64_C(8)
#define AARCH64_LINUX_CMSG_ALIGN(length) \
    (((qword_t) (length) + AARCH64_LINUX_CMSG_ALIGNMENT - 1) & \
            ~(AARCH64_LINUX_CMSG_ALIGNMENT - 1))
#define AARCH64_LINUX_CMSG_LEN(payload_length) \
    (sizeof(struct aarch64_linux_cmsghdr) + (qword_t) (payload_length))
#define AARCH64_LINUX_CMSG_SPACE(payload_length) \
    (sizeof(struct aarch64_linux_cmsghdr) + \
            AARCH64_LINUX_CMSG_ALIGN(payload_length))

_Static_assert(AARCH64_LINUX_CMSG_LEN(sizeof(sdword_t)) == 20 &&
        AARCH64_LINUX_CMSG_SPACE(sizeof(sdword_t)) == 24,
        "AArch64 Linux 单 fd 控制消息必须占用 20/24 字节");

#define AARCH64_LINUX_MSG_CMSG_CLOEXEC UINT32_C(0x40000000)
#define AARCH64_LINUX_MSG_CMSG_COMPAT UINT32_C(0x80000000)

// ancillary 的 level/type 属于 guest wire，不能依赖 Apple SDK 的取值。
#define AARCH64_LINUX_SOL_IP INT32_C(0)
#define AARCH64_LINUX_SOL_IPV6 INT32_C(41)
#define AARCH64_LINUX_SOL_UDP INT32_C(17)

#define AARCH64_LINUX_IP_TOS INT32_C(1)
#define AARCH64_LINUX_IP_TTL INT32_C(2)
#define AARCH64_LINUX_IP_RETOPTS INT32_C(7)
#define AARCH64_LINUX_IP_PKTINFO INT32_C(8)
#define AARCH64_LINUX_IP_PROTOCOL INT32_C(52)

#define AARCH64_LINUX_IPV6_2292PKTINFO INT32_C(2)
#define AARCH64_LINUX_IPV6_2292HOPOPTS INT32_C(3)
#define AARCH64_LINUX_IPV6_2292DSTOPTS INT32_C(4)
#define AARCH64_LINUX_IPV6_2292RTHDR INT32_C(5)
#define AARCH64_LINUX_IPV6_2292HOPLIMIT INT32_C(8)
#define AARCH64_LINUX_IPV6_FLOWINFO INT32_C(11)
#define AARCH64_LINUX_IPV6_PKTINFO INT32_C(50)
#define AARCH64_LINUX_IPV6_HOPLIMIT INT32_C(52)
#define AARCH64_LINUX_IPV6_HOPOPTS INT32_C(54)
#define AARCH64_LINUX_IPV6_RTHDRDSTOPTS INT32_C(55)
#define AARCH64_LINUX_IPV6_RTHDR INT32_C(57)
#define AARCH64_LINUX_IPV6_DSTOPTS INT32_C(59)
#define AARCH64_LINUX_IPV6_DONTFRAG INT32_C(62)
#define AARCH64_LINUX_IPV6_TCLASS INT32_C(67)

#define AARCH64_LINUX_UDP_SEGMENT INT32_C(103)

#endif
