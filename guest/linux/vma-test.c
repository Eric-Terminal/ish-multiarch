#include <assert.h>

#include "guest/linux/mman.h"
#include "guest/linux/vma.h"

#define PAGE(index) \
    ((guest_addr_t) ((qword_t) (index) * GUEST_MEMORY_PAGE_SIZE))

static struct guest_linux_vma mapping(guest_addr_t first,
        guest_addr_t last, qword_t protection,
        enum guest_linux_vma_source source) {
    return (struct guest_linux_vma) {
        .first = first,
        .last = last,
        .protection = protection,
        .source = source,
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
        {PAGE(1), PAGE(3), read,
                GUEST_LINUX_VMA_SOURCE_ANONYMOUS_PRIVATE},
        {PAGE(3), PAGE(5), read,
                GUEST_LINUX_VMA_SOURCE_ANONYMOUS_SHARED},
    };
    assert_same_set(&set, adjacent, array_size(adjacent));

    assert(guest_linux_vma_protect_tracked(
            &set, PAGE(2), PAGE(4), write));
    const struct guest_linux_vma protected[] = {
        {PAGE(1), PAGE(2), read,
                GUEST_LINUX_VMA_SOURCE_ANONYMOUS_PRIVATE},
        {PAGE(2), PAGE(3), write,
                GUEST_LINUX_VMA_SOURCE_ANONYMOUS_PRIVATE},
        {PAGE(3), PAGE(4), write,
                GUEST_LINUX_VMA_SOURCE_ANONYMOUS_SHARED},
        {PAGE(4), PAGE(5), read,
                GUEST_LINUX_VMA_SOURCE_ANONYMOUS_SHARED},
    };
    assert_same_set(&set, protected, array_size(protected));

    guest_linux_vma_set_destroy(&set);
    guest_linux_vma_set_init(&set);
    assert(guest_linux_vma_insert(&set, mapping(PAGE(1), PAGE(6), read,
            GUEST_LINUX_VMA_SOURCE_ANONYMOUS_SHARED)));
    assert(guest_linux_vma_insert(&set, mapping(PAGE(2), PAGE(5), execute,
            GUEST_LINUX_VMA_SOURCE_ANONYMOUS_PRIVATE)));
    const struct guest_linux_vma split[] = {
        {PAGE(1), PAGE(2), read,
                GUEST_LINUX_VMA_SOURCE_ANONYMOUS_SHARED},
        {PAGE(2), PAGE(5), execute,
                GUEST_LINUX_VMA_SOURCE_ANONYMOUS_PRIVATE},
        {PAGE(5), PAGE(6), read,
                GUEST_LINUX_VMA_SOURCE_ANONYMOUS_SHARED},
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
        {PAGE(1), PAGE(3), read,
                GUEST_LINUX_VMA_SOURCE_ANONYMOUS_PRIVATE},
        {PAGE(3), PAGE(5), execute,
                GUEST_LINUX_VMA_SOURCE_ANONYMOUS_PRIVATE},
        {PAGE(7), PAGE(9), execute,
                GUEST_LINUX_VMA_SOURCE_ANONYMOUS_PRIVATE},
        {PAGE(9), PAGE(10), execute, GUEST_LINUX_VMA_SOURCE_BRK},
        {PAGE(10), PAGE(11), read_write, GUEST_LINUX_VMA_SOURCE_BRK},
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
        {PAGE(1), PAGE(3), read,
                GUEST_LINUX_VMA_SOURCE_ANONYMOUS_PRIVATE},
        {PAGE(5), PAGE(7), read,
                GUEST_LINUX_VMA_SOURCE_ANONYMOUS_PRIVATE},
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

int main(void) {
    test_insert_override_merge_and_find();
    test_source_boundaries_and_clone_independence();
    test_remove_trim_split_and_holes();
    test_strict_subrange_split_and_remerge();
    test_protect_only_tracked_intersections();
    test_clone_boundaries_and_independence();
    test_allocation_failure_is_transactional();
    guest_linux_vma_test_fail_allocation_at(SIZE_MAX);
    assert(guest_linux_vma_test_live_allocation_count() == 0);
    return 0;
}
