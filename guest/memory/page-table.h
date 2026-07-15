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
// source 未启用并发时调用者须冻结它；destination 未初始化。
// 成功后私有 backing 独立，共享 backing 由两个页表共同持有。
bool guest_page_table_clone(struct guest_page_table *destination,
        const struct guest_page_table *source);
// 调用方已持有 source 的读事务时使用，避免复合快照重复加锁。
bool guest_page_table_clone_locked(struct guest_page_table *destination,
        const struct guest_page_table *source);
#if defined(GUEST_PAGE_TABLE_TESTING)
void guest_page_table_test_fail_clone_allocation_at(size_t index);
size_t guest_page_table_test_clone_allocation_count(void);
#endif
void guest_page_table_destroy(struct guest_page_table *table);
// 共享页表的复合映射变更须持有写锁；单次 TLB 访问会自动加锁。
// 必须在把页表发布给第二个线程前启用；启用后不再关闭。
void guest_page_table_enable_concurrency(struct guest_page_table *table);
bool guest_page_table_read_lock(struct guest_page_table *table);
void guest_page_table_read_unlock(
        struct guest_page_table *table, bool locked);
bool guest_page_table_write_lock(struct guest_page_table *table);
void guest_page_table_write_unlock(
        struct guest_page_table *table, bool locked);
// host_page 是借用指针；ALREADY_MAPPED 时原映射可能是共享页。
// 运行期的共享页数据访问必须经过 TLB，不能绕过同步域直接读写该指针。
enum guest_page_table_result guest_page_table_map(
        struct guest_page_table *table, guest_addr_t page_base,
        unsigned permissions, byte_t **host_page);
// host_page 只适合冻结页表或调用方已知为私有页的管理操作。
// 可能与其他地址空间共享的数据访问必须经过 TLB。
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
// 创建可由 clone 后页表共同持有的零页；映射元数据仍归各页表独立管理。
enum guest_page_table_result guest_page_table_map_zero_shared_range(
        struct guest_page_table *table, struct guest_page_range range,
        unsigned permissions, bool replace);
enum guest_page_table_result guest_page_table_unmap_range(
        struct guest_page_table *table, struct guest_page_range range,
        bool allow_holes);
enum guest_page_table_result guest_page_table_protect_range(
        struct guest_page_table *table, struct guest_page_range range,
        unsigned permissions);

#endif
