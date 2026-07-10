#include <assert.h>
#include <stdlib.h>

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

static const struct guest_address_space_ops page_table_ops = {
    .resolve_page = resolve_page,
};

bool guest_page_table_init(struct guest_page_table *table,
        byte_t address_bits) {
    *table = (struct guest_page_table) {0};
    table->root = calloc(1, sizeof(*table->root));
    if (table->root == NULL)
        return false;
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
    for (unsigned i = 0; i < GUEST_PAGE_TABLE_ENTRY_COUNT; i++) {
        if (table->root->entries[i] != NULL)
            destroy_level1(table->root->entries[i]);
    }
    free(table->root);
    table->root = NULL;
}

static bool valid_page(const struct guest_page_table *table,
        guest_addr_t page_base) {
    return (page_base & GUEST_MEMORY_PAGE_MASK) == 0 &&
            guest_address_space_contains(&table->address_space,
                    page_base, GUEST_MEMORY_PAGE_SIZE);
}

static void *allocate_slot(void **slot, size_t size) {
    if (*slot == NULL)
        *slot = calloc(1, size);
    return *slot;
}

enum guest_page_table_result guest_page_table_map(
        struct guest_page_table *table, guest_addr_t page_base,
        unsigned permissions, byte_t **host_page) {
    assert((permissions & ~GUEST_MEMORY_PERMISSION_MASK) == 0);
    assert(host_page != NULL);
    if (!valid_page(table, page_base))
        return GUEST_PAGE_TABLE_INVALID_ADDRESS;

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

    struct guest_page_mapping *mapping =
            &leaf->entries[page_index(page_base, 12)];
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

enum guest_page_table_result guest_page_table_unmap(
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
