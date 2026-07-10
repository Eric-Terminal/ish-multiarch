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

enum guest_memory_access {
    GUEST_MEMORY_READ = 1 << 0,
    GUEST_MEMORY_WRITE = 1 << 1,
    GUEST_MEMORY_EXECUTE = 1 << 2,
};

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
    enum guest_memory_fault_kind (*resolve_page)(void *opaque,
            guest_addr_t page_base, enum guest_memory_access access,
            struct guest_page_view *view);
};

struct guest_address_space {
    const struct guest_address_space_ops *ops;
    void *opaque;
    qword_t generation;
    byte_t address_bits;
};

void guest_address_space_init(struct guest_address_space *space,
        const struct guest_address_space_ops *ops, void *opaque,
        byte_t address_bits);
void guest_address_space_changed(struct guest_address_space *space);
bool guest_address_space_contains(const struct guest_address_space *space,
        guest_addr_t address, size_t size);
enum guest_memory_fault_kind guest_address_space_resolve_page(
        struct guest_address_space *space, guest_addr_t page_base,
        enum guest_memory_access access, struct guest_page_view *view);

#endif
