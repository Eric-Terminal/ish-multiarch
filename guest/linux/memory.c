#include <assert.h>

#include "guest/linux/memory.h"

static bool page_end(guest_addr_t address, guest_addr_t *end) {
    if (address > UINT64_MAX - GUEST_MEMORY_PAGE_MASK)
        return false;
    *end = (address + GUEST_MEMORY_PAGE_MASK) & ~GUEST_MEMORY_PAGE_MASK;
    return true;
}

void guest_linux_mm_init(struct guest_linux_mm *memory,
        struct guest_page_table *page_table, guest_addr_t start_brk,
        guest_addr_t brk_limit) {
    assert(memory != NULL && page_table != NULL);
    assert((start_brk & GUEST_MEMORY_PAGE_MASK) == 0);
    assert(brk_limit >= start_brk);
    assert(guest_address_space_contains(&page_table->address_space,
            start_brk, 0) &&
            guest_address_space_contains(&page_table->address_space,
                    brk_limit, 0));
    *memory = (struct guest_linux_mm) {
        .page_table = page_table,
        .start_brk = start_brk,
        .brk = start_brk,
        .brk_limit = brk_limit,
    };
}

static void rollback_growth(struct guest_linux_mm *memory,
        guest_addr_t first, guest_addr_t end) {
    for (guest_addr_t page = first; page < end;
            page += GUEST_MEMORY_PAGE_SIZE)
        assert(guest_page_table_unmap(memory->page_table, page) ==
                GUEST_PAGE_TABLE_OK);
}

guest_addr_t guest_linux_brk(struct guest_linux_mm *memory,
        guest_addr_t requested) {
    if (requested == 0)
        return memory->brk;
    if (requested < memory->start_brk || requested > memory->brk_limit ||
            !guest_address_space_contains(
                    &memory->page_table->address_space, requested, 0))
        return memory->brk;

    guest_addr_t old_end;
    guest_addr_t new_end;
    if (!page_end(memory->brk, &old_end) ||
            !page_end(requested, &new_end))
        return memory->brk;

    if (new_end > old_end) {
        guest_addr_t page = old_end;
        for (; page < new_end; page += GUEST_MEMORY_PAGE_SIZE) {
            byte_t *host_page;
            enum guest_page_table_result result = guest_page_table_map(
                    memory->page_table, page,
                    GUEST_MEMORY_READ | GUEST_MEMORY_WRITE, &host_page);
            if (result != GUEST_PAGE_TABLE_OK) {
                rollback_growth(memory, old_end, page);
                return memory->brk;
            }
        }
    } else if (new_end < old_end) {
        for (guest_addr_t page = new_end; page < old_end;
                page += GUEST_MEMORY_PAGE_SIZE)
            assert(guest_page_table_unmap(memory->page_table, page) ==
                    GUEST_PAGE_TABLE_OK);
    }

    memory->brk = requested;
    return memory->brk;
}
