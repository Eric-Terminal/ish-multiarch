#include <assert.h>

#include "guest/linux/mman.h"
#include "guest/linux/vma.h"

#define PAGE(index) \
    ((guest_addr_t) ((qword_t) (index) * GUEST_MEMORY_PAGE_SIZE))

struct pager_probe {
    unsigned releases;
};

static enum guest_file_page_result unused_read_page(void *opaque,
        qword_t file_offset, byte_t *page, dword_t *valid_bytes) {
    use(opaque);
    use(file_offset);
    use(page);
    use(valid_bytes);
    return GUEST_FILE_PAGE_END_OF_FILE;
}

static void pager_released(
        struct guest_file_pager *pager, void *opaque) {
    use(pager);
    ((struct pager_probe *) opaque)->releases++;
}

static struct guest_file_pager *pager_create(
        qword_t identity, struct pager_probe *probe) {
    return guest_file_pager_create(identity,
            (struct guest_file_pager_provider) {
                .opaque = probe,
                .read_page = unused_read_page,
                .release = pager_released,
            });
}

static struct guest_linux_vma mapping(guest_addr_t first,
        guest_addr_t last, qword_t protection,
        enum guest_linux_vma_source source) {
    return (struct guest_linux_vma) {
        .first = first,
        .last = last,
        .protection = protection,
        .source = source,
        .maximum_protection = GUEST_LINUX_PROT_READ |
                GUEST_LINUX_PROT_WRITE | GUEST_LINUX_PROT_EXEC,
    };
}

static struct guest_linux_vma file_mapping(
        struct guest_file_pager *pager,
        guest_addr_t first, guest_addr_t last,
        qword_t file_offset, qword_t protection,
        qword_t maximum_protection) {
    return (struct guest_linux_vma) {
        .first = first,
        .last = last,
        .protection = protection,
        .source = GUEST_LINUX_VMA_SOURCE_FILE_PRIVATE,
        .maximum_protection = maximum_protection,
        .file_pager = pager,
        .file_offset = file_offset,
    };
}

static void assert_entry(const struct guest_linux_vma_set *set,
        size_t index, struct guest_linux_vma expected) {
    assert(index < set->count);
    const struct guest_linux_vma *actual = &set->entries[index];
    assert(actual->first == expected.first);
    assert(actual->last == expected.last);
    assert(actual->protection == expected.protection);
    assert(actual->source == expected.source);
    assert(actual->maximum_protection == expected.maximum_protection);
    assert(actual->file_pager == expected.file_pager);
    assert(actual->file_offset == expected.file_offset);
}

static void assert_same_set(const struct guest_linux_vma_set *set,
        const struct guest_linux_vma *expected, size_t count) {
    assert(set->count == count);
    for (size_t index = 0; index < count; index++)
        assert_entry(set, index, expected[index]);
}

static void test_insert_override_merge_and_find(void) {
    const qword_t read = GUEST_LINUX_PROT_READ;
    const qword_t read_write = read | GUEST_LINUX_PROT_WRITE;
    const qword_t execute = GUEST_LINUX_PROT_EXEC;
    struct guest_linux_vma_set set;
    guest_linux_vma_set_init(&set);
    assert(guest_linux_vma_find(&set, 0) == NULL);

    assert(guest_linux_vma_insert(&set, mapping(PAGE(3), PAGE(5), read,
            GUEST_LINUX_VMA_SOURCE_ANONYMOUS_PRIVATE)));
    assert(guest_linux_vma_insert(&set, mapping(PAGE(1), PAGE(3), read,
            GUEST_LINUX_VMA_SOURCE_ANONYMOUS_PRIVATE)));
    assert(set.count == 1);
    assert_entry(&set, 0, mapping(PAGE(1), PAGE(5), read,
            GUEST_LINUX_VMA_SOURCE_ANONYMOUS_PRIVATE));

    assert(guest_linux_vma_insert(&set,
            mapping(PAGE(7), PAGE(9), read_write,
                    GUEST_LINUX_VMA_SOURCE_BRK)));
    assert(guest_linux_vma_insert(&set, mapping(PAGE(5), PAGE(7), read,
            GUEST_LINUX_VMA_SOURCE_ANONYMOUS_PRIVATE)));
    assert(set.count == 2);
    assert_entry(&set, 0, mapping(PAGE(1), PAGE(7), read,
            GUEST_LINUX_VMA_SOURCE_ANONYMOUS_PRIVATE));
    assert_entry(&set, 1, mapping(PAGE(7), PAGE(9), read_write,
            GUEST_LINUX_VMA_SOURCE_BRK));

    assert(guest_linux_vma_insert(&set, mapping(PAGE(2), PAGE(8), execute,
            GUEST_LINUX_VMA_SOURCE_ANONYMOUS_PRIVATE)));
    assert(set.count == 3);
    assert_entry(&set, 0, mapping(PAGE(1), PAGE(2), read,
            GUEST_LINUX_VMA_SOURCE_ANONYMOUS_PRIVATE));
    assert_entry(&set, 1, mapping(PAGE(2), PAGE(8), execute,
            GUEST_LINUX_VMA_SOURCE_ANONYMOUS_PRIVATE));
    assert_entry(&set, 2, mapping(PAGE(8), PAGE(9), read_write,
            GUEST_LINUX_VMA_SOURCE_BRK));

    assert(guest_linux_vma_find(&set, PAGE(1) - 1) == NULL);
    assert(guest_linux_vma_find(&set, PAGE(1))->protection == read);
    assert(guest_linux_vma_find(&set, PAGE(2))->protection == execute);
    assert(guest_linux_vma_find(&set, PAGE(8))->source ==
            GUEST_LINUX_VMA_SOURCE_BRK);
    assert(guest_linux_vma_find(&set, PAGE(9)) == NULL);

    assert(guest_linux_vma_insert(&set, mapping(PAGE(1), PAGE(9), execute,
            GUEST_LINUX_VMA_SOURCE_ANONYMOUS_PRIVATE)));
    assert(set.count == 1);
    assert_entry(&set, 0, mapping(PAGE(1), PAGE(9), execute,
            GUEST_LINUX_VMA_SOURCE_ANONYMOUS_PRIVATE));

    // 已有区间完整包含同属性请求时不应为了空操作承担 OOM 风险。
    guest_linux_vma_test_fail_allocation_at(0);
    assert(guest_linux_vma_insert(&set, mapping(PAGE(2), PAGE(8), execute,
            GUEST_LINUX_VMA_SOURCE_ANONYMOUS_PRIVATE)));
    assert(guest_linux_vma_test_allocation_count() == 0);
    assert(set.count == 1);

    guest_linux_vma_test_fail_allocation_at(SIZE_MAX);
    guest_linux_vma_set_destroy(&set);
}

static void test_source_boundaries_and_clone_independence(void) {
    const qword_t read = GUEST_LINUX_PROT_READ;
    const qword_t write = GUEST_LINUX_PROT_WRITE;
    const qword_t execute = GUEST_LINUX_PROT_EXEC;
    struct guest_linux_vma_set set;
    guest_linux_vma_set_init(&set);

    // 权限相同也不能跨越私有、共享来源合并。
    assert(guest_linux_vma_insert(&set, mapping(PAGE(1), PAGE(3), read,
            GUEST_LINUX_VMA_SOURCE_ANONYMOUS_PRIVATE)));
    assert(guest_linux_vma_insert(&set, mapping(PAGE(3), PAGE(5), read,
            GUEST_LINUX_VMA_SOURCE_ANONYMOUS_SHARED)));
    const struct guest_linux_vma adjacent[] = {
        mapping(PAGE(1), PAGE(3), read,
                GUEST_LINUX_VMA_SOURCE_ANONYMOUS_PRIVATE),
        mapping(PAGE(3), PAGE(5), read,
                GUEST_LINUX_VMA_SOURCE_ANONYMOUS_SHARED),
    };
    assert_same_set(&set, adjacent, array_size(adjacent));

    assert(guest_linux_vma_protect_tracked(
            &set, PAGE(2), PAGE(4), write));
    const struct guest_linux_vma protected[] = {
        mapping(PAGE(1), PAGE(2), read,
                GUEST_LINUX_VMA_SOURCE_ANONYMOUS_PRIVATE),
        mapping(PAGE(2), PAGE(3), write,
                GUEST_LINUX_VMA_SOURCE_ANONYMOUS_PRIVATE),
        mapping(PAGE(3), PAGE(4), write,
                GUEST_LINUX_VMA_SOURCE_ANONYMOUS_SHARED),
        mapping(PAGE(4), PAGE(5), read,
                GUEST_LINUX_VMA_SOURCE_ANONYMOUS_SHARED),
    };
    assert_same_set(&set, protected, array_size(protected));

    guest_linux_vma_set_destroy(&set);
    guest_linux_vma_set_init(&set);
    assert(guest_linux_vma_insert(&set, mapping(PAGE(1), PAGE(6), read,
            GUEST_LINUX_VMA_SOURCE_ANONYMOUS_SHARED)));
    assert(guest_linux_vma_insert(&set, mapping(PAGE(2), PAGE(5), execute,
            GUEST_LINUX_VMA_SOURCE_ANONYMOUS_PRIVATE)));
    const struct guest_linux_vma split[] = {
        mapping(PAGE(1), PAGE(2), read,
                GUEST_LINUX_VMA_SOURCE_ANONYMOUS_SHARED),
        mapping(PAGE(2), PAGE(5), execute,
                GUEST_LINUX_VMA_SOURCE_ANONYMOUS_PRIVATE),
        mapping(PAGE(5), PAGE(6), read,
                GUEST_LINUX_VMA_SOURCE_ANONYMOUS_SHARED),
    };
    assert_same_set(&set, split, array_size(split));

    struct guest_linux_vma_set copy;
    assert(guest_linux_vma_set_clone(&copy, &set));
    assert(copy.entries != set.entries);
    assert_same_set(&copy, split, array_size(split));
    assert(guest_linux_vma_remove(&set, PAGE(1), PAGE(2)));
    assert(set.count == 2 && copy.count == 3);
    assert(copy.entries[0].source ==
            GUEST_LINUX_VMA_SOURCE_ANONYMOUS_SHARED);
    assert(guest_linux_vma_protect_tracked(
            &copy, PAGE(2), PAGE(5), write));
    assert(copy.entries[1].source ==
            GUEST_LINUX_VMA_SOURCE_ANONYMOUS_PRIVATE);
    assert(copy.entries[1].protection == write);
    assert(set.entries[0].source ==
            GUEST_LINUX_VMA_SOURCE_ANONYMOUS_PRIVATE);
    assert(set.entries[0].protection == execute);

    guest_linux_vma_set_destroy(&copy);
    guest_linux_vma_set_destroy(&set);
}

static void test_remove_trim_split_and_holes(void) {
    struct guest_linux_vma_set set;
    guest_linux_vma_set_init(&set);
    assert(guest_linux_vma_insert(&set,
            mapping(PAGE(1), PAGE(9), GUEST_LINUX_PROT_READ,
                    GUEST_LINUX_VMA_SOURCE_ANONYMOUS_PRIVATE)));

    assert(guest_linux_vma_remove(&set, PAGE(3), PAGE(5)));
    assert(set.count == 2);
    assert_entry(&set, 0,
            mapping(PAGE(1), PAGE(3), GUEST_LINUX_PROT_READ,
                    GUEST_LINUX_VMA_SOURCE_ANONYMOUS_PRIVATE));
    assert_entry(&set, 1,
            mapping(PAGE(5), PAGE(9), GUEST_LINUX_PROT_READ,
                    GUEST_LINUX_VMA_SOURCE_ANONYMOUS_PRIVATE));

    assert(guest_linux_vma_remove(&set, 0, PAGE(2)));
    assert(guest_linux_vma_remove(&set, PAGE(7), PAGE(10)));
    assert(set.count == 2);
    assert_entry(&set, 0,
            mapping(PAGE(2), PAGE(3), GUEST_LINUX_PROT_READ,
                    GUEST_LINUX_VMA_SOURCE_ANONYMOUS_PRIVATE));
    assert_entry(&set, 1,
            mapping(PAGE(5), PAGE(7), GUEST_LINUX_PROT_READ,
                    GUEST_LINUX_VMA_SOURCE_ANONYMOUS_PRIVATE));

    guest_linux_vma_test_fail_allocation_at(0);
    assert(guest_linux_vma_remove(&set, PAGE(3), PAGE(5)));
    assert(guest_linux_vma_test_allocation_count() == 0);
    assert(set.count == 2);

    guest_linux_vma_test_fail_allocation_at(SIZE_MAX);
    assert(guest_linux_vma_remove(&set, 0, PAGE(10)));
    assert(set.count == 0 && set.entries == NULL);
    guest_linux_vma_set_destroy(&set);
    guest_linux_vma_set_destroy(&set);
}

static void test_strict_subrange_split_and_remerge(void) {
    struct guest_linux_vma_set set;
    guest_linux_vma_set_init(&set);
    assert(guest_linux_vma_insert(&set,
            mapping(PAGE(1), PAGE(9), GUEST_LINUX_PROT_READ,
                    GUEST_LINUX_VMA_SOURCE_ANONYMOUS_PRIVATE)));

    // 单区间内覆盖会同时保留左右残片，打满 insert 的 n + 2 上界。
    assert(guest_linux_vma_insert(&set,
            mapping(PAGE(3), PAGE(7), GUEST_LINUX_PROT_EXEC,
                    GUEST_LINUX_VMA_SOURCE_ANONYMOUS_PRIVATE)));
    assert(set.count == 3);
    assert_entry(&set, 0,
            mapping(PAGE(1), PAGE(3), GUEST_LINUX_PROT_READ,
                    GUEST_LINUX_VMA_SOURCE_ANONYMOUS_PRIVATE));
    assert_entry(&set, 1,
            mapping(PAGE(3), PAGE(7), GUEST_LINUX_PROT_EXEC,
                    GUEST_LINUX_VMA_SOURCE_ANONYMOUS_PRIVATE));
    assert_entry(&set, 2,
            mapping(PAGE(7), PAGE(9), GUEST_LINUX_PROT_READ,
                    GUEST_LINUX_VMA_SOURCE_ANONYMOUS_PRIVATE));

    assert(guest_linux_vma_protect_tracked(
            &set, PAGE(3), PAGE(7), GUEST_LINUX_PROT_READ));
    assert(set.count == 1);
    assert_entry(&set, 0,
            mapping(PAGE(1), PAGE(9), GUEST_LINUX_PROT_READ,
                    GUEST_LINUX_VMA_SOURCE_ANONYMOUS_PRIVATE));
    guest_linux_vma_set_destroy(&set);
}

static void test_protect_only_tracked_intersections(void) {
    const qword_t read = GUEST_LINUX_PROT_READ;
    const qword_t read_write = read | GUEST_LINUX_PROT_WRITE;
    const qword_t execute = GUEST_LINUX_PROT_EXEC;
    struct guest_linux_vma_set set;
    guest_linux_vma_set_init(&set);
    assert(guest_linux_vma_insert(&set, mapping(PAGE(1), PAGE(5), read,
            GUEST_LINUX_VMA_SOURCE_ANONYMOUS_PRIVATE)));
    assert(guest_linux_vma_insert(&set, mapping(PAGE(7), PAGE(9), read,
            GUEST_LINUX_VMA_SOURCE_ANONYMOUS_PRIVATE)));
    assert(guest_linux_vma_insert(&set,
            mapping(PAGE(9), PAGE(11), read_write,
                    GUEST_LINUX_VMA_SOURCE_BRK)));

    assert(guest_linux_vma_protect_tracked(
            &set, PAGE(3), PAGE(10), execute));
    const struct guest_linux_vma expected[] = {
        mapping(PAGE(1), PAGE(3), read,
                GUEST_LINUX_VMA_SOURCE_ANONYMOUS_PRIVATE),
        mapping(PAGE(3), PAGE(5), execute,
                GUEST_LINUX_VMA_SOURCE_ANONYMOUS_PRIVATE),
        mapping(PAGE(7), PAGE(9), execute,
                GUEST_LINUX_VMA_SOURCE_ANONYMOUS_PRIVATE),
        mapping(PAGE(9), PAGE(10), execute,
                GUEST_LINUX_VMA_SOURCE_BRK),
        mapping(PAGE(10), PAGE(11), read_write,
                GUEST_LINUX_VMA_SOURCE_BRK),
    };
    assert_same_set(&set, expected, array_size(expected));

    // 范围覆盖并超过整个 VMA 时只产生一次修改，不追加原区间副本。
    assert(guest_linux_vma_protect_tracked(
            &set, 0, PAGE(6), read_write));
    assert(set.count == 4);
    assert_entry(&set, 0,
            mapping(PAGE(1), PAGE(5), read_write,
                    GUEST_LINUX_VMA_SOURCE_ANONYMOUS_PRIVATE));
    assert_entry(&set, 1,
            mapping(PAGE(7), PAGE(9), execute,
                    GUEST_LINUX_VMA_SOURCE_ANONYMOUS_PRIVATE));

    // 未登记的空洞可能属于 ELF/栈/跳板，VMA 层只保持原样并成功返回。
    guest_linux_vma_test_fail_allocation_at(0);
    assert(guest_linux_vma_protect_tracked(
            &set, PAGE(5), PAGE(7), execute));
    assert(guest_linux_vma_test_allocation_count() == 0);
    assert(guest_linux_vma_protect_tracked(
            &set, PAGE(7), PAGE(9), execute));
    assert(guest_linux_vma_test_allocation_count() == 0);

    guest_linux_vma_test_fail_allocation_at(SIZE_MAX);
    assert(guest_linux_vma_protect_tracked(
            &set, PAGE(7), PAGE(11), read_write));
    assert(set.count == 3);
    assert_entry(&set, 0,
            mapping(PAGE(1), PAGE(5), read_write,
                    GUEST_LINUX_VMA_SOURCE_ANONYMOUS_PRIVATE));
    assert_entry(&set, 1,
            mapping(PAGE(7), PAGE(9), read_write,
                    GUEST_LINUX_VMA_SOURCE_ANONYMOUS_PRIVATE));
    assert_entry(&set, 2,
            mapping(PAGE(9), PAGE(11), read_write,
                    GUEST_LINUX_VMA_SOURCE_BRK));
    guest_linux_vma_set_destroy(&set);
}

static void test_clone_boundaries_and_independence(void) {
    struct guest_linux_vma_set source;
    struct guest_linux_vma_set copy;
    guest_linux_vma_set_init(&source);
    assert(guest_linux_vma_insert(&source,
            mapping(0, PAGE(1), GUEST_LINUX_PROT_READ,
                    GUEST_LINUX_VMA_SOURCE_ANONYMOUS_PRIVATE)));
    guest_addr_t high_last = (guest_addr_t)
            (UINT64_MAX & ~GUEST_MEMORY_PAGE_MASK);
    guest_addr_t high_first = high_last - GUEST_MEMORY_PAGE_SIZE;
    assert(guest_linux_vma_insert(&source,
            mapping(high_first, high_last, GUEST_LINUX_PROT_EXEC,
                    GUEST_LINUX_VMA_SOURCE_BRK)));
    assert(guest_linux_vma_set_clone(&copy, &source));
    assert(copy.entries != source.entries);
    assert_same_set(&copy, source.entries, source.count);
    assert(guest_linux_vma_find(&copy, 0) == &copy.entries[0]);
    assert(guest_linux_vma_find(&copy, PAGE(1)) == NULL);
    assert(guest_linux_vma_find(&copy, high_first) == &copy.entries[1]);
    assert(guest_linux_vma_find(&copy, high_last - 1) == &copy.entries[1]);
    assert(guest_linux_vma_find(&copy, high_last) == NULL);

    assert(guest_linux_vma_remove(&source, 0, PAGE(1)));
    assert(source.count == 1 && copy.count == 2);
    assert(copy.entries[0].first == 0);
    guest_linux_vma_set_destroy(&copy);
    guest_linux_vma_set_destroy(&source);

    struct guest_linux_vma_set empty;
    struct guest_linux_vma_set empty_copy;
    guest_linux_vma_set_init(&empty);
    guest_linux_vma_test_fail_allocation_at(0);
    assert(guest_linux_vma_set_clone(&empty_copy, &empty));
    assert(guest_linux_vma_test_allocation_count() == 0);
    assert(empty_copy.count == 0 && empty_copy.entries == NULL);
    guest_linux_vma_set_destroy(&empty_copy);
    guest_linux_vma_set_destroy(&empty);
}

static void test_allocation_failure_is_transactional(void) {
    const qword_t read = GUEST_LINUX_PROT_READ;
    struct guest_linux_vma_set set;
    guest_linux_vma_set_init(&set);
    guest_linux_vma_test_fail_allocation_at(SIZE_MAX);
    assert(guest_linux_vma_insert(&set, mapping(PAGE(1), PAGE(3), read,
            GUEST_LINUX_VMA_SOURCE_ANONYMOUS_PRIVATE)));
    assert(guest_linux_vma_insert(&set, mapping(PAGE(5), PAGE(7), read,
            GUEST_LINUX_VMA_SOURCE_ANONYMOUS_PRIVATE)));
    const struct guest_linux_vma snapshot[] = {
        mapping(PAGE(1), PAGE(3), read,
                GUEST_LINUX_VMA_SOURCE_ANONYMOUS_PRIVATE),
        mapping(PAGE(5), PAGE(7), read,
                GUEST_LINUX_VMA_SOURCE_ANONYMOUS_PRIVATE),
    };
    assert_same_set(&set, snapshot, array_size(snapshot));
    assert(guest_linux_vma_test_live_allocation_count() == 1);

    guest_linux_vma_test_fail_allocation_at(0);
    assert(!guest_linux_vma_insert(&set,
            mapping(PAGE(2), PAGE(6), GUEST_LINUX_PROT_EXEC,
                    GUEST_LINUX_VMA_SOURCE_ANONYMOUS_PRIVATE)));
    assert(guest_linux_vma_test_allocation_count() == 1);
    assert_same_set(&set, snapshot, array_size(snapshot));
    assert(guest_linux_vma_test_live_allocation_count() == 1);

    guest_linux_vma_test_fail_allocation_at(0);
    assert(!guest_linux_vma_remove(&set, PAGE(2), PAGE(6)));
    assert_same_set(&set, snapshot, array_size(snapshot));
    assert(guest_linux_vma_test_live_allocation_count() == 1);

    guest_linux_vma_test_fail_allocation_at(0);
    assert(!guest_linux_vma_protect_tracked(
            &set, PAGE(2), PAGE(6), GUEST_LINUX_PROT_WRITE));
    assert_same_set(&set, snapshot, array_size(snapshot));
    assert(guest_linux_vma_test_live_allocation_count() == 1);

    struct guest_linux_vma_set failed_copy;
    guest_linux_vma_test_fail_allocation_at(0);
    assert(!guest_linux_vma_set_clone(&failed_copy, &set));
    assert(failed_copy.count == 0 && failed_copy.entries == NULL);
    assert_same_set(&set, snapshot, array_size(snapshot));
    assert(guest_linux_vma_test_live_allocation_count() == 1);

    guest_linux_vma_test_fail_allocation_at(SIZE_MAX);
    guest_linux_vma_set_destroy(&failed_copy);
    guest_linux_vma_set_destroy(&set);
    assert(guest_linux_vma_test_live_allocation_count() == 0);
}

static void test_file_offsets_ownership_and_merge(void) {
    const qword_t read = GUEST_LINUX_PROT_READ;
    const qword_t all = GUEST_LINUX_PROT_MASK;
    const qword_t base_offset = UINT64_C(0x10000);
    struct pager_probe first_probe = {0};
    struct pager_probe second_probe = {0};
    struct guest_file_pager *first_pager = pager_create(1, &first_probe);
    struct guest_file_pager *second_pager = pager_create(2, &second_probe);
    assert(first_pager != NULL && second_pager != NULL);

    struct guest_linux_vma_set set;
    guest_linux_vma_set_init(&set);
    assert(guest_linux_vma_insert(&set,
            file_mapping(first_pager, PAGE(1), PAGE(9),
                    base_offset, read, all)));
    assert(guest_file_pager_test_reference_count(first_pager) == 2);

    assert(guest_linux_vma_remove(&set, PAGE(3), PAGE(5)));
    assert(set.count == 2);
    assert_entry(&set, 0,
            file_mapping(first_pager, PAGE(1), PAGE(3),
                    base_offset, read, all));
    assert_entry(&set, 1,
            file_mapping(first_pager, PAGE(5), PAGE(9),
                    base_offset + PAGE(4), read, all));
    assert(guest_file_pager_test_reference_count(first_pager) == 3);

    assert(guest_linux_vma_protect_tracked(
            &set, PAGE(6), PAGE(8), GUEST_LINUX_PROT_EXEC));
    assert(set.count == 4);
    assert_entry(&set, 1,
            file_mapping(first_pager, PAGE(5), PAGE(6),
                    base_offset + PAGE(4), read, all));
    assert_entry(&set, 2,
            file_mapping(first_pager, PAGE(6), PAGE(8),
                    base_offset + PAGE(5),
                    GUEST_LINUX_PROT_EXEC, all));
    assert_entry(&set, 3,
            file_mapping(first_pager, PAGE(8), PAGE(9),
                    base_offset + PAGE(7), read, all));
    assert(guest_file_pager_test_reference_count(first_pager) == 5);

    struct guest_linux_vma_set copy;
    assert(guest_linux_vma_set_clone(&copy, &set));
    assert(guest_file_pager_test_reference_count(first_pager) == 9);
    guest_linux_vma_set_destroy(&copy);
    assert(guest_file_pager_test_reference_count(first_pager) == 5);
    guest_linux_vma_set_destroy(&set);
    assert(guest_file_pager_test_reference_count(first_pager) == 1);

    guest_linux_vma_set_init(&set);
    assert(guest_linux_vma_insert(&set,
            file_mapping(first_pager, PAGE(1), PAGE(3),
                    base_offset, read, all)));
    assert(guest_linux_vma_insert(&set,
            file_mapping(first_pager, PAGE(3), PAGE(5),
                    base_offset + PAGE(2), read, all)));
    assert(set.count == 1);
    assert_entry(&set, 0,
            file_mapping(first_pager, PAGE(1), PAGE(5),
                    base_offset, read, all));

    // 完整包含且逻辑文件偏移一致的子区间仍是无分配空操作。
    guest_linux_vma_test_fail_allocation_at(0);
    assert(guest_linux_vma_insert(&set,
            file_mapping(first_pager, PAGE(2), PAGE(4),
                    base_offset + PAGE(1), read, all)));
    assert(guest_linux_vma_test_allocation_count() == 0 && set.count == 1);
    guest_linux_vma_test_fail_allocation_at(SIZE_MAX);

    // 同 pager/权限不代表同一文件区间，固定覆盖必须替换中间偏移。
    assert(guest_linux_vma_insert(&set,
            file_mapping(first_pager, PAGE(2), PAGE(4),
                    base_offset + PAGE(4), read, all)));
    const struct guest_linux_vma replaced_offsets[] = {
        file_mapping(first_pager, PAGE(1), PAGE(2),
                base_offset, read, all),
        file_mapping(first_pager, PAGE(2), PAGE(4),
                base_offset + PAGE(4), read, all),
        file_mapping(first_pager, PAGE(4), PAGE(5),
                base_offset + PAGE(3), read, all),
    };
    assert_same_set(&set, replaced_offsets,
            array_size(replaced_offsets));
    guest_linux_vma_set_destroy(&set);
    guest_linux_vma_set_init(&set);
    assert(guest_linux_vma_insert(&set,
            file_mapping(first_pager, PAGE(1), PAGE(5),
                    base_offset, read, all)));

    // 文件偏移不连续、最大权限不同或 pager 不同都不能合并。
    assert(guest_linux_vma_insert(&set,
            file_mapping(first_pager, PAGE(5), PAGE(6),
                    base_offset + PAGE(5), read, all)));
    assert(guest_linux_vma_insert(&set,
            file_mapping(first_pager, PAGE(6), PAGE(7),
                    base_offset + PAGE(6), read, read)));
    assert(guest_linux_vma_insert(&set,
            file_mapping(second_pager, PAGE(7), PAGE(8),
                    base_offset + PAGE(7), read, all)));
    assert(set.count == 4);

    unsigned references =
            guest_file_pager_test_reference_count(first_pager);
    guest_linux_vma_test_fail_allocation_at(0);
    assert(!guest_linux_vma_insert(&set,
            file_mapping(first_pager, PAGE(2), PAGE(7),
                    base_offset + PAGE(1), read, all)));
    assert(guest_file_pager_test_reference_count(first_pager) == references);
    guest_linux_vma_test_fail_allocation_at(SIZE_MAX);

    guest_linux_vma_set_destroy(&set);
    guest_file_pager_release(second_pager);
    guest_file_pager_release(first_pager);
    assert(first_probe.releases == 1 && second_probe.releases == 1);
}

int main(void) {
    test_insert_override_merge_and_find();
    test_source_boundaries_and_clone_independence();
    test_remove_trim_split_and_holes();
    test_strict_subrange_split_and_remerge();
    test_protect_only_tracked_intersections();
    test_clone_boundaries_and_independence();
    test_allocation_failure_is_transactional();
    test_file_offsets_ownership_and_merge();
    guest_linux_vma_test_fail_allocation_at(SIZE_MAX);
    assert(guest_linux_vma_test_live_allocation_count() == 0);
    return 0;
}
