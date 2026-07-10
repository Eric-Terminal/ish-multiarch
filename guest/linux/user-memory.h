#ifndef GUEST_LINUX_USER_MEMORY_H
#define GUEST_LINUX_USER_MEMORY_H

#include "guest/memory/tlb.h"

bool guest_linux_copy_from_user(struct guest_tlb *tlb,
        guest_addr_t address, void *destination, size_t size,
        struct guest_memory_fault *fault);
bool guest_linux_copy_to_user(struct guest_tlb *tlb,
        guest_addr_t address, const void *source, size_t size,
        struct guest_memory_fault *fault);

#endif
