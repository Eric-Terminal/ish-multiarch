#include <assert.h>
#include <stdlib.h>

#include "guest/linux/vma.h"

#if defined(GUEST_LINUX_VMA_TESTING)
static size_t allocation_fail_at = SIZE_MAX;
static size_t allocation_count;
static size_t live_allocation_count;

void guest_linux_vma_test_fail_allocation_at(size_t index) {
    allocation_fail_at = index;
    allocation_count = 0;
}

size_t guest_linux_vma_test_allocation_count(void) {
    return allocation_count;
}

size_t guest_linux_vma_test_live_allocation_count(void) {
    return live_allocation_count;
}

static bool allocation_fails(void) {
    return allocation_count++ == allocation_fail_at;
}
#endif

static struct guest_linux_vma *allocate_entries(size_t count) {
    if (count == 0)
        return NULL;
    if (count > SIZE_MAX / sizeof(struct guest_linux_vma))
        return NULL;
#if defined(GUEST_LINUX_VMA_TESTING)
    if (allocation_fails())
        return NULL;
#endif
    struct guest_linux_vma *entries = malloc(count * sizeof(*entries));
#if defined(GUEST_LINUX_VMA_TESTING)
    if (entries != NULL)
        live_allocation_count++;
#endif
    return entries;
}

static void free_entries(struct guest_linux_vma *entries) {
    if (entries == NULL)
        return;
#if defined(GUEST_LINUX_VMA_TESTING)
    assert(live_allocation_count != 0);
    live_allocation_count--;
#endif
    free(entries);
}

static bool file_mapping(const struct guest_linux_vma *entry) {
    return entry->source == GUEST_LINUX_VMA_SOURCE_FILE_PRIVATE;
}

static void retain_entry(const struct guest_linux_vma *entry) {
    if (file_mapping(entry))
        guest_file_pager_retain(entry->file_pager);
}

static void release_entry(const struct guest_linux_vma *entry) {
    if (file_mapping(entry))
        guest_file_pager_release(entry->file_pager);
}

static void release_entries(
        struct guest_linux_vma *entries, size_t count) {
    for (size_t index = 0; index < count; index++)
        release_entry(&entries[index]);
    free_entries(entries);
}

static bool file_offsets_are_contiguous(
        const struct guest_linux_vma *left,
        const struct guest_linux_vma *right) {
    if (!file_mapping(left))
        return true;
    qword_t length = left->last - left->first;
    return left->file_offset <= UINT64_MAX - length &&
            left->file_offset + length == right->file_offset;
}

static bool same_mapping_attributes(const struct guest_linux_vma *left,
        const struct guest_linux_vma *right) {
    return left->protection == right->protection &&
            left->maximum_protection == right->maximum_protection &&
            left->source == right->source &&
            left->file_pager == right->file_pager;
}

static bool same_adjacent_mapping(const struct guest_linux_vma *left,
        const struct guest_linux_vma *right) {
    return same_mapping_attributes(left, right) &&
            file_offsets_are_contiguous(left, right);
}

static bool contains_same_mapping(const struct guest_linux_vma *existing,
        const struct guest_linux_vma *mapping) {
    if (!same_mapping_attributes(existing, mapping))
        return false;
    if (!file_mapping(existing))
        return true;
    qword_t delta = mapping->first - existing->first;
    return existing->file_offset <= UINT64_MAX - delta &&
            existing->file_offset + delta == mapping->file_offset;
}

static void set_first(struct guest_linux_vma *entry,
        guest_addr_t first) {
    assert(first >= entry->first && first < entry->last);
    if (file_mapping(entry)) {
        qword_t delta = first - entry->first;
        assert(entry->file_offset <= UINT64_MAX - delta);
        entry->file_offset += delta;
    }
    entry->first = first;
}

static void assert_valid(const struct guest_linux_vma_set *set) {
    assert(set != NULL);
    assert(set->count == 0 || set->entries != NULL);
    assert(set->count != 0 || set->entries == NULL);
    for (size_t index = 0; index < set->count; index++) {
        const struct guest_linux_vma *entry = &set->entries[index];
        assert(entry->first < entry->last);
        assert((entry->protection & ~entry->maximum_protection) == 0);
        if (file_mapping(entry)) {
            assert(entry->file_pager != NULL);
            assert((entry->file_offset & GUEST_MEMORY_PAGE_MASK) == 0);
        } else {
            assert(entry->file_pager == NULL && entry->file_offset == 0);
        }
        if (index == 0)
            continue;
        const struct guest_linux_vma *previous =
                &set->entries[index - 1];
        assert(previous->last <= entry->first);
        // 同属性邻接区间必须已经合并，避免后续查询依赖插入历史。
        assert(previous->last != entry->first ||
                !same_adjacent_mapping(previous, entry));
    }
}

struct vma_builder {
    struct guest_linux_vma *entries;
    size_t count;
    size_t capacity;
};

static void append_entry(struct vma_builder *builder,
        struct guest_linux_vma entry) {
    assert(entry.first < entry.last);
    if (builder->count != 0) {
        struct guest_linux_vma *previous =
                &builder->entries[builder->count - 1];
        assert(previous->last <= entry.first);
        if (previous->last == entry.first &&
                same_adjacent_mapping(previous, &entry)) {
            previous->last = entry.last;
            return;
        }
    }
    assert(builder->count < builder->capacity);
    retain_entry(&entry);
    builder->entries[builder->count++] = entry;
}

static void commit_builder(struct guest_linux_vma_set *set,
        struct vma_builder *builder) {
    if (builder->count == 0) {
        free_entries(builder->entries);
        builder->entries = NULL;
    }
    release_entries(set->entries, set->count);
    set->entries = builder->entries;
    set->count = builder->count;
    assert_valid(set);
}

void guest_linux_vma_set_init(struct guest_linux_vma_set *set) {
    assert(set != NULL);
    *set = (struct guest_linux_vma_set) {0};
}

bool guest_linux_vma_set_clone(struct guest_linux_vma_set *destination,
        const struct guest_linux_vma_set *source) {
    assert(destination != NULL);
    assert(destination != source);
    assert_valid(source);
    *destination = (struct guest_linux_vma_set) {0};
    if (source->count == 0)
        return true;
    destination->entries = allocate_entries(source->count);
    if (destination->entries == NULL)
        return false;
    for (size_t index = 0; index < source->count; index++) {
        destination->entries[index] = source->entries[index];
        retain_entry(&destination->entries[index]);
    }
    destination->count = source->count;
    assert_valid(destination);
    return true;
}

void guest_linux_vma_set_destroy(struct guest_linux_vma_set *set) {
    assert_valid(set);
    release_entries(set->entries, set->count);
    *set = (struct guest_linux_vma_set) {0};
}

const struct guest_linux_vma *guest_linux_vma_find(
        const struct guest_linux_vma_set *set, guest_addr_t address) {
    assert(set != NULL);
    assert(set->count == 0 || set->entries != NULL);
    size_t first = 0;
    size_t count = set->count;
    while (count != 0) {
        size_t step = count / 2;
        size_t index = first + step;
        const struct guest_linux_vma *entry = &set->entries[index];
        if (address < entry->first) {
            count = step;
        } else if (address >= entry->last) {
            first = index + 1;
            count -= step + 1;
        } else {
            return entry;
        }
    }
    return NULL;
}

bool guest_linux_vma_insert(struct guest_linux_vma_set *set,
        struct guest_linux_vma mapping) {
    assert_valid(set);
    assert(mapping.first < mapping.last);

    const struct guest_linux_vma *existing =
            guest_linux_vma_find(set, mapping.first);
    if (existing != NULL && existing->last >= mapping.last &&
            contains_same_mapping(existing, &mapping))
        return true;
    if (set->count > SIZE_MAX - 2)
        return false;
    struct vma_builder builder = {
        .entries = allocate_entries(set->count + 2),
        .capacity = set->count + 2,
    };
    if (builder.entries == NULL)
        return false;

    size_t index = 0;
    while (index < set->count &&
            set->entries[index].last <= mapping.first)
        append_entry(&builder, set->entries[index++]);
    if (index < set->count &&
            set->entries[index].first < mapping.first) {
        struct guest_linux_vma left = set->entries[index];
        left.last = mapping.first;
        append_entry(&builder, left);
    }
    append_entry(&builder, mapping);
    while (index < set->count &&
            set->entries[index].first < mapping.last) {
        if (set->entries[index].last > mapping.last) {
            struct guest_linux_vma right = set->entries[index];
            set_first(&right, mapping.last);
            append_entry(&builder, right);
        }
        index++;
    }
    while (index < set->count)
        append_entry(&builder, set->entries[index++]);

    commit_builder(set, &builder);
    return true;
}

bool guest_linux_vma_remove(struct guest_linux_vma_set *set,
        guest_addr_t first, guest_addr_t last) {
    assert_valid(set);
    assert(first < last);
    bool intersects = false;
    for (size_t index = 0; index < set->count; index++) {
        if (set->entries[index].first < last &&
                set->entries[index].last > first) {
            intersects = true;
            break;
        }
    }
    if (!intersects)
        return true;
    if (set->count == SIZE_MAX)
        return false;
    struct vma_builder builder = {
        .entries = allocate_entries(set->count + 1),
        .capacity = set->count + 1,
    };
    if (builder.entries == NULL)
        return false;

    for (size_t index = 0; index < set->count; index++) {
        struct guest_linux_vma entry = set->entries[index];
        if (entry.last <= first || entry.first >= last) {
            append_entry(&builder, entry);
            continue;
        }
        if (entry.first < first) {
            struct guest_linux_vma left = entry;
            left.last = first;
            append_entry(&builder, left);
        }
        if (entry.last > last) {
            set_first(&entry, last);
            append_entry(&builder, entry);
        }
    }

    commit_builder(set, &builder);
    return true;
}

bool guest_linux_vma_protect_tracked(struct guest_linux_vma_set *set,
        guest_addr_t first, guest_addr_t last, qword_t protection) {
    assert_valid(set);
    assert(first < last);
    bool changes = false;
    for (size_t index = 0; index < set->count; index++) {
        const struct guest_linux_vma *entry = &set->entries[index];
        if (entry->first < last && entry->last > first &&
                entry->protection != protection) {
            changes = true;
            break;
        }
    }
    if (!changes)
        return true;
    if (set->count > SIZE_MAX - 2)
        return false;
    struct vma_builder builder = {
        .entries = allocate_entries(set->count + 2),
        .capacity = set->count + 2,
    };
    if (builder.entries == NULL)
        return false;

    for (size_t index = 0; index < set->count; index++) {
        struct guest_linux_vma entry = set->entries[index];
        if (entry.last <= first || entry.first >= last) {
            append_entry(&builder, entry);
            continue;
        }
        if (entry.first < first) {
            struct guest_linux_vma left = entry;
            left.last = first;
            append_entry(&builder, left);
            set_first(&entry, first);
        }
        bool has_right = entry.last > last;
        struct guest_linux_vma right = entry;
        if (has_right) {
            entry.last = last;
            set_first(&right, last);
        }
        entry.protection = protection;
        append_entry(&builder, entry);
        if (has_right)
            append_entry(&builder, right);
    }

    commit_builder(set, &builder);
    return true;
}
