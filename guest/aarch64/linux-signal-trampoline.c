#include <assert.h>
#include <string.h>

#include "guest/aarch64/linux-signal-trampoline.h"

// mov x8, #139; svc #0。显式字节序避免依赖 host 整数布局。
static const byte_t rt_sigreturn_code[] = {
    0x68, 0x11, 0x80, 0xd2,
    0x01, 0x00, 0x00, 0xd4,
};

_Static_assert(sizeof(rt_sigreturn_code) == 8,
        "AArch64 rt_sigreturn trampoline 必须保持两条指令");

enum guest_page_table_result aarch64_linux_map_signal_trampoline(
        struct guest_page_table *table, guest_addr_t page_base,
        guest_addr_t *entry) {
    assert(table != NULL && entry != NULL);
    // runtime 以 0 表示未配置默认 trampoline，不能把有效入口映射到零页。
    if (page_base == 0)
        return GUEST_PAGE_TABLE_INVALID_ADDRESS;
    byte_t *page;
    enum guest_page_table_result result = guest_page_table_map_special(
            table, page_base,
            GUEST_MEMORY_READ | GUEST_MEMORY_EXECUTE, &page);
    if (result != GUEST_PAGE_TABLE_OK)
        return result;
    memcpy(page, rt_sigreturn_code, sizeof(rt_sigreturn_code));
    *entry = page_base;
    return GUEST_PAGE_TABLE_OK;
}
