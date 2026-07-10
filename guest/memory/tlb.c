#include <assert.h>
#include <string.h>

#include "guest/memory/tlb.h"

struct guest_tlb_access_chunk {
    byte_t *host;
    size_t size;
};

_Static_assert(GUEST_TLB_MAX_ACCESS_SIZE <= GUEST_MEMORY_PAGE_SIZE,
        "guest TLB 的单次访问最多只能跨越两个页面");

void guest_tlb_init(struct guest_tlb *tlb,
        struct guest_address_space *address_space) {
    *tlb = (struct guest_tlb) {
        .address_space = address_space,
        .observed_generation = address_space->generation,
    };
}

void guest_tlb_flush(struct guest_tlb *tlb) {
    memset(tlb->entries, 0, sizeof(tlb->entries));
    tlb->observed_generation = tlb->address_space->generation;
}

static unsigned guest_tlb_index(guest_addr_t page_base) {
    return (unsigned) ((page_base >> GUEST_MEMORY_PAGE_BITS) &
            (GUEST_TLB_SIZE - 1));
}

static enum guest_memory_fault_kind resolve_host_pointer(struct guest_tlb *tlb,
        guest_addr_t address, enum guest_memory_access access, byte_t **host) {
    if (tlb->observed_generation != tlb->address_space->generation)
        guest_tlb_flush(tlb);

    guest_addr_t page_base = address & ~GUEST_MEMORY_PAGE_MASK;
    struct guest_tlb_entry *entry = &tlb->entries[guest_tlb_index(page_base)];
    if (entry->valid && entry->guest_page == page_base &&
            (entry->permissions & access) != 0) {
        *host = entry->host_page + (address & GUEST_MEMORY_PAGE_MASK);
        return GUEST_MEMORY_FAULT_NONE;
    }

    struct guest_page_view view;
    enum guest_memory_fault_kind fault = guest_address_space_resolve_page(
            tlb->address_space, page_base, access, &view);
    if (fault != GUEST_MEMORY_FAULT_NONE)
        return fault;

    if (tlb->observed_generation != tlb->address_space->generation)
        guest_tlb_flush(tlb);
    entry = &tlb->entries[guest_tlb_index(page_base)];
    *entry = (struct guest_tlb_entry) {
        .guest_page = page_base,
        .host_page = view.host_page,
        .permissions = view.permissions,
        .valid = true,
    };
    *host = entry->host_page + (address & GUEST_MEMORY_PAGE_MASK);
    return GUEST_MEMORY_FAULT_NONE;
}

static guest_addr_t address_size_fault(const struct guest_address_space *space,
        guest_addr_t address) {
    if (space->address_bits == 64)
        return address;
    guest_addr_t limit = UINT64_C(1) << space->address_bits;
    return address < limit ? limit : address;
}

static bool prepare_access(struct guest_tlb *tlb, guest_addr_t address,
        size_t size, enum guest_memory_access access,
        struct guest_tlb_access_chunk chunks[2], unsigned *chunk_count,
        struct guest_memory_fault *fault) {
    assert(size <= GUEST_TLB_MAX_ACCESS_SIZE);
    *fault = (struct guest_memory_fault) {
        .address = address,
        .access = access,
        .kind = GUEST_MEMORY_FAULT_NONE,
    };
    *chunk_count = 0;
    if (size == 0)
        return true;
    if (!guest_address_space_contains(tlb->address_space, address, size)) {
        fault->address = address_size_fault(tlb->address_space, address);
        fault->kind = GUEST_MEMORY_FAULT_ADDRESS_SIZE;
        return false;
    }

    guest_addr_t current = address;
    size_t remaining = size;
    while (remaining != 0) {
        assert(*chunk_count < 2);
        size_t page_remaining = (size_t) (GUEST_MEMORY_PAGE_SIZE -
                (current & GUEST_MEMORY_PAGE_MASK));
        size_t chunk_size = remaining < page_remaining ? remaining : page_remaining;
        byte_t *host;
        enum guest_memory_fault_kind kind = resolve_host_pointer(
                tlb, current, access, &host);
        if (kind != GUEST_MEMORY_FAULT_NONE) {
            fault->address = current;
            fault->kind = kind;
            return false;
        }
        chunks[*chunk_count] = (struct guest_tlb_access_chunk) {
            .host = host,
            .size = chunk_size,
        };
        (*chunk_count)++;
        current += (guest_addr_t) chunk_size;
        remaining -= chunk_size;
    }
    return true;
}

bool guest_tlb_read(struct guest_tlb *tlb, guest_addr_t address,
        void *destination, size_t size, enum guest_memory_access access,
        struct guest_memory_fault *fault) {
    assert(access == GUEST_MEMORY_READ || access == GUEST_MEMORY_EXECUTE);
    struct guest_tlb_access_chunk chunks[2];
    unsigned chunk_count;
    if (!prepare_access(tlb, address, size, access, chunks, &chunk_count, fault))
        return false;

    byte_t *output = destination;
    for (unsigned i = 0; i < chunk_count; i++) {
        memcpy(output, chunks[i].host, chunks[i].size);
        output += chunks[i].size;
    }
    return true;
}

bool guest_tlb_write(struct guest_tlb *tlb, guest_addr_t address,
        const void *source, size_t size, struct guest_memory_fault *fault) {
    struct guest_tlb_access_chunk chunks[2];
    unsigned chunk_count;
    if (!prepare_access(tlb, address, size, GUEST_MEMORY_WRITE,
            chunks, &chunk_count, fault))
        return false;

    const byte_t *input = source;
    for (unsigned i = 0; i < chunk_count; i++) {
        memcpy(chunks[i].host, input, chunks[i].size);
        input += chunks[i].size;
    }
    return true;
}
