#include <assert.h>
#include <stdlib.h>
#include "guest/memory/page-backing.h"
#include "guest/memory/page-table.h"

#define GUEST_PAGE_TABLE_INDEX_BITS 9
#define GUEST_PAGE_TABLE_ENTRY_COUNT (1 << GUEST_PAGE_TABLE_INDEX_BITS)
#define GUEST_PAGE_TABLE_INDEX_MASK (GUEST_PAGE_TABLE_ENTRY_COUNT - 1)

struct guest_page_mapping {
    struct guest_page_backing *backing;
    struct guest_file_source *file_source;
    qword_t file_offset;
    unsigned permissions;
    enum guest_page_origin origin;
    // 物理 backing 可能由多个地址空间共同持有，因此访问必须经过
    // 同步域。
    bool shared_backing;
    // 逻辑上仍是私有文件页；首次写前必须换入匿名 backing。
    bool copy_on_write;
};

struct guest_page_table_leaf {
    struct guest_page_mapping entries[GUEST_PAGE_TABLE_ENTRY_COUNT];
};

struct guest_page_table_node {
    void *entries[GUEST_PAGE_TABLE_ENTRY_COUNT];
};

struct prepared_page_mapping {
    struct guest_page_mapping *mapping;
    struct guest_page_backing *new_backing;
    struct guest_page_backing *old_backing;
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
    return mapping->backing == NULL ? NULL : mapping;
}

static bool set_nonfile_origin(struct guest_page_mapping *mapping,
        enum guest_page_origin origin, bool shared_backing) {
    assert(origin == GUEST_PAGE_ORIGIN_ANONYMOUS ||
            origin == GUEST_PAGE_ORIGIN_SPECIAL);
    bool changed = mapping->origin != origin ||
            mapping->file_source != NULL || mapping->file_offset != 0 ||
            mapping->shared_backing != shared_backing ||
            mapping->copy_on_write;
    struct guest_file_source *old_source = mapping->file_source;
    mapping->origin = origin;
    mapping->file_source = NULL;
    mapping->file_offset = 0;
    mapping->shared_backing = shared_backing;
    mapping->copy_on_write = false;
    guest_file_source_release(old_source);
    return changed;
}

static bool set_file_source(struct guest_page_mapping *mapping,
        struct guest_file_source *source, qword_t file_offset,
        bool copy_on_write) {
    assert(source != NULL);
    assert((file_offset & GUEST_MEMORY_PAGE_MASK) == 0);
    bool changed = mapping->origin != GUEST_PAGE_ORIGIN_FILE ||
            mapping->file_source != source ||
            mapping->file_offset != file_offset ||
            !mapping->shared_backing ||
            mapping->copy_on_write != copy_on_write;
    guest_file_source_retain(source);
    struct guest_file_source *old_source = mapping->file_source;
    mapping->origin = GUEST_PAGE_ORIGIN_FILE;
    mapping->file_source = source;
    mapping->file_offset = file_offset;
    mapping->shared_backing = true;
    mapping->copy_on_write = copy_on_write;
    guest_file_source_release(old_source);
    return changed;
}

static enum guest_memory_fault_kind resolve_page(void *opaque,
        guest_addr_t page_base, enum guest_memory_access access,
        struct guest_page_view *view) {
    struct guest_page_table *table = opaque;
    use(access);
    struct guest_page_mapping *mapping = find_mapping(table, page_base);
    if (mapping == NULL)
        return GUEST_MEMORY_FAULT_UNMAPPED;
    const struct guest_page_sync *sync =
            guest_page_backing_sync(mapping->backing);
    *view = (struct guest_page_view) {
        .host_page = guest_page_backing_bytes(mapping->backing),
        .permissions = mapping->permissions,
        .origin = mapping->origin,
        .backing_identity = guest_page_sync_identity(sync),
        .file_identity = mapping->file_source == NULL ? 0 :
                guest_file_source_identity(mapping->file_source),
        .file_offset = mapping->file_offset,
        .copy_on_write = mapping->copy_on_write,
        .sync = mapping->shared_backing ? sync : NULL,
    };
    assert((view->origin == GUEST_PAGE_ORIGIN_FILE) ==
            (view->file_identity != 0));
    assert(!view->copy_on_write ||
            (view->origin == GUEST_PAGE_ORIGIN_FILE && view->sync != NULL));
    return GUEST_MEMORY_FAULT_NONE;
}

static enum guest_memory_page_in_result page_in(void *opaque,
        guest_addr_t page_base, enum guest_memory_access access,
        struct guest_memory_fault *fault) {
    struct guest_page_table *table = opaque;
    if (table->fault_ops == NULL || table->fault_ops->page_in == NULL)
        return GUEST_MEMORY_PAGE_IN_FAILED;
    return table->fault_ops->page_in(
            table->fault_opaque, page_base, access, fault);
}

static bool address_space_read_lock(void *opaque) {
    return guest_page_table_read_lock(opaque);
}

static void address_space_read_unlock(void *opaque, bool locked) {
    guest_page_table_read_unlock(opaque, locked);
}

static bool address_space_write_lock(void *opaque) {
    return guest_page_table_write_lock(opaque);
}

static void address_space_write_unlock(void *opaque, bool locked) {
    guest_page_table_write_unlock(opaque, locked);
}

struct prepared_private_file_cow {
    struct guest_page_mapping *mapping;
    struct guest_page_backing *new_backing;
    struct guest_page_backing *old_backing;
    struct guest_file_source *old_source;
};

static void discard_private_file_cow(
        struct prepared_private_file_cow *prepared, size_t count) {
    for (size_t index = 0; index < count; index++) {
        if (prepared[index].new_backing != NULL)
            guest_page_backing_release(prepared[index].new_backing);
    }
    free(prepared);
}

static bool prepare_private_file_write(
        void *opaque, guest_addr_t address, size_t size,
        struct guest_memory_fault *fault) {
    // 调用者已持有页表写事务，并已验证整段 WRITE 权限。
    struct guest_page_table *table = opaque;
    guest_addr_t first = address & ~GUEST_MEMORY_PAGE_MASK;
    guest_addr_t last = (address + (guest_addr_t) size - 1) &
            ~GUEST_MEMORY_PAGE_MASK;
    size_t cow_count = 0;
    for (guest_addr_t page = first;; page += GUEST_MEMORY_PAGE_SIZE) {
        struct guest_page_mapping *mapping = find_mapping(table, page);
        assert(mapping != NULL);
        if (mapping->copy_on_write)
            cow_count++;
        if (page == last)
            break;
    }
    if (cow_count == 0)
        return true;
    if (cow_count > SIZE_MAX / sizeof(struct prepared_private_file_cow)) {
        fault->kind = GUEST_MEMORY_FAULT_OUT_OF_MEMORY;
        return false;
    }

    struct prepared_private_file_cow *prepared =
            calloc(cow_count, sizeof(*prepared));
    if (prepared == NULL) {
        fault->kind = GUEST_MEMORY_FAULT_OUT_OF_MEMORY;
        return false;
    }
    size_t prepared_count = 0;
    for (guest_addr_t page = first;; page += GUEST_MEMORY_PAGE_SIZE) {
        struct guest_page_mapping *mapping = find_mapping(table, page);
        assert(mapping != NULL);
        if (mapping->copy_on_write) {
            struct guest_page_backing *copy =
                    guest_page_backing_clone(mapping->backing);
            if (copy == NULL) {
                fault->address = page;
                fault->kind = GUEST_MEMORY_FAULT_OUT_OF_MEMORY;
                discard_private_file_cow(prepared, cow_count);
                return false;
            }
            prepared[prepared_count++] =
                    (struct prepared_private_file_cow) {
                .mapping = mapping,
                .new_backing = copy,
            };
        }
        if (page == last)
            break;
    }
    assert(prepared_count == cow_count);

    for (size_t index = 0; index < cow_count; index++) {
        struct prepared_private_file_cow *entry = &prepared[index];
        struct guest_page_mapping *mapping = entry->mapping;
        assert(mapping->copy_on_write &&
                mapping->origin == GUEST_PAGE_ORIGIN_FILE &&
                mapping->shared_backing && mapping->file_source != NULL);
        entry->old_backing = mapping->backing;
        entry->old_source = mapping->file_source;
        mapping->backing = entry->new_backing;
        entry->new_backing = NULL;
        mapping->file_source = NULL;
        mapping->file_offset = 0;
        mapping->origin = GUEST_PAGE_ORIGIN_ANONYMOUS;
        mapping->shared_backing = false;
        mapping->copy_on_write = false;
    }
    guest_address_space_changed(&table->address_space);
    for (size_t index = 0; index < cow_count; index++) {
        guest_file_source_release(prepared[index].old_source);
        guest_page_backing_release(prepared[index].old_backing);
    }
    free(prepared);
    return true;
}

static const struct guest_address_space_ops page_table_ops = {
    .read_lock = address_space_read_lock,
    .read_unlock = address_space_read_unlock,
    .write_lock = address_space_write_lock,
    .write_unlock = address_space_write_unlock,
    .prepare_write = prepare_private_file_write,
    .resolve_page = resolve_page,
    .page_in = page_in,
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

static void *clone_calloc(size_t count, size_t size) {
    return clone_allocation_fails() ? NULL : calloc(count, size);
}
#else
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
    for (unsigned i = 0; i < GUEST_PAGE_TABLE_ENTRY_COUNT; i++) {
        if (leaf->entries[i].backing != NULL) {
            guest_file_source_release(leaf->entries[i].file_source);
            guest_page_backing_release(leaf->entries[i].backing);
        }
    }
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

bool guest_page_table_read_lock(struct guest_page_table *table) {
    if (!atomic_load_explicit(&table->concurrent, memory_order_acquire))
        return false;
    assert(pthread_rwlock_rdlock(&table->lock) == 0);
    return true;
}

void guest_page_table_read_unlock(
        struct guest_page_table *table, bool locked) {
    if (locked)
        assert(pthread_rwlock_unlock(&table->lock) == 0);
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
        if (source_mapping->backing == NULL)
            continue;
        struct guest_page_mapping *copy_mapping = &copy->entries[i];
        if (source_mapping->shared_backing) {
            guest_page_backing_retain(source_mapping->backing);
            copy_mapping->backing = source_mapping->backing;
        } else {
#if defined(GUEST_PAGE_TABLE_TESTING)
            if (clone_allocation_fails()) {
                destroy_leaf(copy);
                return NULL;
            }
#endif
            copy_mapping->backing = guest_page_backing_clone(
                    source_mapping->backing);
            if (copy_mapping->backing == NULL) {
                destroy_leaf(copy);
                return NULL;
            }
        }
        copy_mapping->permissions = source_mapping->permissions;
        copy_mapping->origin = source_mapping->origin;
        copy_mapping->file_source = guest_file_source_retain(
                source_mapping->file_source);
        copy_mapping->file_offset = source_mapping->file_offset;
        copy_mapping->shared_backing = source_mapping->shared_backing;
        copy_mapping->copy_on_write = source_mapping->copy_on_write;
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

bool guest_page_table_clone_locked(struct guest_page_table *destination,
        const struct guest_page_table *source) {
    if (destination == NULL || destination == source)
        return false;
    *destination = (struct guest_page_table) {0};
    if (source == NULL || source->root == NULL)
        return false;
    if (!guest_page_table_init(destination,
            source->address_space.address_bits))
        return false;

    for (unsigned i = 0; i < GUEST_PAGE_TABLE_ENTRY_COUNT; i++) {
        if (source->root->entries[i] == NULL)
            continue;
        destination->root->entries[i] =
                clone_level1(source->root->entries[i]);
        if (destination->root->entries[i] == NULL) {
            guest_page_table_destroy(destination);
            return false;
        }
    }
    destination->address_space.generation =
            source->address_space.generation;
    return true;
}

bool guest_page_table_clone(struct guest_page_table *destination,
        const struct guest_page_table *source) {
    if (destination == NULL || source == NULL || source->root == NULL ||
            destination == source)
        return false;
    bool locked = guest_page_table_read_lock(
            (struct guest_page_table *) source);
    bool result = guest_page_table_clone_locked(destination, source);
    guest_page_table_read_unlock(
            (struct guest_page_table *) source, locked);
    return result;
}

void guest_page_table_set_fault_ops(struct guest_page_table *table,
        const struct guest_page_table_fault_ops *ops, void *opaque) {
    assert(table != NULL && table->root != NULL);
    assert((ops == NULL) == (opaque == NULL));
    assert(ops == NULL || ops->page_in != NULL);
    table->fault_ops = ops;
    table->fault_opaque = opaque;
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

static enum guest_page_table_result map_page(
        struct guest_page_table *table, guest_addr_t page_base,
        unsigned permissions, enum guest_page_origin origin,
        struct guest_file_source *source, qword_t file_offset,
        byte_t **host_page) {
    assert((permissions & ~GUEST_MEMORY_PERMISSION_MASK) == 0);
    assert(host_page != NULL);
    assert((origin == GUEST_PAGE_ORIGIN_FILE) == (source != NULL));
    assert(origin == GUEST_PAGE_ORIGIN_FILE || file_offset == 0);
    if (!valid_page(table, page_base))
        return GUEST_PAGE_TABLE_INVALID_ADDRESS;

    struct guest_page_mapping *mapping;
    enum guest_page_table_result prepare = prepare_mapping_slot(
            table, page_base, &mapping);
    if (prepare != GUEST_PAGE_TABLE_OK)
        return prepare;
    if (mapping->backing != NULL) {
        *host_page = guest_page_backing_bytes(mapping->backing);
        return GUEST_PAGE_TABLE_ALREADY_MAPPED;
    }
    mapping->backing = guest_page_backing_create();
    if (mapping->backing == NULL)
        return GUEST_PAGE_TABLE_OUT_OF_MEMORY;
    mapping->permissions = permissions;
    if (origin == GUEST_PAGE_ORIGIN_FILE)
        (void) set_file_source(mapping, source, file_offset, true);
    else
        (void) set_nonfile_origin(mapping, origin, false);
    *host_page = guest_page_backing_bytes(mapping->backing);
    guest_address_space_changed(&table->address_space);
    return GUEST_PAGE_TABLE_OK;
}

enum guest_page_table_result guest_page_table_map(
        struct guest_page_table *table, guest_addr_t page_base,
        unsigned permissions, byte_t **host_page) {
    return map_page(table, page_base, permissions,
            GUEST_PAGE_ORIGIN_ANONYMOUS, NULL, 0, host_page);
}

enum guest_page_table_result guest_page_table_map_file(
        struct guest_page_table *table, guest_addr_t page_base,
        unsigned permissions, struct guest_file_source *source,
        qword_t file_offset, byte_t **host_page) {
    return map_page(table, page_base, permissions,
            GUEST_PAGE_ORIGIN_FILE, source, file_offset, host_page);
}

static enum guest_page_table_result map_file_backing(
        struct guest_page_table *table, guest_addr_t page_base,
        unsigned permissions, struct guest_file_source *source,
        qword_t file_offset, struct guest_page_backing *backing,
        bool copy_on_write) {
    assert((permissions & ~GUEST_MEMORY_PERMISSION_MASK) == 0);
    assert(source != NULL && backing != NULL);
    assert((file_offset & GUEST_MEMORY_PAGE_MASK) == 0);
    if (!valid_page(table, page_base))
        return GUEST_PAGE_TABLE_INVALID_ADDRESS;

    struct guest_page_mapping *mapping;
    enum guest_page_table_result prepare = prepare_mapping_slot(
            table, page_base, &mapping);
    if (prepare != GUEST_PAGE_TABLE_OK)
        return prepare;
    if (mapping->backing != NULL)
        return GUEST_PAGE_TABLE_ALREADY_MAPPED;

    guest_page_backing_retain(backing);
    mapping->backing = backing;
    mapping->permissions = permissions;
    (void) set_file_source(
            mapping, source, file_offset, copy_on_write);
    guest_address_space_changed(&table->address_space);
    return GUEST_PAGE_TABLE_OK;
}

enum guest_page_table_result guest_page_table_map_private_file_backing(
        struct guest_page_table *table, guest_addr_t page_base,
        unsigned permissions, struct guest_file_source *source,
        qword_t file_offset, struct guest_page_backing *backing) {
    return map_file_backing(table, page_base, permissions,
            source, file_offset, backing, true);
}

enum guest_page_table_result guest_page_table_map_shared_file_backing(
        struct guest_page_table *table, guest_addr_t page_base,
        unsigned permissions, struct guest_file_source *source,
        qword_t file_offset, struct guest_page_backing *backing) {
    return map_file_backing(table, page_base, permissions,
            source, file_offset, backing, false);
}

enum guest_page_table_result guest_page_table_map_special(
        struct guest_page_table *table, guest_addr_t page_base,
        unsigned permissions, byte_t **host_page) {
    return map_page(table, page_base, permissions,
            GUEST_PAGE_ORIGIN_SPECIAL, NULL, 0, host_page);
}

enum guest_page_table_result guest_page_table_set_file_source(
        struct guest_page_table *table, guest_addr_t page_base,
        struct guest_file_source *source, qword_t file_offset) {
    if (!valid_page(table, page_base))
        return GUEST_PAGE_TABLE_INVALID_ADDRESS;
    struct guest_page_mapping *mapping = find_mapping(table, page_base);
    if (mapping == NULL)
        return GUEST_PAGE_TABLE_NOT_MAPPED;
    if (set_file_source(mapping, source, file_offset, true))
        guest_address_space_changed(&table->address_space);
    return GUEST_PAGE_TABLE_OK;
}

enum guest_page_table_result guest_page_table_set_origin(
        struct guest_page_table *table, guest_addr_t page_base,
        enum guest_page_origin origin) {
    assert(origin == GUEST_PAGE_ORIGIN_ANONYMOUS ||
            origin == GUEST_PAGE_ORIGIN_SPECIAL);
    if (!valid_page(table, page_base))
        return GUEST_PAGE_TABLE_INVALID_ADDRESS;
    struct guest_page_mapping *mapping = find_mapping(table, page_base);
    if (mapping == NULL)
        return GUEST_PAGE_TABLE_NOT_MAPPED;
    bool copy_backing = mapping->copy_on_write ||
            (origin == GUEST_PAGE_ORIGIN_SPECIAL &&
                    mapping->shared_backing);
    struct guest_page_backing *old_backing = NULL;
    if (copy_backing) {
        struct guest_page_backing *copy =
                guest_page_backing_clone(mapping->backing);
        if (copy == NULL)
            return GUEST_PAGE_TABLE_OUT_OF_MEMORY;
        old_backing = mapping->backing;
        mapping->backing = copy;
    }
    bool shared_backing = copy_backing ? false :
            mapping->shared_backing;
    if (set_nonfile_origin(mapping, origin, shared_backing))
        guest_address_space_changed(&table->address_space);
    if (old_backing != NULL)
        guest_page_backing_release(old_backing);
    return GUEST_PAGE_TABLE_OK;
}

static void discard_prepared_pages(struct prepared_page_mapping *prepared,
        size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (prepared[i].new_backing != NULL)
            guest_page_backing_release(prepared[i].new_backing);
    }
    free(prepared);
}

static enum guest_page_table_result map_zero_range(
        struct guest_page_table *table, struct guest_page_range range,
        unsigned permissions, bool replace, bool shared) {
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
        prepared[i].new_backing = guest_page_backing_create();
        if (prepared[i].new_backing == NULL) {
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
        prepared[i].old_backing = prepared[i].mapping->backing;
        prepared[i].mapping->backing = prepared[i].new_backing;
        prepared[i].mapping->permissions = permissions;
        (void) set_nonfile_origin(prepared[i].mapping,
                GUEST_PAGE_ORIGIN_ANONYMOUS, shared);
        prepared[i].new_backing = NULL;
    }
    guest_address_space_changed(&table->address_space);
    for (size_t i = 0; i < count; i++) {
        if (prepared[i].old_backing != NULL)
            guest_page_backing_release(prepared[i].old_backing);
    }
    free(prepared);
    return GUEST_PAGE_TABLE_OK;
}

enum guest_page_table_result guest_page_table_map_zero_range(
        struct guest_page_table *table, struct guest_page_range range,
        unsigned permissions, bool replace) {
    return map_zero_range(table, range, permissions, replace, false);
}

enum guest_page_table_result guest_page_table_map_zero_shared_range(
        struct guest_page_table *table, struct guest_page_range range,
        unsigned permissions, bool replace) {
    return map_zero_range(table, range, permissions, replace, true);
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
    *host_page = guest_page_backing_bytes(mapping->backing);
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
        if (leaf->entries[i].backing != NULL)
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
    if (leaf == NULL || leaf->entries[index3].backing == NULL)
        return GUEST_PAGE_TABLE_NOT_MAPPED;

    guest_file_source_release(leaf->entries[index3].file_source);
    guest_page_backing_release(leaf->entries[index3].backing);
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

enum guest_page_table_result guest_page_table_protect_present_range(
        struct guest_page_table *table, struct guest_page_range range,
        unsigned permissions) {
    assert((permissions & ~GUEST_MEMORY_PERMISSION_MASK) == 0);
    if (!valid_range(table, range))
        return GUEST_PAGE_TABLE_INVALID_ADDRESS;

    bool changed = false;
    guest_addr_t page = range.first;
    for (qword_t index = 0; index < range.page_count; index++) {
        struct guest_page_mapping *mapping = find_mapping(table, page);
        if (mapping != NULL && mapping->permissions != permissions) {
            mapping->permissions = permissions;
            changed = true;
        }
        page += GUEST_MEMORY_PAGE_SIZE;
    }
    if (changed)
        guest_address_space_changed(&table->address_space);
    return GUEST_PAGE_TABLE_OK;
}
