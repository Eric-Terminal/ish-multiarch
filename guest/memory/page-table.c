#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "guest/memory/page-table.h"

#define GUEST_PAGE_TABLE_INDEX_BITS 9
#define GUEST_PAGE_TABLE_ENTRY_COUNT (1 << GUEST_PAGE_TABLE_INDEX_BITS)
#define GUEST_PAGE_TABLE_INDEX_MASK (GUEST_PAGE_TABLE_ENTRY_COUNT - 1)

struct guest_page_mapping {
    byte_t *host_page;
    unsigned permissions;
};

struct guest_page_table_leaf {
    struct guest_page_mapping entries[GUEST_PAGE_TABLE_ENTRY_COUNT];
};

struct guest_page_table_node {
    void *entries[GUEST_PAGE_TABLE_ENTRY_COUNT];
};

struct prepared_page_mapping {
    struct guest_page_mapping *mapping;
    byte_t *new_page;
    byte_t *old_page;
};

static unsigned page_index(guest_addr_t address, byte_t shift) {
    return (unsigned) ((address >> shift) & GUEST_PAGE_TABLE_INDEX_MASK);
}

static struct guest_page_mapping *find_mapping(struct guest_page_table *table,
        guest_addr_t page_base) {
    struct guest_page_table_node *level1 =
            table->root->entries[page_index(page_base, 39)];
    if (level1 == NULL)
        return NULL;
    struct guest_page_table_node *level2 =
            level1->entries[page_index(page_base, 30)];
    if (level2 == NULL)
        return NULL;
    struct guest_page_table_leaf *leaf =
            level2->entries[page_index(page_base, 21)];
    if (leaf == NULL)
        return NULL;
    struct guest_page_mapping *mapping =
            &leaf->entries[page_index(page_base, 12)];
    return mapping->host_page == NULL ? NULL : mapping;
}

static enum guest_memory_fault_kind resolve_page(void *opaque,
        guest_addr_t page_base, enum guest_memory_access access,
        struct guest_page_view *view) {
    struct guest_page_table *table = opaque;
    use(access);
    struct guest_page_mapping *mapping = find_mapping(table, page_base);
    if (mapping == NULL)
        return GUEST_MEMORY_FAULT_UNMAPPED;
    *view = (struct guest_page_view) {
        .host_page = mapping->host_page,
        .permissions = mapping->permissions,
    };
    return GUEST_MEMORY_FAULT_NONE;
}

static bool address_space_read_lock(void *opaque) {
    struct guest_page_table *table = opaque;
    if (!atomic_load_explicit(&table->concurrent, memory_order_acquire))
        return false;
    assert(pthread_rwlock_rdlock(&table->lock) == 0);
    return true;
}

static void address_space_read_unlock(void *opaque, bool locked) {
    struct guest_page_table *table = opaque;
    if (locked)
        assert(pthread_rwlock_unlock(&table->lock) == 0);
}

static bool address_space_write_lock(void *opaque) {
    return guest_page_table_write_lock(opaque);
}

static void address_space_write_unlock(void *opaque, bool locked) {
    guest_page_table_write_unlock(opaque, locked);
}

static const struct guest_address_space_ops page_table_ops = {
    .read_lock = address_space_read_lock,
    .read_unlock = address_space_read_unlock,
    .write_lock = address_space_write_lock,
    .write_unlock = address_space_write_unlock,
    .resolve_page = resolve_page,
};

#if defined(GUEST_PAGE_TABLE_TESTING)
static size_t clone_allocation_fail_at = SIZE_MAX;
static size_t clone_allocation_count;

void guest_page_table_test_fail_clone_allocation_at(size_t index) {
    clone_allocation_fail_at = index;
    clone_allocation_count = 0;
}

size_t guest_page_table_test_clone_allocation_count(void) {
    return clone_allocation_count;
}

static bool clone_allocation_fails(void) {
    return clone_allocation_count++ == clone_allocation_fail_at;
}

static void *clone_malloc(size_t size) {
    return clone_allocation_fails() ? NULL : malloc(size);
}

static void *clone_calloc(size_t count, size_t size) {
    return clone_allocation_fails() ? NULL : calloc(count, size);
}
#else
#define clone_malloc malloc
#define clone_calloc calloc
#endif

bool guest_page_table_init(struct guest_page_table *table,
        byte_t address_bits) {
    *table = (struct guest_page_table) {0};
    if (address_bits <= GUEST_MEMORY_PAGE_BITS || address_bits > 48)
        return false;
    if (pthread_rwlock_init(&table->lock, NULL) != 0)
        return false;
    table->root = clone_calloc(1, sizeof(*table->root));
    if (table->root == NULL) {
        assert(pthread_rwlock_destroy(&table->lock) == 0);
        *table = (struct guest_page_table) {0};
        return false;
    }
    guest_address_space_init(&table->address_space,
            &page_table_ops, table, address_bits);
    return true;
}

static void destroy_leaf(struct guest_page_table_leaf *leaf) {
    for (unsigned i = 0; i < GUEST_PAGE_TABLE_ENTRY_COUNT; i++)
        free(leaf->entries[i].host_page);
    free(leaf);
}

static void destroy_level2(struct guest_page_table_node *level2) {
    for (unsigned i = 0; i < GUEST_PAGE_TABLE_ENTRY_COUNT; i++) {
        if (level2->entries[i] != NULL)
            destroy_leaf(level2->entries[i]);
    }
    free(level2);
}

static void destroy_level1(struct guest_page_table_node *level1) {
    for (unsigned i = 0; i < GUEST_PAGE_TABLE_ENTRY_COUNT; i++) {
        if (level1->entries[i] != NULL)
            destroy_level2(level1->entries[i]);
    }
    free(level1);
}

void guest_page_table_destroy(struct guest_page_table *table) {
    if (table->root == NULL)
        return;
    assert(pthread_rwlock_wrlock(&table->lock) == 0);
    for (unsigned i = 0; i < GUEST_PAGE_TABLE_ENTRY_COUNT; i++) {
        if (table->root->entries[i] != NULL)
            destroy_level1(table->root->entries[i]);
    }
    free(table->root);
    table->root = NULL;
    assert(pthread_rwlock_unlock(&table->lock) == 0);
    assert(pthread_rwlock_destroy(&table->lock) == 0);
}

void guest_page_table_enable_concurrency(struct guest_page_table *table) {
    atomic_store_explicit(&table->concurrent, true, memory_order_release);
}

bool guest_page_table_write_lock(struct guest_page_table *table) {
    if (!atomic_load_explicit(&table->concurrent, memory_order_acquire))
        return false;
    assert(pthread_rwlock_wrlock(&table->lock) == 0);
    return true;
}

void guest_page_table_write_unlock(
        struct guest_page_table *table, bool locked) {
    if (locked)
        assert(pthread_rwlock_unlock(&table->lock) == 0);
}

static struct guest_page_table_leaf *clone_leaf(
        const struct guest_page_table_leaf *source) {
    struct guest_page_table_leaf *copy = clone_calloc(1, sizeof(*copy));
    if (copy == NULL)
        return NULL;
    for (unsigned i = 0; i < GUEST_PAGE_TABLE_ENTRY_COUNT; i++) {
        const struct guest_page_mapping *source_mapping =
                &source->entries[i];
        if (source_mapping->host_page == NULL)
            continue;
        struct guest_page_mapping *copy_mapping = &copy->entries[i];
        copy_mapping->host_page = clone_malloc(GUEST_MEMORY_PAGE_SIZE);
        if (copy_mapping->host_page == NULL) {
            destroy_leaf(copy);
            return NULL;
        }
        memcpy(copy_mapping->host_page, source_mapping->host_page,
                GUEST_MEMORY_PAGE_SIZE);
        copy_mapping->permissions = source_mapping->permissions;
    }
    return copy;
}

static struct guest_page_table_node *clone_level2(
        const struct guest_page_table_node *source) {
    struct guest_page_table_node *copy = clone_calloc(1, sizeof(*copy));
    if (copy == NULL)
        return NULL;
    for (unsigned i = 0; i < GUEST_PAGE_TABLE_ENTRY_COUNT; i++) {
        if (source->entries[i] == NULL)
            continue;
        copy->entries[i] = clone_leaf(source->entries[i]);
        if (copy->entries[i] == NULL) {
            destroy_level2(copy);
            return NULL;
        }
    }
    return copy;
}

static struct guest_page_table_node *clone_level1(
        const struct guest_page_table_node *source) {
    struct guest_page_table_node *copy = clone_calloc(1, sizeof(*copy));
    if (copy == NULL)
        return NULL;
    for (unsigned i = 0; i < GUEST_PAGE_TABLE_ENTRY_COUNT; i++) {
        if (source->entries[i] == NULL)
            continue;
        copy->entries[i] = clone_level2(source->entries[i]);
        if (copy->entries[i] == NULL) {
            destroy_level1(copy);
            return NULL;
        }
    }
    return copy;
}

bool guest_page_table_clone(struct guest_page_table *destination,
        const struct guest_page_table *source) {
    if (destination == NULL || destination == source)
        return false;
    *destination = (struct guest_page_table) {0};
    if (source == NULL || source->root == NULL)
        return false;
    bool source_locked = address_space_read_lock((void *) source);
    if (!guest_page_table_init(destination,
            source->address_space.address_bits)) {
        address_space_read_unlock((void *) source, source_locked);
        return false;
    }

    for (unsigned i = 0; i < GUEST_PAGE_TABLE_ENTRY_COUNT; i++) {
        if (source->root->entries[i] == NULL)
            continue;
        destination->root->entries[i] =
                clone_level1(source->root->entries[i]);
        if (destination->root->entries[i] == NULL) {
            guest_page_table_destroy(destination);
            address_space_read_unlock((void *) source, source_locked);
            return false;
        }
    }
    destination->address_space.generation =
            source->address_space.generation;
    address_space_read_unlock((void *) source, source_locked);
    return true;
}

static bool valid_page(const struct guest_page_table *table,
        guest_addr_t page_base) {
    return (page_base & GUEST_MEMORY_PAGE_MASK) == 0 &&
            guest_address_space_contains(&table->address_space,
                    page_base, GUEST_MEMORY_PAGE_SIZE);
}

static bool valid_range(const struct guest_page_table *table,
        struct guest_page_range range) {
    if ((range.first & GUEST_MEMORY_PAGE_MASK) != 0)
        return false;
    if (range.page_count == 0)
        return guest_address_space_contains(
                &table->address_space, range.first, 0);
    if (!valid_page(table, range.first))
        return false;

    qword_t max_address = (UINT64_C(1) <<
            table->address_space.address_bits) - 1;
    qword_t available_pages = ((max_address - range.first) >>
            GUEST_MEMORY_PAGE_BITS) + 1;
    return range.page_count <= available_pages;
}

static void *allocate_slot(void **slot, size_t size) {
    if (*slot == NULL)
        *slot = calloc(1, size);
    return *slot;
}

static enum guest_page_table_result prepare_mapping_slot(
        struct guest_page_table *table, guest_addr_t page_base,
        struct guest_page_mapping **mapping) {
    struct guest_page_table_node *level1 = allocate_slot(
            &table->root->entries[page_index(page_base, 39)],
            sizeof(*level1));
    if (level1 == NULL)
        return GUEST_PAGE_TABLE_OUT_OF_MEMORY;
    struct guest_page_table_node *level2 = allocate_slot(
            &level1->entries[page_index(page_base, 30)],
            sizeof(*level2));
    if (level2 == NULL)
        return GUEST_PAGE_TABLE_OUT_OF_MEMORY;
    struct guest_page_table_leaf *leaf = allocate_slot(
            &level2->entries[page_index(page_base, 21)],
            sizeof(*leaf));
    if (leaf == NULL)
        return GUEST_PAGE_TABLE_OUT_OF_MEMORY;
    *mapping = &leaf->entries[page_index(page_base, 12)];
    return GUEST_PAGE_TABLE_OK;
}

enum guest_page_table_result guest_page_table_map(
        struct guest_page_table *table, guest_addr_t page_base,
        unsigned permissions, byte_t **host_page) {
    assert((permissions & ~GUEST_MEMORY_PERMISSION_MASK) == 0);
    assert(host_page != NULL);
    if (!valid_page(table, page_base))
        return GUEST_PAGE_TABLE_INVALID_ADDRESS;

    struct guest_page_mapping *mapping;
    enum guest_page_table_result prepare = prepare_mapping_slot(
            table, page_base, &mapping);
    if (prepare != GUEST_PAGE_TABLE_OK)
        return prepare;
    if (mapping->host_page != NULL) {
        *host_page = mapping->host_page;
        return GUEST_PAGE_TABLE_ALREADY_MAPPED;
    }
    mapping->host_page = calloc(1, GUEST_MEMORY_PAGE_SIZE);
    if (mapping->host_page == NULL)
        return GUEST_PAGE_TABLE_OUT_OF_MEMORY;
    mapping->permissions = permissions;
    *host_page = mapping->host_page;
    guest_address_space_changed(&table->address_space);
    return GUEST_PAGE_TABLE_OK;
}

static void discard_prepared_pages(struct prepared_page_mapping *prepared,
        size_t count) {
    for (size_t i = 0; i < count; i++)
        free(prepared[i].new_page);
    free(prepared);
}

enum guest_page_table_result guest_page_table_map_zero_range(
        struct guest_page_table *table, struct guest_page_range range,
        unsigned permissions, bool replace) {
    assert((permissions & ~GUEST_MEMORY_PERMISSION_MASK) == 0);
    if (!valid_range(table, range))
        return GUEST_PAGE_TABLE_INVALID_ADDRESS;
    if (range.page_count == 0)
        return GUEST_PAGE_TABLE_OK;
    if (range.page_count >
            (qword_t) (SIZE_MAX / sizeof(struct prepared_page_mapping)))
        return GUEST_PAGE_TABLE_OUT_OF_MEMORY;

    guest_addr_t page = range.first;
    if (!replace) {
        for (qword_t i = 0; i < range.page_count; i++) {
            if (find_mapping(table, page) != NULL)
                return GUEST_PAGE_TABLE_ALREADY_MAPPED;
            page += GUEST_MEMORY_PAGE_SIZE;
        }
    }

    size_t count = (size_t) range.page_count;
    struct prepared_page_mapping *prepared =
            calloc(count, sizeof(*prepared));
    if (prepared == NULL)
        return GUEST_PAGE_TABLE_OUT_OF_MEMORY;
    for (size_t i = 0; i < count; i++) {
        prepared[i].new_page = calloc(1, GUEST_MEMORY_PAGE_SIZE);
        if (prepared[i].new_page == NULL) {
            discard_prepared_pages(prepared, count);
            return GUEST_PAGE_TABLE_OUT_OF_MEMORY;
        }
    }

    page = range.first;
    for (size_t i = 0; i < count; i++) {
        enum guest_page_table_result result = prepare_mapping_slot(
                table, page, &prepared[i].mapping);
        if (result != GUEST_PAGE_TABLE_OK) {
            /*
             * 已挂接的空节点不含 backing，对查询完全不可见，并会在后续
             * 使用或销毁页表时复用、释放。可见映射直到提交阶段才会改变。
             */
            discard_prepared_pages(prepared, count);
            return result;
        }
        page += GUEST_MEMORY_PAGE_SIZE;
    }

    for (size_t i = 0; i < count; i++) {
        prepared[i].old_page = prepared[i].mapping->host_page;
        prepared[i].mapping->host_page = prepared[i].new_page;
        prepared[i].mapping->permissions = permissions;
        prepared[i].new_page = NULL;
    }
    guest_address_space_changed(&table->address_space);
    for (size_t i = 0; i < count; i++)
        free(prepared[i].old_page);
    free(prepared);
    return GUEST_PAGE_TABLE_OK;
}

enum guest_page_table_result guest_page_table_lookup(
        struct guest_page_table *table, guest_addr_t page_base,
        byte_t **host_page, unsigned *permissions) {
    assert(host_page != NULL && permissions != NULL);
    if (!valid_page(table, page_base))
        return GUEST_PAGE_TABLE_INVALID_ADDRESS;
    struct guest_page_mapping *mapping = find_mapping(table, page_base);
    if (mapping == NULL)
        return GUEST_PAGE_TABLE_NOT_MAPPED;
    *host_page = mapping->host_page;
    *permissions = mapping->permissions;
    return GUEST_PAGE_TABLE_OK;
}

static bool node_is_empty(const struct guest_page_table_node *node) {
    for (unsigned i = 0; i < GUEST_PAGE_TABLE_ENTRY_COUNT; i++) {
        if (node->entries[i] != NULL)
            return false;
    }
    return true;
}

static bool leaf_is_empty(const struct guest_page_table_leaf *leaf) {
    for (unsigned i = 0; i < GUEST_PAGE_TABLE_ENTRY_COUNT; i++) {
        if (leaf->entries[i].host_page != NULL)
            return false;
    }
    return true;
}

static enum guest_page_table_result unmap_page(
        struct guest_page_table *table, guest_addr_t page_base) {
    if (!valid_page(table, page_base))
        return GUEST_PAGE_TABLE_INVALID_ADDRESS;
    unsigned index0 = page_index(page_base, 39);
    unsigned index1 = page_index(page_base, 30);
    unsigned index2 = page_index(page_base, 21);
    unsigned index3 = page_index(page_base, 12);
    struct guest_page_table_node *level1 = table->root->entries[index0];
    if (level1 == NULL)
        return GUEST_PAGE_TABLE_NOT_MAPPED;
    struct guest_page_table_node *level2 = level1->entries[index1];
    if (level2 == NULL)
        return GUEST_PAGE_TABLE_NOT_MAPPED;
    struct guest_page_table_leaf *leaf = level2->entries[index2];
    if (leaf == NULL || leaf->entries[index3].host_page == NULL)
        return GUEST_PAGE_TABLE_NOT_MAPPED;

    free(leaf->entries[index3].host_page);
    leaf->entries[index3] = (struct guest_page_mapping) {0};
    if (leaf_is_empty(leaf)) {
        free(leaf);
        level2->entries[index2] = NULL;
        if (node_is_empty(level2)) {
            free(level2);
            level1->entries[index1] = NULL;
            if (node_is_empty(level1)) {
                free(level1);
                table->root->entries[index0] = NULL;
            }
        }
    }
    return GUEST_PAGE_TABLE_OK;
}

enum guest_page_table_result guest_page_table_unmap(
        struct guest_page_table *table, guest_addr_t page_base) {
    enum guest_page_table_result result = unmap_page(table, page_base);
    if (result == GUEST_PAGE_TABLE_OK)
        guest_address_space_changed(&table->address_space);
    return result;
}

enum guest_page_table_result guest_page_table_unmap_range(
        struct guest_page_table *table, struct guest_page_range range,
        bool allow_holes) {
    if (!valid_range(table, range))
        return GUEST_PAGE_TABLE_INVALID_ADDRESS;

    guest_addr_t page = range.first;
    if (!allow_holes) {
        for (qword_t i = 0; i < range.page_count; i++) {
            if (find_mapping(table, page) == NULL)
                return GUEST_PAGE_TABLE_NOT_MAPPED;
            page += GUEST_MEMORY_PAGE_SIZE;
        }
    }

    bool changed = false;
    page = range.first;
    for (qword_t i = 0; i < range.page_count; i++) {
        enum guest_page_table_result result = unmap_page(table, page);
        if (result == GUEST_PAGE_TABLE_OK) {
            changed = true;
        } else if (result != GUEST_PAGE_TABLE_NOT_MAPPED) {
            assert(false);
            return result;
        }
        page += GUEST_MEMORY_PAGE_SIZE;
    }
    if (changed)
        guest_address_space_changed(&table->address_space);
    return GUEST_PAGE_TABLE_OK;
}

enum guest_page_table_result guest_page_table_protect(
        struct guest_page_table *table, guest_addr_t page_base,
        unsigned permissions) {
    assert((permissions & ~GUEST_MEMORY_PERMISSION_MASK) == 0);
    if (!valid_page(table, page_base))
        return GUEST_PAGE_TABLE_INVALID_ADDRESS;
    struct guest_page_mapping *mapping = find_mapping(table, page_base);
    if (mapping == NULL)
        return GUEST_PAGE_TABLE_NOT_MAPPED;
    if (mapping->permissions != permissions) {
        mapping->permissions = permissions;
        guest_address_space_changed(&table->address_space);
    }
    return GUEST_PAGE_TABLE_OK;
}

enum guest_page_table_result guest_page_table_protect_range(
        struct guest_page_table *table, struct guest_page_range range,
        unsigned permissions) {
    assert((permissions & ~GUEST_MEMORY_PERMISSION_MASK) == 0);
    if (!valid_range(table, range))
        return GUEST_PAGE_TABLE_INVALID_ADDRESS;

    guest_addr_t page = range.first;
    for (qword_t i = 0; i < range.page_count; i++) {
        if (find_mapping(table, page) == NULL)
            return GUEST_PAGE_TABLE_NOT_MAPPED;
        page += GUEST_MEMORY_PAGE_SIZE;
    }

    bool changed = false;
    page = range.first;
    for (qword_t i = 0; i < range.page_count; i++) {
        struct guest_page_mapping *mapping = find_mapping(table, page);
        if (mapping == NULL) {
            assert(false);
            return GUEST_PAGE_TABLE_NOT_MAPPED;
        }
        if (mapping->permissions != permissions) {
            mapping->permissions = permissions;
            changed = true;
        }
        page += GUEST_MEMORY_PAGE_SIZE;
    }
    if (changed)
        guest_address_space_changed(&table->address_space);
    return GUEST_PAGE_TABLE_OK;
}
