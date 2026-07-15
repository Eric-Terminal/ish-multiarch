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

static bool same_attributes(const struct guest_linux_vma *left,
        const struct guest_linux_vma *right) {
    return left->protection == right->protection &&
            left->source == right->source;
}

static void assert_valid(const struct guest_linux_vma_set *set) {
    assert(set != NULL);
    assert(set->count == 0 || set->entries != NULL);
    assert(set->count != 0 || set->entries == NULL);
    for (size_t index = 0; index < set->count; index++) {
        const struct guest_linux_vma *entry = &set->entries[index];
        assert(entry->first < entry->last);
        if (index == 0)
            continue;
        const struct guest_linux_vma *previous =
                &set->entries[index - 1];
        assert(previous->last <= entry->first);
        // 同属性邻接区间必须已经合并，避免后续查询依赖插入历史。
        assert(previous->last != entry->first ||
                !same_attributes(previous, entry));
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
                same_attributes(previous, &entry)) {
            previous->last = entry.last;
            return;
        }
    }
    assert(builder->count < builder->capacity);
    builder->entries[builder->count++] = entry;
}

static void commit_builder(struct guest_linux_vma_set *set,
        struct vma_builder *builder) {
    if (builder->count == 0) {
        free_entries(builder->entries);
        builder->entries = NULL;
    }
    free_entries(set->entries);
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
    for (size_t index = 0; index < source->count; index++)
        destination->entries[index] = source->entries[index];
    destination->count = source->count;
    assert_valid(destination);
    return true;
}

void guest_linux_vma_set_destroy(struct guest_linux_vma_set *set) {
    assert_valid(set);
    free_entries(set->entries);
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
            same_attributes(existing, &mapping))
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
            right.first = mapping.last;
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
            entry.first = last;
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
            entry.first = first;
        }
        bool has_right = entry.last > last;
        struct guest_linux_vma right = entry;
        if (has_right) {
            entry.last = last;
            right.first = last;
        }
        entry.protection = protection;
        append_entry(&builder, entry);
        if (has_right)
            append_entry(&builder, right);
    }

    commit_builder(set, &builder);
    return true;
}
