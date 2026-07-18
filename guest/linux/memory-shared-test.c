#include <assert.h>

#include "guest/linux/errno.h"
#include "guest/linux/memory.h"
#include "guest/linux/mman.h"
#include "guest/memory/page-backing.h"
#include "guest/memory/tlb.h"

#define BRK_BASE UINT64_C(0x0000600000000000)
#define BRK_LIMIT (BRK_BASE + 3 * GUEST_MEMORY_PAGE_SIZE)
#define TEST_BASE (BRK_LIMIT + 32 * GUEST_MEMORY_PAGE_SIZE)
#define PRIVATE_ANONYMOUS \
    (GUEST_LINUX_MAP_PRIVATE | GUEST_LINUX_MAP_ANONYMOUS)
#define SHARED_ANONYMOUS \
    (GUEST_LINUX_MAP_SHARED | GUEST_LINUX_MAP_ANONYMOUS)
#define READ_WRITE \
    (GUEST_LINUX_PROT_READ | GUEST_LINUX_PROT_WRITE)

struct memory_fixture {
    struct guest_page_table table;
    struct guest_linux_mm memory;
    struct guest_tlb tlb;
};

static qword_t encoded_error(unsigned error) {
    return (qword_t) -(sqword_t) error;
}

static void reset_failures(void) {
    guest_linux_vma_test_fail_allocation_at(SIZE_MAX);
    guest_page_backing_test_fail_allocation_at(SIZE_MAX);
    guest_page_table_test_fail_clone_allocation_at(SIZE_MAX);
}

static void fixture_init(struct memory_fixture *fixture) {
    *fixture = (struct memory_fixture) {0};
    assert(guest_page_table_init(&fixture->table, 48));
    guest_linux_mm_init(&fixture->memory, &fixture->table,
            BRK_BASE, BRK_LIMIT);
    guest_tlb_init(&fixture->tlb, &fixture->table.address_space);
}

static void fixture_clone(struct memory_fixture *destination,
        const struct memory_fixture *source) {
    *destination = (struct memory_fixture) {0};
    assert(guest_linux_mm_clone(&destination->memory,
            &destination->table, &source->memory));
    guest_tlb_init(&destination->tlb,
            &destination->table.address_space);
}

static void fixture_destroy(struct memory_fixture *fixture) {
    guest_linux_mm_destroy(&fixture->memory);
    guest_page_table_destroy(&fixture->table);
    *fixture = (struct memory_fixture) {0};
}

static void assert_clean_lifecycle(void) {
    assert(guest_page_backing_test_live_count() == 0);
    assert(guest_linux_vma_test_live_allocation_count() == 0);
}

static byte_t read_byte(struct memory_fixture *fixture,
        guest_addr_t address) {
    struct guest_memory_fault fault;
    byte_t value;
    assert(guest_tlb_read(&fixture->tlb, address,
            &value, sizeof(value), GUEST_MEMORY_READ, &fault));
    return value;
}

static void write_byte(struct memory_fixture *fixture,
        guest_addr_t address, byte_t value) {
    struct guest_memory_fault fault;
    assert(guest_tlb_write(&fixture->tlb, address,
            &value, sizeof(value), &fault));
}

static void assert_read_fault(struct memory_fixture *fixture,
        guest_addr_t address, enum guest_memory_fault_kind expected) {
    struct guest_memory_fault fault;
    byte_t value;
    assert(!guest_tlb_read(&fixture->tlb, address,
            &value, sizeof(value), GUEST_MEMORY_READ, &fault));
    assert(fault.kind == expected);
}

static void assert_write_fault(struct memory_fixture *fixture,
        guest_addr_t address, enum guest_memory_fault_kind expected) {
    struct guest_memory_fault fault;
    const byte_t value = 0xff;
    assert(!guest_tlb_write(&fixture->tlb, address,
            &value, sizeof(value), &fault));
    assert(fault.kind == expected);
}

static qword_t page_sync_identity(struct memory_fixture *fixture,
        guest_addr_t page) {
    bool locked = guest_page_table_read_lock(&fixture->table);
    struct guest_page_view view;
    assert(guest_address_space_resolve_page(
            &fixture->table.address_space, page,
            GUEST_MEMORY_READ, &view) == GUEST_MEMORY_FAULT_NONE);
    qword_t identity = view.sync == NULL ? 0 :
            guest_page_sync_identity(view.sync);
    guest_page_table_read_unlock(&fixture->table, locked);
    return identity;
}

static void assert_vma(struct memory_fixture *fixture,
        guest_addr_t address, enum guest_linux_vma_source source,
        qword_t protection) {
    const struct guest_linux_vma *mapping = guest_linux_vma_find(
            &fixture->memory.vmas, address);
    assert(mapping != NULL);
    assert(mapping->source == source);
    assert(mapping->protection == protection);
}

static void map_fixed(struct memory_fixture *fixture,
        guest_addr_t address, qword_t page_count,
        qword_t protection, qword_t flags) {
    assert(guest_linux_mmap(&fixture->memory, address,
            page_count * GUEST_MEMORY_PAGE_SIZE, protection,
            flags | GUEST_LINUX_MAP_FIXED, UINT64_MAX, 0) == address);
}

static void test_success_and_independent_mappings(void) {
    reset_failures();
    assert_clean_lifecycle();
    struct memory_fixture fixture;
    fixture_init(&fixture);

    qword_t first_result = guest_linux_mmap(&fixture.memory, 0,
            2 * GUEST_MEMORY_PAGE_SIZE, READ_WRITE,
            SHARED_ANONYMOUS, UINT64_MAX, GUEST_MEMORY_PAGE_SIZE);
    assert(first_result == fixture.memory.mmap_base);
    guest_addr_t first = (guest_addr_t) first_result;
    qword_t second_result = guest_linux_mmap(&fixture.memory, 0,
            GUEST_MEMORY_PAGE_SIZE, READ_WRITE,
            SHARED_ANONYMOUS, UINT64_MAX, 0);
    assert(second_result == first + 2 * GUEST_MEMORY_PAGE_SIZE);
    guest_addr_t second = (guest_addr_t) second_result;

    assert_vma(&fixture, first,
            GUEST_LINUX_VMA_SOURCE_ANONYMOUS_SHARED, READ_WRITE);
    assert_vma(&fixture, second,
            GUEST_LINUX_VMA_SOURCE_ANONYMOUS_SHARED, READ_WRITE);
    assert(page_sync_identity(&fixture, first) != 0);
    assert(page_sync_identity(&fixture, second) != 0);
    assert(page_sync_identity(&fixture, first) !=
            page_sync_identity(&fixture, second));
    assert(read_byte(&fixture, first) == 0);
    assert(read_byte(&fixture, second) == 0);
    write_byte(&fixture, first, 0x41);
    write_byte(&fixture, second, 0x52);
    assert(read_byte(&fixture, first) == 0x41);
    assert(read_byte(&fixture, second) == 0x52);

    fixture_destroy(&fixture);
    assert_clean_lifecycle();
}

static void assert_mmap_error_before_allocation(
        struct memory_fixture *fixture, guest_addr_t address,
        qword_t length, qword_t flags, qword_t offset,
        unsigned expected_error) {
    guest_linux_vma_test_fail_allocation_at(0);
    guest_page_backing_test_fail_allocation_at(0);
    assert(guest_linux_mmap(&fixture->memory, address, length,
            READ_WRITE, flags, UINT64_MAX, offset) ==
            encoded_error(expected_error));
    assert(guest_linux_vma_test_allocation_count() == 0);
    assert(guest_page_backing_test_allocation_count() == 0);
    reset_failures();
}

static void test_flags_and_error_priority(void) {
    reset_failures();
    assert_clean_lifecycle();
    struct memory_fixture fixture;
    fixture_init(&fixture);
    map_fixed(&fixture, TEST_BASE, 1, READ_WRITE, SHARED_ANONYMOUS);

    assert_mmap_error_before_allocation(&fixture, 0,
            GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_MAP_ANONYMOUS,
            0, GUEST_LINUX_EINVAL);
    assert_mmap_error_before_allocation(&fixture, 0,
            GUEST_MEMORY_PAGE_SIZE,
            GUEST_LINUX_MAP_SHARED | GUEST_LINUX_MAP_PRIVATE |
                    GUEST_LINUX_MAP_ANONYMOUS,
            0, GUEST_LINUX_EINVAL);
    assert_mmap_error_before_allocation(&fixture, 0,
            GUEST_MEMORY_PAGE_SIZE,
            UINT64_C(0x4) | GUEST_LINUX_MAP_ANONYMOUS,
            0, GUEST_LINUX_EINVAL);
    assert_mmap_error_before_allocation(&fixture, 0,
            GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_MAP_SHARED,
            0, GUEST_LINUX_EINVAL);
    assert_mmap_error_before_allocation(&fixture, 0,
            GUEST_MEMORY_PAGE_SIZE,
            SHARED_ANONYMOUS | UINT64_C(0x40),
            0, GUEST_LINUX_EINVAL);
    assert_mmap_error_before_allocation(&fixture, 0,
            GUEST_MEMORY_PAGE_SIZE, SHARED_ANONYMOUS,
            1, GUEST_LINUX_EINVAL);
    // flags/type 校验早于长度溢出和现有映射冲突。
    assert_mmap_error_before_allocation(&fixture, 0,
            UINT64_MAX,
            GUEST_LINUX_MAP_SHARED | GUEST_LINUX_MAP_PRIVATE |
                    GUEST_LINUX_MAP_ANONYMOUS,
            0, GUEST_LINUX_EINVAL);
    assert_mmap_error_before_allocation(&fixture, TEST_BASE,
            GUEST_MEMORY_PAGE_SIZE,
            GUEST_LINUX_MAP_SHARED | GUEST_LINUX_MAP_PRIVATE |
                    GUEST_LINUX_MAP_ANONYMOUS |
                    GUEST_LINUX_MAP_FIXED_NOREPLACE,
            0, GUEST_LINUX_EINVAL);
    assert_mmap_error_before_allocation(&fixture, TEST_BASE,
            GUEST_MEMORY_PAGE_SIZE,
            SHARED_ANONYMOUS | GUEST_LINUX_MAP_FIXED_NOREPLACE,
            0, GUEST_LINUX_EEXIST);
    assert_mmap_error_before_allocation(&fixture, 0,
            UINT64_MAX, SHARED_ANONYMOUS,
            0, GUEST_LINUX_ENOMEM);
    assert_mmap_error_before_allocation(&fixture, 0,
            0, SHARED_ANONYMOUS, 0, GUEST_LINUX_EINVAL);
    assert_mmap_error_before_allocation(&fixture, TEST_BASE + 1,
            GUEST_MEMORY_PAGE_SIZE,
            SHARED_ANONYMOUS | GUEST_LINUX_MAP_FIXED,
            0, GUEST_LINUX_EINVAL);

    guest_addr_t empty = TEST_BASE + GUEST_MEMORY_PAGE_SIZE;
    assert(guest_linux_mmap(&fixture.memory, empty,
            GUEST_MEMORY_PAGE_SIZE, READ_WRITE,
            SHARED_ANONYMOUS | GUEST_LINUX_MAP_FIXED_NOREPLACE,
            UINT64_MAX, 0) == empty);
    assert_vma(&fixture, empty,
            GUEST_LINUX_VMA_SOURCE_ANONYMOUS_SHARED, READ_WRITE);

    fixture_destroy(&fixture);
    assert_clean_lifecycle();
}

static void test_fork_visibility_and_independent_metadata(void) {
    reset_failures();
    assert_clean_lifecycle();
    struct memory_fixture parent;
    fixture_init(&parent);
    map_fixed(&parent, TEST_BASE, 5, READ_WRITE, SHARED_ANONYMOUS);
    for (qword_t index = 0; index < 5; index++) {
        write_byte(&parent, TEST_BASE + index * GUEST_MEMORY_PAGE_SIZE,
                (byte_t) (0x40 + index));
    }

    struct memory_fixture child;
    fixture_clone(&child, &parent);
    assert(child.memory.vmas.entries != parent.memory.vmas.entries);
    assert(guest_page_backing_test_live_count() == 5);
    assert(read_byte(&child, TEST_BASE) == 0x40);
    write_byte(&child, TEST_BASE, 0x70);
    assert(read_byte(&parent, TEST_BASE) == 0x70);
    write_byte(&parent, TEST_BASE, 0x71);
    assert(read_byte(&child, TEST_BASE) == 0x71);

    guest_addr_t protected_page = TEST_BASE + GUEST_MEMORY_PAGE_SIZE;
    assert(guest_linux_mprotect(&child.memory, protected_page,
            GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_PROT_READ) == 0);
    assert_vma(&child, protected_page,
            GUEST_LINUX_VMA_SOURCE_ANONYMOUS_SHARED,
            GUEST_LINUX_PROT_READ);
    assert_vma(&parent, protected_page,
            GUEST_LINUX_VMA_SOURCE_ANONYMOUS_SHARED, READ_WRITE);
    assert_write_fault(&child, protected_page,
            GUEST_MEMORY_FAULT_PERMISSION);
    write_byte(&parent, protected_page, 0x81);
    assert(read_byte(&child, protected_page) == 0x81);

    guest_addr_t unmapped_page = TEST_BASE + 2 * GUEST_MEMORY_PAGE_SIZE;
    assert(guest_linux_munmap(&child.memory, unmapped_page,
            GUEST_MEMORY_PAGE_SIZE) == 0);
    assert_read_fault(&child, unmapped_page, GUEST_MEMORY_FAULT_UNMAPPED);
    assert(read_byte(&parent, unmapped_page) == 0x42);
    assert(guest_linux_vma_find(&child.memory.vmas, unmapped_page) == NULL);
    assert_vma(&parent, unmapped_page,
            GUEST_LINUX_VMA_SOURCE_ANONYMOUS_SHARED, READ_WRITE);

    guest_addr_t private_page = TEST_BASE + 3 * GUEST_MEMORY_PAGE_SIZE;
    map_fixed(&child, private_page, 1, READ_WRITE, PRIVATE_ANONYMOUS);
    assert(page_sync_identity(&child, private_page) == 0);
    assert(read_byte(&child, private_page) == 0);
    assert(read_byte(&parent, private_page) == 0x43);
    write_byte(&child, private_page, 0x91);
    assert(read_byte(&parent, private_page) == 0x43);
    assert_vma(&child, private_page,
            GUEST_LINUX_VMA_SOURCE_ANONYMOUS_PRIVATE, READ_WRITE);
    assert_vma(&parent, private_page,
            GUEST_LINUX_VMA_SOURCE_ANONYMOUS_SHARED, READ_WRITE);

    guest_addr_t replaced_shared =
            TEST_BASE + 4 * GUEST_MEMORY_PAGE_SIZE;
    qword_t parent_identity = page_sync_identity(&parent, replaced_shared);
    map_fixed(&child, replaced_shared, 1, READ_WRITE, SHARED_ANONYMOUS);
    assert(page_sync_identity(&child, replaced_shared) != 0);
    assert(page_sync_identity(&child, replaced_shared) != parent_identity);
    assert(read_byte(&child, replaced_shared) == 0);
    assert(read_byte(&parent, replaced_shared) == 0x44);
    write_byte(&child, replaced_shared, 0xa1);
    assert(read_byte(&parent, replaced_shared) == 0x44);

    fixture_destroy(&parent);
    assert(read_byte(&child, TEST_BASE) == 0x71);
    write_byte(&child, TEST_BASE, 0xb1);
    assert(read_byte(&child, TEST_BASE) == 0xb1);
    fixture_destroy(&child);
    assert_clean_lifecycle();
}

static void test_mixed_dontneed(void) {
    reset_failures();
    assert_clean_lifecycle();
    struct memory_fixture parent;
    fixture_init(&parent);
    map_fixed(&parent, TEST_BASE, 1, READ_WRITE, PRIVATE_ANONYMOUS);
    map_fixed(&parent, TEST_BASE + GUEST_MEMORY_PAGE_SIZE,
            1, READ_WRITE, SHARED_ANONYMOUS);
    write_byte(&parent, TEST_BASE, 0x11);
    write_byte(&parent, TEST_BASE + GUEST_MEMORY_PAGE_SIZE, 0x22);

    struct memory_fixture child;
    fixture_clone(&child, &parent);
    write_byte(&parent, TEST_BASE, 0x33);
    assert(read_byte(&child, TEST_BASE) == 0x11);
    assert(guest_linux_madvise(&child.memory, TEST_BASE,
            2 * GUEST_MEMORY_PAGE_SIZE,
            GUEST_LINUX_MADV_DONTNEED) == 0);
    assert(read_byte(&child, TEST_BASE) == 0);
    assert(read_byte(&parent, TEST_BASE) == 0x33);
    assert(read_byte(&child,
            TEST_BASE + GUEST_MEMORY_PAGE_SIZE) == 0x22);
    assert(read_byte(&parent,
            TEST_BASE + GUEST_MEMORY_PAGE_SIZE) == 0x22);
    write_byte(&child, TEST_BASE + GUEST_MEMORY_PAGE_SIZE, 0x44);
    assert(read_byte(&parent,
            TEST_BASE + GUEST_MEMORY_PAGE_SIZE) == 0x44);

    // 空洞决定最终返回 ENOMEM，但前面的私有 VMA 仍按 Linux 语义处理。
    write_byte(&child, TEST_BASE, 0x55);
    assert(guest_linux_munmap(&child.memory,
            TEST_BASE + GUEST_MEMORY_PAGE_SIZE,
            GUEST_MEMORY_PAGE_SIZE) == 0);
    assert(guest_linux_madvise(&child.memory, TEST_BASE,
            2 * GUEST_MEMORY_PAGE_SIZE,
            GUEST_LINUX_MADV_DONTNEED) ==
            encoded_error(GUEST_LINUX_ENOMEM));
    assert(read_byte(&child, TEST_BASE) == 0);

    fixture_destroy(&child);
    fixture_destroy(&parent);
    assert_clean_lifecycle();
}

static void assert_original_shared_range(struct memory_fixture *fixture,
        qword_t generation, const qword_t identities[3]) {
    assert(fixture->table.address_space.generation == generation);
    for (qword_t index = 0; index < 3; index++) {
        guest_addr_t page = TEST_BASE +
                index * GUEST_MEMORY_PAGE_SIZE;
        assert(read_byte(fixture, page) == (byte_t) (0x60 + index));
        assert(page_sync_identity(fixture, page) == identities[index]);
        assert_vma(fixture, page,
                GUEST_LINUX_VMA_SOURCE_ANONYMOUS_SHARED, READ_WRITE);
    }
}

static void test_oom_rollback(void) {
    reset_failures();
    assert_clean_lifecycle();
    struct memory_fixture fixture;
    fixture_init(&fixture);
    map_fixed(&fixture, TEST_BASE, 3, READ_WRITE, SHARED_ANONYMOUS);
    qword_t identities[3];
    for (qword_t index = 0; index < 3; index++) {
        guest_addr_t page = TEST_BASE +
                index * GUEST_MEMORY_PAGE_SIZE;
        write_byte(&fixture, page, (byte_t) (0x60 + index));
        identities[index] = page_sync_identity(&fixture, page);
    }
    qword_t generation = fixture.table.address_space.generation;
    unsigned live_backings = guest_page_backing_test_live_count();
    size_t live_vmas = guest_linux_vma_test_live_allocation_count();
    assert(live_backings == 3 && live_vmas == 1);

    // 候选 VMA 的 clone 与覆盖构造任一失败，都不能触碰页表。
    for (size_t fail_at = 0; fail_at < 2; fail_at++) {
        guest_linux_vma_test_fail_allocation_at(fail_at);
        guest_page_backing_test_fail_allocation_at(0);
        assert(guest_linux_mmap(&fixture.memory, TEST_BASE,
                3 * GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_PROT_READ,
                PRIVATE_ANONYMOUS | GUEST_LINUX_MAP_FIXED,
                UINT64_MAX, 0) == encoded_error(GUEST_LINUX_ENOMEM));
        assert(guest_linux_vma_test_allocation_count() == fail_at + 1);
        assert(guest_page_backing_test_allocation_count() == 0);
        assert(guest_page_backing_test_live_count() == live_backings);
        assert(guest_linux_vma_test_live_allocation_count() == live_vmas);
        assert_original_shared_range(&fixture, generation, identities);
    }

    guest_linux_vma_test_fail_allocation_at(SIZE_MAX);
    // 页后备逐页预分配失败时，候选 VMA 与全部临时 backing 一并回滚。
    for (size_t fail_at = 0; fail_at < 3; fail_at++) {
        guest_page_backing_test_fail_allocation_at(fail_at);
        assert(guest_linux_mmap(&fixture.memory, TEST_BASE,
                3 * GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_PROT_READ,
                SHARED_ANONYMOUS | GUEST_LINUX_MAP_FIXED,
                UINT64_MAX, 0) == encoded_error(GUEST_LINUX_ENOMEM));
        assert(guest_page_backing_test_allocation_count() == fail_at + 1);
        assert(guest_page_backing_test_live_count() == live_backings);
        assert(guest_linux_vma_test_live_allocation_count() == live_vmas);
        assert_original_shared_range(&fixture, generation, identities);
    }

    // 切换为私有映射同样只能在所有新 backing 就绪后提交类型变化。
    for (size_t fail_at = 0; fail_at < 3; fail_at++) {
        guest_page_backing_test_fail_allocation_at(fail_at);
        assert(guest_linux_mmap(&fixture.memory, TEST_BASE,
                3 * GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_PROT_EXEC,
                PRIVATE_ANONYMOUS | GUEST_LINUX_MAP_FIXED,
                UINT64_MAX, 0) == encoded_error(GUEST_LINUX_ENOMEM));
        assert(guest_page_backing_test_allocation_count() == fail_at + 1);
        assert(guest_page_backing_test_live_count() == live_backings);
        assert(guest_linux_vma_test_live_allocation_count() == live_vmas);
        assert_original_shared_range(&fixture, generation, identities);
    }

    reset_failures();
    fixture_destroy(&fixture);
    assert_clean_lifecycle();
}

static void assert_failed_clone_is_empty(
        const struct guest_linux_mm *memory,
        const struct guest_page_table *table) {
    assert(memory->page_table == NULL);
    assert(memory->vmas.entries == NULL && memory->vmas.count == 0);
    assert(table->root == NULL);
}

static void test_clone_oom_rollback(void) {
    reset_failures();
    assert_clean_lifecycle();
    struct memory_fixture source;
    fixture_init(&source);
    map_fixed(&source, TEST_BASE, 3, READ_WRITE, SHARED_ANONYMOUS);
    qword_t identities[3];
    for (qword_t index = 0; index < 3; index++) {
        guest_addr_t page = TEST_BASE +
                index * GUEST_MEMORY_PAGE_SIZE;
        write_byte(&source, page, (byte_t) (0x60 + index));
        identities[index] = page_sync_identity(&source, page);
    }
    // 放在更高页表叶节点，确保 clone 已 retain 共享页后才复制私有页。
    guest_addr_t private_page = TEST_BASE + (UINT64_C(1) << 21);
    map_fixed(&source, private_page, 1, READ_WRITE, PRIVATE_ANONYMOUS);
    write_byte(&source, private_page, 0x7f);
    qword_t generation = source.table.address_space.generation;
    unsigned live_backings = guest_page_backing_test_live_count();
    size_t live_vmas = guest_linux_vma_test_live_allocation_count();
    assert(live_backings == 4 && live_vmas == 1);

    guest_page_table_test_fail_clone_allocation_at(SIZE_MAX);
    struct memory_fixture probe;
    fixture_clone(&probe, &source);
    size_t table_clone_allocations =
            guest_page_table_test_clone_allocation_count();
    assert(table_clone_allocations > 4);
    fixture_destroy(&probe);

    for (size_t fail_at = 0;
            fail_at < table_clone_allocations; fail_at++) {
        guest_page_table_test_fail_clone_allocation_at(fail_at);
        struct guest_linux_mm failed_memory;
        struct guest_page_table failed_table;
        assert(!guest_linux_mm_clone(
                &failed_memory, &failed_table, &source.memory));
        assert_failed_clone_is_empty(&failed_memory, &failed_table);
        assert(guest_page_backing_test_live_count() == live_backings);
        assert(guest_linux_vma_test_live_allocation_count() == live_vmas);
        assert_original_shared_range(&source, generation, identities);
        assert(read_byte(&source, private_page) == 0x7f);
        guest_linux_mm_destroy(&failed_memory);
        guest_page_table_destroy(&failed_table);
    }
    guest_page_table_test_fail_clone_allocation_at(SIZE_MAX);

    struct guest_linux_mm failed_memory;
    struct guest_page_table failed_table;
    guest_linux_vma_test_fail_allocation_at(0);
    assert(!guest_linux_mm_clone(
            &failed_memory, &failed_table, &source.memory));
    assert_failed_clone_is_empty(&failed_memory, &failed_table);
    assert(guest_page_backing_test_live_count() == live_backings);
    assert(guest_linux_vma_test_live_allocation_count() == live_vmas);
    assert_original_shared_range(&source, generation, identities);
    assert(read_byte(&source, private_page) == 0x7f);
    assert(page_sync_identity(&source, private_page) == 0);
    guest_linux_mm_destroy(&failed_memory);
    guest_page_table_destroy(&failed_table);

    guest_linux_vma_test_fail_allocation_at(SIZE_MAX);
    guest_page_backing_test_fail_allocation_at(0);
    assert(!guest_linux_mm_clone(
            &failed_memory, &failed_table, &source.memory));
    assert(guest_page_backing_test_allocation_count() == 1);
    assert_failed_clone_is_empty(&failed_memory, &failed_table);
    assert(guest_page_backing_test_live_count() == live_backings);
    assert(guest_linux_vma_test_live_allocation_count() == live_vmas);
    assert_original_shared_range(&source, generation, identities);
    assert(read_byte(&source, private_page) == 0x7f);
    assert(page_sync_identity(&source, private_page) == 0);
    guest_linux_mm_destroy(&failed_memory);
    guest_page_table_destroy(&failed_table);

    reset_failures();
    fixture_destroy(&source);
    assert_clean_lifecycle();
}

int main(void) {
    test_success_and_independent_mappings();
    test_flags_and_error_priority();
    test_fork_visibility_and_independent_metadata();
    test_mixed_dontneed();
    test_oom_rollback();
    test_clone_oom_rollback();
    reset_failures();
    assert_clean_lifecycle();
    return 0;
}
