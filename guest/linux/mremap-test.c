#include <assert.h>
#include <string.h>

#include "guest/linux/errno.h"
#include "guest/linux/memory.h"
#include "guest/linux/mman.h"
#include "guest/memory/page-backing.h"
#include "guest/memory/tlb.h"

#define TEST_BRK UINT64_C(0x00100000)
#define TEST_BRK_LIMIT UINT64_C(0x00200000)

struct memory_fixture {
    struct guest_page_table table;
    struct guest_linux_mm memory;
    struct guest_tlb tlb;
};

static qword_t linux_error(unsigned error) {
    return (qword_t) -(sqword_t) error;
}

static void fixture_init(struct memory_fixture *fixture) {
    memset(fixture, 0, sizeof(*fixture));
    assert(guest_page_table_init(&fixture->table, 48));
    guest_linux_mm_init(&fixture->memory, &fixture->table,
            TEST_BRK, TEST_BRK_LIMIT);
    guest_page_table_enable_concurrency(&fixture->table);
    guest_tlb_init(&fixture->tlb, &fixture->table.address_space);
    guest_linux_vma_test_fail_allocation_at(SIZE_MAX);
    guest_page_table_test_fail_remap_allocation_at(SIZE_MAX);
    guest_page_backing_test_fail_allocation_at(SIZE_MAX);
}

static void fixture_destroy(struct memory_fixture *fixture) {
    guest_linux_mm_destroy(&fixture->memory);
    guest_page_table_destroy(&fixture->table);
    assert(guest_linux_vma_test_live_allocation_count() == 0);
}

static guest_addr_t map_anonymous(struct memory_fixture *fixture,
        guest_addr_t address, qword_t page_count, bool shared) {
    qword_t flags = (shared ? GUEST_LINUX_MAP_SHARED :
            GUEST_LINUX_MAP_PRIVATE) | GUEST_LINUX_MAP_ANONYMOUS |
            GUEST_LINUX_MAP_FIXED;
    qword_t result = guest_linux_mmap(&fixture->memory, address,
            page_count * GUEST_MEMORY_PAGE_SIZE,
            GUEST_LINUX_PROT_READ | GUEST_LINUX_PROT_WRITE,
            flags, UINT64_MAX, 0);
    assert(result == address);
    return address;
}

static byte_t *lookup_page(struct guest_page_table *table,
        guest_addr_t page, unsigned expected_permissions) {
    bool locked = guest_page_table_read_lock(table);
    byte_t *host_page;
    unsigned permissions;
    assert(guest_page_table_lookup(table, page,
            &host_page, &permissions) == GUEST_PAGE_TABLE_OK);
    assert(permissions == expected_permissions);
    guest_page_table_read_unlock(table, locked);
    return host_page;
}

static void assert_not_mapped(struct guest_page_table *table,
        guest_addr_t page) {
    bool locked = guest_page_table_read_lock(table);
    byte_t *host_page;
    unsigned permissions;
    assert(guest_page_table_lookup(table, page,
            &host_page, &permissions) == GUEST_PAGE_TABLE_NOT_MAPPED);
    guest_page_table_read_unlock(table, locked);
}

static void write_byte(struct guest_tlb *tlb,
        guest_addr_t address, byte_t value) {
    struct guest_memory_fault fault;
    assert(guest_tlb_write(tlb, address,
            &value, sizeof(value), &fault));
}

static byte_t read_byte(struct guest_tlb *tlb, guest_addr_t address) {
    struct guest_memory_fault fault;
    byte_t value = 0;
    assert(guest_tlb_read(tlb, address, &value, sizeof(value),
            GUEST_MEMORY_READ, &fault));
    return value;
}

static void assert_unmapped_fault(struct guest_tlb *tlb,
        guest_addr_t address) {
    struct guest_memory_fault fault;
    byte_t value = 0;
    assert(!guest_tlb_read(tlb, address, &value, sizeof(value),
            GUEST_MEMORY_READ, &fault));
    assert(fault.kind == GUEST_MEMORY_FAULT_UNMAPPED);
}

static void test_parameters_and_in_place_resize(void) {
    struct memory_fixture fixture;
    fixture_init(&fixture);
    guest_addr_t base = (guest_addr_t) fixture.memory.mmap_base +
            UINT64_C(0x100000);
    map_anonymous(&fixture, base, 1, false);
    write_byte(&fixture.tlb, base, UINT8_C(0x41));

    assert(guest_linux_mremap(&fixture.memory, base,
            2 * GUEST_MEMORY_PAGE_SIZE, 2 * GUEST_MEMORY_PAGE_SIZE,
            0, UINT64_MAX) == base);
    assert(guest_linux_mremap(&fixture.memory, base + 1,
            GUEST_MEMORY_PAGE_SIZE, GUEST_MEMORY_PAGE_SIZE,
            0, 0) == linux_error(GUEST_LINUX_EINVAL));
    assert(guest_linux_mremap(&fixture.memory, base,
            GUEST_MEMORY_PAGE_SIZE, 0, 0, 0) ==
            linux_error(GUEST_LINUX_EINVAL));
    assert(guest_linux_mremap(&fixture.memory, base,
            GUEST_MEMORY_PAGE_SIZE, GUEST_MEMORY_PAGE_SIZE,
            UINT64_C(0x80), 0) == linux_error(GUEST_LINUX_EINVAL));
    assert(guest_linux_mremap(&fixture.memory, base,
            GUEST_MEMORY_PAGE_SIZE, GUEST_MEMORY_PAGE_SIZE,
            GUEST_LINUX_MREMAP_FIXED,
            base + 8 * GUEST_MEMORY_PAGE_SIZE) ==
            linux_error(GUEST_LINUX_EINVAL));
    assert(guest_linux_mremap(&fixture.memory, base,
            GUEST_MEMORY_PAGE_SIZE, 2 * GUEST_MEMORY_PAGE_SIZE,
            GUEST_LINUX_MREMAP_MAYMOVE |
                    GUEST_LINUX_MREMAP_DONTUNMAP,
            base + 8 * GUEST_MEMORY_PAGE_SIZE) ==
            linux_error(GUEST_LINUX_EINVAL));
    assert(guest_linux_mremap(&fixture.memory, base,
            GUEST_MEMORY_PAGE_SIZE, GUEST_MEMORY_PAGE_SIZE,
            GUEST_LINUX_MREMAP_MAYMOVE |
                    GUEST_LINUX_MREMAP_FIXED,
            base) == linux_error(GUEST_LINUX_EINVAL));
    assert(guest_linux_mremap(&fixture.memory,
            base + 32 * GUEST_MEMORY_PAGE_SIZE,
            GUEST_MEMORY_PAGE_SIZE, GUEST_MEMORY_PAGE_SIZE,
            0, 0) == linux_error(GUEST_LINUX_EFAULT));
    assert(guest_linux_mremap(&fixture.memory, base, 0,
            GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_MREMAP_MAYMOVE, 0) ==
            linux_error(GUEST_LINUX_EINVAL));

    map_anonymous(&fixture, base + GUEST_MEMORY_PAGE_SIZE, 1, false);
    assert(guest_linux_mremap(&fixture.memory, base,
            GUEST_MEMORY_PAGE_SIZE, 3 * GUEST_MEMORY_PAGE_SIZE,
            0, 0) == linux_error(GUEST_LINUX_ENOMEM));
    assert(guest_linux_munmap(&fixture.memory,
            base + GUEST_MEMORY_PAGE_SIZE, GUEST_MEMORY_PAGE_SIZE) == 0);

    qword_t generation = fixture.table.address_space.generation;
    assert(guest_linux_mremap(&fixture.memory, base,
            GUEST_MEMORY_PAGE_SIZE, 3 * GUEST_MEMORY_PAGE_SIZE,
            0, 0) == base);
    assert(fixture.table.address_space.generation == generation + 1);
    assert(read_byte(&fixture.tlb, base) == UINT8_C(0x41));
    const unsigned read_write = GUEST_MEMORY_READ | GUEST_MEMORY_WRITE;
    assert(lookup_page(&fixture.table,
            base + GUEST_MEMORY_PAGE_SIZE, read_write)[0] == 0);
    assert(lookup_page(&fixture.table,
            base + 2 * GUEST_MEMORY_PAGE_SIZE, read_write)[0] == 0);

    write_byte(&fixture.tlb,
            base + GUEST_MEMORY_PAGE_SIZE, UINT8_C(0x62));
    assert(guest_linux_mremap(&fixture.memory, base,
            3 * GUEST_MEMORY_PAGE_SIZE - 1,
            GUEST_MEMORY_PAGE_SIZE + 1, 0, 0) == base);
    assert(read_byte(&fixture.tlb,
            base + GUEST_MEMORY_PAGE_SIZE) == UINT8_C(0x62));
    assert_not_mapped(&fixture.table,
            base + 2 * GUEST_MEMORY_PAGE_SIZE);

    guest_addr_t shared = base + 16 * GUEST_MEMORY_PAGE_SIZE;
    map_anonymous(&fixture, shared, 1, true);
    assert(guest_linux_mremap(&fixture.memory, shared,
            GUEST_MEMORY_PAGE_SIZE, 2 * GUEST_MEMORY_PAGE_SIZE,
            0, 0) == linux_error(GUEST_LINUX_EFAULT));

    guest_addr_t cross_vma = base + 32 * GUEST_MEMORY_PAGE_SIZE;
    map_anonymous(&fixture, cross_vma, 1, false);
    map_anonymous(&fixture,
            cross_vma + GUEST_MEMORY_PAGE_SIZE, 1, true);
    assert(guest_linux_mremap(&fixture.memory, cross_vma,
            2 * GUEST_MEMORY_PAGE_SIZE, GUEST_MEMORY_PAGE_SIZE,
            0, 0) == cross_vma);
    assert_not_mapped(&fixture.table,
            cross_vma + GUEST_MEMORY_PAGE_SIZE);

    fixture_destroy(&fixture);
    assert(guest_page_backing_test_live_count() == 0);
}

static void test_move_growth_and_failures(void) {
    struct memory_fixture fixture;
    fixture_init(&fixture);
    guest_addr_t base = (guest_addr_t) fixture.memory.mmap_base +
            UINT64_C(0x200000);
    map_anonymous(&fixture, base, 2, false);
    write_byte(&fixture.tlb, base, UINT8_C(0x21));
    write_byte(&fixture.tlb,
            base + GUEST_MEMORY_PAGE_SIZE, UINT8_C(0x32));
    map_anonymous(&fixture,
            base + 2 * GUEST_MEMORY_PAGE_SIZE, 1, false);

    qword_t generation = fixture.table.address_space.generation;
    guest_linux_vma_test_fail_allocation_at(0);
    assert(guest_linux_mremap(&fixture.memory, base,
            2 * GUEST_MEMORY_PAGE_SIZE, 3 * GUEST_MEMORY_PAGE_SIZE,
            GUEST_LINUX_MREMAP_MAYMOVE, 0) ==
            linux_error(GUEST_LINUX_ENOMEM));
    guest_linux_vma_test_fail_allocation_at(SIZE_MAX);
    assert(fixture.table.address_space.generation == generation);
    assert(read_byte(&fixture.tlb, base) == UINT8_C(0x21));
    assert(read_byte(&fixture.tlb,
            base + GUEST_MEMORY_PAGE_SIZE) == UINT8_C(0x32));

    guest_page_backing_test_fail_allocation_at(0);
    assert(guest_linux_mremap(&fixture.memory, base,
            2 * GUEST_MEMORY_PAGE_SIZE, 3 * GUEST_MEMORY_PAGE_SIZE,
            GUEST_LINUX_MREMAP_MAYMOVE, 0) ==
            linux_error(GUEST_LINUX_ENOMEM));
    guest_page_backing_test_fail_allocation_at(SIZE_MAX);
    assert(fixture.table.address_space.generation == generation);
    assert(read_byte(&fixture.tlb, base) == UINT8_C(0x21));
    assert(read_byte(&fixture.tlb,
            base + GUEST_MEMORY_PAGE_SIZE) == UINT8_C(0x32));

    qword_t moved_result = guest_linux_mremap(&fixture.memory, base,
            2 * GUEST_MEMORY_PAGE_SIZE, 3 * GUEST_MEMORY_PAGE_SIZE,
            GUEST_LINUX_MREMAP_MAYMOVE, 0);
    assert((sqword_t) moved_result >= 0 && moved_result != base);
    guest_addr_t moved = (guest_addr_t) moved_result;
    assert_unmapped_fault(&fixture.tlb, base);
    assert(read_byte(&fixture.tlb, moved) == UINT8_C(0x21));
    assert(read_byte(&fixture.tlb,
            moved + GUEST_MEMORY_PAGE_SIZE) == UINT8_C(0x32));
    assert(read_byte(&fixture.tlb,
            moved + 2 * GUEST_MEMORY_PAGE_SIZE) == 0);
    assert(read_byte(&fixture.tlb,
            base + 2 * GUEST_MEMORY_PAGE_SIZE) == 0);

    fixture_destroy(&fixture);
    assert(guest_page_backing_test_live_count() == 0);

    fixture_init(&fixture);
    base = (guest_addr_t) fixture.memory.mmap_base + UINT64_C(0x300000);
    guest_addr_t target = base + (UINT64_C(1) << 22);
    map_anonymous(&fixture, base, 1, false);
    map_anonymous(&fixture, target, 1, false);
    write_byte(&fixture.tlb, base, UINT8_C(0x55));
    write_byte(&fixture.tlb, target, UINT8_C(0xaa));

    guest_page_table_test_fail_remap_allocation_at(0);
    assert(guest_linux_mremap(&fixture.memory, base,
            GUEST_MEMORY_PAGE_SIZE, GUEST_MEMORY_PAGE_SIZE,
            GUEST_LINUX_MREMAP_MAYMOVE |
                    GUEST_LINUX_MREMAP_FIXED,
            target) == linux_error(GUEST_LINUX_ENOMEM));
    guest_page_table_test_fail_remap_allocation_at(SIZE_MAX);
    assert(read_byte(&fixture.tlb, base) == UINT8_C(0x55));
    assert_unmapped_fault(&fixture.tlb, target);

    assert(guest_linux_mremap(&fixture.memory, base,
            GUEST_MEMORY_PAGE_SIZE, GUEST_MEMORY_PAGE_SIZE,
            GUEST_LINUX_MREMAP_MAYMOVE |
                    GUEST_LINUX_MREMAP_FIXED,
            target) == target);
    assert_unmapped_fault(&fixture.tlb, base);
    assert(read_byte(&fixture.tlb, target) == UINT8_C(0x55));

    fixture_destroy(&fixture);
    assert(guest_page_backing_test_live_count() == 0);
}

static void test_fixed_shrink_and_failure_side_effects(void) {
    const qword_t fixed_flags = GUEST_LINUX_MREMAP_MAYMOVE |
            GUEST_LINUX_MREMAP_FIXED;
    struct memory_fixture fixture;
    fixture_init(&fixture);
    guest_addr_t source = (guest_addr_t) fixture.memory.mmap_base +
            UINT64_C(0x600000);
    guest_addr_t target = source + (UINT64_C(1) << 23);
    map_anonymous(&fixture, source, 3, false);
    map_anonymous(&fixture, target, 2, false);
    write_byte(&fixture.tlb, source, UINT8_C(0x31));
    write_byte(&fixture.tlb,
            source + GUEST_MEMORY_PAGE_SIZE, UINT8_C(0x42));

    assert(guest_linux_mremap(&fixture.memory, source,
            3 * GUEST_MEMORY_PAGE_SIZE,
            2 * GUEST_MEMORY_PAGE_SIZE, fixed_flags, target) == target);
    for (qword_t index = 0; index < 3; index++)
        assert_unmapped_fault(&fixture.tlb,
                source + index * GUEST_MEMORY_PAGE_SIZE);
    assert(read_byte(&fixture.tlb, target) == UINT8_C(0x31));
    assert(read_byte(&fixture.tlb,
            target + GUEST_MEMORY_PAGE_SIZE) == UINT8_C(0x42));
    const struct guest_linux_vma *mapping = guest_linux_vma_find(
            &fixture.memory.vmas, target);
    assert(mapping != NULL && mapping->first == target &&
            mapping->last == target + 2 * GUEST_MEMORY_PAGE_SIZE);
    fixture_destroy(&fixture);
    assert(guest_page_backing_test_live_count() == 0);

    fixture_init(&fixture);
    source = (guest_addr_t) fixture.memory.mmap_base +
            UINT64_C(0x700000);
    target = source + (UINT64_C(1) << 23);
    map_anonymous(&fixture, source, 2, false);
    map_anonymous(&fixture, target, 1, false);
    write_byte(&fixture.tlb, source, UINT8_C(0x53));
    write_byte(&fixture.tlb,
            source + GUEST_MEMORY_PAGE_SIZE, UINT8_C(0x64));
    guest_page_table_test_fail_remap_allocation_at(0);
    assert(guest_linux_mremap(&fixture.memory, source,
            2 * GUEST_MEMORY_PAGE_SIZE, GUEST_MEMORY_PAGE_SIZE,
            fixed_flags, target) == linux_error(GUEST_LINUX_ENOMEM));
    guest_page_table_test_fail_remap_allocation_at(SIZE_MAX);
    mapping = guest_linux_vma_find(&fixture.memory.vmas, source);
    assert(mapping != NULL && mapping->first == source &&
            mapping->last == source + GUEST_MEMORY_PAGE_SIZE);
    assert(read_byte(&fixture.tlb, source) == UINT8_C(0x53));
    assert_unmapped_fault(&fixture.tlb,
            source + GUEST_MEMORY_PAGE_SIZE);
    assert_unmapped_fault(&fixture.tlb, target);
    assert(guest_linux_mremap(&fixture.memory, source,
            GUEST_MEMORY_PAGE_SIZE, GUEST_MEMORY_PAGE_SIZE,
            fixed_flags, target) == target);
    fixture_destroy(&fixture);
    assert(guest_page_backing_test_live_count() == 0);

    bool observed_late_vma_failure = false;
    for (size_t failure = 0;
            failure < 12 && !observed_late_vma_failure; failure++) {
        fixture_init(&fixture);
        source = (guest_addr_t) fixture.memory.mmap_base +
                UINT64_C(0x800000);
        target = source + (UINT64_C(1) << 23);
        map_anonymous(&fixture, source, 2, false);
        map_anonymous(&fixture, target, 1, false);
        write_byte(&fixture.tlb, source, UINT8_C(0x75));
        guest_linux_vma_test_fail_allocation_at(failure);
        qword_t result = guest_linux_mremap(&fixture.memory, source,
                2 * GUEST_MEMORY_PAGE_SIZE, GUEST_MEMORY_PAGE_SIZE,
                fixed_flags, target);
        guest_linux_vma_test_fail_allocation_at(SIZE_MAX);
        mapping = guest_linux_vma_find(&fixture.memory.vmas, source);
        const struct guest_linux_vma *target_mapping =
                guest_linux_vma_find(&fixture.memory.vmas, target);
        if (result == linux_error(GUEST_LINUX_ENOMEM) &&
                mapping != NULL && mapping->first == source &&
                mapping->last == source + GUEST_MEMORY_PAGE_SIZE &&
                target_mapping == NULL) {
            assert(read_byte(&fixture.tlb, source) == UINT8_C(0x75));
            assert_unmapped_fault(&fixture.tlb,
                    source + GUEST_MEMORY_PAGE_SIZE);
            assert_unmapped_fault(&fixture.tlb, target);
            observed_late_vma_failure = true;
        }
        fixture_destroy(&fixture);
        assert(guest_page_backing_test_live_count() == 0);
    }
    assert(observed_late_vma_failure);
}

static void test_dontunmap_shared_duplicate_and_fork(void) {
    struct memory_fixture fixture;
    fixture_init(&fixture);
    const unsigned read_write = GUEST_MEMORY_READ | GUEST_MEMORY_WRITE;
    guest_addr_t base = (guest_addr_t) fixture.memory.mmap_base +
            UINT64_C(0x400000);
    guest_addr_t destination = base + 32 * GUEST_MEMORY_PAGE_SIZE;
    map_anonymous(&fixture, base, 2, false);
    write_byte(&fixture.tlb, base, UINT8_C(0x81));
    write_byte(&fixture.tlb,
            base + GUEST_MEMORY_PAGE_SIZE, UINT8_C(0x92));
    byte_t *old_pages[] = {
        lookup_page(&fixture.table, base, read_write),
        lookup_page(&fixture.table,
                base + GUEST_MEMORY_PAGE_SIZE, read_write),
    };
    assert(guest_linux_mremap(&fixture.memory, base,
            2 * GUEST_MEMORY_PAGE_SIZE, 2 * GUEST_MEMORY_PAGE_SIZE,
            GUEST_LINUX_MREMAP_MAYMOVE |
                    GUEST_LINUX_MREMAP_DONTUNMAP,
            destination) == destination);
    for (size_t index = 0; index < array_size(old_pages); index++) {
        byte_t *source_page = lookup_page(&fixture.table,
                base + index * GUEST_MEMORY_PAGE_SIZE, read_write);
        byte_t *destination_page = lookup_page(&fixture.table,
                destination + index * GUEST_MEMORY_PAGE_SIZE,
                read_write);
        assert(source_page != old_pages[index] && source_page[0] == 0);
        assert(destination_page == old_pages[index] &&
                destination_page[0] ==
                        (byte_t) (UINT8_C(0x81) + index * UINT8_C(0x11)));
    }

    guest_addr_t zero_hint_source =
            base + 128 * GUEST_MEMORY_PAGE_SIZE;
    map_anonymous(&fixture, zero_hint_source, 1, false);
    write_byte(&fixture.tlb, zero_hint_source, UINT8_C(0x47));
    byte_t *zero_hint_page = lookup_page(
            &fixture.table, zero_hint_source, read_write);
    qword_t zero_hint_result = guest_linux_mremap(
            &fixture.memory, zero_hint_source,
            GUEST_MEMORY_PAGE_SIZE, GUEST_MEMORY_PAGE_SIZE,
            GUEST_LINUX_MREMAP_MAYMOVE |
                    GUEST_LINUX_MREMAP_DONTUNMAP,
            0);
    assert((sqword_t) zero_hint_result >= 0 && zero_hint_result != 0 &&
            zero_hint_result != zero_hint_source &&
            zero_hint_result >= fixture.memory.mmap_base &&
            zero_hint_result < fixture.memory.mmap_limit);
    assert(lookup_page(&fixture.table,
            (guest_addr_t) zero_hint_result, read_write) == zero_hint_page);
    assert(lookup_page(&fixture.table,
            zero_hint_source, read_write) != zero_hint_page);
    assert(read_byte(&fixture.tlb, zero_hint_source) == 0);

    guest_addr_t shared = base + 64 * GUEST_MEMORY_PAGE_SIZE;
    guest_addr_t occupied_hint =
            shared + 16 * GUEST_MEMORY_PAGE_SIZE;
    map_anonymous(&fixture, shared, 1, true);
    map_anonymous(&fixture, occupied_hint, 1, false);
    write_byte(&fixture.tlb, shared, UINT8_C(0x63));
    write_byte(&fixture.tlb, occupied_hint, UINT8_C(0x18));
    byte_t *shared_page = lookup_page(
            &fixture.table, shared, read_write);
    qword_t shared_result = guest_linux_mremap(&fixture.memory, shared,
            GUEST_MEMORY_PAGE_SIZE, GUEST_MEMORY_PAGE_SIZE,
            GUEST_LINUX_MREMAP_MAYMOVE |
                    GUEST_LINUX_MREMAP_DONTUNMAP,
            occupied_hint);
    assert((sqword_t) shared_result >= 0 &&
            shared_result != shared && shared_result != occupied_hint);
    guest_addr_t shared_destination = (guest_addr_t) shared_result;
    assert(read_byte(&fixture.tlb, occupied_hint) == UINT8_C(0x18));
    assert(lookup_page(&fixture.table, shared,
            read_write) == shared_page);
    assert(lookup_page(&fixture.table, shared_destination,
            read_write) == shared_page);
    write_byte(&fixture.tlb, shared_destination, UINT8_C(0x74));
    assert(read_byte(&fixture.tlb, shared) == UINT8_C(0x74));

    qword_t duplicate_result = guest_linux_mremap(&fixture.memory,
            shared, 0, GUEST_MEMORY_PAGE_SIZE,
            GUEST_LINUX_MREMAP_MAYMOVE, 0);
    assert((sqword_t) duplicate_result >= 0);
    guest_addr_t duplicate = (guest_addr_t) duplicate_result;
    assert(duplicate != shared &&
            lookup_page(&fixture.table, duplicate,
                    read_write) == shared_page);
    assert(guest_linux_mremap(&fixture.memory, base, 0,
            GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_MREMAP_MAYMOVE, 0) ==
            linux_error(GUEST_LINUX_EINVAL));

    guest_addr_t fork_source = base + 96 * GUEST_MEMORY_PAGE_SIZE;
    guest_addr_t fork_destination =
            fork_source + 16 * GUEST_MEMORY_PAGE_SIZE;
    map_anonymous(&fixture, fork_source, 1, false);
    write_byte(&fixture.tlb, fork_source, UINT8_C(0xb5));
    struct guest_page_table child_table;
    struct guest_linux_mm child_memory;
    assert(guest_linux_mm_clone(
            &child_memory, &child_table, &fixture.memory));
    struct guest_tlb child_tlb;
    guest_tlb_init(&child_tlb, &child_table.address_space);

    assert(guest_linux_mremap(&fixture.memory, fork_source,
            GUEST_MEMORY_PAGE_SIZE, GUEST_MEMORY_PAGE_SIZE,
            GUEST_LINUX_MREMAP_MAYMOVE |
                    GUEST_LINUX_MREMAP_FIXED,
            fork_destination) == fork_destination);
    assert_unmapped_fault(&fixture.tlb, fork_source);
    assert(read_byte(&fixture.tlb, fork_destination) == UINT8_C(0xb5));
    assert(read_byte(&child_tlb, fork_source) == UINT8_C(0xb5));
    assert_unmapped_fault(&child_tlb, fork_destination);

    guest_linux_mm_destroy(&child_memory);
    guest_page_table_destroy(&child_table);
    fixture_destroy(&fixture);
    assert(guest_page_backing_test_live_count() == 0);
}

int main(void) {
    guest_page_backing_test_fail_allocation_at(SIZE_MAX);
    guest_page_table_test_fail_remap_allocation_at(SIZE_MAX);
    guest_linux_vma_test_fail_allocation_at(SIZE_MAX);
    test_parameters_and_in_place_resize();
    test_move_growth_and_failures();
    test_fixed_shrink_and_failure_side_effects();
    test_dontunmap_shared_duplicate_and_fork();
    assert(guest_page_backing_test_live_count() == 0);
    return 0;
}
