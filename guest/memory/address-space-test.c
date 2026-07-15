#include <assert.h>
#include <stdint.h>

#include "guest/memory/address-space.h"

#define TEST_PAGE UINT64_C(0x0000123456789000)

struct test_mapping {
    byte_t page[GUEST_MEMORY_PAGE_SIZE];
    unsigned calls;
    guest_addr_t last_page;
    enum guest_memory_access last_access;
};

static enum guest_memory_fault_kind resolve_test_page(void *opaque,
        guest_addr_t page_base, enum guest_memory_access access,
        struct guest_page_view *view) {
    struct test_mapping *mapping = opaque;
    mapping->calls++;
    mapping->last_page = page_base;
    mapping->last_access = access;
    if (page_base != TEST_PAGE)
        return GUEST_MEMORY_FAULT_UNMAPPED;
    view->host_page = mapping->page;
    view->permissions = GUEST_MEMORY_READ | GUEST_MEMORY_EXECUTE;
    return GUEST_MEMORY_FAULT_NONE;
}

static const struct guest_address_space_ops test_ops = {
    .resolve_page = resolve_test_page,
};

int main(void) {
    struct test_mapping mapping = {0};
    struct guest_address_space space;
    struct guest_page_sync stale_sync = {0};
    struct guest_page_view view = {.sync = &stale_sync};

    guest_address_space_init(&space, &test_ops, &mapping, 48);
    assert(space.generation == 1);
    assert(space.exclusive_sequence == 0);
    assert(guest_address_space_contains(&space, TEST_PAGE,
            GUEST_MEMORY_PAGE_SIZE));
    assert(guest_address_space_contains(&space,
            (UINT64_C(1) << 48) - 1, 1));
    assert(!guest_address_space_contains(&space,
            (UINT64_C(1) << 48) - 1, 2));
    assert(!guest_address_space_contains(&space, UINT64_C(1) << 48, 1));

    assert(guest_address_space_resolve_page(&space, TEST_PAGE,
            GUEST_MEMORY_READ, &view) == GUEST_MEMORY_FAULT_NONE);
    assert(view.host_page == mapping.page);
    assert(view.sync == NULL);
    assert(mapping.calls == 1);
    assert(mapping.last_page == TEST_PAGE);
    assert(mapping.last_access == GUEST_MEMORY_READ);

    assert(guest_address_space_resolve_page(&space, TEST_PAGE,
            GUEST_MEMORY_WRITE, &view) == GUEST_MEMORY_FAULT_PERMISSION);
    assert(guest_address_space_resolve_page(&space,
            TEST_PAGE + GUEST_MEMORY_PAGE_SIZE, GUEST_MEMORY_READ,
            &view) == GUEST_MEMORY_FAULT_UNMAPPED);
    assert(guest_address_space_resolve_page(&space, TEST_PAGE + 1,
            GUEST_MEMORY_READ, &view) == GUEST_MEMORY_FAULT_ALIGNMENT);
    assert(mapping.calls == 3);

    qword_t first_generation;
    qword_t adjacent_generation;
    first_generation = guest_address_space_track_exclusive(
            &space, TEST_PAGE);
    adjacent_generation = guest_address_space_track_exclusive(&space,
            TEST_PAGE + GUEST_MEMORY_EXCLUSIVE_GRANULE_SIZE);
    assert(first_generation != adjacent_generation);
    guest_address_space_written(&space, TEST_PAGE + 4, 1);
    assert(!guest_address_space_exclusive_matches(
            &space, TEST_PAGE, first_generation));
    assert(guest_address_space_exclusive_matches(&space,
            TEST_PAGE + GUEST_MEMORY_EXCLUSIVE_GRANULE_SIZE,
            adjacent_generation));

    guest_address_space_changed(&space);
    assert(space.generation == 2);
    assert(!guest_address_space_exclusive_matches(&space,
            TEST_PAGE + GUEST_MEMORY_EXCLUSIVE_GRANULE_SIZE,
            adjacent_generation));

    enum {
        reservation_capacity = GUEST_MEMORY_EXCLUSIVE_BUCKET_COUNT *
                GUEST_MEMORY_EXCLUSIVE_WAYS,
        reservation_count = reservation_capacity + 1,
    };
    qword_t generations[reservation_count];
    for (unsigned index = 0; index < reservation_count; index++) {
        guest_addr_t address = TEST_PAGE +
                index * GUEST_MEMORY_EXCLUSIVE_GRANULE_SIZE;
        generations[index] = guest_address_space_track_exclusive(
                &space, address);
    }
    unsigned retained = 0;
    for (unsigned index = 0; index < reservation_count; index++) {
        guest_addr_t address = TEST_PAGE +
                index * GUEST_MEMORY_EXCLUSIVE_GRANULE_SIZE;
        if (guest_address_space_exclusive_matches(
                &space, address, generations[index]))
            retained++;
    }
    assert(retained != 0 && retained <= reservation_capacity &&
            retained < reservation_count);
    return 0;
}
