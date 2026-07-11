#include <assert.h>

#include "misc.h"
#include "guest/aarch64/linux-file-abi.h"
#include "guest/linux/syscall-service.h"

int main(void) {
    static_assert(sizeof(struct guest_linux_user_fault) == 16,
            "syscall service 故障结构不能随 guest 地址宽度变化");
    static_assert(offsetof(struct guest_linux_user_fault, address) == 0 &&
            offsetof(struct guest_linux_user_fault, access) == 8 &&
            offsetof(struct guest_linux_user_fault, kind) == 12,
            "syscall service 故障结构偏移必须跨 guest 一致");
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
