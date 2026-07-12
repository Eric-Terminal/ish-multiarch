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

struct guest_memory_fault {
    guest_addr_t address;
    enum guest_memory_access access;
    enum guest_memory_fault_kind kind;
};

struct guest_page_view {
    byte_t *host_page;
    unsigned permissions;
};

struct guest_address_space_ops {
    // 非空时，TLB 会用这两组回调包住解析与实际 host 内存访问。
    bool (*read_lock)(void *opaque);
    void (*read_unlock)(void *opaque, bool locked);
    bool (*write_lock)(void *opaque);
    void (*write_unlock)(void *opaque, bool locked);
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
bool guest_address_space_contains(const struct guest_address_space *space,
        guest_addr_t address, size_t size);
enum guest_memory_fault_kind guest_address_space_resolve_page(
        struct guest_address_space *space, guest_addr_t page_base,
        enum guest_memory_access access, struct guest_page_view *view);

#endif
