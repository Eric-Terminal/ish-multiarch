#include <assert.h>
#include <string.h>

#include "guest/aarch64/linux-futex-abi.h"

int main(void) {
    const struct aarch64_linux_robust_list_head head = {
        .next = UINT64_C(0x0102030405060708),
        .futex_offset = -INT64_C(2),
        .list_op_pending = UINT64_C(0x1112131415161718),
    };
    static const byte_t expected[24] = {
        0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
        0xfe, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11,
    };

    assert(memcmp(&head, expected, sizeof(expected)) == 0);
    assert(AARCH64_LINUX_FUTEX_WAITERS == UINT32_C(0x80000000));
    assert(AARCH64_LINUX_FUTEX_OWNER_DIED == UINT32_C(0x40000000));
    assert(AARCH64_LINUX_FUTEX_TID_MASK == UINT32_C(0x3fffffff));
    assert((AARCH64_LINUX_FUTEX_WAITERS |
            AARCH64_LINUX_FUTEX_OWNER_DIED |
            AARCH64_LINUX_FUTEX_TID_MASK) == UINT32_MAX);
    assert(AARCH64_LINUX_ROBUST_LIST_PI == UINT64_C(0x1));
    assert(AARCH64_LINUX_ROBUST_LIST_MODIFIER_MASK == UINT64_C(0x1));
    assert(AARCH64_LINUX_ROBUST_LIST_ADDRESS_MASK ==
            UINT64_C(0xfffffffffffffffe));
    assert((UINT64_C(0x123456789abcdef1) &
            AARCH64_LINUX_ROBUST_LIST_ADDRESS_MASK) ==
            UINT64_C(0x123456789abcdef0));
    assert(AARCH64_LINUX_ROBUST_LIST_LIMIT == UINT32_C(2048));
    return 0;
}
