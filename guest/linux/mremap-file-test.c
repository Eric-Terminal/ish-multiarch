#include <assert.h>
#include <string.h>

#include "guest/linux/errno.h"
#include "guest/linux/memory.h"
#include "guest/linux/mman.h"
#include "guest/memory/page-backing.h"
#include "guest/memory/tlb.h"

#define TEST_BRK UINT64_C(0x00100000)
#define TEST_BRK_LIMIT UINT64_C(0x00200000)
#define FILE_PAGE_COUNT 8

struct file_probe {
    byte_t data[FILE_PAGE_COUNT * GUEST_MEMORY_PAGE_SIZE];
    qword_t last_read_offset;
    unsigned reads;
    unsigned writes;
    unsigned syncs;
    unsigned releases;
};

struct memory_fixture {
    struct guest_page_table table;
    struct guest_linux_mm memory;
    struct guest_tlb tlb;
};

static qword_t linux_error(unsigned error) {
    return (qword_t) -(sqword_t) error;
}

static enum guest_file_page_result probe_read_page(void *opaque,
        qword_t file_offset, byte_t *page, dword_t *valid_bytes) {
    struct file_probe *probe = opaque;
    probe->reads++;
    probe->last_read_offset = file_offset;
    if (file_offset >= sizeof(probe->data))
        return GUEST_FILE_PAGE_END_OF_FILE;
    memcpy(page, probe->data + file_offset, GUEST_MEMORY_PAGE_SIZE);
    *valid_bytes = GUEST_MEMORY_PAGE_SIZE;
    return GUEST_FILE_PAGE_OK;
}

static enum guest_file_sync_result probe_write_page(void *opaque,
        qword_t file_offset, const byte_t *page) {
    struct file_probe *probe = opaque;
    assert(file_offset <= sizeof(probe->data) - GUEST_MEMORY_PAGE_SIZE);
    memcpy(probe->data + file_offset, page, GUEST_MEMORY_PAGE_SIZE);
    probe->writes++;
    return GUEST_FILE_SYNC_OK;
}

static enum guest_file_sync_result probe_sync_range(void *opaque,
        qword_t file_offset, qword_t length) {
    struct file_probe *probe = opaque;
    use(file_offset, length);
    probe->syncs++;
    return GUEST_FILE_SYNC_OK;
}

static void probe_release(struct guest_file_pager *pager, void *opaque) {
    use(pager);
    struct file_probe *probe = opaque;
    probe->releases++;
}

static void probe_init(struct file_probe *probe) {
    memset(probe, 0, sizeof(*probe));
    for (size_t page = 0; page < FILE_PAGE_COUNT; page++) {
        memset(probe->data + page * GUEST_MEMORY_PAGE_SIZE,
                (int) (page + 1), GUEST_MEMORY_PAGE_SIZE);
    }
}

static struct guest_file_pager *probe_pager(struct file_probe *probe,
        qword_t identity) {
    return guest_file_pager_create(identity,
            (struct guest_file_pager_provider) {
                .opaque = probe,
                .read_page = probe_read_page,
                .write_page = probe_write_page,
                .sync_range = probe_sync_range,
                .release = probe_release,
            });
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

static guest_addr_t map_file(struct memory_fixture *fixture,
        struct guest_file_pager *pager, guest_addr_t address,
        qword_t page_count, qword_t page_offset, bool shared) {
    qword_t protection = GUEST_LINUX_PROT_READ |
            GUEST_LINUX_PROT_WRITE;
    qword_t flags = (shared ? GUEST_LINUX_MAP_SHARED :
            GUEST_LINUX_MAP_PRIVATE) | GUEST_LINUX_MAP_FIXED;
    qword_t result = shared ? guest_linux_mmap_file_shared(
            &fixture->memory, address,
            page_count * GUEST_MEMORY_PAGE_SIZE,
            protection, GUEST_LINUX_PROT_MASK, flags,
            pager, page_offset * GUEST_MEMORY_PAGE_SIZE) :
            guest_linux_mmap_file_private(&fixture->memory, address,
                    page_count * GUEST_MEMORY_PAGE_SIZE,
                    protection, GUEST_LINUX_PROT_MASK, flags,
                    pager, page_offset * GUEST_MEMORY_PAGE_SIZE);
    assert(result == address);
    return address;
}

static byte_t *lookup_page(struct guest_page_table *table,
        guest_addr_t page) {
    bool locked = guest_page_table_read_lock(table);
    byte_t *host_page;
    unsigned permissions;
    assert(guest_page_table_lookup(table, page,
            &host_page, &permissions) == GUEST_PAGE_TABLE_OK);
    assert(permissions == (GUEST_MEMORY_READ | GUEST_MEMORY_WRITE));
    guest_page_table_read_unlock(table, locked);
    return host_page;
}

static void assert_not_resident(struct guest_page_table *table,
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

static void test_lazy_expand_and_offset_continuity(void) {
    struct memory_fixture fixture;
    fixture_init(&fixture);
    struct file_probe probe;
    probe_init(&probe);
    struct guest_file_pager *pager = probe_pager(&probe, 301);
    assert(pager != NULL);
    guest_addr_t base = (guest_addr_t) fixture.memory.mmap_base +
            UINT64_C(0x100000);
    map_file(&fixture, pager, base, 1, 2, false);
    assert(probe.reads == 0);

    qword_t generation = fixture.table.address_space.generation;
    assert(guest_linux_mremap(&fixture.memory, base,
            GUEST_MEMORY_PAGE_SIZE, 3 * GUEST_MEMORY_PAGE_SIZE,
            0, 0) == base);
    assert(probe.reads == 0 &&
            fixture.table.address_space.generation == generation + 1);
    for (qword_t index = 0; index < 3; index++)
        assert_not_resident(&fixture.table,
                base + index * GUEST_MEMORY_PAGE_SIZE);

    const struct guest_linux_vma *mapping = guest_linux_vma_find(
            &fixture.memory.vmas,
            base + 2 * GUEST_MEMORY_PAGE_SIZE);
    assert(mapping != NULL &&
            mapping->source == GUEST_LINUX_VMA_SOURCE_FILE_PRIVATE &&
            mapping->first == base &&
            mapping->last == base + 3 * GUEST_MEMORY_PAGE_SIZE &&
            mapping->file_offset == 2 * GUEST_MEMORY_PAGE_SIZE);
    assert(read_byte(&fixture.tlb,
            base + 2 * GUEST_MEMORY_PAGE_SIZE) == UINT8_C(5));
    assert(probe.reads == 1 &&
            probe.last_read_offset == 4 * GUEST_MEMORY_PAGE_SIZE);

    guest_file_pager_release(pager);
    fixture_destroy(&fixture);
    assert(probe.releases == 1 &&
            guest_page_backing_test_live_count() == 0);
}

static void test_private_cow_move_dontunmap_and_fork(void) {
    struct memory_fixture fixture;
    fixture_init(&fixture);
    struct file_probe probe;
    probe_init(&probe);
    struct guest_file_pager *pager = probe_pager(&probe, 302);
    assert(pager != NULL);
    guest_addr_t base = (guest_addr_t) fixture.memory.mmap_base +
            UINT64_C(0x200000);
    guest_addr_t destination = base + (UINT64_C(1) << 22);
    map_file(&fixture, pager, base, 1, 1, false);
    assert(read_byte(&fixture.tlb, base) == UINT8_C(2));
    write_byte(&fixture.tlb, base, UINT8_C(0xe1));
    byte_t *cow_page = lookup_page(&fixture.table, base);

    struct guest_page_table child_table;
    struct guest_linux_mm child_memory;
    assert(guest_linux_mm_clone(
            &child_memory, &child_table, &fixture.memory));
    struct guest_tlb child_tlb;
    guest_tlb_init(&child_tlb, &child_table.address_space);
    assert(read_byte(&child_tlb, base) == UINT8_C(0xe1));

    assert(guest_linux_mremap(&fixture.memory, base,
            GUEST_MEMORY_PAGE_SIZE, GUEST_MEMORY_PAGE_SIZE,
            GUEST_LINUX_MREMAP_MAYMOVE |
                    GUEST_LINUX_MREMAP_FIXED,
            destination) == destination);
    assert_unmapped_fault(&fixture.tlb, base);
    assert(read_byte(&fixture.tlb, destination) == UINT8_C(0xe1));
    assert(lookup_page(&fixture.table, destination) == cow_page);
    assert(read_byte(&child_tlb, base) == UINT8_C(0xe1));
    assert_unmapped_fault(&child_tlb, destination);

    guest_addr_t dontunmap_source =
            base + 32 * GUEST_MEMORY_PAGE_SIZE;
    map_file(&fixture, pager, dontunmap_source, 1, 3, false);
    assert(read_byte(&fixture.tlb, dontunmap_source) == UINT8_C(4));
    write_byte(&fixture.tlb, dontunmap_source, UINT8_C(0xf2));
    byte_t *private_copy = lookup_page(
            &fixture.table, dontunmap_source);
    unsigned reads_before = probe.reads;
    qword_t dontunmap_result = guest_linux_mremap(
            &fixture.memory, dontunmap_source,
            GUEST_MEMORY_PAGE_SIZE, GUEST_MEMORY_PAGE_SIZE,
            GUEST_LINUX_MREMAP_MAYMOVE |
                    GUEST_LINUX_MREMAP_DONTUNMAP,
            0);
    assert((sqword_t) dontunmap_result >= 0 &&
            dontunmap_result != 0 &&
            dontunmap_result != dontunmap_source);
    guest_addr_t dontunmap_destination =
            (guest_addr_t) dontunmap_result;
    assert(read_byte(&fixture.tlb,
            dontunmap_destination) == UINT8_C(0xf2));
    assert(lookup_page(&fixture.table,
            dontunmap_destination) == private_copy);
    assert(read_byte(&fixture.tlb, dontunmap_source) == UINT8_C(4));
    assert(lookup_page(&fixture.table,
            dontunmap_source) != private_copy);
    assert(probe.reads == reads_before);

    guest_linux_mm_destroy(&child_memory);
    guest_page_table_destroy(&child_table);
    guest_file_pager_release(pager);
    fixture_destroy(&fixture);
    assert(probe.releases == 1 &&
            guest_page_backing_test_live_count() == 0);
}

static void test_shared_dirty_dontunmap_and_zero_duplicate(void) {
    struct memory_fixture fixture;
    fixture_init(&fixture);
    struct file_probe probe;
    probe_init(&probe);
    struct guest_file_pager *pager = probe_pager(&probe, 303);
    assert(pager != NULL);
    guest_addr_t source = (guest_addr_t) fixture.memory.mmap_base +
            UINT64_C(0x300000);
    guest_addr_t destination = source + (UINT64_C(1) << 22);
    map_file(&fixture, pager, source, 1, 5, true);
    assert(read_byte(&fixture.tlb, source) == UINT8_C(6));
    write_byte(&fixture.tlb, source, UINT8_C(0x96));
    byte_t *shared_page = lookup_page(&fixture.table, source);
    unsigned reads_before = probe.reads;

    assert(guest_linux_mremap(&fixture.memory, source,
            GUEST_MEMORY_PAGE_SIZE, GUEST_MEMORY_PAGE_SIZE,
            GUEST_LINUX_MREMAP_MAYMOVE |
                    GUEST_LINUX_MREMAP_FIXED |
                    GUEST_LINUX_MREMAP_DONTUNMAP,
            destination) == destination);
    assert(lookup_page(&fixture.table, destination) == shared_page);
    assert(read_byte(&fixture.tlb, source) == UINT8_C(0x96));
    assert(lookup_page(&fixture.table, source) == shared_page);
    assert(probe.reads == reads_before);
    write_byte(&fixture.tlb, source, UINT8_C(0xa7));
    assert(read_byte(&fixture.tlb, destination) == UINT8_C(0xa7));

    qword_t duplicate_result = guest_linux_mremap(&fixture.memory,
            source, 0, 2 * GUEST_MEMORY_PAGE_SIZE,
            GUEST_LINUX_MREMAP_MAYMOVE, 0);
    assert((sqword_t) duplicate_result >= 0);
    guest_addr_t duplicate = (guest_addr_t) duplicate_result;
    assert(duplicate != source &&
            read_byte(&fixture.tlb, duplicate) == UINT8_C(0xa7));
    assert(lookup_page(&fixture.table, duplicate) == shared_page);
    assert(read_byte(&fixture.tlb,
            duplicate + GUEST_MEMORY_PAGE_SIZE) == UINT8_C(7));

    guest_addr_t private_source =
            source + 32 * GUEST_MEMORY_PAGE_SIZE;
    map_file(&fixture, pager, private_source, 1, 0, false);
    assert(guest_linux_mremap(&fixture.memory, private_source, 0,
            GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_MREMAP_MAYMOVE, 0) ==
            linux_error(GUEST_LINUX_EINVAL));

    guest_file_pager_release(pager);
    fixture_destroy(&fixture);
    assert(probe.releases == 1 && probe.writes != 0 &&
            guest_page_backing_test_live_count() == 0);
}

static void test_lazy_fixed_growth_and_vma_oom(void) {
    struct memory_fixture fixture;
    fixture_init(&fixture);
    struct file_probe probe;
    probe_init(&probe);
    struct guest_file_pager *pager = probe_pager(&probe, 304);
    assert(pager != NULL);
    guest_addr_t source = (guest_addr_t) fixture.memory.mmap_base +
            UINT64_C(0x500000);
    guest_addr_t target = source + (UINT64_C(1) << 23);
    map_file(&fixture, pager, source, 2, 0, false);
    assert(probe.reads == 0);

    guest_linux_vma_test_fail_allocation_at(0);
    assert(guest_linux_mremap(&fixture.memory, source,
            2 * GUEST_MEMORY_PAGE_SIZE, 3 * GUEST_MEMORY_PAGE_SIZE,
            GUEST_LINUX_MREMAP_MAYMOVE, 0) ==
            linux_error(GUEST_LINUX_ENOMEM));
    guest_linux_vma_test_fail_allocation_at(SIZE_MAX);
    assert(probe.reads == 0);
    assert(guest_linux_vma_find(&fixture.memory.vmas, source) != NULL);

    qword_t generation = fixture.table.address_space.generation;
    assert(guest_linux_mremap(&fixture.memory, source,
            2 * GUEST_MEMORY_PAGE_SIZE, 3 * GUEST_MEMORY_PAGE_SIZE,
            GUEST_LINUX_MREMAP_MAYMOVE |
                    GUEST_LINUX_MREMAP_FIXED,
            target) == target);
    assert(probe.reads == 0 &&
            fixture.table.address_space.generation == generation + 1);
    assert(guest_linux_vma_find(&fixture.memory.vmas, source) == NULL);
    const struct guest_linux_vma *mapping = guest_linux_vma_find(
            &fixture.memory.vmas,
            target + 2 * GUEST_MEMORY_PAGE_SIZE);
    assert(mapping != NULL && mapping->first == target &&
            mapping->last == target + 3 * GUEST_MEMORY_PAGE_SIZE &&
            mapping->file_offset == 0);
    assert(read_byte(&fixture.tlb,
            target + 2 * GUEST_MEMORY_PAGE_SIZE) == UINT8_C(3));
    assert(probe.last_read_offset == 2 * GUEST_MEMORY_PAGE_SIZE);

    guest_file_pager_release(pager);
    fixture_destroy(&fixture);
    assert(probe.releases == 1 &&
            guest_page_backing_test_live_count() == 0);
}

static void test_subrange_move_and_offset_overflow(void) {
    struct memory_fixture fixture;
    fixture_init(&fixture);
    struct file_probe probe;
    probe_init(&probe);
    struct guest_file_pager *pager = probe_pager(&probe, 305);
    assert(pager != NULL);
    guest_addr_t base = (guest_addr_t) fixture.memory.mmap_base +
            UINT64_C(0x600000);
    guest_addr_t target = base + (UINT64_C(1) << 23);
    map_file(&fixture, pager, base, 3, 1, false);

    assert(guest_linux_mremap(&fixture.memory,
            base + GUEST_MEMORY_PAGE_SIZE,
            GUEST_MEMORY_PAGE_SIZE, GUEST_MEMORY_PAGE_SIZE,
            GUEST_LINUX_MREMAP_MAYMOVE |
                    GUEST_LINUX_MREMAP_FIXED,
            target) == target);
    assert(probe.reads == 0);
    const struct guest_linux_vma *prefix = guest_linux_vma_find(
            &fixture.memory.vmas, base);
    const struct guest_linux_vma *suffix = guest_linux_vma_find(
            &fixture.memory.vmas,
            base + 2 * GUEST_MEMORY_PAGE_SIZE);
    const struct guest_linux_vma *moved = guest_linux_vma_find(
            &fixture.memory.vmas, target);
    assert(prefix != NULL && prefix->first == base &&
            prefix->last == base + GUEST_MEMORY_PAGE_SIZE &&
            prefix->file_offset == GUEST_MEMORY_PAGE_SIZE);
    assert(guest_linux_vma_find(&fixture.memory.vmas,
            base + GUEST_MEMORY_PAGE_SIZE) == NULL);
    assert(suffix != NULL &&
            suffix->first == base + 2 * GUEST_MEMORY_PAGE_SIZE &&
            suffix->last == base + 3 * GUEST_MEMORY_PAGE_SIZE &&
            suffix->file_offset == 3 * GUEST_MEMORY_PAGE_SIZE);
    assert(moved != NULL && moved->first == target &&
            moved->last == target + GUEST_MEMORY_PAGE_SIZE &&
            moved->file_offset == 2 * GUEST_MEMORY_PAGE_SIZE);
    assert_unmapped_fault(&fixture.tlb,
            base + GUEST_MEMORY_PAGE_SIZE);
    assert(read_byte(&fixture.tlb, target) == UINT8_C(3));
    assert(probe.reads == 1 &&
            probe.last_read_offset == 2 * GUEST_MEMORY_PAGE_SIZE);

    guest_file_pager_release(pager);
    fixture_destroy(&fixture);
    assert(probe.releases == 1 &&
            guest_page_backing_test_live_count() == 0);

    fixture_init(&fixture);
    probe_init(&probe);
    pager = probe_pager(&probe, 306);
    assert(pager != NULL);
    base = (guest_addr_t) fixture.memory.mmap_base +
            UINT64_C(0x700000);
    qword_t high_offset = UINT64_MAX -
            (2 * GUEST_MEMORY_PAGE_SIZE - 1);
    qword_t mapped = guest_linux_mmap_file_private(
            &fixture.memory, base, GUEST_MEMORY_PAGE_SIZE,
            GUEST_LINUX_PROT_READ | GUEST_LINUX_PROT_WRITE,
            GUEST_LINUX_PROT_MASK,
            GUEST_LINUX_MAP_PRIVATE | GUEST_LINUX_MAP_FIXED,
            pager, high_offset);
    assert(mapped == base && probe.reads == 0);
    qword_t generation = fixture.table.address_space.generation;
    assert(guest_linux_mremap(&fixture.memory, base,
            GUEST_MEMORY_PAGE_SIZE, 3 * GUEST_MEMORY_PAGE_SIZE,
            0, 0) == linux_error(GUEST_LINUX_EINVAL));
    const struct guest_linux_vma *mapping = guest_linux_vma_find(
            &fixture.memory.vmas, base);
    assert(mapping != NULL && mapping->first == base &&
            mapping->last == base + GUEST_MEMORY_PAGE_SIZE &&
            mapping->file_offset == high_offset &&
            fixture.table.address_space.generation == generation &&
            probe.reads == 0);

    guest_file_pager_release(pager);
    fixture_destroy(&fixture);
    assert(probe.releases == 1 &&
            guest_page_backing_test_live_count() == 0);
}

int main(void) {
    guest_page_backing_test_fail_allocation_at(SIZE_MAX);
    guest_page_table_test_fail_remap_allocation_at(SIZE_MAX);
    guest_linux_vma_test_fail_allocation_at(SIZE_MAX);
    test_lazy_expand_and_offset_continuity();
    test_private_cow_move_dontunmap_and_fork();
    test_shared_dirty_dontunmap_and_zero_duplicate();
    test_lazy_fixed_growth_and_vma_oom();
    test_subrange_move_and_offset_overflow();
    assert(guest_page_backing_test_live_count() == 0);
    return 0;
}
