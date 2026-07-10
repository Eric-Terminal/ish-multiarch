#include <assert.h>

#include "misc.h"

int main(void) {
#if defined(ISH_GUEST_I386)
    static_assert(sizeof(guest_addr_t) == 4, "i386 guest 地址必须为 32 位");
    static_assert(sizeof(guest_long_t) == 4, "i386 guest long 必须为 32 位");
    assert(GUEST_VIRTUAL_ADDRESS_BITS == 32);
#elif defined(ISH_GUEST_AARCH64)
    static_assert(sizeof(guest_addr_t) == 8, "AArch64 guest 地址必须为 64 位");
    static_assert(sizeof(guest_long_t) == 8, "AArch64 guest long 必须为 64 位");
    assert(GUEST_VIRTUAL_ADDRESS_BITS == 48);
#endif
    return 0;
}
