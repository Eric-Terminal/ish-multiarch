#ifndef GUEST_MEMORY_PAGE_TABLE_H
#define GUEST_MEMORY_PAGE_TABLE_H

#include <pthread.h>
#include <stdatomic.h>

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
    pthread_rwlock_t lock;
    atomic_bool concurrent;
};

struct guest_page_range {
    guest_addr_t first;
    qword_t page_count;
};

bool guest_page_table_init(struct guest_page_table *table,
        byte_t address_bits);
// 调用者须冻结 source；destination 未初始化，成功后拥有独立 backing，失败后为空。
bool guest_page_table_clone(struct guest_page_table *destination,
        const struct guest_page_table *source);
#if defined(GUEST_PAGE_TABLE_TESTING)
void guest_page_table_test_fail_clone_allocation_at(size_t index);
size_t guest_page_table_test_clone_allocation_count(void);
#endif
void guest_page_table_destroy(struct guest_page_table *table);
// 共享页表的复合映射变更须持有写锁；单次 TLB 访问会自动加锁。
// 必须在把页表发布给第二个线程前启用；启用后不再关闭。
void guest_page_table_enable_concurrency(struct guest_page_table *table);
bool guest_page_table_write_lock(struct guest_page_table *table);
void guest_page_table_write_unlock(
        struct guest_page_table *table, bool locked);
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
enum guest_page_table_result guest_page_table_map_zero_range(
        struct guest_page_table *table, struct guest_page_range range,
        unsigned permissions, bool replace);
enum guest_page_table_result guest_page_table_unmap_range(
        struct guest_page_table *table, struct guest_page_range range,
        bool allow_holes);
enum guest_page_table_result guest_page_table_protect_range(
        struct guest_page_table *table, struct guest_page_range range,
        unsigned permissions);

#endif
