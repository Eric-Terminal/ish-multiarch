#ifndef GUEST_MEMORY_ADDRESS_SPACE_H
#define GUEST_MEMORY_ADDRESS_SPACE_H

#ifdef __KERNEL__
#include <linux/stddef.h>
#else
#include <stddef.h>
#endif

#include "misc.h"

#define GUEST_MEMORY_PAGE_BITS 12
#define GUEST_MEMORY_PAGE_SIZE (UINT64_C(1) << GUEST_MEMORY_PAGE_BITS)
#define GUEST_MEMORY_PAGE_MASK (GUEST_MEMORY_PAGE_SIZE - 1)
#define GUEST_MEMORY_EXCLUSIVE_GRANULE_BITS 4
#define GUEST_MEMORY_EXCLUSIVE_GRANULE_SIZE \
    (UINT64_C(1) << GUEST_MEMORY_EXCLUSIVE_GRANULE_BITS)
#define GUEST_MEMORY_EXCLUSIVE_BUCKET_BITS 6
#define GUEST_MEMORY_EXCLUSIVE_BUCKET_COUNT \
    (1 << GUEST_MEMORY_EXCLUSIVE_BUCKET_BITS)
#define GUEST_MEMORY_EXCLUSIVE_WAYS 4

enum guest_memory_access {
    GUEST_MEMORY_READ = 1 << 0,
    GUEST_MEMORY_WRITE = 1 << 1,
    GUEST_MEMORY_EXECUTE = 1 << 2,
};
#define GUEST_MEMORY_PERMISSION_MASK ((unsigned) (GUEST_MEMORY_READ | \
        GUEST_MEMORY_WRITE | GUEST_MEMORY_EXECUTE))

enum guest_memory_fault_kind {
    GUEST_MEMORY_FAULT_NONE,
    GUEST_MEMORY_FAULT_ADDRESS_SIZE,
    GUEST_MEMORY_FAULT_UNMAPPED,
    GUEST_MEMORY_FAULT_PERMISSION,
    GUEST_MEMORY_FAULT_ALIGNMENT,
};

// 页面来源只描述 guest 可观察的后备类型，不暴露宿主对象或指针。
enum guest_page_origin {
    GUEST_PAGE_ORIGIN_ANONYMOUS,
    GUEST_PAGE_ORIGIN_FILE,
    GUEST_PAGE_ORIGIN_SPECIAL,
};

struct guest_memory_fault {
    guest_addr_t address;
    enum guest_memory_access access;
    enum guest_memory_fault_kind kind;
};

struct guest_page_sync;

struct guest_page_sync_ops {
    // track/matches/written 只能在对应同步域写锁内调用。
    void (*read_lock)(void *opaque);
    void (*read_unlock)(void *opaque);
    void (*write_lock)(void *opaque);
    void (*write_unlock)(void *opaque);
    qword_t (*track_exclusive)(void *opaque, size_t page_offset);
    bool (*exclusive_matches)(void *opaque, size_t page_offset,
            qword_t generation);
    void (*written)(void *opaque, size_t page_offset, size_t size);
};

// identity 保留 0；同一进程内所有当前及历史同步域身份均唯一且不复用。
// 每个同步域只对应一个物理 4 KiB 页；复用该域的 view 必须返回同一
// host_page，页内 offset 才能与 identity 共同构成稳定物理键。
// TLB 以 identity 排序共享页锁，Linux 层也可据此构造物理页键。
struct guest_page_sync {
    const struct guest_page_sync_ops *ops;
    void *opaque;
    qword_t identity;
};

struct guest_page_view {
    byte_t *host_page;
    unsigned permissions;
    enum guest_page_origin origin;
    // 页表后备身份在当前及历史对象间不复用；私有页也必须提供非零值。
    qword_t backing_identity;
    // NULL 表示页面只受所属 address space 的事务锁保护。非空借用指针
    // 可以缓存，但只能在持有 address space 锁且映射世代仍匹配时使用。
    const struct guest_page_sync *sync;
};

struct guest_address_space_ops {
    // 非空时，TLB 会用这两组回调包住解析与实际 host 内存访问。
    bool (*read_lock)(void *opaque);
    void (*read_unlock)(void *opaque, bool locked);
    bool (*write_lock)(void *opaque);
    void (*write_unlock)(void *opaque, bool locked);
    // 非空时，在写访问权限已成功解析后执行不依赖实际存储的后备转换。
    void (*write_prepared)(void *opaque, guest_addr_t address, size_t size);
    // 非空时，在写事务实际提交后更新页级后备元数据。
    void (*written)(void *opaque, guest_addr_t address, size_t size);
    enum guest_memory_fault_kind (*resolve_page)(void *opaque,
            guest_addr_t page_base, enum guest_memory_access access,
            struct guest_page_view *view);
};

struct guest_exclusive_record {
    guest_addr_t granule_base;
    qword_t generation;
};

struct guest_address_space {
    const struct guest_address_space_ops *ops;
    void *opaque;
    qword_t generation;
    qword_t exclusive_sequence;
    // 完整粒度 tag 隔离无关写；组满轮换只会让旧 STXR 保守失败。
    // 固定容量同时避免 guest 用大量 LDXR 地址制造宿主内存增长。
    struct guest_exclusive_record exclusive_records
            [GUEST_MEMORY_EXCLUSIVE_BUCKET_COUNT]
            [GUEST_MEMORY_EXCLUSIVE_WAYS];
    byte_t exclusive_next_way[GUEST_MEMORY_EXCLUSIVE_BUCKET_COUNT];
    byte_t address_bits;
};

void guest_address_space_init(struct guest_address_space *space,
        const struct guest_address_space_ops *ops, void *opaque,
        byte_t address_bits);
void guest_address_space_changed(struct guest_address_space *space);
qword_t guest_address_space_track_exclusive(
        struct guest_address_space *space, guest_addr_t address);
bool guest_address_space_exclusive_matches(
        const struct guest_address_space *space, guest_addr_t address,
        qword_t generation);
void guest_address_space_written(struct guest_address_space *space,
        guest_addr_t address, size_t size);
bool guest_address_space_read_lock(struct guest_address_space *space);
void guest_address_space_read_unlock(
        struct guest_address_space *space, bool locked);
bool guest_address_space_write_lock(struct guest_address_space *space);
void guest_address_space_write_unlock(
        struct guest_address_space *space, bool locked);
void guest_address_space_write_prepared(
        struct guest_address_space *space,
        guest_addr_t address, size_t size);
bool guest_address_space_contains(const struct guest_address_space *space,
        guest_addr_t address, size_t size);
enum guest_memory_fault_kind guest_address_space_resolve_page(
        struct guest_address_space *space, guest_addr_t page_base,
        enum guest_memory_access access, struct guest_page_view *view);
qword_t guest_page_sync_identity(const struct guest_page_sync *sync);
// 调用者先持 address space 锁；多域访问须按 identity 升序加锁并逆序释放。
void guest_page_sync_read_lock(const struct guest_page_sync *sync);
void guest_page_sync_read_unlock(const struct guest_page_sync *sync);
void guest_page_sync_write_lock(const struct guest_page_sync *sync);
void guest_page_sync_write_unlock(const struct guest_page_sync *sync);
qword_t guest_page_sync_track_exclusive(
        const struct guest_page_sync *sync, size_t page_offset);
bool guest_page_sync_exclusive_matches(
        const struct guest_page_sync *sync, size_t page_offset,
        qword_t generation);
void guest_page_sync_written(const struct guest_page_sync *sync,
        size_t page_offset, size_t size);

#endif
