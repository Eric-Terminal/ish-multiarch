#include <assert.h>
#include "guest/linux/memory.h"
#include "guest/memory/tlb.h"

#define BRK_BASE UINT64_C(0x0000600000000000)
#define BRK_LIMIT (BRK_BASE + 3 * GUEST_MEMORY_PAGE_SIZE)
#define TOP_PAGE ((UINT64_C(1) << 48) - GUEST_MEMORY_PAGE_SIZE)

static void assert_not_mapped(struct guest_page_table *table,
        guest_addr_t page) {
    byte_t *host_page;
    unsigned permissions;
    assert(guest_page_table_lookup(table, page,
            &host_page, &permissions) == GUEST_PAGE_TABLE_NOT_MAPPED);
}

static void test_growth_and_shrink(void) {
    struct guest_page_table table;
    assert(guest_page_table_init(&table, 48));
    struct guest_linux_mm memory;
    guest_linux_mm_init(&memory, &table, BRK_BASE, BRK_LIMIT);
    assert(guest_linux_brk(&memory, 0) == BRK_BASE);
    assert(guest_linux_brk(&memory, BRK_BASE - 1) == BRK_BASE);

    qword_t generation = table.address_space.generation;
    assert(guest_linux_brk(&memory, BRK_LIMIT + 1) == BRK_BASE);
    assert(table.address_space.generation == generation);
    assert_not_mapped(&table, BRK_BASE);

    generation = table.address_space.generation;
    assert(guest_linux_brk(&memory, BRK_BASE + 64) == BRK_BASE + 64);
    assert(table.address_space.generation == generation + 1);
    byte_t *first_page;
    unsigned permissions;
    assert(guest_page_table_lookup(&table, BRK_BASE,
            &first_page, &permissions) == GUEST_PAGE_TABLE_OK);
    assert(permissions == (GUEST_MEMORY_READ | GUEST_MEMORY_WRITE));
    for (size_t i = 0; i < GUEST_MEMORY_PAGE_SIZE; i++)
        assert(first_page[i] == 0);

    first_page[0] = 0x5a;
    generation = table.address_space.generation;
    assert(guest_linux_brk(&memory, BRK_BASE + 128) == BRK_BASE + 128);
    assert(table.address_space.generation == generation);
    assert(first_page[0] == 0x5a);

    assert(guest_linux_brk(&memory,
            BRK_BASE + GUEST_MEMORY_PAGE_SIZE + 32) ==
            BRK_BASE + GUEST_MEMORY_PAGE_SIZE + 32);
    byte_t *second_page;
    assert(guest_page_table_lookup(&table,
            BRK_BASE + GUEST_MEMORY_PAGE_SIZE,
            &second_page, &permissions) == GUEST_PAGE_TABLE_OK);
    for (size_t i = 0; i < GUEST_MEMORY_PAGE_SIZE; i++)
        assert(second_page[i] == 0);

    struct guest_tlb tlb;
    guest_tlb_init(&tlb, &table.address_space);
    struct guest_memory_fault fault;
    byte_t value = 0x33;
    assert(guest_tlb_write(&tlb, BRK_BASE + GUEST_MEMORY_PAGE_SIZE,
            &value, 1, &fault));
    assert(guest_linux_brk(&memory,
            BRK_BASE + GUEST_MEMORY_PAGE_SIZE + 1) ==
            BRK_BASE + GUEST_MEMORY_PAGE_SIZE + 1);
    assert(guest_linux_brk(&memory,
            BRK_BASE + GUEST_MEMORY_PAGE_SIZE) ==
            BRK_BASE + GUEST_MEMORY_PAGE_SIZE);
    assert(!guest_tlb_read(&tlb, BRK_BASE + GUEST_MEMORY_PAGE_SIZE,
            &value, 1, GUEST_MEMORY_READ, &fault));
    assert(fault.kind == GUEST_MEMORY_FAULT_UNMAPPED);

    assert(guest_linux_brk(&memory, BRK_BASE) == BRK_BASE);
    assert_not_mapped(&table, BRK_BASE);
    guest_page_table_destroy(&table);
}

static void test_conflict_rollback(void) {
    struct guest_page_table table;
    assert(guest_page_table_init(&table, 48));
    struct guest_linux_mm memory;
    guest_linux_mm_init(&memory, &table, BRK_BASE, BRK_LIMIT);

    byte_t *occupied;
    assert(guest_page_table_map(&table,
            BRK_BASE + GUEST_MEMORY_PAGE_SIZE,
            GUEST_MEMORY_READ, &occupied) == GUEST_PAGE_TABLE_OK);
    occupied[0] = 0xa5;
    assert(guest_linux_brk(&memory,
            BRK_BASE + GUEST_MEMORY_PAGE_SIZE + 1) == BRK_BASE);
    assert(memory.brk == BRK_BASE);
    assert_not_mapped(&table, BRK_BASE);

    byte_t *same_page;
    unsigned permissions;
    assert(guest_page_table_lookup(&table,
            BRK_BASE + GUEST_MEMORY_PAGE_SIZE,
            &same_page, &permissions) == GUEST_PAGE_TABLE_OK);
    assert(same_page == occupied);
    assert(same_page[0] == 0xa5);
    assert(permissions == GUEST_MEMORY_READ);
    guest_page_table_destroy(&table);
}

static void test_address_limit(void) {
    struct guest_page_table table;
    assert(guest_page_table_init(&table, 48));
    struct guest_linux_mm memory;
    guest_linux_mm_init(&memory, &table, TOP_PAGE,
            (UINT64_C(1) << 48) - 1);
    assert(guest_linux_brk(&memory, TOP_PAGE + 1) == TOP_PAGE + 1);
    assert(guest_linux_brk(&memory, UINT64_C(1) << 48) == TOP_PAGE + 1);
    assert(guest_linux_brk(&memory, UINT64_MAX) == TOP_PAGE + 1);
    assert(guest_linux_brk(&memory, TOP_PAGE) == TOP_PAGE);
    assert_not_mapped(&table, TOP_PAGE);
    guest_page_table_destroy(&table);
}

int main(void) {
    test_growth_and_shrink();
    test_conflict_rollback();
    test_address_limit();
    return 0;
}
