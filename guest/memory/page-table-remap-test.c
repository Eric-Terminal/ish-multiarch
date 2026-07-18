#include <assert.h>
#include <string.h>

#include "guest/memory/page-backing.h"
#include "guest/memory/page-table.h"
#include "guest/memory/tlb.h"

#define MIXED_SOURCE UINT64_C(0x000020001ff000)
#define MIXED_DESTINATION UINT64_C(0x000040003ff000)
#define ALIAS_SOURCE UINT64_C(0x0000600000200000)
#define ALIAS_DESTINATION UINT64_C(0x0000600040200000)
#define PRIVATE_SOURCE UINT64_C(0x0000700000400000)
#define PRIVATE_DESTINATION UINT64_C(0x0000700040400000)
#define OOM_SOURCE UINT64_C(0x00000010001ff000)
#define OOM_DESTINATION UINT64_C(0x00005000001ff000)
#define OOM_SOURCE_PAGE_COUNT 2
#define EXPANDED_PAGE_COUNT 4
#define SHARED_PATH_BOUNDARY UINT64_C(0x0000003000200000)

struct file_source_probe {
    unsigned releases;
};

static void release_file_source(void *opaque) {
    struct file_source_probe *probe = opaque;
    probe->releases++;
}

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

static void reset_allocation_failures(void) {
    guest_page_backing_test_fail_allocation_at(SIZE_MAX);
    guest_page_table_test_fail_remap_allocation_at(SIZE_MAX);
}

static void map_oom_source(struct guest_page_table *table,
        unsigned permissions, byte_t *source_pages[OOM_SOURCE_PAGE_COUNT]) {
    const struct guest_page_range source = {
        .first = OOM_SOURCE,
        .page_count = OOM_SOURCE_PAGE_COUNT,
    };
    assert(guest_page_table_map_zero_range(table, source,
            permissions, false) == GUEST_PAGE_TABLE_OK);
    for (size_t index = 0; index < OOM_SOURCE_PAGE_COUNT; index++) {
        source_pages[index] = lookup_page(table,
                OOM_SOURCE + index * GUEST_MEMORY_PAGE_SIZE,
                permissions);
        source_pages[index][0] = (byte_t) (UINT8_C(0x61) + index);
    }
}

static void assert_oom_source_unchanged(struct guest_page_table *table,
        unsigned permissions,
        byte_t *const source_pages[OOM_SOURCE_PAGE_COUNT]) {
    for (size_t index = 0; index < OOM_SOURCE_PAGE_COUNT; index++) {
        byte_t *page = lookup_page(table,
                OOM_SOURCE + index * GUEST_MEMORY_PAGE_SIZE,
                permissions);
        assert(page == source_pages[index] &&
                page[0] == (byte_t) (UINT8_C(0x61) + index));
    }
}

static void assert_destination_not_mapped(struct guest_page_table *table,
        size_t page_count) {
    for (size_t index = 0; index < page_count; index++) {
        assert_not_mapped(table,
                OOM_DESTINATION + index * GUEST_MEMORY_PAGE_SIZE);
    }
}

static struct guest_page_view resolve_view(struct guest_page_table *table,
        guest_addr_t page) {
    struct guest_page_view view;
    assert(guest_address_space_resolve_page(&table->address_space,
            page, GUEST_MEMORY_READ, &view) == GUEST_MEMORY_FAULT_NONE);
    return view;
}

static void assert_same_view(const struct guest_page_view *expected,
        const struct guest_page_view *actual) {
    assert(actual->host_page == expected->host_page);
    assert(actual->permissions == expected->permissions);
    assert(actual->origin == expected->origin);
    assert(actual->backing_identity == expected->backing_identity);
    assert(actual->file_identity == expected->file_identity);
    assert(actual->file_offset == expected->file_offset);
    assert(actual->copy_on_write == expected->copy_on_write);
    assert(actual->sync == expected->sync);
    assert(actual->access_sync == expected->access_sync);
}

static void test_move_preserves_mixed_mapping_state(void) {
    assert(guest_page_backing_test_live_count() == 0);
    struct guest_page_table table;
    assert(guest_page_table_init(&table, 48));
    guest_page_table_test_fail_remap_allocation_at(SIZE_MAX);

    const unsigned read_write = GUEST_MEMORY_READ | GUEST_MEMORY_WRITE;
    byte_t *private_page;
    assert(guest_page_table_map(&table, MIXED_SOURCE,
            read_write, &private_page) == GUEST_PAGE_TABLE_OK);
    private_page[0] = UINT8_C(0x31);

    const struct guest_page_range shared_range = {
        .first = MIXED_SOURCE + 2 * GUEST_MEMORY_PAGE_SIZE,
        .page_count = 1,
    };
    assert(guest_page_table_map_zero_shared_range(&table, shared_range,
            read_write, false) == GUEST_PAGE_TABLE_OK);
    lookup_page(&table, shared_range.first, read_write)[0] = UINT8_C(0x52);

    struct file_source_probe source_probe = {0};
    struct guest_file_source *source = guest_file_source_create(
            UINT64_C(0x1234abcd), &source_probe, release_file_source);
    assert(source != NULL);
    byte_t *private_file_page;
    assert(guest_page_table_map_file(&table,
            MIXED_SOURCE + 3 * GUEST_MEMORY_PAGE_SIZE,
            read_write, source, 7 * GUEST_MEMORY_PAGE_SIZE,
            &private_file_page) == GUEST_PAGE_TABLE_OK);
    private_file_page[0] = UINT8_C(0x73);

    struct guest_page_backing *shared_file_backing =
            guest_page_backing_create();
    assert(shared_file_backing != NULL);
    guest_page_backing_bytes(shared_file_backing)[0] = UINT8_C(0x94);
    assert(guest_page_table_map_shared_file_backing(&table,
            MIXED_SOURCE + 4 * GUEST_MEMORY_PAGE_SIZE,
            read_write, source, 11 * GUEST_MEMORY_PAGE_SIZE,
            shared_file_backing) == GUEST_PAGE_TABLE_OK);
    guest_page_backing_release(shared_file_backing);
    guest_file_source_release(source);
    assert(source_probe.releases == 0);

    const bool resident[] = {true, false, true, true, true};
    struct guest_page_view before[array_size(resident)];
    for (size_t index = 0; index < array_size(resident); index++) {
        guest_addr_t page = MIXED_SOURCE +
                index * GUEST_MEMORY_PAGE_SIZE;
        if (resident[index])
            before[index] = resolve_view(&table, page);
        else
            assert_not_mapped(&table, page);
    }

    struct guest_tlb tlb;
    guest_tlb_init(&tlb, &table.address_space);
    struct guest_memory_fault fault;
    byte_t exclusive_value;
    struct guest_tlb_exclusive_token token;
    assert(guest_tlb_load_exclusive(&tlb, MIXED_SOURCE,
            &exclusive_value, sizeof(exclusive_value), &token, &fault));
    assert(exclusive_value == UINT8_C(0x31));

    qword_t generation = table.address_space.generation;
    const struct guest_page_range source_range = {
        .first = MIXED_SOURCE,
        .page_count = array_size(resident),
    };
    assert(guest_page_table_move_range(&table, source_range,
            MIXED_DESTINATION) == GUEST_PAGE_TABLE_OK);
    assert(table.address_space.generation == generation + 1);

    const byte_t replacement = UINT8_C(0xaa);
    assert(guest_tlb_store_exclusive(&tlb, MIXED_SOURCE,
            &exclusive_value, &replacement, sizeof(replacement),
            token, &fault) == GUEST_TLB_EXCLUSIVE_FAILED);
    assert(!guest_tlb_read(&tlb, MIXED_SOURCE,
            &exclusive_value, sizeof(exclusive_value),
            GUEST_MEMORY_READ, &fault));
    assert(fault.kind == GUEST_MEMORY_FAULT_UNMAPPED);

    for (size_t index = 0; index < array_size(resident); index++) {
        guest_addr_t source_page = MIXED_SOURCE +
                index * GUEST_MEMORY_PAGE_SIZE;
        guest_addr_t destination_page = MIXED_DESTINATION +
                index * GUEST_MEMORY_PAGE_SIZE;
        assert_not_mapped(&table, source_page);
        if (resident[index]) {
            struct guest_page_view after =
                    resolve_view(&table, destination_page);
            assert_same_view(&before[index], &after);
        } else {
            assert_not_mapped(&table, destination_page);
        }
    }
    assert(lookup_page(&table, MIXED_DESTINATION, read_write)[0] ==
            UINT8_C(0x31));

    guest_page_table_destroy(&table);
    assert(source_probe.releases == 1);
    assert(guest_page_backing_test_live_count() == 0);
}

static void test_alias_and_zero_source_replacement(void) {
    assert(guest_page_backing_test_live_count() == 0);
    struct guest_page_table table;
    assert(guest_page_table_init(&table, 48));
    guest_page_table_test_fail_remap_allocation_at(SIZE_MAX);
    guest_page_backing_test_fail_allocation_at(SIZE_MAX);
    const unsigned permissions = GUEST_MEMORY_READ | GUEST_MEMORY_WRITE;

    const struct guest_page_range shared = {
        .first = ALIAS_SOURCE,
        .page_count = 2,
    };
    assert(guest_page_table_map_zero_shared_range(&table, shared,
            permissions, false) == GUEST_PAGE_TABLE_OK);
    byte_t *shared_pages[2];
    for (size_t index = 0; index < array_size(shared_pages); index++) {
        shared_pages[index] = lookup_page(&table,
                shared.first + index * GUEST_MEMORY_PAGE_SIZE,
                permissions);
        shared_pages[index][0] = (byte_t) (UINT8_C(0x40) + index);
    }
    qword_t generation = table.address_space.generation;
    assert(guest_page_table_alias_shared_range(&table, shared,
            ALIAS_DESTINATION) == GUEST_PAGE_TABLE_OK);
    assert(table.address_space.generation == generation + 1);
    for (size_t index = 0; index < array_size(shared_pages); index++) {
        assert(lookup_page(&table,
                ALIAS_DESTINATION + index * GUEST_MEMORY_PAGE_SIZE,
                permissions) == shared_pages[index]);
    }

    const struct guest_page_range private = {
        .first = PRIVATE_SOURCE,
        .page_count = 2,
    };
    assert(guest_page_table_map_zero_range(&table, private,
            permissions, false) == GUEST_PAGE_TABLE_OK);
    byte_t *private_pages[2];
    for (size_t index = 0; index < array_size(private_pages); index++) {
        private_pages[index] = lookup_page(&table,
                private.first + index * GUEST_MEMORY_PAGE_SIZE,
                permissions);
        private_pages[index][0] = (byte_t) (UINT8_C(0x70) + index);
    }
    generation = table.address_space.generation;
    assert(guest_page_table_alias_shared_range(&table, private,
            PRIVATE_DESTINATION) == GUEST_PAGE_TABLE_INVALID_ADDRESS);
    assert(table.address_space.generation == generation);
    assert_not_mapped(&table, PRIVATE_DESTINATION);

    guest_page_backing_test_fail_allocation_at(0);
    assert(guest_page_table_move_range_replace_zero(&table, private,
            PRIVATE_DESTINATION, GUEST_MEMORY_READ) ==
            GUEST_PAGE_TABLE_OUT_OF_MEMORY);
    assert(table.address_space.generation == generation);
    for (size_t index = 0; index < array_size(private_pages); index++) {
        assert(lookup_page(&table,
                private.first + index * GUEST_MEMORY_PAGE_SIZE,
                permissions) == private_pages[index]);
        assert_not_mapped(&table,
                PRIVATE_DESTINATION + index * GUEST_MEMORY_PAGE_SIZE);
    }

    guest_page_backing_test_fail_allocation_at(SIZE_MAX);
    guest_page_table_test_fail_remap_allocation_at(0);
    assert(guest_page_table_move_range_replace_zero(&table, private,
            PRIVATE_DESTINATION, GUEST_MEMORY_READ) ==
            GUEST_PAGE_TABLE_OUT_OF_MEMORY);
    assert(table.address_space.generation == generation);
    guest_page_table_test_fail_remap_allocation_at(SIZE_MAX);

    assert(guest_page_table_move_range_replace_zero(&table, private,
            PRIVATE_DESTINATION, GUEST_MEMORY_READ) == GUEST_PAGE_TABLE_OK);
    assert(table.address_space.generation == generation + 1);
    for (size_t index = 0; index < array_size(private_pages); index++) {
        byte_t *source_page = lookup_page(&table,
                private.first + index * GUEST_MEMORY_PAGE_SIZE,
                GUEST_MEMORY_READ);
        byte_t *destination_page = lookup_page(&table,
                PRIVATE_DESTINATION + index * GUEST_MEMORY_PAGE_SIZE,
                permissions);
        assert(source_page != private_pages[index] && source_page[0] == 0);
        assert(destination_page == private_pages[index] &&
                destination_page[0] == (byte_t) (UINT8_C(0x70) + index));
        assert(resolve_view(&table,
                private.first + index * GUEST_MEMORY_PAGE_SIZE).sync == NULL);
    }

    guest_page_table_destroy(&table);
    assert(guest_page_backing_test_live_count() == 0);
}

static size_t successful_move_allocation_count(void) {
    assert(guest_page_backing_test_live_count() == 0);
    reset_allocation_failures();
    struct guest_page_table table;
    assert(guest_page_table_init(&table, 48));
    const struct guest_page_range source = {
        .first = OOM_SOURCE,
        .page_count = OOM_SOURCE_PAGE_COUNT,
    };
    assert(guest_page_table_map_zero_range(&table, source,
            GUEST_MEMORY_READ, false) == GUEST_PAGE_TABLE_OK);
    guest_page_table_test_fail_remap_allocation_at(SIZE_MAX);
    assert(guest_page_table_move_range(&table, source,
            OOM_DESTINATION) == GUEST_PAGE_TABLE_OK);
    size_t count = guest_page_table_test_remap_allocation_count();
    guest_page_table_destroy(&table);
    assert(guest_page_backing_test_live_count() == 0);
    return count;
}

static void test_validation_collision_and_allocation_failures(void) {
    reset_allocation_failures();
    struct guest_page_table table;
    assert(guest_page_table_init(&table, 48));
    const struct guest_page_range source = {
        .first = OOM_SOURCE,
        .page_count = OOM_SOURCE_PAGE_COUNT,
    };
    byte_t *source_pages[OOM_SOURCE_PAGE_COUNT];
    map_oom_source(&table, GUEST_MEMORY_READ, source_pages);
    byte_t *collision;
    assert(guest_page_table_map(&table,
            OOM_DESTINATION + GUEST_MEMORY_PAGE_SIZE,
            GUEST_MEMORY_READ, &collision) == GUEST_PAGE_TABLE_OK);
    qword_t generation = table.address_space.generation;
    assert(guest_page_table_move_range(&table, source,
            OOM_DESTINATION) == GUEST_PAGE_TABLE_ALREADY_MAPPED);
    assert(table.address_space.generation == generation);
    assert(lookup_page(&table, OOM_SOURCE,
            GUEST_MEMORY_READ) == source_pages[0]);
    assert_not_mapped(&table, OOM_DESTINATION);
    assert(lookup_page(&table,
            OOM_DESTINATION + GUEST_MEMORY_PAGE_SIZE,
            GUEST_MEMORY_READ) == collision);
    assert(guest_page_table_move_range(&table, source,
            OOM_SOURCE + GUEST_MEMORY_PAGE_SIZE) ==
            GUEST_PAGE_TABLE_INVALID_ADDRESS);
    assert(guest_page_table_move_range(&table, source,
            OOM_DESTINATION + 1) == GUEST_PAGE_TABLE_INVALID_ADDRESS);
    assert(guest_page_table_move_range(&table, source,
            UINT64_C(1) << 48) == GUEST_PAGE_TABLE_INVALID_ADDRESS);
    const struct guest_page_range empty = {
        .first = OOM_SOURCE,
        .page_count = 0,
    };
    assert(guest_page_table_move_range(&table, empty,
            OOM_DESTINATION) == GUEST_PAGE_TABLE_OK);
    assert(table.address_space.generation == generation);
    guest_page_table_destroy(&table);

    size_t allocation_count = successful_move_allocation_count();
    // 两页跨过 2 MiB 边界，目标至少需要三级路径和第二个 leaf。
    assert(allocation_count >= 4);
    for (size_t fail_at = 0; fail_at < allocation_count; fail_at++) {
        assert(guest_page_backing_test_live_count() == 0);
        assert(guest_page_table_init(&table, 48));
        map_oom_source(&table, GUEST_MEMORY_READ, source_pages);
        generation = table.address_space.generation;
        unsigned live = guest_page_backing_test_live_count();
        guest_page_table_test_fail_remap_allocation_at(fail_at);
        assert(guest_page_table_move_range(&table, source,
                OOM_DESTINATION) == GUEST_PAGE_TABLE_OUT_OF_MEMORY);
        assert(table.address_space.generation == generation);
        assert(guest_page_backing_test_live_count() == live);
        assert_oom_source_unchanged(
                &table, GUEST_MEMORY_READ, source_pages);
        assert_destination_not_mapped(&table, OOM_SOURCE_PAGE_COUNT);

        guest_page_table_test_fail_remap_allocation_at(SIZE_MAX);
        assert(guest_page_table_move_range(&table, source,
                OOM_DESTINATION) == GUEST_PAGE_TABLE_OK);
        assert(table.address_space.generation == generation + 1);
        assert(guest_page_backing_test_live_count() == live);
        for (size_t index = 0; index < array_size(source_pages); index++) {
            assert_not_mapped(&table,
                    OOM_SOURCE + index * GUEST_MEMORY_PAGE_SIZE);
            byte_t *destination_page = lookup_page(&table,
                    OOM_DESTINATION + index * GUEST_MEMORY_PAGE_SIZE,
                    GUEST_MEMORY_READ);
            assert(destination_page == source_pages[index] &&
                    destination_page[0] ==
                            (byte_t) (UINT8_C(0x61) + index));
        }
        guest_page_table_destroy(&table);
    }
    assert(guest_page_backing_test_live_count() == 0);
}

struct allocation_counts {
    size_t remap;
    size_t backing;
};

static void assert_expand_zero_success(struct guest_page_table *table,
        byte_t *const source_pages[OOM_SOURCE_PAGE_COUNT]) {
    const unsigned read_write = GUEST_MEMORY_READ | GUEST_MEMORY_WRITE;
    for (size_t index = 0; index < OOM_SOURCE_PAGE_COUNT; index++) {
        assert_not_mapped(table,
                OOM_SOURCE + index * GUEST_MEMORY_PAGE_SIZE);
        byte_t *destination_page = lookup_page(table,
                OOM_DESTINATION + index * GUEST_MEMORY_PAGE_SIZE,
                GUEST_MEMORY_READ);
        assert(destination_page == source_pages[index] &&
                destination_page[0] ==
                        (byte_t) (UINT8_C(0x61) + index));
    }
    for (size_t index = OOM_SOURCE_PAGE_COUNT;
            index < EXPANDED_PAGE_COUNT; index++) {
        byte_t *destination_page = lookup_page(table,
                OOM_DESTINATION + index * GUEST_MEMORY_PAGE_SIZE,
                read_write);
        assert(destination_page[0] == 0);
    }
}

static void assert_replace_zero_success(struct guest_page_table *table,
        byte_t *const source_pages[OOM_SOURCE_PAGE_COUNT]) {
    const unsigned read_write = GUEST_MEMORY_READ | GUEST_MEMORY_WRITE;
    for (size_t index = 0; index < OOM_SOURCE_PAGE_COUNT; index++) {
        byte_t *source_page = lookup_page(table,
                OOM_SOURCE + index * GUEST_MEMORY_PAGE_SIZE,
                GUEST_MEMORY_READ);
        byte_t *destination_page = lookup_page(table,
                OOM_DESTINATION + index * GUEST_MEMORY_PAGE_SIZE,
                read_write);
        assert(source_page != source_pages[index] && source_page[0] == 0);
        assert(destination_page == source_pages[index] &&
                destination_page[0] ==
                        (byte_t) (UINT8_C(0x61) + index));
    }
}

static struct allocation_counts successful_expand_zero_allocation_counts(
        void) {
    assert(guest_page_backing_test_live_count() == 0);
    reset_allocation_failures();
    struct guest_page_table table;
    assert(guest_page_table_init(&table, 48));
    byte_t *source_pages[OOM_SOURCE_PAGE_COUNT];
    map_oom_source(&table, GUEST_MEMORY_READ, source_pages);
    reset_allocation_failures();

    const struct guest_page_range source = {
        .first = OOM_SOURCE,
        .page_count = OOM_SOURCE_PAGE_COUNT,
    };
    qword_t generation = table.address_space.generation;
    assert(guest_page_table_move_range_expand_zero(&table, source,
            OOM_DESTINATION, EXPANDED_PAGE_COUNT,
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE) ==
            GUEST_PAGE_TABLE_OK);
    struct allocation_counts counts = {
        .remap = guest_page_table_test_remap_allocation_count(),
        .backing = guest_page_backing_test_allocation_count(),
    };
    assert(table.address_space.generation == generation + 1);
    assert_expand_zero_success(&table, source_pages);
    guest_page_table_destroy(&table);
    assert(guest_page_backing_test_live_count() == 0);
    return counts;
}

static void run_expand_zero_failure_and_retry(
        size_t remap_fail_at, size_t backing_fail_at) {
    assert((remap_fail_at == SIZE_MAX) !=
            (backing_fail_at == SIZE_MAX));
    assert(guest_page_backing_test_live_count() == 0);
    reset_allocation_failures();
    struct guest_page_table table;
    assert(guest_page_table_init(&table, 48));
    byte_t *source_pages[OOM_SOURCE_PAGE_COUNT];
    map_oom_source(&table, GUEST_MEMORY_READ, source_pages);
    unsigned live = guest_page_backing_test_live_count();
    qword_t generation = table.address_space.generation;

    guest_page_table_test_fail_remap_allocation_at(remap_fail_at);
    guest_page_backing_test_fail_allocation_at(backing_fail_at);
    const struct guest_page_range source = {
        .first = OOM_SOURCE,
        .page_count = OOM_SOURCE_PAGE_COUNT,
    };
    assert(guest_page_table_move_range_expand_zero(&table, source,
            OOM_DESTINATION, EXPANDED_PAGE_COUNT,
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE) ==
            GUEST_PAGE_TABLE_OUT_OF_MEMORY);
    assert(table.address_space.generation == generation);
    assert(guest_page_backing_test_live_count() == live);
    assert_oom_source_unchanged(
            &table, GUEST_MEMORY_READ, source_pages);
    assert_destination_not_mapped(&table, EXPANDED_PAGE_COUNT);

    reset_allocation_failures();
    assert(guest_page_table_move_range_expand_zero(&table, source,
            OOM_DESTINATION, EXPANDED_PAGE_COUNT,
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE) ==
            GUEST_PAGE_TABLE_OK);
    assert(table.address_space.generation == generation + 1);
    assert(guest_page_backing_test_live_count() ==
            live + EXPANDED_PAGE_COUNT - OOM_SOURCE_PAGE_COUNT);
    assert_expand_zero_success(&table, source_pages);
    guest_page_table_destroy(&table);
    assert(guest_page_backing_test_live_count() == 0);
}

static void test_expand_zero_allocation_failures(void) {
    struct allocation_counts counts =
            successful_expand_zero_allocation_counts();
    // 指针数组、三级目标路径和跨边界的第二个 leaf 都必须可注入失败。
    assert(counts.remap >= 5);
    assert(counts.backing ==
            EXPANDED_PAGE_COUNT - OOM_SOURCE_PAGE_COUNT);
    for (size_t fail_at = 0; fail_at < counts.remap; fail_at++)
        run_expand_zero_failure_and_retry(fail_at, SIZE_MAX);
    for (size_t fail_at = 0; fail_at < counts.backing; fail_at++)
        run_expand_zero_failure_and_retry(SIZE_MAX, fail_at);
}

static size_t successful_shared_path_expand_allocation_count(void) {
    reset_allocation_failures();
    struct guest_page_table table;
    assert(guest_page_table_init(&table, 48));
    guest_addr_t source_page =
            SHARED_PATH_BOUNDARY + GUEST_MEMORY_PAGE_SIZE;
    byte_t *source;
    assert(guest_page_table_map(&table, source_page,
            GUEST_MEMORY_READ, &source) == GUEST_PAGE_TABLE_OK);
    source[0] = UINT8_C(0x9b);
    reset_allocation_failures();

    const struct guest_page_range source_range = {
        .first = source_page,
        .page_count = 1,
    };
    qword_t generation = table.address_space.generation;
    unsigned live = guest_page_backing_test_live_count();
    guest_addr_t destination =
            SHARED_PATH_BOUNDARY - GUEST_MEMORY_PAGE_SIZE;
    assert(guest_page_table_move_range_expand_zero(&table, source_range,
            destination, 2, GUEST_MEMORY_READ | GUEST_MEMORY_WRITE) ==
            GUEST_PAGE_TABLE_OK);
    size_t allocation_count =
            guest_page_table_test_remap_allocation_count();
    assert(table.address_space.generation == generation + 1);
    assert_not_mapped(&table, source_page);
    assert(lookup_page(&table, destination,
            GUEST_MEMORY_READ) == source);
    assert(lookup_page(&table, SHARED_PATH_BOUNDARY,
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE)[0] == 0);
    assert(guest_page_backing_test_live_count() == live + 1);
    guest_page_table_destroy(&table);
    assert(guest_page_backing_test_live_count() == 0);
    return allocation_count;
}

static void test_expand_zero_shared_preallocated_path(void) {
    size_t allocation_count =
            successful_shared_path_expand_allocation_count();
    // 指针数组与边界前一个 leaf 均须在可见提交前准备完成。
    assert(allocation_count >= 2);
    for (size_t fail_at = 0;
            fail_at < allocation_count; fail_at++) {
        reset_allocation_failures();
        struct guest_page_table table;
        assert(guest_page_table_init(&table, 48));
        guest_addr_t source_page =
                SHARED_PATH_BOUNDARY + GUEST_MEMORY_PAGE_SIZE;
        byte_t *source;
        assert(guest_page_table_map(&table, source_page,
                GUEST_MEMORY_READ, &source) == GUEST_PAGE_TABLE_OK);
        source[0] = UINT8_C(0x9b);
        unsigned live = guest_page_backing_test_live_count();
        qword_t generation = table.address_space.generation;
        guest_addr_t destination =
                SHARED_PATH_BOUNDARY - GUEST_MEMORY_PAGE_SIZE;
        guest_page_table_test_fail_remap_allocation_at(fail_at);
        const struct guest_page_range source_range = {
            .first = source_page,
            .page_count = 1,
        };
        assert(guest_page_table_move_range_expand_zero(
                &table, source_range, destination, 2,
                GUEST_MEMORY_READ | GUEST_MEMORY_WRITE) ==
                GUEST_PAGE_TABLE_OUT_OF_MEMORY);
        assert(table.address_space.generation == generation &&
                guest_page_backing_test_live_count() == live);
        assert(lookup_page(&table, source_page,
                GUEST_MEMORY_READ) == source &&
                source[0] == UINT8_C(0x9b));
        assert_not_mapped(&table, destination);
        assert_not_mapped(&table, SHARED_PATH_BOUNDARY);

        reset_allocation_failures();
        assert(guest_page_table_move_range_expand_zero(
                &table, source_range, destination, 2,
                GUEST_MEMORY_READ | GUEST_MEMORY_WRITE) ==
                GUEST_PAGE_TABLE_OK);
        assert(table.address_space.generation == generation + 1);
        assert_not_mapped(&table, source_page);
        assert(lookup_page(&table, destination,
                GUEST_MEMORY_READ) == source);
        assert(lookup_page(&table, SHARED_PATH_BOUNDARY,
                GUEST_MEMORY_READ | GUEST_MEMORY_WRITE)[0] == 0);
        guest_page_table_destroy(&table);
        assert(guest_page_backing_test_live_count() == 0);
    }
}

static struct allocation_counts successful_replace_zero_allocation_counts(
        void) {
    assert(guest_page_backing_test_live_count() == 0);
    reset_allocation_failures();
    struct guest_page_table table;
    assert(guest_page_table_init(&table, 48));
    byte_t *source_pages[OOM_SOURCE_PAGE_COUNT];
    map_oom_source(&table,
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE, source_pages);
    reset_allocation_failures();

    const struct guest_page_range source = {
        .first = OOM_SOURCE,
        .page_count = OOM_SOURCE_PAGE_COUNT,
    };
    qword_t generation = table.address_space.generation;
    assert(guest_page_table_move_range_replace_zero(&table, source,
            OOM_DESTINATION, GUEST_MEMORY_READ) == GUEST_PAGE_TABLE_OK);
    struct allocation_counts counts = {
        .remap = guest_page_table_test_remap_allocation_count(),
        .backing = guest_page_backing_test_allocation_count(),
    };
    assert(table.address_space.generation == generation + 1);
    assert_replace_zero_success(&table, source_pages);
    guest_page_table_destroy(&table);
    assert(guest_page_backing_test_live_count() == 0);
    return counts;
}

static void run_replace_zero_failure_and_retry(
        size_t remap_fail_at, size_t backing_fail_at) {
    assert((remap_fail_at == SIZE_MAX) !=
            (backing_fail_at == SIZE_MAX));
    assert(guest_page_backing_test_live_count() == 0);
    reset_allocation_failures();
    struct guest_page_table table;
    assert(guest_page_table_init(&table, 48));
    const unsigned read_write = GUEST_MEMORY_READ | GUEST_MEMORY_WRITE;
    byte_t *source_pages[OOM_SOURCE_PAGE_COUNT];
    map_oom_source(&table, read_write, source_pages);
    unsigned live = guest_page_backing_test_live_count();
    qword_t generation = table.address_space.generation;

    guest_page_table_test_fail_remap_allocation_at(remap_fail_at);
    guest_page_backing_test_fail_allocation_at(backing_fail_at);
    const struct guest_page_range source = {
        .first = OOM_SOURCE,
        .page_count = OOM_SOURCE_PAGE_COUNT,
    };
    assert(guest_page_table_move_range_replace_zero(&table, source,
            OOM_DESTINATION, GUEST_MEMORY_READ) ==
            GUEST_PAGE_TABLE_OUT_OF_MEMORY);
    assert(table.address_space.generation == generation);
    assert(guest_page_backing_test_live_count() == live);
    assert_oom_source_unchanged(&table, read_write, source_pages);
    assert_destination_not_mapped(&table, OOM_SOURCE_PAGE_COUNT);

    reset_allocation_failures();
    assert(guest_page_table_move_range_replace_zero(&table, source,
            OOM_DESTINATION, GUEST_MEMORY_READ) == GUEST_PAGE_TABLE_OK);
    assert(table.address_space.generation == generation + 1);
    assert(guest_page_backing_test_live_count() ==
            live + OOM_SOURCE_PAGE_COUNT);
    assert_replace_zero_success(&table, source_pages);
    guest_page_table_destroy(&table);
    assert(guest_page_backing_test_live_count() == 0);
}

static void test_replace_zero_allocation_failures(void) {
    struct allocation_counts counts =
            successful_replace_zero_allocation_counts();
    // 指针数组、三级目标路径和跨边界的第二个 leaf 都必须可注入失败。
    assert(counts.remap >= 5);
    assert(counts.backing == OOM_SOURCE_PAGE_COUNT);
    for (size_t fail_at = 0; fail_at < counts.remap; fail_at++)
        run_replace_zero_failure_and_retry(fail_at, SIZE_MAX);
    for (size_t fail_at = 0; fail_at < counts.backing; fail_at++)
        run_replace_zero_failure_and_retry(SIZE_MAX, fail_at);
}

int main(void) {
    reset_allocation_failures();
    test_move_preserves_mixed_mapping_state();
    test_alias_and_zero_source_replacement();
    test_validation_collision_and_allocation_failures();
    test_expand_zero_allocation_failures();
    test_expand_zero_shared_preallocated_path();
    test_replace_zero_allocation_failures();
    assert(guest_page_backing_test_live_count() == 0);
    return 0;
}
