#ifndef GUEST_AARCH64_LINUX_MEMORY_H
#define GUEST_AARCH64_LINUX_MEMORY_H

#include "guest/memory/page-table.h"

struct aarch64_linux_mm {
    struct guest_page_table *page_table;
    guest_addr_t start_brk;
    guest_addr_t brk;
    guest_addr_t brk_limit;
};

void aarch64_linux_mm_init(struct aarch64_linux_mm *memory,
        struct guest_page_table *page_table, guest_addr_t start_brk,
        guest_addr_t brk_limit);
guest_addr_t aarch64_linux_brk(struct aarch64_linux_mm *memory,
        guest_addr_t requested);

#endif
