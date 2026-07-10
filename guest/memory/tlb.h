#ifndef GUEST_MEMORY_TLB_H
#define GUEST_MEMORY_TLB_H

#include "guest/memory/address-space.h"

#define GUEST_TLB_BITS 8
#define GUEST_TLB_SIZE (1 << GUEST_TLB_BITS)
#define GUEST_TLB_MAX_ACCESS_SIZE 16

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

_Static_assert(sizeof(guest_addr_t) == 8,
        "独立 guest TLB 要求 64 位地址类型");
_Static_assert((GUEST_TLB_SIZE & (GUEST_TLB_SIZE - 1)) == 0,
        "guest TLB 容量必须为二次幂");

void guest_tlb_init(struct guest_tlb *tlb,
        struct guest_address_space *address_space);
void guest_tlb_flush(struct guest_tlb *tlb);
bool guest_tlb_read(struct guest_tlb *tlb, guest_addr_t address,
        void *destination, size_t size, enum guest_memory_access access,
        struct guest_memory_fault *fault);
bool guest_tlb_write(struct guest_tlb *tlb, guest_addr_t address,
        const void *source, size_t size, struct guest_memory_fault *fault);

#endif
