#ifndef GUEST_MEMORY_PAGE_TABLE_H
#define GUEST_MEMORY_PAGE_TABLE_H

#include "guest/memory/address-space.h"

enum guest_page_table_result {
    GUEST_PAGE_TABLE_OK,
    GUEST_PAGE_TABLE_INVALID_ADDRESS,
    GUEST_PAGE_TABLE_ALREADY_MAPPED,
    GUEST_PAGE_TABLE_NOT_MAPPED,
    GUEST_PAGE_TABLE_OUT_OF_MEMORY,
};

struct guest_page_table_node;

struct guest_page_table {
    struct guest_address_space address_space;
    struct guest_page_table_node *root;
};

bool guest_page_table_init(struct guest_page_table *table,
        byte_t address_bits);
void guest_page_table_destroy(struct guest_page_table *table);
enum guest_page_table_result guest_page_table_map(
        struct guest_page_table *table, guest_addr_t page_base,
        unsigned permissions, byte_t **host_page);
enum guest_page_table_result guest_page_table_lookup(
        struct guest_page_table *table, guest_addr_t page_base,
        byte_t **host_page, unsigned *permissions);
enum guest_page_table_result guest_page_table_unmap(
        struct guest_page_table *table, guest_addr_t page_base);
enum guest_page_table_result guest_page_table_protect(
        struct guest_page_table *table, guest_addr_t page_base,
        unsigned permissions);

#endif
