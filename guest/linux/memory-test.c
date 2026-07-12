#include <assert.h>
#include <pthread.h>
#include <string.h>

#include "guest/linux/errno.h"
#include "guest/linux/memory.h"
#include "guest/linux/mman.h"
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
    assert(guest_linux_mmap(&memory, TOP_PAGE,
            GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_PROT_READ,
            GUEST_LINUX_MAP_PRIVATE | GUEST_LINUX_MAP_ANONYMOUS |
                    GUEST_LINUX_MAP_FIXED,
            UINT64_MAX, 0) == TOP_PAGE);
    lookup_page(&table, TOP_PAGE, GUEST_MEMORY_READ);
    guest_page_table_destroy(&table);
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
    guest_page_table_destroy(&table);
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
    result = guest_linux_mmap(&memory, hint, GUEST_MEMORY_PAGE_SIZE,
            GUEST_LINUX_PROT_WRITE,
            GUEST_LINUX_MAP_PRIVATE | GUEST_LINUX_MAP_ANONYMOUS |
                    GUEST_LINUX_MAP_FIXED_NOREPLACE,
            UINT64_MAX, 0);
    assert(result == encoded_error(GUEST_LINUX_EEXIST));
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
    guest_page_table_destroy(&table);
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
            GUEST_LINUX_MAP_SHARED | GUEST_LINUX_MAP_ANONYMOUS,
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
    guest_page_table_destroy(&table);
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

    assert(guest_linux_munmap(&memory, base,
            3 * GUEST_MEMORY_PAGE_SIZE) == 0);
    assert(table.address_space.generation == generation + 1);
    assert_not_mapped(&table, base);
    assert_not_mapped(&table, base + GUEST_MEMORY_PAGE_SIZE);
    assert_not_mapped(&table, base + 2 * GUEST_MEMORY_PAGE_SIZE);
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
    guest_page_table_destroy(&table);
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
    guest_page_table_destroy(&table);
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
    guest_page_table_destroy(&table);
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
    test_concurrent_access_and_protection();
    return 0;
}
