#ifndef GUEST_MEMORY_TLB_H
#define GUEST_MEMORY_TLB_H

#include "guest/memory/address-space.h"

#define GUEST_TLB_BITS 8
#define GUEST_TLB_SIZE (1 << GUEST_TLB_BITS)
#define GUEST_TLB_MAX_ACCESS_SIZE 32

struct guest_tlb_entry {
    guest_addr_t guest_page;
    byte_t *host_page;
    unsigned permissions;
    bool valid;
};

struct guest_tlb {
    struct guest_address_space *address_space;
    qword_t observed_generation;
    struct guest_tlb_entry entries[GUEST_TLB_SIZE];
};

struct guest_tlb_exclusive_token {
    const struct guest_address_space *address_space;
    qword_t mapping_generation;
    qword_t write_generation;
};

enum guest_tlb_store_exclusive_result {
    GUEST_TLB_EXCLUSIVE_STORED,
    GUEST_TLB_EXCLUSIVE_FAILED,
    GUEST_TLB_EXCLUSIVE_FAULT,
};

enum guest_tlb_compare_exchange_result {
    GUEST_TLB_COMPARE_EXCHANGE_EXCHANGED,
    GUEST_TLB_COMPARE_EXCHANGE_MISMATCH,
    GUEST_TLB_COMPARE_EXCHANGE_FAULT,
};

_Static_assert(sizeof(guest_addr_t) == 8,
        "独立 guest TLB 要求 64 位地址类型");
_Static_assert((GUEST_TLB_SIZE & (GUEST_TLB_SIZE - 1)) == 0,
        "guest TLB 容量必须为二次幂");

void guest_tlb_init(struct guest_tlb *tlb,
        struct guest_address_space *address_space);
void guest_tlb_bind(struct guest_tlb *tlb,
        struct guest_address_space *address_space);
void guest_tlb_flush(struct guest_tlb *tlb);
bool guest_tlb_read(struct guest_tlb *tlb, guest_addr_t address,
        void *destination, size_t size, enum guest_memory_access access,
        struct guest_memory_fault *fault);
// 返回的数据与保留令牌来自同一个读事务。
bool guest_tlb_load_exclusive(struct guest_tlb *tlb,
        guest_addr_t address, void *destination, size_t size,
        struct guest_tlb_exclusive_token *token,
        struct guest_memory_fault *fault);
bool guest_tlb_write(struct guest_tlb *tlb, guest_addr_t address,
        const void *source, size_t size, struct guest_memory_fault *fault);
// 在同一个写事务中校验保留和值并写入；过期保留不访问失效映射。
enum guest_tlb_store_exclusive_result guest_tlb_store_exclusive(
        struct guest_tlb *tlb, guest_addr_t address,
        const void *expected, const void *replacement, size_t size,
        struct guest_tlb_exclusive_token token,
        struct guest_memory_fault *fault);
// observed 仅在完整预检成功后更新，返回值区分交换、比较失败与访存故障。
enum guest_tlb_compare_exchange_result guest_tlb_compare_exchange(
        struct guest_tlb *tlb, guest_addr_t address,
        const void *expected, const void *replacement, void *observed,
        size_t size, struct guest_memory_fault *fault);

#endif
