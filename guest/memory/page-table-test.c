#include <assert.h>
#include <string.h>

#include "guest/memory/page-table.h"
#include "guest/memory/tlb.h"

#define HIGH_PAGE UINT64_C(0x0000123456789000)
#define NEXT_PAGE (HIGH_PAGE + GUEST_MEMORY_PAGE_SIZE)
#define SIBLING_LEAF_PAGE (HIGH_PAGE + (UINT64_C(1) << 21))
#define SIBLING_LEVEL2_PAGE (HIGH_PAGE + (UINT64_C(1) << 30))
#define RANGE_BASE UINT64_C(0x0000200000000000)

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

    guest_page_table_test_fail_clone_allocation_at(SIZE_MAX);
    struct guest_page_table copy;
    assert(guest_page_table_clone(&copy, &source));
    size_t clone_allocations =
            guest_page_table_test_clone_allocation_count();
    assert(clone_allocations > 4);
    assert(copy.address_space.address_bits ==
            source.address_space.address_bits);
    assert(copy.address_space.generation ==
            source.address_space.generation);
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
    }
    guest_page_table_test_fail_clone_allocation_at(SIZE_MAX);

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
    test_single_page_operations();
    test_supported_address_widths();
    test_atomic_map_and_replace();
    test_conflicts_and_holes();
    test_tree_and_address_boundaries();
    test_independent_clone();
    return 0;
}
