#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <string.h>

#include "guest/linux/errno.h"
#include "guest/linux/memory.h"
#include "guest/linux/mman.h"
#include "guest/memory/page-backing.h"
#include "guest/memory/tlb.h"

#define BRK_BASE UINT64_C(0x0000600000000000)
#define BRK_LIMIT (BRK_BASE + 3 * GUEST_MEMORY_PAGE_SIZE)
#define TOP_PAGE ((UINT64_C(1) << 48) - GUEST_MEMORY_PAGE_SIZE)
#define CONCURRENCY_ITERATIONS 5000

static qword_t encoded_error(unsigned error) {
    return (qword_t) -(sqword_t) error;
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

static void destroy_memory(struct guest_linux_mm *memory,
        struct guest_page_table *table) {
    guest_linux_mm_destroy(memory);
    guest_page_table_destroy(table);
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
    const struct guest_linux_vma *heap_vma =
            guest_linux_vma_find(&memory.vmas, BRK_BASE);
    assert(heap_vma != NULL &&
            heap_vma->source == GUEST_LINUX_VMA_SOURCE_BRK &&
            heap_vma->last == BRK_BASE + GUEST_MEMORY_PAGE_SIZE);
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
    assert(memory.vmas.count == 0);
    destroy_memory(&memory, &table);
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
    destroy_memory(&memory, &table);
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
    assert(guest_linux_mmap(&memory, TOP_PAGE,
            GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_PROT_READ,
            GUEST_LINUX_MAP_PRIVATE | GUEST_LINUX_MAP_ANONYMOUS |
                    GUEST_LINUX_MAP_FIXED,
            UINT64_MAX, 0) == TOP_PAGE);
    lookup_page(&table, TOP_PAGE, GUEST_MEMORY_READ);
    destroy_memory(&memory, &table);
}

static void test_busybox_mmap_paths(void) {
    struct guest_page_table table;
    assert(guest_page_table_init(&table, 48));
    struct guest_linux_mm memory;
    guest_linux_mm_init(&memory, &table, BRK_BASE, BRK_LIMIT);
    assert(memory.mmap_base == BRK_LIMIT);
    assert(memory.mmap_limit > memory.mmap_base);

    assert(guest_linux_brk(&memory,
            BRK_BASE + 2 * GUEST_MEMORY_PAGE_SIZE) ==
            BRK_BASE + 2 * GUEST_MEMORY_PAGE_SIZE);
    byte_t *heap = lookup_page(&table, BRK_BASE,
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE);
    memset(heap, 0xa5, GUEST_MEMORY_PAGE_SIZE);
    qword_t result = guest_linux_mmap(&memory, BRK_BASE,
            GUEST_MEMORY_PAGE_SIZE, 0,
            GUEST_LINUX_MAP_FIXED | GUEST_LINUX_MAP_PRIVATE |
                    GUEST_LINUX_MAP_ANONYMOUS,
            UINT64_MAX, 0);
    assert(result == BRK_BASE);
    byte_t *guard = lookup_page(&table, BRK_BASE, 0);
    for (size_t i = 0; i < GUEST_MEMORY_PAGE_SIZE; i++)
        assert(guard[i] == 0);

    struct guest_tlb tlb;
    guest_tlb_init(&tlb, &table.address_space);
    struct guest_memory_fault fault;
    byte_t value = 0x5a;
    assert(!guest_tlb_read(&tlb, BRK_BASE, &value, 1,
            GUEST_MEMORY_READ, &fault));
    assert(fault.kind == GUEST_MEMORY_FAULT_PERMISSION);
    assert(!guest_tlb_read(&tlb, BRK_BASE, &value, 1,
            GUEST_MEMORY_EXECUTE, &fault));
    assert(fault.kind == GUEST_MEMORY_FAULT_PERMISSION);
    assert(!guest_tlb_write(&tlb, BRK_BASE, &value, 1, &fault));
    assert(fault.kind == GUEST_MEMORY_FAULT_PERMISSION);

    result = guest_linux_mmap(&memory, 0, GUEST_MEMORY_PAGE_SIZE,
            GUEST_LINUX_PROT_READ | GUEST_LINUX_PROT_WRITE,
            GUEST_LINUX_MAP_PRIVATE | GUEST_LINUX_MAP_ANONYMOUS,
            UINT64_MAX, 0);
    assert(result == memory.mmap_base);
    byte_t *anonymous = lookup_page(&table, (guest_addr_t) result,
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE);
    for (size_t i = 0; i < GUEST_MEMORY_PAGE_SIZE; i++)
        assert(anonymous[i] == 0);
    assert(guest_tlb_write(&tlb, (guest_addr_t) result,
            &value, 1, &fault));
    byte_t output = 0;
    assert(guest_tlb_read(&tlb, (guest_addr_t) result,
            &output, 1, GUEST_MEMORY_READ, &fault));
    assert(output == value);
    destroy_memory(&memory, &table);
}

static void test_hints_and_fixed_replacement(void) {
    struct guest_page_table table;
    assert(guest_page_table_init(&table, 48));
    struct guest_linux_mm memory;
    guest_linux_mm_init(&memory, &table, BRK_BASE, BRK_LIMIT);
    guest_addr_t base = (guest_addr_t) memory.mmap_base;

    byte_t *obstacle;
    assert(guest_page_table_map(&table, base,
            GUEST_MEMORY_READ, &obstacle) == GUEST_PAGE_TABLE_OK);
    obstacle[0] = 0x41;
    guest_addr_t hint = base + 10 * GUEST_MEMORY_PAGE_SIZE;
    qword_t result = guest_linux_mmap(&memory, hint + 123,
            2 * GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_PROT_READ,
            GUEST_LINUX_MAP_PRIVATE | GUEST_LINUX_MAP_ANONYMOUS,
            UINT64_MAX, 0);
    assert(result == hint);
    byte_t *hinted = lookup_page(&table, hint, GUEST_MEMORY_READ);
    hinted[0] = 0x52;

    result = guest_linux_mmap(&memory, hint + 123,
            GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_PROT_READ,
            GUEST_LINUX_MAP_PRIVATE | GUEST_LINUX_MAP_ANONYMOUS,
            UINT64_MAX, 0);
    assert(result == base + GUEST_MEMORY_PAGE_SIZE);
    assert(obstacle[0] == 0x41 && hinted[0] == 0x52);

    qword_t generation = table.address_space.generation;
    guest_linux_vma_test_fail_allocation_at(0);
    result = guest_linux_mmap(&memory, hint, GUEST_MEMORY_PAGE_SIZE,
            GUEST_LINUX_PROT_WRITE,
            GUEST_LINUX_MAP_PRIVATE | GUEST_LINUX_MAP_ANONYMOUS |
                    GUEST_LINUX_MAP_FIXED_NOREPLACE,
            UINT64_MAX, 0);
    assert(result == encoded_error(GUEST_LINUX_EEXIST));
    assert(guest_linux_vma_test_allocation_count() == 0);
    guest_linux_vma_test_fail_allocation_at(SIZE_MAX);
    assert(table.address_space.generation == generation);
    assert(lookup_page(&table, hint, GUEST_MEMORY_READ)[0] == 0x52);

    result = guest_linux_mmap(&memory, hint, GUEST_MEMORY_PAGE_SIZE,
            GUEST_LINUX_PROT_EXEC,
            GUEST_LINUX_MAP_PRIVATE | GUEST_LINUX_MAP_ANONYMOUS |
                    GUEST_LINUX_MAP_FIXED,
            UINT64_MAX, 0);
    assert(result == hint);
    byte_t *replacement = lookup_page(&table, hint,
            GUEST_MEMORY_EXECUTE);
    for (size_t i = 0; i < GUEST_MEMORY_PAGE_SIZE; i++)
        assert(replacement[i] == 0);

    guest_addr_t empty = base + 20 * GUEST_MEMORY_PAGE_SIZE;
    result = guest_linux_mmap(&memory, empty, GUEST_MEMORY_PAGE_SIZE,
            GUEST_LINUX_PROT_READ,
            GUEST_LINUX_MAP_PRIVATE | GUEST_LINUX_MAP_ANONYMOUS |
                    GUEST_LINUX_MAP_FIXED_NOREPLACE,
            UINT64_MAX, 0);
    assert(result == empty);
    destroy_memory(&memory, &table);
}

static void test_mmap_validation(void) {
    struct guest_page_table table;
    assert(guest_page_table_init(&table, 48));
    struct guest_linux_mm memory;
    guest_linux_mm_init(&memory, &table, BRK_BASE, BRK_LIMIT);
    qword_t private_anonymous = GUEST_LINUX_MAP_PRIVATE |
            GUEST_LINUX_MAP_ANONYMOUS;

    assert(guest_linux_mmap(&memory, 0, 0, 0,
            private_anonymous, UINT64_MAX, 0) ==
            encoded_error(GUEST_LINUX_EINVAL));
    assert(guest_linux_mmap(&memory, 0, UINT64_MAX, 0,
            private_anonymous, UINT64_MAX, 0) ==
            encoded_error(GUEST_LINUX_ENOMEM));
    assert(guest_linux_mmap(&memory, BRK_BASE + 1,
            GUEST_MEMORY_PAGE_SIZE, 0,
            private_anonymous | GUEST_LINUX_MAP_FIXED,
            UINT64_MAX, 0) == encoded_error(GUEST_LINUX_EINVAL));
    assert(guest_linux_mmap(&memory, 0, GUEST_MEMORY_PAGE_SIZE,
            UINT64_C(0x8), private_anonymous, UINT64_MAX, 0) ==
            encoded_error(GUEST_LINUX_EINVAL));
    assert(guest_linux_mmap(&memory, 0, GUEST_MEMORY_PAGE_SIZE, 0,
            GUEST_LINUX_MAP_SHARED | GUEST_LINUX_MAP_PRIVATE |
                    GUEST_LINUX_MAP_ANONYMOUS,
            UINT64_MAX, 0) == encoded_error(GUEST_LINUX_EINVAL));
    assert(guest_linux_mmap(&memory, 0, GUEST_MEMORY_PAGE_SIZE, 0,
            GUEST_LINUX_MAP_PRIVATE, UINT64_MAX, 0) ==
            encoded_error(GUEST_LINUX_EINVAL));
    assert(guest_linux_mmap(&memory, 0, GUEST_MEMORY_PAGE_SIZE, 0,
            private_anonymous | UINT64_C(0x40), UINT64_MAX, 0) ==
            encoded_error(GUEST_LINUX_EINVAL));
    assert(guest_linux_mmap(&memory, 0, GUEST_MEMORY_PAGE_SIZE, 0,
            private_anonymous, UINT64_MAX, 1) ==
            encoded_error(GUEST_LINUX_EINVAL));

    qword_t arena_size = memory.mmap_limit - memory.mmap_base;
    assert(guest_linux_mmap(&memory, 0, arena_size + 1, 0,
            private_anonymous, UINT64_MAX, 0) ==
            encoded_error(GUEST_LINUX_ENOMEM));
    assert(guest_linux_mmap(&memory, TOP_PAGE,
            GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_PROT_READ,
            private_anonymous | GUEST_LINUX_MAP_FIXED,
            UINT64_MAX, 0) == TOP_PAGE);
    assert(guest_linux_mmap(&memory, TOP_PAGE,
            GUEST_MEMORY_PAGE_SIZE + 1, GUEST_LINUX_PROT_READ,
            private_anonymous | GUEST_LINUX_MAP_FIXED,
            UINT64_MAX, 0) == encoded_error(GUEST_LINUX_ENOMEM));
    destroy_memory(&memory, &table);
}

static void test_mprotect_and_munmap(void) {
    struct guest_page_table table;
    assert(guest_page_table_init(&table, 48));
    struct guest_linux_mm memory;
    guest_linux_mm_init(&memory, &table, BRK_BASE, BRK_LIMIT);
    guest_addr_t base = (guest_addr_t) memory.mmap_base +
            40 * GUEST_MEMORY_PAGE_SIZE;
    qword_t flags = GUEST_LINUX_MAP_PRIVATE |
            GUEST_LINUX_MAP_ANONYMOUS | GUEST_LINUX_MAP_FIXED;
    assert(guest_linux_mmap(&memory, base,
            3 * GUEST_MEMORY_PAGE_SIZE,
            GUEST_LINUX_PROT_READ | GUEST_LINUX_PROT_WRITE,
            flags, UINT64_MAX, 0) == base);
    byte_t *first = lookup_page(&table, base,
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE);
    byte_t *last = lookup_page(&table,
            base + 2 * GUEST_MEMORY_PAGE_SIZE,
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE);
    first[0] = 0x63;
    last[0] = 0x74;

    assert(guest_linux_mprotect(&memory,
            base + GUEST_MEMORY_PAGE_SIZE,
            GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_PROT_EXEC) == 0);
    assert(memory.vmas.count == 3);
    assert(guest_linux_vma_find(&memory.vmas, base)->protection ==
            (GUEST_LINUX_PROT_READ | GUEST_LINUX_PROT_WRITE));
    assert(guest_linux_vma_find(&memory.vmas,
            base + GUEST_MEMORY_PAGE_SIZE)->protection ==
            GUEST_LINUX_PROT_EXEC);
    assert(guest_linux_mprotect(&memory,
            base + GUEST_MEMORY_PAGE_SIZE,
            GUEST_MEMORY_PAGE_SIZE,
            GUEST_LINUX_PROT_READ | GUEST_LINUX_PROT_WRITE) == 0);
    assert(memory.vmas.count == 1);

    struct guest_tlb tlb;
    guest_tlb_init(&tlb, &table.address_space);
    struct guest_memory_fault fault;
    byte_t value;
    assert(guest_tlb_read(&tlb, base, &value, 1,
            GUEST_MEMORY_READ, &fault));
    assert(value == 0x63);
    assert(guest_linux_mprotect(&memory, base,
            3 * GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_PROT_EXEC) == 0);
    lookup_page(&table, base, GUEST_MEMORY_EXECUTE);
    assert(!guest_tlb_read(&tlb, base, &value, 1,
            GUEST_MEMORY_READ, &fault));
    assert(fault.kind == GUEST_MEMORY_FAULT_PERMISSION);
    assert(guest_tlb_read(&tlb, base, &value, 1,
            GUEST_MEMORY_EXECUTE, &fault));
    assert(value == 0x63);

    assert(guest_linux_mprotect(&memory, base,
            3 * GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_PROT_WRITE) == 0);
    lookup_page(&table, base,
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE);
    assert(guest_linux_munmap(&memory,
            base + GUEST_MEMORY_PAGE_SIZE,
            GUEST_MEMORY_PAGE_SIZE) == 0);
    assert(memory.vmas.count == 2);
    assert(guest_linux_vma_find(&memory.vmas,
            base + GUEST_MEMORY_PAGE_SIZE) == NULL);
    qword_t generation = table.address_space.generation;
    assert(guest_linux_mprotect(&memory, base,
            3 * GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_PROT_READ) ==
            encoded_error(GUEST_LINUX_ENOMEM));
    assert(table.address_space.generation == generation);
    assert(lookup_page(&table, base,
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE)[0] == 0x63);
    assert(lookup_page(&table,
            base + 2 * GUEST_MEMORY_PAGE_SIZE,
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE)[0] == 0x74);
    assert(guest_linux_vma_find(&memory.vmas, base)->protection ==
            GUEST_LINUX_PROT_WRITE);
    assert(guest_linux_vma_find(&memory.vmas,
            base + 2 * GUEST_MEMORY_PAGE_SIZE)->protection ==
            GUEST_LINUX_PROT_WRITE);

    assert(guest_linux_munmap(&memory, base,
            3 * GUEST_MEMORY_PAGE_SIZE) == 0);
    assert(table.address_space.generation == generation + 1);
    assert_not_mapped(&table, base);
    assert_not_mapped(&table, base + GUEST_MEMORY_PAGE_SIZE);
    assert_not_mapped(&table, base + 2 * GUEST_MEMORY_PAGE_SIZE);
    assert(memory.vmas.count == 0);
    generation = table.address_space.generation;
    assert(guest_linux_munmap(&memory, base,
            3 * GUEST_MEMORY_PAGE_SIZE) == 0);
    assert(table.address_space.generation == generation);

    assert(guest_linux_mprotect(&memory, base, 0,
            GUEST_LINUX_PROT_READ) == 0);
    assert(guest_linux_mprotect(&memory, base + 1, 0,
            GUEST_LINUX_PROT_READ) == encoded_error(GUEST_LINUX_EINVAL));
    assert(guest_linux_mprotect(&memory, base, 0,
            UINT64_C(0x8)) == 0);
    assert(guest_linux_mprotect(&memory, TOP_PAGE,
            GUEST_MEMORY_PAGE_SIZE + 1, GUEST_LINUX_PROT_READ) ==
            encoded_error(GUEST_LINUX_ENOMEM));
    assert(guest_linux_munmap(&memory, base + 1,
            GUEST_MEMORY_PAGE_SIZE) == encoded_error(GUEST_LINUX_EINVAL));
    assert(guest_linux_munmap(&memory, base, 0) ==
            encoded_error(GUEST_LINUX_EINVAL));
    assert(guest_linux_munmap(&memory, TOP_PAGE,
            GUEST_MEMORY_PAGE_SIZE + 1) ==
            encoded_error(GUEST_LINUX_EINVAL));
    destroy_memory(&memory, &table);
}

static void test_brk_with_hole(void) {
    struct guest_page_table table;
    assert(guest_page_table_init(&table, 48));
    struct guest_linux_mm memory;
    guest_linux_mm_init(&memory, &table, BRK_BASE, BRK_LIMIT);
    assert(guest_linux_brk(&memory, BRK_LIMIT) == BRK_LIMIT);
    assert(guest_linux_munmap(&memory,
            BRK_BASE + GUEST_MEMORY_PAGE_SIZE,
            GUEST_MEMORY_PAGE_SIZE) == 0);
    assert(guest_linux_brk(&memory, BRK_BASE) == BRK_BASE);
    assert_not_mapped(&table, BRK_BASE);
    assert_not_mapped(&table, BRK_BASE + GUEST_MEMORY_PAGE_SIZE);
    assert_not_mapped(&table, BRK_BASE + 2 * GUEST_MEMORY_PAGE_SIZE);
    destroy_memory(&memory, &table);
}

static void test_madvise(void) {
    struct guest_page_table table;
    assert(guest_page_table_init(&table, 48));
    struct guest_linux_mm memory;
    memset(&memory, 0xff, sizeof(memory));
    guest_linux_mm_init(&memory, &table, BRK_BASE, BRK_LIMIT);
    qword_t flags = GUEST_LINUX_MAP_PRIVATE |
            GUEST_LINUX_MAP_ANONYMOUS | GUEST_LINUX_MAP_FIXED;

    guest_addr_t code_page = (guest_addr_t) memory.mmap_base;
    byte_t *code;
    assert(guest_page_table_map(&table, code_page,
            GUEST_MEMORY_READ | GUEST_MEMORY_EXECUTE,
            &code) == GUEST_PAGE_TABLE_OK);
    memset(code, 0xcc, GUEST_MEMORY_PAGE_SIZE);
    assert(guest_linux_madvise(&memory, code_page,
            GUEST_MEMORY_PAGE_SIZE,
            GUEST_LINUX_MADV_DONTNEED) ==
            encoded_error(GUEST_LINUX_EINVAL));
    assert(code[0] == 0xcc &&
            code[GUEST_MEMORY_PAGE_SIZE - 1] == 0xcc);

    assert(guest_linux_mmap(&memory, code_page,
            GUEST_MEMORY_PAGE_SIZE,
            GUEST_LINUX_PROT_READ | GUEST_LINUX_PROT_WRITE,
            flags, UINT64_MAX, 0) == code_page);
    byte_t *replacement = lookup_page(&table, code_page,
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE);
    memset(replacement, 0x5c, GUEST_MEMORY_PAGE_SIZE);
    assert(guest_linux_madvise(&memory, code_page,
            GUEST_MEMORY_PAGE_SIZE,
            GUEST_LINUX_MADV_DONTNEED) == 0);
    assert(replacement[0] == 0 &&
            replacement[GUEST_MEMORY_PAGE_SIZE - 1] == 0);

    replacement[0] = 0x6d;
    struct guest_page_table clone_table;
    struct guest_linux_mm clone;
    assert(guest_linux_mm_clone(&clone, &clone_table, &memory));
    assert(clone.vmas.count == memory.vmas.count);
    assert(clone.vmas.entries != memory.vmas.entries);
    byte_t *cloned = lookup_page(&clone_table, code_page,
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE);
    assert(cloned[0] == 0x6d);
    assert(guest_linux_madvise(&clone, code_page,
            GUEST_MEMORY_PAGE_SIZE,
            GUEST_LINUX_MADV_DONTNEED) == 0);
    assert(cloned[0] == 0 && replacement[0] == 0x6d);
    destroy_memory(&clone, &clone_table);

    assert(guest_linux_munmap(&memory, code_page,
            GUEST_MEMORY_PAGE_SIZE) == 0);
    byte_t *remapped_code;
    assert(guest_page_table_map(&table, code_page,
            GUEST_MEMORY_READ | GUEST_MEMORY_EXECUTE,
            &remapped_code) == GUEST_PAGE_TABLE_OK);
    memset(remapped_code, 0x8e, GUEST_MEMORY_PAGE_SIZE);
    assert(guest_linux_madvise(&memory, code_page,
            GUEST_MEMORY_PAGE_SIZE,
            GUEST_LINUX_MADV_DONTNEED) ==
            encoded_error(GUEST_LINUX_EINVAL));
    assert(remapped_code[0] == 0x8e &&
            remapped_code[GUEST_MEMORY_PAGE_SIZE - 1] == 0x8e);

    guest_addr_t base = (guest_addr_t) memory.mmap_base +
            64 * GUEST_MEMORY_PAGE_SIZE;
    assert(guest_linux_mmap(&memory, base,
            3 * GUEST_MEMORY_PAGE_SIZE,
            GUEST_LINUX_PROT_READ | GUEST_LINUX_PROT_WRITE,
            flags, UINT64_MAX, 0) == base);

    byte_t *first = lookup_page(&table, base,
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE);
    byte_t *middle = lookup_page(&table,
            base + GUEST_MEMORY_PAGE_SIZE,
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE);
    byte_t *last = lookup_page(&table,
            base + 2 * GUEST_MEMORY_PAGE_SIZE,
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE);
    memset(first, 0x11, GUEST_MEMORY_PAGE_SIZE);
    memset(middle, 0x22, GUEST_MEMORY_PAGE_SIZE);
    memset(last, 0x33, GUEST_MEMORY_PAGE_SIZE);

    qword_t generation = table.address_space.generation;
    qword_t reservation = guest_address_space_track_exclusive(
            &table.address_space, base + GUEST_MEMORY_PAGE_SIZE);
    assert(guest_linux_madvise(&memory,
            base + GUEST_MEMORY_PAGE_SIZE,
            GUEST_MEMORY_PAGE_SIZE + 1,
            GUEST_LINUX_MADV_DONTNEED) == 0);
    assert(first[0] == 0x11 && middle[0] == 0 && last[0] == 0);
    assert(table.address_space.generation == generation);
    assert(!guest_address_space_exclusive_matches(
            &table.address_space,
            base + GUEST_MEMORY_PAGE_SIZE, reservation));
    lookup_page(&table, base + GUEST_MEMORY_PAGE_SIZE,
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE);

    first[0] = 0x44;
    assert(guest_linux_madvise(&memory, base,
            GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_MADV_RANDOM) == 0);
    assert(first[0] == 0x44);

    middle[0] = 0x55;
    last[0] = 0x66;
    assert(guest_linux_munmap(&memory,
            base + GUEST_MEMORY_PAGE_SIZE,
            GUEST_MEMORY_PAGE_SIZE) == 0);
    assert(guest_linux_madvise(&memory, base,
            3 * GUEST_MEMORY_PAGE_SIZE,
            GUEST_LINUX_MADV_DONTNEED) ==
            encoded_error(GUEST_LINUX_ENOMEM));
    assert(first[0] == 0x44 && last[0] == 0x66);

    assert(guest_linux_brk(&memory,
            BRK_BASE + GUEST_MEMORY_PAGE_SIZE) ==
            BRK_BASE + GUEST_MEMORY_PAGE_SIZE);
    byte_t *heap = lookup_page(&table, BRK_BASE,
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE);
    memset(heap, 0x77, GUEST_MEMORY_PAGE_SIZE);
    assert(guest_linux_madvise(&memory, BRK_BASE,
            GUEST_MEMORY_PAGE_SIZE,
            GUEST_LINUX_MADV_DONTNEED) == 0);
    assert(heap[0] == 0 && heap[GUEST_MEMORY_PAGE_SIZE - 1] == 0);

    guest_addr_t foreign_page = BRK_BASE - GUEST_MEMORY_PAGE_SIZE;
    byte_t *sentinel;
    assert(guest_page_table_map(&table, foreign_page,
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE,
            &sentinel) == GUEST_PAGE_TABLE_OK);
    memset(sentinel, 0xa5, GUEST_MEMORY_PAGE_SIZE);
    heap[0] = 0x3c;
    assert(guest_linux_madvise(&memory, foreign_page,
            2 * GUEST_MEMORY_PAGE_SIZE,
            GUEST_LINUX_MADV_DONTNEED) ==
            encoded_error(GUEST_LINUX_EINVAL));
    assert(sentinel[0] == 0xa5 &&
            sentinel[GUEST_MEMORY_PAGE_SIZE - 1] == 0xa5);
    assert(heap[0] == 0x3c);

    guest_addr_t fixed_outside = foreign_page -
            GUEST_MEMORY_PAGE_SIZE;
    assert(guest_linux_mmap(&memory, fixed_outside,
            GUEST_MEMORY_PAGE_SIZE,
            GUEST_LINUX_PROT_READ | GUEST_LINUX_PROT_WRITE,
            flags, UINT64_MAX, 0) == fixed_outside);
    byte_t *outside_anonymous = lookup_page(&table, fixed_outside,
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE);
    memset(outside_anonymous, 0xb7, GUEST_MEMORY_PAGE_SIZE);
    assert(guest_linux_madvise(&memory, fixed_outside,
            GUEST_MEMORY_PAGE_SIZE,
            GUEST_LINUX_MADV_DONTNEED) == 0);
    assert(outside_anonymous[0] == 0 &&
            outside_anonymous[GUEST_MEMORY_PAGE_SIZE - 1] == 0);

    assert(guest_linux_madvise(&memory, base + 1, 0,
            GUEST_LINUX_MADV_NORMAL) ==
            encoded_error(GUEST_LINUX_EINVAL));
    assert(guest_linux_madvise(&memory, base, 0, UINT32_MAX) ==
            encoded_error(GUEST_LINUX_EINVAL));
    assert(guest_linux_madvise(&memory, base, 0,
            GUEST_LINUX_MADV_NORMAL) == 0);
    assert(guest_linux_madvise(&memory, TOP_PAGE,
            GUEST_MEMORY_PAGE_SIZE + 1,
            GUEST_LINUX_MADV_NORMAL) ==
            encoded_error(GUEST_LINUX_ENOMEM));
    assert(guest_linux_madvise(&memory, base, UINT64_MAX,
            GUEST_LINUX_MADV_NORMAL) ==
            encoded_error(GUEST_LINUX_EINVAL));
    destroy_memory(&memory, &table);
}

static void test_vma_transaction_failures(void) {
    assert(guest_linux_vma_test_live_allocation_count() == 0);
    assert(guest_page_backing_test_live_count() == 0);
    guest_linux_vma_test_fail_allocation_at(SIZE_MAX);
    guest_page_backing_test_fail_allocation_at(SIZE_MAX);

    struct guest_page_table table;
    assert(guest_page_table_init(&table, 48));
    struct guest_linux_mm memory;
    guest_linux_mm_init(&memory, &table, BRK_BASE, BRK_LIMIT);

    qword_t generation = table.address_space.generation;
    guest_linux_vma_test_fail_allocation_at(0);
    assert(guest_linux_brk(&memory, BRK_BASE + 1) == BRK_BASE);
    assert(memory.brk == BRK_BASE && memory.vmas.count == 0);
    assert(table.address_space.generation == generation);
    assert_not_mapped(&table, BRK_BASE);

    guest_linux_vma_test_fail_allocation_at(SIZE_MAX);
    assert(guest_linux_brk(&memory, BRK_BASE + 1) == BRK_BASE + 1);
    guest_addr_t base = (guest_addr_t) memory.mmap_base +
            96 * GUEST_MEMORY_PAGE_SIZE;
    qword_t flags = GUEST_LINUX_MAP_PRIVATE |
            GUEST_LINUX_MAP_ANONYMOUS | GUEST_LINUX_MAP_FIXED;
    assert(guest_linux_mmap(&memory, base,
            3 * GUEST_MEMORY_PAGE_SIZE,
            GUEST_LINUX_PROT_READ | GUEST_LINUX_PROT_WRITE,
            flags, UINT64_MAX, 0) == base);
    size_t live_vma_allocations =
            guest_linux_vma_test_live_allocation_count();
    assert(live_vma_allocations == 1);

    guest_linux_vma_test_fail_allocation_at(0);
    assert(guest_linux_mprotect(&memory, base,
            GUEST_MEMORY_PAGE_SIZE,
            GUEST_LINUX_PROT_READ | GUEST_LINUX_PROT_WRITE) == 0);
    assert(guest_linux_vma_test_allocation_count() == 0);
    guest_addr_t failed_page = base + 8 * GUEST_MEMORY_PAGE_SIZE;
    guest_linux_vma_test_fail_allocation_at(0);
    assert(guest_linux_munmap(&memory, failed_page,
            GUEST_MEMORY_PAGE_SIZE) == 0);
    assert(guest_linux_vma_test_allocation_count() == 0);
    assert_not_mapped(&table, failed_page);

    generation = table.address_space.generation;
    guest_linux_vma_test_fail_allocation_at(1);
    assert(guest_linux_mprotect(&memory,
            base + GUEST_MEMORY_PAGE_SIZE,
            GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_PROT_EXEC) ==
            encoded_error(GUEST_LINUX_ENOMEM));
    assert(guest_linux_vma_test_allocation_count() == 2);
    assert(guest_linux_vma_test_live_allocation_count() ==
            live_vma_allocations);
    assert(table.address_space.generation == generation);
    lookup_page(&table, base + GUEST_MEMORY_PAGE_SIZE,
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE);
    assert(guest_linux_vma_find(&memory.vmas,
            base + GUEST_MEMORY_PAGE_SIZE)->protection ==
            (GUEST_LINUX_PROT_READ | GUEST_LINUX_PROT_WRITE));

    guest_linux_vma_test_fail_allocation_at(1);
    assert(guest_linux_munmap(&memory,
            base + GUEST_MEMORY_PAGE_SIZE,
            GUEST_MEMORY_PAGE_SIZE) ==
            encoded_error(GUEST_LINUX_ENOMEM));
    assert(guest_linux_vma_test_allocation_count() == 2);
    assert(guest_linux_vma_test_live_allocation_count() ==
            live_vma_allocations);
    assert(table.address_space.generation == generation);
    lookup_page(&table, base + GUEST_MEMORY_PAGE_SIZE,
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE);

    guest_linux_vma_test_fail_allocation_at(1);
    assert(guest_linux_mmap(&memory, failed_page,
            GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_PROT_READ,
            flags, UINT64_MAX, 0) == encoded_error(GUEST_LINUX_ENOMEM));
    assert(guest_linux_vma_test_allocation_count() == 2);
    assert(table.address_space.generation == generation);
    assert_not_mapped(&table, failed_page);

    guest_linux_vma_test_fail_allocation_at(SIZE_MAX);
    size_t live_backings = guest_page_backing_test_live_count();
    guest_page_backing_test_fail_allocation_at(0);
    assert(guest_linux_mmap(&memory, failed_page,
            GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_PROT_READ,
            flags, UINT64_MAX, 0) == encoded_error(GUEST_LINUX_ENOMEM));
    assert(guest_page_backing_test_allocation_count() == 1);
    assert(guest_page_backing_test_live_count() == live_backings);
    assert(guest_linux_vma_test_live_allocation_count() ==
            live_vma_allocations);
    assert(table.address_space.generation == generation);
    assert_not_mapped(&table, failed_page);

    guest_page_backing_test_fail_allocation_at(SIZE_MAX);
    struct guest_linux_mm failed_memory;
    struct guest_page_table failed_table;
    guest_linux_vma_test_fail_allocation_at(0);
    assert(!guest_linux_mm_clone(
            &failed_memory, &failed_table, &memory));
    assert(failed_memory.page_table == NULL);
    assert(failed_memory.vmas.entries == NULL &&
            failed_memory.vmas.count == 0);
    assert(failed_table.root == NULL);
    assert(guest_linux_vma_test_live_allocation_count() ==
            live_vma_allocations);
    guest_linux_mm_destroy(&failed_memory);
    guest_page_table_destroy(&failed_table);

    guest_linux_vma_test_fail_allocation_at(SIZE_MAX);
    guest_page_backing_test_fail_allocation_at(0);
    assert(!guest_linux_mm_clone(
            &failed_memory, &failed_table, &memory));
    assert(guest_page_backing_test_allocation_count() == 1);
    assert(failed_memory.page_table == NULL &&
            failed_memory.vmas.entries == NULL &&
            failed_memory.vmas.count == 0);
    assert(failed_table.root == NULL);
    assert(guest_page_backing_test_live_count() == live_backings);
    assert(guest_linux_vma_test_live_allocation_count() ==
            live_vma_allocations);
    guest_linux_mm_destroy(&failed_memory);
    guest_page_table_destroy(&failed_table);

    guest_page_backing_test_fail_allocation_at(SIZE_MAX);
    destroy_memory(&memory, &table);
    guest_linux_mm_destroy(&memory);
    assert(guest_linux_vma_test_live_allocation_count() == 0);
    assert(guest_page_backing_test_live_count() == 0);
}

static void test_membarrier_commands_and_lifecycle(void) {
    struct guest_page_table source_table;
    struct guest_page_table early_copy_table;
    struct guest_page_table registered_copy_table;
    struct guest_page_table fresh_table;
    assert(guest_page_table_init(&source_table, 48));
    struct guest_linux_mm source;
    guest_linux_mm_init(&source, &source_table, BRK_BASE, BRK_LIMIT);

    assert(guest_linux_membarrier(&source,
            GUEST_LINUX_MEMBARRIER_CMD_QUERY, 0) ==
            GUEST_LINUX_MEMBARRIER_SUPPORTED_COMMANDS);
    assert(guest_linux_membarrier(&source,
            GUEST_LINUX_MEMBARRIER_CMD_QUERY, 1) ==
            encoded_error(GUEST_LINUX_EINVAL));
    assert(guest_linux_membarrier(&source,
            GUEST_LINUX_MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED, 1) ==
            encoded_error(GUEST_LINUX_EINVAL));
    assert(guest_linux_membarrier(&source, 1, 0) ==
            encoded_error(GUEST_LINUX_EINVAL));
    assert(guest_linux_membarrier(&source,
            GUEST_LINUX_MEMBARRIER_CMD_PRIVATE_EXPEDITED |
                    GUEST_LINUX_MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED,
            0) == encoded_error(GUEST_LINUX_EINVAL));
    assert(guest_linux_membarrier(&source, -1, 0) ==
            encoded_error(GUEST_LINUX_EINVAL));
    assert(guest_linux_membarrier(&source,
            GUEST_LINUX_MEMBARRIER_CMD_GET_REGISTRATIONS, 0) == 0);
    assert(guest_linux_membarrier(&source,
            GUEST_LINUX_MEMBARRIER_CMD_PRIVATE_EXPEDITED, 0) ==
            encoded_error(GUEST_LINUX_EPERM));

    struct guest_linux_mm early_copy;
    assert(guest_linux_mm_clone(
            &early_copy, &early_copy_table, &source));
    assert(guest_linux_membarrier(&source,
            GUEST_LINUX_MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED, 0) == 0);
    assert(guest_linux_membarrier(&source,
            GUEST_LINUX_MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED, 0) == 0);
    assert(guest_linux_membarrier(&source,
            GUEST_LINUX_MEMBARRIER_CMD_GET_REGISTRATIONS, 0) ==
            GUEST_LINUX_MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED);
    assert(guest_linux_membarrier(&source,
            GUEST_LINUX_MEMBARRIER_CMD_PRIVATE_EXPEDITED, 0) == 0);
    assert(guest_linux_membarrier(&early_copy,
            GUEST_LINUX_MEMBARRIER_CMD_GET_REGISTRATIONS, 0) == 0);

    struct guest_linux_mm registered_copy;
    assert(guest_linux_mm_clone(
            &registered_copy, &registered_copy_table, &source));
    assert(guest_linux_membarrier(&registered_copy,
            GUEST_LINUX_MEMBARRIER_CMD_GET_REGISTRATIONS, 0) ==
            GUEST_LINUX_MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED);
    assert(guest_linux_membarrier(&registered_copy,
            GUEST_LINUX_MEMBARRIER_CMD_PRIVATE_EXPEDITED, 0) == 0);

    assert(guest_page_table_init(&fresh_table, 48));
    struct guest_linux_mm fresh;
    // exec 通过 runtime 初始化全新 mm，不能继承旧映像的注册状态。
    guest_linux_mm_init(&fresh, &fresh_table, BRK_BASE, BRK_LIMIT);
    assert(guest_linux_membarrier(&fresh,
            GUEST_LINUX_MEMBARRIER_CMD_GET_REGISTRATIONS, 0) == 0);

    destroy_memory(&fresh, &fresh_table);
    destroy_memory(&registered_copy, &registered_copy_table);
    destroy_memory(&early_copy, &early_copy_table);
    destroy_memory(&source, &source_table);
}

struct membarrier_drain_context {
    struct guest_linux_mm *memory;
    atomic_bool reader_locked;
    atomic_bool release_reader;
    atomic_bool barrier_started;
    atomic_bool barrier_returned;
};

static void *hold_membarrier_read_lock(void *opaque) {
    struct membarrier_drain_context *context = opaque;
    bool locked = guest_page_table_read_lock(
            context->memory->page_table);
    assert(locked);
    atomic_store_explicit(
            &context->reader_locked, true, memory_order_release);
    while (!atomic_load_explicit(
            &context->release_reader, memory_order_acquire))
        sched_yield();
    guest_page_table_read_unlock(context->memory->page_table, locked);
    return NULL;
}

static void *run_private_membarrier(void *opaque) {
    struct membarrier_drain_context *context = opaque;
    atomic_store_explicit(
            &context->barrier_started, true, memory_order_release);
    assert(guest_linux_membarrier(context->memory,
            GUEST_LINUX_MEMBARRIER_CMD_PRIVATE_EXPEDITED, 0) == 0);
    atomic_store_explicit(
            &context->barrier_returned, true, memory_order_release);
    return NULL;
}

static void test_membarrier_drains_concurrent_access(void) {
    struct guest_page_table table;
    assert(guest_page_table_init(&table, 48));
    guest_page_table_enable_concurrency(&table);
    struct guest_linux_mm memory;
    guest_linux_mm_init(&memory, &table, BRK_BASE, BRK_LIMIT);
    assert(guest_linux_membarrier(&memory,
            GUEST_LINUX_MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED, 0) == 0);

    struct membarrier_drain_context context = {
        .memory = &memory,
    };
    atomic_init(&context.reader_locked, false);
    atomic_init(&context.release_reader, false);
    atomic_init(&context.barrier_started, false);
    atomic_init(&context.barrier_returned, false);
    pthread_t reader;
    pthread_t barrier;
    assert(pthread_create(&reader, NULL,
            hold_membarrier_read_lock, &context) == 0);
    while (!atomic_load_explicit(
            &context.reader_locked, memory_order_acquire))
        sched_yield();
    assert(pthread_create(&barrier, NULL,
            run_private_membarrier, &context) == 0);
    while (!atomic_load_explicit(
            &context.barrier_started, memory_order_acquire))
        sched_yield();
    for (unsigned attempt = 0; attempt < 100; attempt++)
        sched_yield();
    assert(!atomic_load_explicit(
            &context.barrier_returned, memory_order_acquire));

    atomic_store_explicit(
            &context.release_reader, true, memory_order_release);
    assert(pthread_join(reader, NULL) == 0);
    assert(pthread_join(barrier, NULL) == 0);
    assert(atomic_load_explicit(
            &context.barrier_returned, memory_order_acquire));
    destroy_memory(&memory, &table);
}

struct concurrency_context {
    struct guest_page_table *table;
    guest_addr_t address;
};

static void *concurrent_reader(void *opaque) {
    const struct concurrency_context *context = opaque;
    struct guest_tlb tlb;
    guest_tlb_init(&tlb, &context->table->address_space);
    struct guest_memory_fault fault;
    dword_t value;
    for (unsigned i = 0; i < CONCURRENCY_ITERATIONS; i++)
        assert(guest_tlb_read(&tlb, context->address,
                &value, sizeof(value), GUEST_MEMORY_READ, &fault));
    return NULL;
}

static void *concurrent_writer(void *opaque) {
    const struct concurrency_context *context = opaque;
    struct guest_tlb tlb;
    guest_tlb_init(&tlb, &context->table->address_space);
    struct guest_memory_fault fault;
    for (dword_t value = 0; value < CONCURRENCY_ITERATIONS; value++)
        assert(guest_tlb_write(&tlb, context->address,
                &value, sizeof(value), &fault));
    return NULL;
}

static void test_concurrent_access_and_protection(void) {
    struct guest_page_table table;
    assert(guest_page_table_init(&table, 48));
    guest_page_table_enable_concurrency(&table);
    struct guest_linux_mm memory;
    guest_linux_mm_init(&memory, &table, BRK_BASE, BRK_LIMIT);
    guest_addr_t address = (guest_addr_t) memory.mmap_base;
    qword_t protection = GUEST_LINUX_PROT_READ |
            GUEST_LINUX_PROT_WRITE;
    assert(guest_linux_mmap(&memory, address,
            GUEST_MEMORY_PAGE_SIZE, protection,
            GUEST_LINUX_MAP_PRIVATE | GUEST_LINUX_MAP_ANONYMOUS |
                    GUEST_LINUX_MAP_FIXED,
            UINT64_MAX, 0) == address);

    struct concurrency_context context = {&table, address};
    pthread_t reader;
    pthread_t writer;
    assert(pthread_create(&reader, NULL,
            concurrent_reader, &context) == 0);
    assert(pthread_create(&writer, NULL,
            concurrent_writer, &context) == 0);
    for (unsigned i = 0; i < CONCURRENCY_ITERATIONS; i++)
        assert(guest_linux_mprotect(&memory, address,
                GUEST_MEMORY_PAGE_SIZE, protection) == 0);
    assert(pthread_join(reader, NULL) == 0);
    assert(pthread_join(writer, NULL) == 0);
    destroy_memory(&memory, &table);
}

int main(void) {
    test_growth_and_shrink();
    test_conflict_rollback();
    test_address_limit();
    test_busybox_mmap_paths();
    test_hints_and_fixed_replacement();
    test_mmap_validation();
    test_mprotect_and_munmap();
    test_brk_with_hole();
    test_madvise();
    test_vma_transaction_failures();
    test_membarrier_commands_and_lifecycle();
    test_membarrier_drains_concurrent_access();
    test_concurrent_access_and_protection();
    return 0;
}
