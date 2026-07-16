#include <assert.h>
#include <string.h>

#include "guest/memory/page-backing.h"
#include "guest/memory/page-table.h"
#include "guest/memory/tlb.h"

#define HIGH_PAGE UINT64_C(0x0000123456789000)
#define NEXT_PAGE (HIGH_PAGE + GUEST_MEMORY_PAGE_SIZE)
#define SIBLING_LEAF_PAGE (HIGH_PAGE + (UINT64_C(1) << 21))
#define SIBLING_LEVEL2_PAGE (HIGH_PAGE + (UINT64_C(1) << 30))
#define RANGE_BASE UINT64_C(0x0000200000000000)
#define SHARED_BASE UINT64_C(0x0000000000400000)

static byte_t *lookup_page(struct guest_page_table *table,
        guest_addr_t page, unsigned expected_permissions) {
    byte_t *host_page;
    unsigned permissions;
    assert(guest_page_table_lookup(table, page,
            &host_page, &permissions) == GUEST_PAGE_TABLE_OK);
    assert(permissions == expected_permissions);
    return host_page;
}

static void assert_not_mapped(struct guest_page_table *table,
        guest_addr_t page) {
    byte_t *host_page;
    unsigned permissions;
    assert(guest_page_table_lookup(table, page,
            &host_page, &permissions) == GUEST_PAGE_TABLE_NOT_MAPPED);
}

static struct guest_page_view resolve_view(struct guest_page_table *table,
        guest_addr_t page) {
    struct guest_page_view view;
    assert(guest_address_space_resolve_page(&table->address_space, page,
            GUEST_MEMORY_READ, &view) == GUEST_MEMORY_FAULT_NONE);
    return view;
}

static byte_t tlb_read_byte(struct guest_tlb *tlb, guest_addr_t address) {
    struct guest_memory_fault fault;
    byte_t value;
    assert(guest_tlb_read(tlb, address, &value, sizeof(value),
            GUEST_MEMORY_READ, &fault));
    return value;
}

static void tlb_write_byte(struct guest_tlb *tlb, guest_addr_t address,
        byte_t value) {
    struct guest_memory_fault fault;
    assert(guest_tlb_write(tlb, address, &value, sizeof(value), &fault));
}

static void test_single_page_operations(void) {
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
}

static void test_supported_address_widths(void) {
    static const byte_t rejected[] = {12, 49, 64};
    for (size_t i = 0; i < array_size(rejected); i++) {
        struct guest_page_table table;
        assert(!guest_page_table_init(&table, rejected[i]));
        assert(table.root == NULL);
        guest_page_table_destroy(&table);
    }

    struct guest_page_table narrow;
    assert(guest_page_table_init(&narrow, 13));
    byte_t *host_page;
    assert(guest_page_table_map(&narrow, GUEST_MEMORY_PAGE_SIZE,
            GUEST_MEMORY_READ, &host_page) == GUEST_PAGE_TABLE_OK);
    assert(guest_page_table_map(&narrow, 2 * GUEST_MEMORY_PAGE_SIZE,
            GUEST_MEMORY_READ, &host_page) ==
            GUEST_PAGE_TABLE_INVALID_ADDRESS);
    guest_page_table_destroy(&narrow);
}

static void test_backing_allocation_failures(void) {
    assert(guest_page_backing_test_live_count() == 0);
    struct guest_page_table table;
    assert(guest_page_table_init(&table, 48));
    qword_t generation = table.address_space.generation;

    guest_page_backing_test_fail_allocation_at(0);
    byte_t *page;
    assert(guest_page_table_map(&table, RANGE_BASE,
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE, &page) ==
            GUEST_PAGE_TABLE_OUT_OF_MEMORY);
    assert(table.address_space.generation == generation);
    assert_not_mapped(&table, RANGE_BASE);
    assert(guest_page_backing_test_live_count() == 0);

    guest_page_backing_test_fail_allocation_at(SIZE_MAX);
    assert(guest_page_table_map(&table, RANGE_BASE,
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE, &page) ==
            GUEST_PAGE_TABLE_OK);
    page[0] = 0x5a;
    assert(guest_page_backing_test_live_count() == 1);
    generation = table.address_space.generation;
    const struct guest_page_range replacement = {
        .first = RANGE_BASE,
        .page_count = 3,
    };
    for (size_t fail_at = 0; fail_at < 3; fail_at++) {
        guest_page_backing_test_fail_allocation_at(fail_at);
        assert(guest_page_table_map_zero_range(&table, replacement,
                GUEST_MEMORY_READ, true) ==
                GUEST_PAGE_TABLE_OUT_OF_MEMORY);
        assert(guest_page_backing_test_allocation_count() == fail_at + 1);
        assert(table.address_space.generation == generation);
        assert(lookup_page(&table, RANGE_BASE,
                GUEST_MEMORY_READ | GUEST_MEMORY_WRITE) == page);
        assert(page[0] == 0x5a);
        assert_not_mapped(&table, RANGE_BASE + GUEST_MEMORY_PAGE_SIZE);
        assert_not_mapped(&table,
                RANGE_BASE + 2 * GUEST_MEMORY_PAGE_SIZE);
        assert(guest_page_backing_test_live_count() == 1);
    }

    guest_page_backing_test_fail_allocation_at(SIZE_MAX);
    assert(guest_page_table_map_zero_range(&table, replacement,
            GUEST_MEMORY_READ, true) == GUEST_PAGE_TABLE_OK);
    assert(table.address_space.generation == generation + 1);
    assert(guest_page_backing_test_live_count() == 3);
    for (qword_t index = 0; index < replacement.page_count; index++) {
        byte_t *replacement_page = lookup_page(&table,
                replacement.first + index * GUEST_MEMORY_PAGE_SIZE,
                GUEST_MEMORY_READ);
        for (size_t offset = 0; offset < GUEST_MEMORY_PAGE_SIZE; offset++)
            assert(replacement_page[offset] == 0);
    }
    guest_page_table_destroy(&table);
    assert(guest_page_backing_test_live_count() == 0);
}

static void test_shared_map_rollback(void) {
    assert(guest_page_backing_test_live_count() == 0);
    struct guest_page_table table;
    assert(guest_page_table_init(&table, 48));
    const struct guest_page_range range = {
        .first = SHARED_BASE,
        .page_count = 3,
    };

    guest_page_backing_test_fail_allocation_at(SIZE_MAX);
    assert(guest_page_table_map_zero_shared_range(&table, range,
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE, false) ==
            GUEST_PAGE_TABLE_OK);
    byte_t *pages[3];
    const struct guest_page_sync *syncs[3];
    for (qword_t index = 0; index < range.page_count; index++) {
        guest_addr_t address = range.first +
                index * GUEST_MEMORY_PAGE_SIZE;
        pages[index] = lookup_page(&table, address,
                GUEST_MEMORY_READ | GUEST_MEMORY_WRITE);
        pages[index][0] = (byte_t) (0x51 + index);
        syncs[index] = resolve_view(&table, address).sync;
        assert(syncs[index] != NULL);
    }
    assert(guest_page_backing_test_live_count() == range.page_count);
    qword_t generation = table.address_space.generation;

    guest_page_backing_test_fail_allocation_at(0);
    assert(guest_page_table_map_zero_shared_range(&table, range,
            GUEST_MEMORY_READ, false) ==
            GUEST_PAGE_TABLE_ALREADY_MAPPED);
    assert(guest_page_backing_test_allocation_count() == 0);

    for (size_t fail_at = 0; fail_at < 3; fail_at++) {
        guest_page_backing_test_fail_allocation_at(fail_at);
        assert(guest_page_table_map_zero_shared_range(&table, range,
                GUEST_MEMORY_EXECUTE, true) ==
                GUEST_PAGE_TABLE_OUT_OF_MEMORY);
        assert(guest_page_backing_test_allocation_count() == fail_at + 1);
        assert(table.address_space.generation == generation);
        assert(guest_page_backing_test_live_count() == range.page_count);
        for (qword_t index = 0; index < range.page_count; index++) {
            guest_addr_t address = range.first +
                    index * GUEST_MEMORY_PAGE_SIZE;
            assert(lookup_page(&table, address,
                    GUEST_MEMORY_READ | GUEST_MEMORY_WRITE) ==
                    pages[index]);
            assert(pages[index][0] == (byte_t) (0x51 + index));
            assert(resolve_view(&table, address).sync == syncs[index]);
        }
    }

    guest_page_backing_test_fail_allocation_at(SIZE_MAX);
    guest_page_table_destroy(&table);
    assert(guest_page_backing_test_live_count() == 0);
}

static void test_mapping_kind_replacement(void) {
    assert(guest_page_backing_test_live_count() == 0);
    guest_page_backing_test_fail_allocation_at(SIZE_MAX);
    struct guest_page_table source;
    assert(guest_page_table_init(&source, 48));
    const struct guest_page_range one_page = {
        .first = SHARED_BASE,
        .page_count = 1,
    };

    assert(guest_page_table_map_zero_range(&source, one_page,
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE, false) ==
            GUEST_PAGE_TABLE_OK);
    byte_t *private_page = lookup_page(&source, SHARED_BASE,
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE);
    assert(resolve_view(&source, SHARED_BASE).sync == NULL);

    assert(guest_page_table_map_zero_shared_range(&source, one_page,
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE, true) ==
            GUEST_PAGE_TABLE_OK);
    byte_t *shared_page = lookup_page(&source, SHARED_BASE,
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE);
    assert(shared_page != private_page);
    assert(resolve_view(&source, SHARED_BASE).sync != NULL);
    shared_page[0] = 0x6a;

    struct guest_page_table shared_copy;
    assert(guest_page_table_clone(&shared_copy, &source));
    assert(lookup_page(&shared_copy, SHARED_BASE,
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE) == shared_page);

    assert(guest_page_table_map_zero_range(&source, one_page,
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE, true) ==
            GUEST_PAGE_TABLE_OK);
    byte_t *new_private_page = lookup_page(&source, SHARED_BASE,
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE);
    assert(new_private_page != shared_page && new_private_page[0] == 0);
    assert(resolve_view(&source, SHARED_BASE).sync == NULL);
    assert(lookup_page(&shared_copy, SHARED_BASE,
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE) == shared_page);
    assert(shared_page[0] == 0x6a);

    struct guest_page_table private_copy;
    assert(guest_page_table_clone(&private_copy, &source));
    assert(lookup_page(&private_copy, SHARED_BASE,
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE) != new_private_page);
    assert(resolve_view(&private_copy, SHARED_BASE).sync == NULL);

    guest_page_table_destroy(&private_copy);
    guest_page_table_destroy(&source);
    assert(shared_page[0] == 0x6a);
    guest_page_table_destroy(&shared_copy);
    assert(guest_page_backing_test_live_count() == 0);
}

static void test_atomic_map_and_replace(void) {
    struct guest_page_table table;
    assert(guest_page_table_init(&table, 48));
    const struct guest_page_range range = {
        .first = RANGE_BASE,
        .page_count = 3,
    };
    byte_t *left;
    byte_t *right;
    assert(guest_page_table_map(&table,
            RANGE_BASE - GUEST_MEMORY_PAGE_SIZE,
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE, &left) ==
            GUEST_PAGE_TABLE_OK);
    assert(guest_page_table_map(&table,
            RANGE_BASE + 3 * GUEST_MEMORY_PAGE_SIZE,
            GUEST_MEMORY_READ, &right) == GUEST_PAGE_TABLE_OK);
    left[0] = 0x31;
    right[0] = 0x42;

    qword_t generation = table.address_space.generation;
    assert(guest_page_table_map_zero_range(&table, range,
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE, false) ==
            GUEST_PAGE_TABLE_OK);
    assert(table.address_space.generation == generation + 1);
    byte_t *old_pages[3];
    for (qword_t i = 0; i < range.page_count; i++) {
        old_pages[i] = lookup_page(&table,
                range.first + i * GUEST_MEMORY_PAGE_SIZE,
                GUEST_MEMORY_READ | GUEST_MEMORY_WRITE);
        for (size_t offset = 0; offset < GUEST_MEMORY_PAGE_SIZE; offset++)
            assert(old_pages[i][offset] == 0);
        old_pages[i][0] = (byte_t) (0x80 + i);
    }

    struct guest_tlb tlb;
    guest_tlb_init(&tlb, &table.address_space);
    struct guest_memory_fault fault;
    byte_t value;
    assert(guest_tlb_read(&tlb, RANGE_BASE, &value, 1,
            GUEST_MEMORY_READ, &fault));
    assert(value == 0x80);
    qword_t observed_generation = tlb.observed_generation;

    generation = table.address_space.generation;
    assert(guest_page_table_map_zero_range(&table, range,
            GUEST_MEMORY_READ, true) == GUEST_PAGE_TABLE_OK);
    assert(table.address_space.generation == generation + 1);
    assert(tlb.observed_generation == observed_generation);
    for (qword_t i = 0; i < range.page_count; i++) {
        byte_t *replacement = lookup_page(&table,
                range.first + i * GUEST_MEMORY_PAGE_SIZE,
                GUEST_MEMORY_READ);
        for (size_t offset = 0; offset < GUEST_MEMORY_PAGE_SIZE; offset++)
            assert(replacement[offset] == 0);
    }
    assert(guest_tlb_read(&tlb, RANGE_BASE, &value, 1,
            GUEST_MEMORY_READ, &fault));
    assert(value == 0);
    assert(tlb.observed_generation == table.address_space.generation);
    assert(!guest_tlb_write(&tlb, RANGE_BASE, &value, 1, &fault));
    assert(fault.kind == GUEST_MEMORY_FAULT_PERMISSION);
    assert(left[0] == 0x31 && right[0] == 0x42);

    generation = table.address_space.generation;
    assert(guest_page_table_protect_range(&table, range, 0) ==
            GUEST_PAGE_TABLE_OK);
    assert(table.address_space.generation == generation + 1);
    for (qword_t i = 0; i < range.page_count; i++)
        lookup_page(&table, range.first + i * GUEST_MEMORY_PAGE_SIZE, 0);
    assert(!guest_tlb_read(&tlb, RANGE_BASE, &value, 1,
            GUEST_MEMORY_READ, &fault));
    assert(fault.kind == GUEST_MEMORY_FAULT_PERMISSION);
    generation = table.address_space.generation;
    assert(guest_page_table_protect_range(&table, range, 0) ==
            GUEST_PAGE_TABLE_OK);
    assert(table.address_space.generation == generation);

    assert(guest_page_table_unmap_range(&table, range, false) ==
            GUEST_PAGE_TABLE_OK);
    assert(table.address_space.generation == generation + 1);
    assert(!guest_tlb_read(&tlb, RANGE_BASE, &value, 1,
            GUEST_MEMORY_READ, &fault));
    assert(fault.kind == GUEST_MEMORY_FAULT_UNMAPPED);
    assert(left[0] == 0x31 && right[0] == 0x42);
    guest_page_table_destroy(&table);
}

static void test_page_origins(void) {
    struct guest_page_table table;
    assert(guest_page_table_init(&table, 48));
    byte_t *page;
    const guest_addr_t special_page =
            NEXT_PAGE + GUEST_MEMORY_PAGE_SIZE;
    assert(guest_page_table_map(&table, HIGH_PAGE,
            GUEST_MEMORY_READ, &page) == GUEST_PAGE_TABLE_OK);
    assert(guest_page_table_map_file(&table, NEXT_PAGE,
            GUEST_MEMORY_READ, &page) == GUEST_PAGE_TABLE_OK);
    assert(guest_page_table_map_special(&table, special_page,
            GUEST_MEMORY_READ | GUEST_MEMORY_EXECUTE,
            &page) == GUEST_PAGE_TABLE_OK);
    assert(resolve_view(&table, HIGH_PAGE).origin ==
            GUEST_PAGE_ORIGIN_ANONYMOUS);
    assert(resolve_view(&table, NEXT_PAGE).origin ==
            GUEST_PAGE_ORIGIN_FILE);
    assert(resolve_view(&table, special_page).origin ==
            GUEST_PAGE_ORIGIN_SPECIAL);

    assert(guest_page_table_protect(&table, NEXT_PAGE,
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE) ==
            GUEST_PAGE_TABLE_OK);
    assert(resolve_view(&table, NEXT_PAGE).origin ==
            GUEST_PAGE_ORIGIN_FILE);
    struct guest_page_table copy;
    assert(guest_page_table_clone(&copy, &table));
    assert(resolve_view(&copy, HIGH_PAGE).origin ==
            GUEST_PAGE_ORIGIN_ANONYMOUS);
    assert(resolve_view(&copy, NEXT_PAGE).origin ==
            GUEST_PAGE_ORIGIN_FILE);
    assert(resolve_view(&copy, special_page).origin ==
            GUEST_PAGE_ORIGIN_SPECIAL);

    struct guest_tlb tlb;
    guest_tlb_init(&tlb, &table.address_space);
    assert(tlb_read_byte(&tlb, NEXT_PAGE) == 0);
    qword_t generation = table.address_space.generation;
    tlb_write_byte(&tlb, NEXT_PAGE, 0x5a);
    assert(resolve_view(&table, NEXT_PAGE).origin ==
            GUEST_PAGE_ORIGIN_ANONYMOUS);
    assert(resolve_view(&copy, NEXT_PAGE).origin ==
            GUEST_PAGE_ORIGIN_FILE);
    assert(table.address_space.generation == generation + 1);
    generation = table.address_space.generation;
    tlb_write_byte(&tlb, NEXT_PAGE + 1, 0xa5);
    assert(table.address_space.generation == generation);
    struct guest_page_table written_copy;
    assert(guest_page_table_clone(&written_copy, &table));
    assert(resolve_view(&written_copy, NEXT_PAGE).origin ==
            GUEST_PAGE_ORIGIN_ANONYMOUS);
    guest_page_table_destroy(&written_copy);

    const struct guest_page_range replacement = {
        .first = NEXT_PAGE,
        .page_count = 2,
    };
    assert(guest_page_table_map_zero_range(&table, replacement,
            GUEST_MEMORY_READ, true) == GUEST_PAGE_TABLE_OK);
    assert(resolve_view(&table, NEXT_PAGE).origin ==
            GUEST_PAGE_ORIGIN_ANONYMOUS);
    assert(resolve_view(&table, special_page).origin ==
            GUEST_PAGE_ORIGIN_ANONYMOUS);
    guest_page_table_destroy(&copy);
    guest_page_table_destroy(&table);
}

static void test_conflicts_and_holes(void) {
    struct guest_page_table table;
    assert(guest_page_table_init(&table, 48));
    const guest_addr_t base = RANGE_BASE + UINT64_C(0x200000);
    byte_t *middle;
    assert(guest_page_table_map(&table, base + GUEST_MEMORY_PAGE_SIZE,
            GUEST_MEMORY_READ, &middle) == GUEST_PAGE_TABLE_OK);
    middle[0] = 0x5a;
    const struct guest_page_range three_pages = {
        .first = base,
        .page_count = 3,
    };
    qword_t generation = table.address_space.generation;
    assert(guest_page_table_map_zero_range(&table, three_pages,
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE, false) ==
            GUEST_PAGE_TABLE_ALREADY_MAPPED);
    assert(table.address_space.generation == generation);
    assert_not_mapped(&table, base);
    assert_not_mapped(&table, base + 2 * GUEST_MEMORY_PAGE_SIZE);
    assert(lookup_page(&table, base + GUEST_MEMORY_PAGE_SIZE,
            GUEST_MEMORY_READ) == middle);
    assert(middle[0] == 0x5a);

    byte_t *first;
    byte_t *last;
    assert(guest_page_table_map(&table, base,
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE, &first) ==
            GUEST_PAGE_TABLE_OK);
    assert(guest_page_table_map(&table,
            base + 2 * GUEST_MEMORY_PAGE_SIZE,
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE, &last) ==
            GUEST_PAGE_TABLE_OK);
    first[0] = 0x11;
    last[0] = 0x22;
    assert(guest_page_table_unmap(&table,
            base + GUEST_MEMORY_PAGE_SIZE) == GUEST_PAGE_TABLE_OK);

    generation = table.address_space.generation;
    assert(guest_page_table_protect_range(&table, three_pages,
            GUEST_MEMORY_READ) == GUEST_PAGE_TABLE_NOT_MAPPED);
    assert(table.address_space.generation == generation);
    assert(lookup_page(&table, base,
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE)[0] == 0x11);
    assert(lookup_page(&table, base + 2 * GUEST_MEMORY_PAGE_SIZE,
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE)[0] == 0x22);
    assert(guest_page_table_unmap_range(&table, three_pages, false) ==
            GUEST_PAGE_TABLE_NOT_MAPPED);
    assert(table.address_space.generation == generation);

    assert(guest_page_table_unmap_range(&table, three_pages, true) ==
            GUEST_PAGE_TABLE_OK);
    assert(table.address_space.generation == generation + 1);
    assert_not_mapped(&table, base);
    assert_not_mapped(&table, base + GUEST_MEMORY_PAGE_SIZE);
    assert_not_mapped(&table, base + 2 * GUEST_MEMORY_PAGE_SIZE);
    generation = table.address_space.generation;
    assert(guest_page_table_unmap_range(&table, three_pages, true) ==
            GUEST_PAGE_TABLE_OK);
    assert(table.address_space.generation == generation);
    guest_page_table_destroy(&table);
}

static void test_tree_and_address_boundaries(void) {
    static const guest_addr_t boundaries[] = {
        UINT64_C(1) << 21,
        UINT64_C(1) << 30,
        UINT64_C(1) << 39,
    };
    struct guest_page_table table;
    assert(guest_page_table_init(&table, 48));
    for (size_t i = 0; i < array_size(boundaries); i++) {
        const struct guest_page_range range = {
            .first = boundaries[i] - GUEST_MEMORY_PAGE_SIZE,
            .page_count = 2,
        };
        qword_t generation = table.address_space.generation;
        assert(guest_page_table_map_zero_range(&table, range,
                GUEST_MEMORY_READ, false) == GUEST_PAGE_TABLE_OK);
        assert(table.address_space.generation == generation + 1);
        lookup_page(&table, range.first, GUEST_MEMORY_READ);
        lookup_page(&table, boundaries[i], GUEST_MEMORY_READ);
        assert(guest_page_table_unmap_range(&table, range, false) ==
                GUEST_PAGE_TABLE_OK);
        assert(table.address_space.generation == generation + 2);
    }

    const struct guest_page_range top = {
        .first = (UINT64_C(1) << 48) - 2 * GUEST_MEMORY_PAGE_SIZE,
        .page_count = 2,
    };
    qword_t generation = table.address_space.generation;
    assert(guest_page_table_map_zero_range(&table, top,
            GUEST_MEMORY_READ, false) == GUEST_PAGE_TABLE_OK);
    assert(table.address_space.generation == generation + 1);
    const struct guest_page_range beyond_top = {
        .first = top.first,
        .page_count = 3,
    };
    generation = table.address_space.generation;
    assert(guest_page_table_map_zero_range(&table, beyond_top,
            GUEST_MEMORY_READ, true) == GUEST_PAGE_TABLE_INVALID_ADDRESS);
    assert(guest_page_table_protect_range(&table, beyond_top,
            GUEST_MEMORY_READ) == GUEST_PAGE_TABLE_INVALID_ADDRESS);
    assert(guest_page_table_unmap_range(&table, beyond_top, true) ==
            GUEST_PAGE_TABLE_INVALID_ADDRESS);
    assert(table.address_space.generation == generation);
    lookup_page(&table, top.first, GUEST_MEMORY_READ);
    lookup_page(&table, top.first + GUEST_MEMORY_PAGE_SIZE,
            GUEST_MEMORY_READ);

    const struct guest_page_range huge = {
        .first = 0,
        .page_count = UINT64_MAX,
    };
    assert(guest_page_table_map_zero_range(&table, huge,
            GUEST_MEMORY_READ, false) == GUEST_PAGE_TABLE_INVALID_ADDRESS);
    assert(guest_page_table_protect_range(&table, huge,
            GUEST_MEMORY_READ) == GUEST_PAGE_TABLE_INVALID_ADDRESS);
    assert(guest_page_table_unmap_range(&table, huge, true) ==
            GUEST_PAGE_TABLE_INVALID_ADDRESS);
    assert(table.address_space.generation == generation);

    const struct guest_page_range empty = {
        .first = RANGE_BASE,
        .page_count = 0,
    };
    assert(guest_page_table_map_zero_range(&table, empty,
            GUEST_MEMORY_READ, false) == GUEST_PAGE_TABLE_OK);
    assert(guest_page_table_protect_range(&table, empty,
            GUEST_MEMORY_READ) == GUEST_PAGE_TABLE_OK);
    assert(guest_page_table_unmap_range(&table, empty, false) ==
            GUEST_PAGE_TABLE_OK);
    assert(table.address_space.generation == generation);
    guest_page_table_destroy(&table);
}

static void test_shared_clone_lifecycle(void) {
    assert(guest_page_backing_test_live_count() == 0);
    struct guest_page_table source;
    assert(guest_page_table_init(&source, 48));

    byte_t *private_page;
    assert(guest_page_table_map(&source, HIGH_PAGE,
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE, &private_page) ==
            GUEST_PAGE_TABLE_OK);
    private_page[0] = 0x31;
    const struct guest_page_range shared_range = {
        .first = SHARED_BASE,
        .page_count = 3,
    };
    assert(guest_page_table_map_zero_shared_range(&source, shared_range,
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE, false) ==
            GUEST_PAGE_TABLE_OK);
    byte_t *shared_pages[3];
    for (qword_t index = 0; index < shared_range.page_count; index++) {
        shared_pages[index] = lookup_page(&source,
                shared_range.first + index * GUEST_MEMORY_PAGE_SIZE,
                GUEST_MEMORY_READ | GUEST_MEMORY_WRITE);
        shared_pages[index][0] = (byte_t) (0x41 + index);
    }
    assert(resolve_view(&source, HIGH_PAGE).sync == NULL);
    assert(resolve_view(&source, SHARED_BASE).sync != NULL);

    guest_page_table_test_fail_clone_allocation_at(SIZE_MAX);
    guest_page_backing_test_fail_allocation_at(SIZE_MAX);
    struct guest_page_table first_copy;
    assert(guest_page_table_clone(&first_copy, &source));
    assert(guest_page_backing_test_allocation_count() == 1);
    guest_page_backing_test_fail_allocation_at(SIZE_MAX);
    struct guest_page_table second_copy;
    assert(guest_page_table_clone(&second_copy, &source));
    assert(guest_page_backing_test_allocation_count() == 1);

    byte_t *first_private = lookup_page(&first_copy, HIGH_PAGE,
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE);
    byte_t *second_private = lookup_page(&second_copy, HIGH_PAGE,
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE);
    assert(first_private != private_page && second_private != private_page &&
            first_private != second_private);
    assert(first_private[0] == private_page[0] &&
            second_private[0] == private_page[0]);
    assert(resolve_view(&first_copy, HIGH_PAGE).sync == NULL);
    assert(resolve_view(&second_copy, HIGH_PAGE).sync == NULL);
    for (qword_t index = 0; index < shared_range.page_count; index++) {
        guest_addr_t page = shared_range.first +
                index * GUEST_MEMORY_PAGE_SIZE;
        assert(lookup_page(&first_copy, page,
                GUEST_MEMORY_READ | GUEST_MEMORY_WRITE) ==
                shared_pages[index]);
        assert(lookup_page(&second_copy, page,
                GUEST_MEMORY_READ | GUEST_MEMORY_WRITE) ==
                shared_pages[index]);
    }
    assert(resolve_view(&source, SHARED_BASE).sync ==
            resolve_view(&first_copy, SHARED_BASE).sync);
    assert(resolve_view(&source, SHARED_BASE).sync ==
            resolve_view(&second_copy, SHARED_BASE).sync);

    struct guest_tlb source_tlb;
    struct guest_tlb first_tlb;
    struct guest_tlb second_tlb;
    guest_tlb_init(&source_tlb, &source.address_space);
    guest_tlb_init(&first_tlb, &first_copy.address_space);
    guest_tlb_init(&second_tlb, &second_copy.address_space);
    tlb_write_byte(&source_tlb, SHARED_BASE, 0x51);
    assert(tlb_read_byte(&first_tlb, SHARED_BASE) == 0x51);
    tlb_write_byte(&first_tlb, SHARED_BASE + 1, 0x61);
    assert(tlb_read_byte(&source_tlb, SHARED_BASE + 1) == 0x61);
    assert(tlb_read_byte(&second_tlb, SHARED_BASE + 1) == 0x61);

    assert(guest_page_table_protect(&first_copy,
            SHARED_BASE + GUEST_MEMORY_PAGE_SIZE, GUEST_MEMORY_READ) ==
            GUEST_PAGE_TABLE_OK);
    lookup_page(&source, SHARED_BASE + GUEST_MEMORY_PAGE_SIZE,
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE);
    lookup_page(&second_copy, SHARED_BASE + GUEST_MEMORY_PAGE_SIZE,
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE);
    struct guest_memory_fault fault;
    byte_t rejected = 0x70;
    assert(!guest_tlb_write(&first_tlb,
            SHARED_BASE + GUEST_MEMORY_PAGE_SIZE,
            &rejected, sizeof(rejected), &fault));
    assert(fault.kind == GUEST_MEMORY_FAULT_PERMISSION);
    tlb_write_byte(&source_tlb,
            SHARED_BASE + GUEST_MEMORY_PAGE_SIZE, 0x71);
    assert(tlb_read_byte(&first_tlb,
            SHARED_BASE + GUEST_MEMORY_PAGE_SIZE) == 0x71);

    assert(guest_page_table_unmap(&first_copy,
            SHARED_BASE + 2 * GUEST_MEMORY_PAGE_SIZE) ==
            GUEST_PAGE_TABLE_OK);
    assert_not_mapped(&first_copy,
            SHARED_BASE + 2 * GUEST_MEMORY_PAGE_SIZE);
    assert(lookup_page(&source,
            SHARED_BASE + 2 * GUEST_MEMORY_PAGE_SIZE,
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE) == shared_pages[2]);
    assert(lookup_page(&second_copy,
            SHARED_BASE + 2 * GUEST_MEMORY_PAGE_SIZE,
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE) == shared_pages[2]);

    byte_t *old_shared = shared_pages[0];
    const struct guest_page_range one_shared_page = {
        .first = SHARED_BASE,
        .page_count = 1,
    };
    assert(guest_page_table_map_zero_shared_range(&source, one_shared_page,
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE, true) ==
            GUEST_PAGE_TABLE_OK);
    byte_t *replacement = lookup_page(&source, SHARED_BASE,
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE);
    assert(replacement != old_shared && replacement[0] == 0);
    assert(lookup_page(&first_copy, SHARED_BASE,
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE) == old_shared);
    assert(lookup_page(&second_copy, SHARED_BASE,
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE) == old_shared);
    tlb_write_byte(&source_tlb, SHARED_BASE, 0xa1);
    assert(tlb_read_byte(&second_tlb, SHARED_BASE) == 0x51);
    tlb_write_byte(&second_tlb, SHARED_BASE, 0xb1);
    assert(tlb_read_byte(&source_tlb, SHARED_BASE) == 0xa1);

    guest_page_table_destroy(&first_copy);
    tlb_write_byte(&source_tlb,
            SHARED_BASE + GUEST_MEMORY_PAGE_SIZE, 0xc1);
    assert(tlb_read_byte(&second_tlb,
            SHARED_BASE + GUEST_MEMORY_PAGE_SIZE) == 0xc1);
    guest_page_table_destroy(&source);
    tlb_write_byte(&second_tlb, SHARED_BASE, 0xd1);
    assert(tlb_read_byte(&second_tlb, SHARED_BASE) == 0xd1);
    assert(tlb_read_byte(&second_tlb,
            SHARED_BASE + 2 * GUEST_MEMORY_PAGE_SIZE) == 0x43);
    guest_page_table_destroy(&second_copy);
    assert(guest_page_backing_test_live_count() == 0);
}

static void test_mixed_clone_failures(void) {
    assert(guest_page_backing_test_live_count() == 0);
    struct guest_page_table source;
    assert(guest_page_table_init(&source, 48));
    const struct guest_page_range shared_range = {
        .first = GUEST_MEMORY_PAGE_SIZE,
        .page_count = 1,
    };
    assert(guest_page_table_map_zero_shared_range(&source, shared_range,
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE, false) ==
            GUEST_PAGE_TABLE_OK);
    byte_t *shared_page = lookup_page(&source, shared_range.first,
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE);
    shared_page[0] = 0x7a;

    static const guest_addr_t private_addresses[] = {
        HIGH_PAGE,
        NEXT_PAGE,
        SIBLING_LEVEL2_PAGE,
    };
    byte_t *private_pages[array_size(private_addresses)];
    for (size_t index = 0; index < array_size(private_addresses); index++) {
        assert(guest_page_table_map(&source, private_addresses[index],
                GUEST_MEMORY_READ | GUEST_MEMORY_WRITE,
                &private_pages[index]) == GUEST_PAGE_TABLE_OK);
        private_pages[index][0] = (byte_t) (0x80 + index);
    }

    guest_page_table_test_fail_clone_allocation_at(SIZE_MAX);
    guest_page_backing_test_fail_allocation_at(SIZE_MAX);
    struct guest_page_table probe;
    assert(guest_page_table_clone(&probe, &source));
    size_t clone_allocations =
            guest_page_table_test_clone_allocation_count();
    size_t private_clone_allocations =
            guest_page_backing_test_allocation_count();
    assert(clone_allocations > 4);
    assert(private_clone_allocations == array_size(private_addresses));
    assert(lookup_page(&probe, shared_range.first,
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE) == shared_page);
    guest_page_table_destroy(&probe);

    for (size_t fail_at = 0; fail_at < clone_allocations; fail_at++) {
        unsigned live_before = guest_page_backing_test_live_count();
        qword_t generation = source.address_space.generation;
        guest_page_table_test_fail_clone_allocation_at(fail_at);
        guest_page_backing_test_fail_allocation_at(SIZE_MAX);
        struct guest_page_table failed;
        assert(!guest_page_table_clone(&failed, &source));
        assert(failed.root == NULL);
        assert(source.address_space.generation == generation);
        assert(guest_page_backing_test_live_count() == live_before);
        assert(lookup_page(&source, shared_range.first,
                GUEST_MEMORY_READ | GUEST_MEMORY_WRITE) == shared_page);
        for (size_t index = 0;
                index < array_size(private_addresses); index++) {
            assert(lookup_page(&source, private_addresses[index],
                    GUEST_MEMORY_READ | GUEST_MEMORY_WRITE) ==
                    private_pages[index]);
            assert(private_pages[index][0] == (byte_t) (0x80 + index));
        }
        guest_page_table_destroy(&failed);

        // 解除源映射后对象必须消失，否则失败路径遗留了一份共享引用。
        assert(guest_page_table_unmap(&source, shared_range.first) ==
                GUEST_PAGE_TABLE_OK);
        assert(guest_page_backing_test_live_count() == live_before - 1);
        guest_page_backing_test_fail_allocation_at(SIZE_MAX);
        assert(guest_page_table_map_zero_shared_range(&source, shared_range,
                GUEST_MEMORY_READ | GUEST_MEMORY_WRITE, false) ==
                GUEST_PAGE_TABLE_OK);
        shared_page = lookup_page(&source, shared_range.first,
                GUEST_MEMORY_READ | GUEST_MEMORY_WRITE);
        shared_page[0] = 0x7a;
        assert(guest_page_backing_test_live_count() == live_before);
    }

    guest_page_table_test_fail_clone_allocation_at(SIZE_MAX);
    for (size_t fail_at = 0;
            fail_at < private_clone_allocations; fail_at++) {
        unsigned live_before = guest_page_backing_test_live_count();
        qword_t generation = source.address_space.generation;
        guest_page_backing_test_fail_allocation_at(fail_at);
        struct guest_page_table failed;
        assert(!guest_page_table_clone(&failed, &source));
        assert(failed.root == NULL);
        assert(source.address_space.generation == generation);
        assert(guest_page_backing_test_allocation_count() == fail_at + 1);
        assert(guest_page_backing_test_live_count() == live_before);
        guest_page_table_destroy(&failed);

        assert(guest_page_table_unmap(&source, shared_range.first) ==
                GUEST_PAGE_TABLE_OK);
        assert(guest_page_backing_test_live_count() == live_before - 1);
        guest_page_backing_test_fail_allocation_at(SIZE_MAX);
        assert(guest_page_table_map_zero_shared_range(&source, shared_range,
                GUEST_MEMORY_READ | GUEST_MEMORY_WRITE, false) ==
                GUEST_PAGE_TABLE_OK);
        shared_page = lookup_page(&source, shared_range.first,
                GUEST_MEMORY_READ | GUEST_MEMORY_WRITE);
        shared_page[0] = 0x7a;
        assert(guest_page_backing_test_live_count() == live_before);
    }

    guest_page_table_test_fail_clone_allocation_at(SIZE_MAX);
    guest_page_backing_test_fail_allocation_at(SIZE_MAX);
    guest_page_table_destroy(&source);
    assert(guest_page_backing_test_live_count() == 0);
}

static void test_independent_clone(void) {
    struct guest_page_table source;
    assert(guest_page_table_init(&source, 48));
    byte_t *first;
    byte_t *second;
    byte_t *third;
    byte_t *top;
    byte_t *sibling_leaf;
    byte_t *sibling_level2;
    const guest_addr_t top_page = (UINT64_C(1) << 48) -
            GUEST_MEMORY_PAGE_SIZE;
    assert(guest_page_table_map(&source, HIGH_PAGE,
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE, &first) ==
            GUEST_PAGE_TABLE_OK);
    assert(guest_page_table_map(&source, NEXT_PAGE,
            GUEST_MEMORY_READ, &second) == GUEST_PAGE_TABLE_OK);
    assert(guest_page_table_map(&source, RANGE_BASE,
            0, &third) == GUEST_PAGE_TABLE_OK);
    assert(guest_page_table_map(&source, top_page,
            GUEST_MEMORY_EXECUTE, &top) == GUEST_PAGE_TABLE_OK);
    assert(guest_page_table_map(&source, SIBLING_LEAF_PAGE,
            GUEST_MEMORY_READ, &sibling_leaf) == GUEST_PAGE_TABLE_OK);
    assert(guest_page_table_map(&source, SIBLING_LEVEL2_PAGE,
            GUEST_MEMORY_READ, &sibling_level2) == GUEST_PAGE_TABLE_OK);
    for (size_t i = 0; i < GUEST_MEMORY_PAGE_SIZE; i++) {
        first[i] = (byte_t) (i * 17 + 3);
        second[i] = (byte_t) (i * 29 + 5);
        third[i] = (byte_t) (i * 31 + 7);
        top[i] = (byte_t) (i * 37 + 11);
        sibling_leaf[i] = (byte_t) (i * 41 + 13);
        sibling_level2[i] = (byte_t) (i * 43 + 17);
    }
    qword_t source_reservation;
    source_reservation = guest_address_space_track_exclusive(
            &source.address_space, HIGH_PAGE);

    guest_page_table_test_fail_clone_allocation_at(SIZE_MAX);
    guest_page_backing_test_fail_allocation_at(SIZE_MAX);
    struct guest_page_table copy;
    assert(guest_page_table_clone(&copy, &source));
    size_t clone_allocations =
            guest_page_table_test_clone_allocation_count();
    size_t backing_clone_allocations =
            guest_page_backing_test_allocation_count();
    assert(clone_allocations > 4);
    assert(backing_clone_allocations == 6);
    assert(copy.address_space.address_bits ==
            source.address_space.address_bits);
    assert(copy.address_space.generation ==
            source.address_space.generation);
    assert(guest_address_space_exclusive_matches(&source.address_space,
            HIGH_PAGE, source_reservation));
    assert(!guest_address_space_exclusive_matches(&copy.address_space,
            HIGH_PAGE, source_reservation));
    assert(copy.address_space.opaque == &copy &&
            source.address_space.opaque == &source);

    byte_t *first_copy = lookup_page(&copy, HIGH_PAGE,
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE);
    byte_t *second_copy = lookup_page(&copy, NEXT_PAGE,
            GUEST_MEMORY_READ);
    byte_t *third_copy = lookup_page(&copy, RANGE_BASE, 0);
    byte_t *top_copy = lookup_page(&copy, top_page,
            GUEST_MEMORY_EXECUTE);
    byte_t *sibling_leaf_copy = lookup_page(&copy,
            SIBLING_LEAF_PAGE, GUEST_MEMORY_READ);
    byte_t *sibling_level2_copy = lookup_page(&copy,
            SIBLING_LEVEL2_PAGE, GUEST_MEMORY_READ);
    assert(first_copy != first && second_copy != second &&
            third_copy != third && top_copy != top &&
            sibling_leaf_copy != sibling_leaf &&
            sibling_level2_copy != sibling_level2);
    assert(memcmp(first_copy, first, GUEST_MEMORY_PAGE_SIZE) == 0 &&
            memcmp(second_copy, second, GUEST_MEMORY_PAGE_SIZE) == 0 &&
            memcmp(third_copy, third, GUEST_MEMORY_PAGE_SIZE) == 0 &&
            memcmp(top_copy, top, GUEST_MEMORY_PAGE_SIZE) == 0 &&
            memcmp(sibling_leaf_copy, sibling_leaf,
                    GUEST_MEMORY_PAGE_SIZE) == 0 &&
            memcmp(sibling_level2_copy, sibling_level2,
                    GUEST_MEMORY_PAGE_SIZE) == 0);
    assert_not_mapped(&copy, RANGE_BASE + GUEST_MEMORY_PAGE_SIZE);

    qword_t source_generation = source.address_space.generation;
    for (size_t fail_at = 0; fail_at < clone_allocations; fail_at++) {
        unsigned live_before = guest_page_backing_test_live_count();
        guest_page_table_test_fail_clone_allocation_at(fail_at);
        struct guest_page_table failed;
        assert(!guest_page_table_clone(&failed, &source));
        assert(failed.root == NULL &&
                source.address_space.generation == source_generation);
        assert(lookup_page(&source, HIGH_PAGE,
                GUEST_MEMORY_READ | GUEST_MEMORY_WRITE) == first);
        assert(lookup_page(&source, NEXT_PAGE,
                GUEST_MEMORY_READ) == second);
        assert(lookup_page(&source, RANGE_BASE, 0) == third);
        assert(lookup_page(&source, top_page,
                GUEST_MEMORY_EXECUTE) == top);
        assert(lookup_page(&source, SIBLING_LEAF_PAGE,
                GUEST_MEMORY_READ) == sibling_leaf);
        assert(lookup_page(&source, SIBLING_LEVEL2_PAGE,
                GUEST_MEMORY_READ) == sibling_level2);
        guest_page_table_destroy(&failed);
        assert(guest_page_backing_test_live_count() == live_before);
    }
    guest_page_table_test_fail_clone_allocation_at(SIZE_MAX);

    for (size_t fail_at = 0;
            fail_at < backing_clone_allocations; fail_at++) {
        unsigned live_before = guest_page_backing_test_live_count();
        guest_page_backing_test_fail_allocation_at(fail_at);
        struct guest_page_table failed;
        assert(!guest_page_table_clone(&failed, &source));
        assert(failed.root == NULL &&
                source.address_space.generation == source_generation);
        assert(guest_page_backing_test_allocation_count() == fail_at + 1);
        assert(lookup_page(&source, HIGH_PAGE,
                GUEST_MEMORY_READ | GUEST_MEMORY_WRITE) == first);
        assert(lookup_page(&source, SIBLING_LEVEL2_PAGE,
                GUEST_MEMORY_READ) == sibling_level2);
        guest_page_table_destroy(&failed);
        assert(guest_page_backing_test_live_count() == live_before);
    }
    guest_page_backing_test_fail_allocation_at(SIZE_MAX);

    first_copy[0] ^= 0xff;
    second[1] ^= 0xff;
    assert(first_copy[0] != first[0] && second_copy[1] != second[1]);
    assert(guest_page_table_protect(&copy, NEXT_PAGE,
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE) ==
            GUEST_PAGE_TABLE_OK);
    lookup_page(&source, NEXT_PAGE, GUEST_MEMORY_READ);
    assert(guest_page_table_unmap(&source, RANGE_BASE) ==
            GUEST_PAGE_TABLE_OK);
    lookup_page(&copy, RANGE_BASE, 0);

    struct guest_tlb copy_tlb;
    guest_tlb_init(&copy_tlb, &copy.address_space);
    struct guest_memory_fault fault;
    byte_t value = 0x42;
    assert(guest_tlb_write(&copy_tlb, NEXT_PAGE,
            &value, sizeof(value), &fault));
    assert(second_copy[0] == value && second[0] != value);

    struct guest_page_table empty_source;
    struct guest_page_table empty_copy;
    assert(guest_page_table_init(&empty_source, 13));
    assert(guest_page_table_clone(&empty_copy, &empty_source));
    assert(empty_copy.root != empty_source.root &&
            empty_copy.address_space.address_bits == 13);
    assert_not_mapped(&empty_copy, GUEST_MEMORY_PAGE_SIZE);
    guest_page_table_destroy(&empty_copy);
    guest_page_table_destroy(&empty_source);

    struct guest_page_table invalid = {0};
    struct guest_page_table rejected = {0};
    assert(!guest_page_table_clone(&rejected, &invalid));
    assert(rejected.root == NULL);
    assert(!guest_page_table_clone(&copy, &copy));
    assert(!guest_page_table_clone(NULL, &source));
    assert(!guest_page_table_clone(&rejected, NULL));

    guest_page_table_destroy(&copy);
    guest_page_table_destroy(&source);
}

int main(void) {
    guest_page_backing_test_fail_allocation_at(SIZE_MAX);
    assert(guest_page_backing_test_live_count() == 0);
    test_single_page_operations();
    test_page_origins();
    test_supported_address_widths();
    test_backing_allocation_failures();
    test_shared_map_rollback();
    test_mapping_kind_replacement();
    test_atomic_map_and_replace();
    test_conflicts_and_holes();
    test_tree_and_address_boundaries();
    test_shared_clone_lifecycle();
    test_mixed_clone_failures();
    test_independent_clone();
    assert(guest_page_backing_test_live_count() == 0);
    return 0;
}
