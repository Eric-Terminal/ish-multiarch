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
    *view = (struct guest_page_view) {
        .host_page = mapping->page,
        .permissions = GUEST_MEMORY_READ | GUEST_MEMORY_EXECUTE,
    };
    return GUEST_MEMORY_FAULT_NONE;
}

static const struct guest_address_space_ops test_ops = {
    .resolve_page = resolve_test_page,
};

int main(void) {
    struct test_mapping mapping = {0};
    struct guest_address_space space;
    struct guest_page_view view;

    guest_address_space_init(&space, &test_ops, &mapping, 48);
    assert(space.generation == 1);
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

    guest_address_space_changed(&space);
    assert(space.generation == 2);
    return 0;
}
