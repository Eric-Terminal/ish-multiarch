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
struct guest_page_backing;

struct guest_page_table_fault_ops {
    // 由 address-space 保证只在未持有页表锁时调用。
    enum guest_memory_page_in_result (*page_in)(void *opaque,
            guest_addr_t page_base, enum guest_memory_access access,
            struct guest_memory_fault *fault);
};

struct guest_page_table {
    struct guest_address_space address_space;
    struct guest_page_table_node *root;
    const struct guest_page_table_fault_ops *fault_ops;
    void *fault_opaque;
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
// 只能在页表尚未发布给并发访问者时设置或清除；clone 不复制 opaque。
void guest_page_table_set_fault_ops(struct guest_page_table *table,
        const struct guest_page_table_fault_ops *ops, void *opaque);
#if defined(GUEST_PAGE_TABLE_TESTING)
void guest_page_table_test_fail_clone_allocation_at(size_t index);
size_t guest_page_table_test_clone_allocation_count(void);
void guest_page_table_test_fail_remap_allocation_at(size_t index);
size_t guest_page_table_test_remap_allocation_count(void);
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
// 文件页与特殊内核页需要保留来源，供 fault/futex 等路径精确判定。
enum guest_page_table_result guest_page_table_map_file(
        struct guest_page_table *table, guest_addr_t page_base,
        unsigned permissions, struct guest_file_source *source,
        qword_t file_offset, byte_t **host_page);
// source/backing 均为借用；成功时页表各自取得一份强引用。
enum guest_page_table_result guest_page_table_map_private_file_backing(
        struct guest_page_table *table, guest_addr_t page_base,
        unsigned permissions, struct guest_file_source *source,
        qword_t file_offset, struct guest_page_backing *backing);
// shared 文件页保留同一 backing，写入不会进入私有 COW。
enum guest_page_table_result guest_page_table_map_shared_file_backing(
        struct guest_page_table *table, guest_addr_t page_base,
        unsigned permissions, struct guest_file_source *source,
        qword_t file_offset, struct guest_page_backing *backing);
enum guest_page_table_result guest_page_table_map_special(
        struct guest_page_table *table, guest_addr_t page_base,
        unsigned permissions, byte_t **host_page);
// FILE 来源必须通过专用接口携带稳定对象身份和页首文件偏移。
enum guest_page_table_result guest_page_table_set_file_source(
        struct guest_page_table *table, guest_addr_t page_base,
        struct guest_file_source *source, qword_t file_offset);
// 这里只接受 ANONYMOUS 或 SPECIAL，并会释放原文件来源引用；私有文件页
// 和共享页转 SPECIAL 会先复制 backing，其余非文件页保留既有共享属性。
enum guest_page_table_result guest_page_table_set_origin(
        struct guest_page_table *table, guest_addr_t page_base,
        enum guest_page_origin origin);
// host_page 只适合冻结页表或调用方已知为私有页的管理操作。
// 可能与其他地址空间共享的数据访问必须经过 TLB。
enum guest_page_table_result guest_page_table_lookup(
        struct guest_page_table *table, guest_addr_t page_base,
        byte_t **host_page, unsigned *permissions);
// 调用方持有页表写锁；只撤销已被 backing 失效域标记的驻留页。
bool guest_page_table_remove_inaccessible(
        struct guest_page_table *table, guest_addr_t page_base);
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
// 调用方持有页表写事务；目标范围必须为空，源空洞会在目标保留为空洞。
// 驻留映射的 backing、文件来源和 COW/共享属性按所有权整体迁移。
enum guest_page_table_result guest_page_table_move_range(
        struct guest_page_table *table, struct guest_page_range source,
        guest_addr_t destination_first);
// 在迁移源驻留页的同一事务中，为目标尾部建立私有匿名零页。
enum guest_page_table_result guest_page_table_move_range_expand_zero(
        struct guest_page_table *table, struct guest_page_range source,
        guest_addr_t destination_first, qword_t destination_page_count,
        unsigned destination_permissions);
// 只为源范围内的共享驻留映射建立别名；lazy 空洞不会被提前换页。
enum guest_page_table_result guest_page_table_alias_shared_range(
        struct guest_page_table *table, struct guest_page_range source,
        guest_addr_t destination_first);
// 迁移私有匿名范围，并以指定权限的全新零页原子替换源映射。
enum guest_page_table_result guest_page_table_move_range_replace_zero(
        struct guest_page_table *table, struct guest_page_range source,
        guest_addr_t destination_first, unsigned source_permissions);
enum guest_page_table_result guest_page_table_protect_range(
        struct guest_page_table *table, struct guest_page_range range,
        unsigned permissions);
// 只更新范围内已经驻留的页，lazy 页空洞不是错误。
enum guest_page_table_result guest_page_table_protect_present_range(
        struct guest_page_table *table, struct guest_page_range range,
        unsigned permissions);

#endif
