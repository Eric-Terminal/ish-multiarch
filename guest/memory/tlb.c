#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "guest/memory/tlb.h"

struct guest_tlb_access_chunk {
    byte_t *host;
    size_t size;
    size_t page_offset;
    const struct guest_page_sync *sync;
};

struct guest_tlb_sync_set {
    const struct guest_page_sync *items[2];
    unsigned count;
};

_Static_assert(GUEST_TLB_MAX_ACCESS_SIZE <= GUEST_MEMORY_PAGE_SIZE,
        "guest TLB 的单次访问最多只能跨越两个页面");

void guest_tlb_init(struct guest_tlb *tlb,
        struct guest_address_space *address_space) {
    *tlb = (struct guest_tlb) {0};
    guest_tlb_bind(tlb, address_space);
}

void guest_tlb_bind(struct guest_tlb *tlb,
        struct guest_address_space *address_space) {
    assert(address_space != NULL);
    bool locked = guest_address_space_read_lock(address_space);
    memset(tlb->entries, 0, sizeof(tlb->entries));
    tlb->address_space = address_space;
    tlb->observed_generation = address_space->generation;
    guest_address_space_read_unlock(address_space, locked);
}

static void flush_unlocked(struct guest_tlb *tlb) {
    assert(tlb->address_space != NULL);
    memset(tlb->entries, 0, sizeof(tlb->entries));
    tlb->observed_generation = tlb->address_space->generation;
}

void guest_tlb_flush(struct guest_tlb *tlb) {
    assert(tlb->address_space != NULL);
    bool locked = guest_address_space_read_lock(tlb->address_space);
    flush_unlocked(tlb);
    guest_address_space_read_unlock(tlb->address_space, locked);
}

static unsigned guest_tlb_index(guest_addr_t page_base) {
    return (unsigned) ((page_base >> GUEST_MEMORY_PAGE_BITS) &
            (GUEST_TLB_SIZE - 1));
}

static enum guest_memory_fault_kind resolve_host_pointer(struct guest_tlb *tlb,
        guest_addr_t address, enum guest_memory_access access,
        struct guest_tlb_access_chunk *chunk) {
    if (tlb->observed_generation != tlb->address_space->generation)
        flush_unlocked(tlb);

    guest_addr_t page_base = address & ~GUEST_MEMORY_PAGE_MASK;
    struct guest_tlb_entry *entry = &tlb->entries[guest_tlb_index(page_base)];
    if (entry->valid && entry->guest_page == page_base &&
            (entry->permissions & access) != 0) {
        chunk->page_offset = (size_t) (address & GUEST_MEMORY_PAGE_MASK);
        chunk->host = entry->host_page + chunk->page_offset;
        chunk->sync = entry->sync;
        return GUEST_MEMORY_FAULT_NONE;
    }

    struct guest_page_view view;
    enum guest_memory_fault_kind fault = guest_address_space_resolve_page(
            tlb->address_space, page_base, access, &view);
    if (fault != GUEST_MEMORY_FAULT_NONE)
        return fault;

    if (tlb->observed_generation != tlb->address_space->generation)
        flush_unlocked(tlb);
    entry = &tlb->entries[guest_tlb_index(page_base)];
    *entry = (struct guest_tlb_entry) {
        .guest_page = page_base,
        .host_page = view.host_page,
        .sync = view.sync,
        .permissions = view.permissions,
        .valid = true,
    };
    chunk->page_offset = (size_t) (address & GUEST_MEMORY_PAGE_MASK);
    chunk->host = entry->host_page + chunk->page_offset;
    chunk->sync = entry->sync;
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
        struct guest_tlb_access_chunk chunk;
        enum guest_memory_fault_kind kind = resolve_host_pointer(
                tlb, current, access, &chunk);
        if (kind != GUEST_MEMORY_FAULT_NONE) {
            fault->address = current;
            fault->kind = kind;
            return false;
        }
        chunk.size = chunk_size;
        chunks[*chunk_count] = chunk;
        (*chunk_count)++;
        current += (guest_addr_t) chunk_size;
        remaining -= chunk_size;
    }
    return true;
}

static void collect_syncs(const struct guest_tlb_access_chunk chunks[2],
        unsigned chunk_count, struct guest_tlb_sync_set *set) {
    // address space 锁始终在外层；同步域按稳定身份加锁，避免反向别名死锁。
    *set = (struct guest_tlb_sync_set) {0};
    for (unsigned chunk = 0; chunk < chunk_count; chunk++) {
        const struct guest_page_sync *sync = chunks[chunk].sync;
        if (sync == NULL)
            continue;
        qword_t identity = guest_page_sync_identity(sync);
        unsigned index = 0;
        while (index < set->count) {
            if (set->items[index] == sync)
                break;
            qword_t existing = guest_page_sync_identity(set->items[index]);
            if (existing == identity)
                abort();
            if (existing > identity)
                break;
            index++;
        }
        if (index < set->count && set->items[index] == sync)
            continue;
        assert(set->count < array_size(set->items));
        for (unsigned move = set->count; move > index; move--)
            set->items[move] = set->items[move - 1];
        set->items[index] = sync;
        set->count++;
    }
}

static void lock_syncs(const struct guest_tlb_sync_set *set, bool write) {
    for (unsigned index = 0; index < set->count; index++) {
        if (write)
            guest_page_sync_write_lock(set->items[index]);
        else
            guest_page_sync_read_lock(set->items[index]);
    }
}

static void unlock_syncs(const struct guest_tlb_sync_set *set, bool write) {
    for (unsigned index = set->count; index != 0; index--) {
        if (write)
            guest_page_sync_write_unlock(set->items[index - 1]);
        else
            guest_page_sync_read_unlock(set->items[index - 1]);
    }
}

static void record_sync_writes(
        const struct guest_tlb_access_chunk chunks[2],
        unsigned chunk_count) {
    for (unsigned index = 0; index < chunk_count; index++) {
        if (chunks[index].sync != NULL) {
            guest_page_sync_written(chunks[index].sync,
                    chunks[index].page_offset, chunks[index].size);
        }
    }
}

bool guest_tlb_read(struct guest_tlb *tlb, guest_addr_t address,
        void *destination, size_t size, enum guest_memory_access access,
        struct guest_memory_fault *fault) {
    assert(access == GUEST_MEMORY_READ || access == GUEST_MEMORY_EXECUTE);
    bool locked = guest_address_space_read_lock(tlb->address_space);
    struct guest_tlb_access_chunk chunks[2];
    unsigned chunk_count;
    if (!prepare_access(tlb, address, size,
            access, chunks, &chunk_count, fault)) {
        guest_address_space_read_unlock(tlb->address_space, locked);
        return false;
    }
    struct guest_tlb_sync_set syncs;
    collect_syncs(chunks, chunk_count, &syncs);
    lock_syncs(&syncs, false);

    byte_t *output = destination;
    for (unsigned i = 0; i < chunk_count; i++) {
        memcpy(output, chunks[i].host, chunks[i].size);
        output += chunks[i].size;
    }
    unlock_syncs(&syncs, false);
    guest_address_space_read_unlock(tlb->address_space, locked);
    return true;
}

static bool exclusive_access_fits_granule(
        guest_addr_t address, size_t size) {
    assert(size != 0);
    return size <= GUEST_MEMORY_EXCLUSIVE_GRANULE_SIZE -
            (size_t) (address &
                    (GUEST_MEMORY_EXCLUSIVE_GRANULE_SIZE - 1));
}

bool guest_tlb_load_exclusive(struct guest_tlb *tlb,
        guest_addr_t address, void *destination, size_t size,
        struct guest_tlb_exclusive_token *token,
        struct guest_memory_fault *fault) {
    assert(token != NULL);
    assert(size <= GUEST_TLB_MAX_ACCESS_SIZE);
    assert(exclusive_access_fits_granule(address, size));
    *token = (struct guest_tlb_exclusive_token) {0};
    bool locked = guest_address_space_write_lock(tlb->address_space);
    struct guest_tlb_access_chunk chunks[2];
    unsigned chunk_count;
    if (!prepare_access(tlb, address, size, GUEST_MEMORY_READ,
            chunks, &chunk_count, fault)) {
        guest_address_space_write_unlock(tlb->address_space, locked);
        return false;
    }
    struct guest_tlb_sync_set syncs;
    collect_syncs(chunks, chunk_count, &syncs);
    lock_syncs(&syncs, true);

    byte_t *output = destination;
    for (unsigned i = 0; i < chunk_count; i++) {
        memcpy(output, chunks[i].host, chunks[i].size);
        output += chunks[i].size;
    }
    token->address_space = tlb->address_space;
    token->mapping_generation = tlb->address_space->generation;
    assert(chunk_count == 1);
    if (chunks[0].sync == NULL) {
        token->write_generation = guest_address_space_track_exclusive(
                tlb->address_space, address);
    } else {
        token->sync_identity = guest_page_sync_identity(chunks[0].sync);
        token->write_generation = guest_page_sync_track_exclusive(
                chunks[0].sync, chunks[0].page_offset);
    }
    unlock_syncs(&syncs, true);
    guest_address_space_write_unlock(tlb->address_space, locked);
    return true;
}

bool guest_tlb_write(struct guest_tlb *tlb, guest_addr_t address,
        const void *source, size_t size, struct guest_memory_fault *fault) {
    bool locked = guest_address_space_write_lock(tlb->address_space);
    struct guest_tlb_access_chunk chunks[2];
    unsigned chunk_count;
    if (!prepare_access(tlb, address, size, GUEST_MEMORY_WRITE,
            chunks, &chunk_count, fault)) {
        guest_address_space_write_unlock(tlb->address_space, locked);
        return false;
    }
    struct guest_tlb_sync_set syncs;
    collect_syncs(chunks, chunk_count, &syncs);
    lock_syncs(&syncs, true);

    const byte_t *input = source;
    for (unsigned i = 0; i < chunk_count; i++) {
        memcpy(chunks[i].host, input, chunks[i].size);
        input += chunks[i].size;
    }
    if (size != 0) {
        record_sync_writes(chunks, chunk_count);
        guest_address_space_written(tlb->address_space, address, size);
    }
    unlock_syncs(&syncs, true);
    guest_address_space_write_unlock(tlb->address_space, locked);
    return true;
}

enum guest_tlb_store_exclusive_result guest_tlb_store_exclusive(
        struct guest_tlb *tlb, guest_addr_t address,
        const void *expected, const void *replacement, size_t size,
        struct guest_tlb_exclusive_token token,
        struct guest_memory_fault *fault) {
    assert(size != 0 && size <= GUEST_TLB_MAX_ACCESS_SIZE);
    assert(exclusive_access_fits_granule(address, size));
    bool locked = guest_address_space_write_lock(tlb->address_space);
    if (tlb->address_space != token.address_space ||
            tlb->address_space->generation != token.mapping_generation) {
        guest_address_space_write_unlock(tlb->address_space, locked);
        return GUEST_TLB_EXCLUSIVE_FAILED;
    }

    struct guest_tlb_access_chunk chunks[2];
    unsigned chunk_count;
    if (!prepare_access(tlb, address, size, GUEST_MEMORY_WRITE,
            chunks, &chunk_count, fault)) {
        guest_address_space_write_unlock(tlb->address_space, locked);
        return GUEST_TLB_EXCLUSIVE_FAULT;
    }
    assert(chunk_count == 1);
    qword_t current_sync_identity = chunks[0].sync == NULL ? 0 :
            guest_page_sync_identity(chunks[0].sync);
    if (current_sync_identity != token.sync_identity) {
        guest_address_space_write_unlock(tlb->address_space, locked);
        return GUEST_TLB_EXCLUSIVE_FAILED;
    }
    struct guest_tlb_sync_set syncs;
    collect_syncs(chunks, chunk_count, &syncs);
    lock_syncs(&syncs, true);
    bool reservation_matches = chunks[0].sync == NULL ?
            guest_address_space_exclusive_matches(tlb->address_space,
                    address, token.write_generation) :
            guest_page_sync_exclusive_matches(chunks[0].sync,
                    chunks[0].page_offset, token.write_generation);
    if (!reservation_matches) {
        unlock_syncs(&syncs, true);
        guest_address_space_write_unlock(tlb->address_space, locked);
        return GUEST_TLB_EXCLUSIVE_FAILED;
    }

    const byte_t *expected_bytes = expected;
    for (unsigned i = 0; i < chunk_count; i++) {
        if (memcmp(chunks[i].host, expected_bytes, chunks[i].size) != 0) {
            unlock_syncs(&syncs, true);
            guest_address_space_write_unlock(tlb->address_space, locked);
            return GUEST_TLB_EXCLUSIVE_FAILED;
        }
        expected_bytes += chunks[i].size;
    }

    const byte_t *replacement_bytes = replacement;
    for (unsigned i = 0; i < chunk_count; i++) {
        memcpy(chunks[i].host, replacement_bytes, chunks[i].size);
        replacement_bytes += chunks[i].size;
    }
    record_sync_writes(chunks, chunk_count);
    guest_address_space_written(tlb->address_space, address, size);
    unlock_syncs(&syncs, true);
    guest_address_space_write_unlock(tlb->address_space, locked);
    return GUEST_TLB_EXCLUSIVE_STORED;
}

enum guest_tlb_compare_exchange_result guest_tlb_compare_exchange(
        struct guest_tlb *tlb, guest_addr_t address,
        const void *expected, const void *replacement, void *observed,
        size_t size, struct guest_memory_fault *fault) {
    assert((size == 4 || size == 8 || size == 16) &&
            size <= GUEST_TLB_MAX_ACCESS_SIZE);
    byte_t expected_bytes[GUEST_TLB_MAX_ACCESS_SIZE];
    byte_t replacement_bytes[GUEST_TLB_MAX_ACCESS_SIZE];
    // CAS 会把 observed 写回期望寄存器，先保留输入以允许两者共用缓冲区。
    memcpy(expected_bytes, expected, size);
    memcpy(replacement_bytes, replacement, size);
    bool locked = guest_address_space_write_lock(tlb->address_space);
    struct guest_tlb_access_chunk chunks[2];
    unsigned chunk_count;
    if (!prepare_access(tlb, address, size, GUEST_MEMORY_WRITE,
            chunks, &chunk_count, fault)) {
        guest_address_space_write_unlock(tlb->address_space, locked);
        return GUEST_TLB_COMPARE_EXCHANGE_FAULT;
    }
    struct guest_tlb_sync_set syncs;
    collect_syncs(chunks, chunk_count, &syncs);
    lock_syncs(&syncs, true);

    byte_t *observed_bytes = observed;
    for (unsigned i = 0; i < chunk_count; i++) {
        memcpy(observed_bytes, chunks[i].host, chunks[i].size);
        observed_bytes += chunks[i].size;
    }
    if (memcmp(observed, expected_bytes, size) != 0) {
        unlock_syncs(&syncs, true);
        guest_address_space_write_unlock(tlb->address_space, locked);
        return GUEST_TLB_COMPARE_EXCHANGE_MISMATCH;
    }

    const byte_t *replacement_input = replacement_bytes;
    for (unsigned i = 0; i < chunk_count; i++) {
        memcpy(chunks[i].host, replacement_input, chunks[i].size);
        replacement_input += chunks[i].size;
    }
    record_sync_writes(chunks, chunk_count);
    guest_address_space_written(tlb->address_space, address, size);
    unlock_syncs(&syncs, true);
    guest_address_space_write_unlock(tlb->address_space, locked);
    return GUEST_TLB_COMPARE_EXCHANGE_EXCHANGED;
}
