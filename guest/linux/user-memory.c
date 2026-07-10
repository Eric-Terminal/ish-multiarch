#include "guest/linux/user-memory.h"

static size_t user_copy_chunk(guest_addr_t address, size_t remaining) {
    size_t page_remaining = (size_t) (GUEST_MEMORY_PAGE_SIZE -
            (address & GUEST_MEMORY_PAGE_MASK));
    size_t chunk = remaining < GUEST_TLB_MAX_ACCESS_SIZE ?
            remaining : GUEST_TLB_MAX_ACCESS_SIZE;
    return chunk < page_remaining ? chunk : page_remaining;
}

bool guest_linux_copy_from_user(struct guest_tlb *tlb,
        guest_addr_t address, void *destination, size_t size,
        struct guest_memory_fault *fault) {
    if (size == 0)
        return guest_tlb_read(tlb, address, destination, 0,
                GUEST_MEMORY_READ, fault);
    byte_t *output = destination;
    size_t remaining = size;
    while (remaining != 0) {
        size_t chunk = user_copy_chunk(address, remaining);
        if (!guest_tlb_read(tlb, address, output, chunk,
                GUEST_MEMORY_READ, fault))
            return false;
        address += (guest_addr_t) chunk;
        output += chunk;
        remaining -= chunk;
    }
    return true;
}

bool guest_linux_copy_to_user(struct guest_tlb *tlb,
        guest_addr_t address, const void *source, size_t size,
        struct guest_memory_fault *fault) {
    if (size == 0)
        return guest_tlb_write(tlb, address, source, 0, fault);
    const byte_t *input = source;
    size_t remaining = size;
    while (remaining != 0) {
        size_t chunk = user_copy_chunk(address, remaining);
        if (!guest_tlb_write(tlb, address, input, chunk, fault))
            return false;
        address += (guest_addr_t) chunk;
        input += chunk;
        remaining -= chunk;
    }
    return true;
}
