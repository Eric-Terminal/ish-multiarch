#include <assert.h>
#include <string.h>

#include "guest/memory/page-table.h"
#include "guest/memory/tlb.h"

#define HIGH_PAGE UINT64_C(0x0000123456789000)
#define NEXT_PAGE (HIGH_PAGE + GUEST_MEMORY_PAGE_SIZE)

int main(void) {
    struct guest_page_table table;
    assert(guest_page_table_init(&table, 48));
    assert(table.address_space.generation == 1);

    byte_t *first;
    assert(guest_page_table_map(&table, HIGH_PAGE,
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE, &first) ==
            GUEST_PAGE_TABLE_OK);
    assert(table.address_space.generation == 2);
    for (size_t i = 0; i < GUEST_MEMORY_PAGE_SIZE; i++)
        assert(first[i] == 0);

    byte_t *duplicate;
    assert(guest_page_table_map(&table, HIGH_PAGE,
            GUEST_MEMORY_READ, &duplicate) ==
            GUEST_PAGE_TABLE_ALREADY_MAPPED);
    assert(duplicate == first);
    unsigned permissions;
    assert(guest_page_table_lookup(&table, HIGH_PAGE,
            &duplicate, &permissions) == GUEST_PAGE_TABLE_OK);
    assert(duplicate == first);
    assert(permissions == (GUEST_MEMORY_READ | GUEST_MEMORY_WRITE));
    assert(table.address_space.generation == 2);

    byte_t *next;
    assert(guest_page_table_map(&table, NEXT_PAGE,
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE, &next) ==
            GUEST_PAGE_TABLE_OK);
    struct guest_tlb tlb;
    guest_tlb_init(&tlb, &table.address_space);
    struct guest_memory_fault fault;
    const byte_t cross_page[] = {0x10, 0x20, 0x30, 0x40};
    byte_t output[sizeof(cross_page)] = {0};
    assert(guest_tlb_write(&tlb, NEXT_PAGE - 2,
            cross_page, sizeof(cross_page), &fault));
    assert(guest_tlb_read(&tlb, NEXT_PAGE - 2,
            output, sizeof(output), GUEST_MEMORY_READ, &fault));
    assert(memcmp(output, cross_page, sizeof(output)) == 0);

    assert(guest_page_table_protect(&table, NEXT_PAGE,
            GUEST_MEMORY_READ) == GUEST_PAGE_TABLE_OK);
    const byte_t first_before[] = {first[GUEST_MEMORY_PAGE_SIZE - 2],
            first[GUEST_MEMORY_PAGE_SIZE - 1]};
    assert(!guest_tlb_write(&tlb, NEXT_PAGE - 2,
            "abcd", 4, &fault));
    assert(fault.kind == GUEST_MEMORY_FAULT_PERMISSION);
    assert(memcmp(&first[GUEST_MEMORY_PAGE_SIZE - 2],
            first_before, sizeof(first_before)) == 0);

    assert(guest_page_table_unmap(&table, HIGH_PAGE) ==
            GUEST_PAGE_TABLE_OK);
    assert(guest_page_table_lookup(&table, HIGH_PAGE,
            &duplicate, &permissions) == GUEST_PAGE_TABLE_NOT_MAPPED);
    assert(!guest_tlb_read(&tlb, HIGH_PAGE, output, 1,
            GUEST_MEMORY_READ, &fault));
    assert(fault.kind == GUEST_MEMORY_FAULT_UNMAPPED);
    assert(guest_page_table_unmap(&table, HIGH_PAGE) ==
            GUEST_PAGE_TABLE_NOT_MAPPED);

    guest_addr_t last_page = (UINT64_C(1) << 48) -
            GUEST_MEMORY_PAGE_SIZE;
    byte_t *last;
    assert(guest_page_table_map(&table, last_page,
            GUEST_MEMORY_READ, &last) == GUEST_PAGE_TABLE_OK);
    last[GUEST_MEMORY_PAGE_SIZE - 1] = 0x5a;
    assert(guest_tlb_read(&tlb, (UINT64_C(1) << 48) - 1,
            output, 1, GUEST_MEMORY_READ, &fault));
    assert(output[0] == 0x5a);

    assert(guest_page_table_map(&table, UINT64_C(1) << 48,
            GUEST_MEMORY_READ, &last) ==
            GUEST_PAGE_TABLE_INVALID_ADDRESS);
    assert(guest_page_table_map(&table, NEXT_PAGE + 1,
            GUEST_MEMORY_READ, &last) ==
            GUEST_PAGE_TABLE_INVALID_ADDRESS);
    assert(guest_page_table_protect(&table,
            NEXT_PAGE + GUEST_MEMORY_PAGE_SIZE,
            GUEST_MEMORY_READ) == GUEST_PAGE_TABLE_NOT_MAPPED);

    guest_page_table_destroy(&table);
    assert(table.root == NULL);
    return 0;
}
