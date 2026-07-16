#include <assert.h>
#include <string.h>

#include "guest/linux/futex-abi.h"

int main(void) {
    const struct guest_linux_futex_waitv waiter = {
        .value = UINT64_C(0x0102030405060708),
        .address = UINT64_C(0x1112131415161718),
        .flags = UINT32_C(0x21222324),
        .reserved = UINT32_C(0x31323334),
    };
    static const byte_t expected_waiter[24] = {
        0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
        0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11,
        0x24, 0x23, 0x22, 0x21, 0x34, 0x33, 0x32, 0x31,
    };
    const struct guest_linux_kernel_timespec timeout = {
        .sec = INT64_C(0x0102030405060708),
        .nsec = -INT64_C(2),
    };
    static const byte_t expected_timeout[16] = {
        0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
        0xfe, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    };

    assert(memcmp(&waiter, expected_waiter,
            sizeof(expected_waiter)) == 0);
    assert(memcmp(&timeout, expected_timeout,
            sizeof(expected_timeout)) == 0);
    assert(GUEST_LINUX_FUTEX_WAITV_MAX == UINT32_C(128));
    assert(GUEST_LINUX_FUTEX2_SIZE_U32 == UINT32_C(2));
    assert(GUEST_LINUX_FUTEX_PRIVATE_FLAG == UINT32_C(128));
    assert(GUEST_LINUX_FUTEX_WAITV_SUPPORTED_FLAGS == UINT32_C(130));
    return 0;
}
