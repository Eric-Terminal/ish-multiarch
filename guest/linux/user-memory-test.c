#include <assert.h>
#include <string.h>

#include "guest/linux/user-memory.h"
#include "guest/memory/page-table.h"

#define FIRST_PAGE UINT64_C(0x00007f1234567000)

int main(void) {
    struct guest_page_table table;
    assert(guest_page_table_init(&table, 48));
    byte_t *first;
    byte_t *second;
    assert(guest_page_table_map(&table, FIRST_PAGE,
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE, &first) ==
            GUEST_PAGE_TABLE_OK);
    assert(guest_page_table_map(&table,
            FIRST_PAGE + GUEST_MEMORY_PAGE_SIZE,
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE, &second) ==
            GUEST_PAGE_TABLE_OK);
    struct guest_tlb tlb;
    guest_tlb_init(&tlb, &table.address_space);

    byte_t source[96];
    for (size_t i = 0; i < sizeof(source); i++)
        source[i] = (byte_t) (i ^ 0x5a);
    guest_addr_t address = FIRST_PAGE + GUEST_MEMORY_PAGE_SIZE - 48;
    struct guest_memory_fault fault;
    assert(guest_linux_copy_to_user(
            &tlb, address, source, sizeof(source), &fault));
    assert(memcmp(first + GUEST_MEMORY_PAGE_SIZE - 48, source, 48) == 0);
    assert(memcmp(second, source + 48, 48) == 0);

    byte_t destination[sizeof(source)] = {0};
    assert(guest_linux_copy_from_user(
            &tlb, address, destination, sizeof(destination), &fault));
    assert(memcmp(destination, source, sizeof(source)) == 0);

    fault = (struct guest_memory_fault) {
        .kind = GUEST_MEMORY_FAULT_PERMISSION,
    };
    assert(guest_linux_copy_from_user(
            &tlb, UINT64_MAX, NULL, 0, &fault));
    assert(fault.kind == GUEST_MEMORY_FAULT_NONE);
    fault.kind = GUEST_MEMORY_FAULT_PERMISSION;
    assert(guest_linux_copy_to_user(
            &tlb, UINT64_MAX, NULL, 0, &fault));
    assert(fault.kind == GUEST_MEMORY_FAULT_NONE);

    memset(second + GUEST_MEMORY_PAGE_SIZE - 17, 0x7c, 17);
    memset(destination, 0, sizeof(destination));
    assert(!guest_linux_copy_from_user(&tlb,
            FIRST_PAGE + 2 * GUEST_MEMORY_PAGE_SIZE - 17,
            destination, 49, &fault));
    assert(fault.kind == GUEST_MEMORY_FAULT_UNMAPPED);
    assert(fault.address == FIRST_PAGE + 2 * GUEST_MEMORY_PAGE_SIZE);
    for (size_t i = 0; i < 17; i++)
        assert(destination[i] == 0x7c);

    memset(second + GUEST_MEMORY_PAGE_SIZE - 19, 0, 19);
    assert(!guest_linux_copy_to_user(&tlb,
            FIRST_PAGE + 2 * GUEST_MEMORY_PAGE_SIZE - 19,
            source, 51, &fault));
    assert(fault.kind == GUEST_MEMORY_FAULT_UNMAPPED);
    assert(fault.address == FIRST_PAGE + 2 * GUEST_MEMORY_PAGE_SIZE);
    assert(memcmp(second + GUEST_MEMORY_PAGE_SIZE - 19, source, 19) == 0);

    guest_page_table_destroy(&table);
    return 0;
}
