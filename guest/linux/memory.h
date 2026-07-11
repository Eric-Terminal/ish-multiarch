#ifndef GUEST_LINUX_MEMORY_H
#define GUEST_LINUX_MEMORY_H

#include "guest/memory/page-table.h"

struct guest_linux_mm {
    struct guest_page_table *page_table;
    guest_addr_t start_brk;
    guest_addr_t brk;
    guest_addr_t brk_limit;
};

void guest_linux_mm_init(struct guest_linux_mm *memory,
        struct guest_page_table *page_table, guest_addr_t start_brk,
        guest_addr_t brk_limit);
guest_addr_t guest_linux_brk(struct guest_linux_mm *memory,
        guest_addr_t requested);

#endif
