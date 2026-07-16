#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "guest/memory/address-space.h"

_Static_assert((GUEST_MEMORY_EXCLUSIVE_GRANULE_SIZE &
        (GUEST_MEMORY_EXCLUSIVE_GRANULE_SIZE - 1)) == 0,
        "独占保留粒度必须为二次幂");
_Static_assert(GUEST_MEMORY_EXCLUSIVE_GRANULE_SIZE >= 16,
        "独占保留粒度必须容纳成对独占访问");
_Static_assert((GUEST_MEMORY_EXCLUSIVE_BUCKET_COUNT &
        (GUEST_MEMORY_EXCLUSIVE_BUCKET_COUNT - 1)) == 0,
        "独占记录桶数必须为二次幂");
_Static_assert(GUEST_MEMORY_EXCLUSIVE_WAYS > 0,
        "独占记录每组至少需要一路");

struct guest_file_source {
    atomic_uint refcount;
    qword_t identity;
    void *opaque;
    void (*release_opaque)(void *opaque);
};

struct guest_file_source *guest_file_source_create(qword_t identity,
        void *opaque, void (*release_opaque)(void *opaque)) {
    assert(identity != 0);
    assert((opaque == NULL) == (release_opaque == NULL));
    struct guest_file_source *source = malloc(sizeof(*source));
    if (source == NULL)
        return NULL;
    *source = (struct guest_file_source) {
        .refcount = ATOMIC_VAR_INIT(1),
        .identity = identity,
        .opaque = opaque,
        .release_opaque = release_opaque,
    };
    return source;
}

struct guest_file_source *guest_file_source_retain(
        struct guest_file_source *source) {
    if (source == NULL)
        return NULL;
    unsigned previous = atomic_fetch_add_explicit(
            &source->refcount, 1, memory_order_relaxed);
    assert(previous != 0 && previous != UINT_MAX);
    return source;
}

void guest_file_source_release(struct guest_file_source *source) {
    if (source == NULL)
        return;
    unsigned previous = atomic_fetch_sub_explicit(
            &source->refcount, 1, memory_order_acq_rel);
    assert(previous != 0);
    if (previous != 1)
        return;
    if (source->release_opaque != NULL)
        source->release_opaque(source->opaque);
    free(source);
}

qword_t guest_file_source_identity(const struct guest_file_source *source) {
    assert(source != NULL && source->identity != 0);
    return source->identity;
}

void guest_address_space_init(struct guest_address_space *space,
        const struct guest_address_space_ops *ops, void *opaque,
        byte_t address_bits) {
    assert(ops != NULL && ops->resolve_page != NULL);
    assert((ops->read_lock == NULL) == (ops->read_unlock == NULL));
    assert((ops->write_lock == NULL) == (ops->write_unlock == NULL));
    assert(address_bits > GUEST_MEMORY_PAGE_BITS);
    assert(address_bits <= sizeof(guest_addr_t) * 8);
    *space = (struct guest_address_space) {
        .ops = ops,
        .opaque = opaque,
        .generation = 1,
        .address_bits = address_bits,
    };
}

void guest_address_space_changed(struct guest_address_space *space) {
    space->generation++;
    // 映射世代已经使旧令牌失效，可以一并重置保留记录。
    memset(space->exclusive_records, 0, sizeof(space->exclusive_records));
    memset(space->exclusive_next_way, 0, sizeof(space->exclusive_next_way));
}

static guest_addr_t exclusive_granule(guest_addr_t address) {
    return address & ~(GUEST_MEMORY_EXCLUSIVE_GRANULE_SIZE - 1);
}

static unsigned exclusive_bucket(guest_addr_t granule_base) {
    qword_t granule = granule_base >> GUEST_MEMORY_EXCLUSIVE_GRANULE_BITS;
    granule ^= granule >> 17;
    granule *= UINT64_C(0x9e3779b97f4a7c15);
    granule ^= granule >> 32;
    return (unsigned) (granule &
            (GUEST_MEMORY_EXCLUSIVE_BUCKET_COUNT - 1));
}

static struct guest_exclusive_record *find_exclusive_record(
        struct guest_address_space *space, guest_addr_t granule_base) {
    struct guest_exclusive_record *records =
            space->exclusive_records[exclusive_bucket(granule_base)];
    for (unsigned way = 0; way < GUEST_MEMORY_EXCLUSIVE_WAYS; way++) {
        if (records[way].generation != 0 &&
                records[way].granule_base == granule_base)
            return &records[way];
    }
    return NULL;
}

qword_t guest_address_space_track_exclusive(
        struct guest_address_space *space, guest_addr_t address) {
    guest_addr_t granule_base = exclusive_granule(address);
    struct guest_exclusive_record *record =
            find_exclusive_record(space, granule_base);
    if (record == NULL) {
        unsigned bucket = exclusive_bucket(granule_base);
        for (unsigned way = 0; way < GUEST_MEMORY_EXCLUSIVE_WAYS; way++) {
            if (space->exclusive_records[bucket][way].generation == 0) {
                record = &space->exclusive_records[bucket][way];
                break;
            }
        }
        if (record == NULL) {
            unsigned way = space->exclusive_next_way[bucket]++ %
                    GUEST_MEMORY_EXCLUSIVE_WAYS;
            record = &space->exclusive_records[bucket][way];
        }
        *record = (struct guest_exclusive_record) {
            .granule_base = granule_base,
            .generation = ++space->exclusive_sequence,
        };
    }
    return record->generation;
}

bool guest_address_space_exclusive_matches(
        const struct guest_address_space *space, guest_addr_t address,
        qword_t generation) {
    guest_addr_t granule_base = exclusive_granule(address);
    const struct guest_exclusive_record *records =
            space->exclusive_records[exclusive_bucket(granule_base)];
    for (unsigned way = 0; way < GUEST_MEMORY_EXCLUSIVE_WAYS; way++) {
        if (generation != 0 && records[way].generation == generation &&
                records[way].granule_base == granule_base)
            return true;
    }
    return false;
}

void guest_address_space_written(struct guest_address_space *space,
        guest_addr_t address, size_t size) {
    assert(size != 0);
    assert(guest_address_space_contains(space, address, size));
    guest_addr_t first = exclusive_granule(address);
    guest_addr_t last = exclusive_granule(
            address + (guest_addr_t) size - 1);
    for (guest_addr_t granule = first;;
            granule += GUEST_MEMORY_EXCLUSIVE_GRANULE_SIZE) {
        struct guest_exclusive_record *record =
                find_exclusive_record(space, granule);
        if (record != NULL)
            record->generation = ++space->exclusive_sequence;
        if (granule == last)
            break;
    }
    if (space->ops->written != NULL)
        space->ops->written(space->opaque, address, size);
}

bool guest_address_space_read_lock(struct guest_address_space *space) {
    return space->ops->read_lock != NULL &&
            space->ops->read_lock(space->opaque);
}

void guest_address_space_read_unlock(
        struct guest_address_space *space, bool locked) {
    if (space->ops->read_unlock != NULL)
        space->ops->read_unlock(space->opaque, locked);
}

bool guest_address_space_write_lock(struct guest_address_space *space) {
    return space->ops->write_lock != NULL &&
            space->ops->write_lock(space->opaque);
}

void guest_address_space_write_unlock(
        struct guest_address_space *space, bool locked) {
    if (space->ops->write_unlock != NULL)
        space->ops->write_unlock(space->opaque, locked);
}

void guest_address_space_write_prepared(
        struct guest_address_space *space,
        guest_addr_t address, size_t size) {
    assert(size != 0);
    assert(guest_address_space_contains(space, address, size));
    if (space->ops->write_prepared != NULL)
        space->ops->write_prepared(space->opaque, address, size);
}

bool guest_address_space_contains(const struct guest_address_space *space,
        guest_addr_t address, size_t size) {
    qword_t max_address = space->address_bits == 64 ?
            UINT64_MAX : (UINT64_C(1) << space->address_bits) - 1;
    qword_t first = address;

    if (first > max_address)
        return false;
    if (size == 0)
        return true;
    return (qword_t) (size - 1) <= max_address - first;
}

enum guest_memory_fault_kind guest_address_space_resolve_page(
        struct guest_address_space *space, guest_addr_t page_base,
        enum guest_memory_access access, struct guest_page_view *view) {
    assert(access == GUEST_MEMORY_READ || access == GUEST_MEMORY_WRITE ||
            access == GUEST_MEMORY_EXECUTE);
    if ((page_base & GUEST_MEMORY_PAGE_MASK) != 0)
        return GUEST_MEMORY_FAULT_ALIGNMENT;
    if (!guest_address_space_contains(space, page_base, GUEST_MEMORY_PAGE_SIZE))
        return GUEST_MEMORY_FAULT_ADDRESS_SIZE;

    *view = (struct guest_page_view) {0};
    enum guest_memory_fault_kind fault = space->ops->resolve_page(
            space->opaque, page_base, access, view);
    if (fault != GUEST_MEMORY_FAULT_NONE)
        return fault;

    assert(view->host_page != NULL);
    if ((view->permissions & access) == 0)
        return GUEST_MEMORY_FAULT_PERMISSION;
    return GUEST_MEMORY_FAULT_NONE;
}

static void validate_page_sync(const struct guest_page_sync *sync) {
    assert(sync != NULL && sync->ops != NULL);
    assert(sync->identity != 0);
    assert(sync->ops->read_lock != NULL && sync->ops->read_unlock != NULL);
    assert(sync->ops->write_lock != NULL && sync->ops->write_unlock != NULL);
    assert(sync->ops->track_exclusive != NULL);
    assert(sync->ops->exclusive_matches != NULL);
    assert(sync->ops->written != NULL);
    use(sync);
}

qword_t guest_page_sync_identity(const struct guest_page_sync *sync) {
    validate_page_sync(sync);
    return sync->identity;
}

void guest_page_sync_read_lock(const struct guest_page_sync *sync) {
    validate_page_sync(sync);
    sync->ops->read_lock(sync->opaque);
}

void guest_page_sync_read_unlock(const struct guest_page_sync *sync) {
    validate_page_sync(sync);
    sync->ops->read_unlock(sync->opaque);
}

void guest_page_sync_write_lock(const struct guest_page_sync *sync) {
    validate_page_sync(sync);
    sync->ops->write_lock(sync->opaque);
}

void guest_page_sync_write_unlock(const struct guest_page_sync *sync) {
    validate_page_sync(sync);
    sync->ops->write_unlock(sync->opaque);
}

qword_t guest_page_sync_track_exclusive(
        const struct guest_page_sync *sync, size_t page_offset) {
    validate_page_sync(sync);
    assert(page_offset < GUEST_MEMORY_PAGE_SIZE);
    return sync->ops->track_exclusive(sync->opaque, page_offset);
}

bool guest_page_sync_exclusive_matches(
        const struct guest_page_sync *sync, size_t page_offset,
        qword_t generation) {
    validate_page_sync(sync);
    assert(page_offset < GUEST_MEMORY_PAGE_SIZE);
    return sync->ops->exclusive_matches(
            sync->opaque, page_offset, generation);
}

void guest_page_sync_written(const struct guest_page_sync *sync,
        size_t page_offset, size_t size) {
    validate_page_sync(sync);
    assert(size != 0 && size <= GUEST_MEMORY_PAGE_SIZE - page_offset);
    sync->ops->written(sync->opaque, page_offset, size);
}
