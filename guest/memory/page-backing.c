#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "guest/memory/page-backing.h"

#define GUEST_PAGE_BACKING_EXCLUSIVE_RECORD_COUNT 8

_Static_assert(GUEST_MEMORY_EXCLUSIVE_GRANULE_SIZE == 16,
        "页后备物理保留粒度必须是 16 字节");

struct guest_page_backing_exclusive_record {
    size_t granule_offset;
    qword_t generation;
};

struct guest_page_backing {
    atomic_uint references;
    pthread_rwlock_t lock;
    struct guest_page_sync sync;
    // 物理保留状态由同步域写锁保护；满表淘汰只会使旧令牌保守失效。
    qword_t exclusive_sequence;
    struct guest_page_backing_exclusive_record exclusive_records
            [GUEST_PAGE_BACKING_EXCLUSIVE_RECORD_COUNT];
    unsigned exclusive_next_record;
    // 数据内嵌于对象，引用存续期间地址不会变化，并保留宿主自然对齐。
    alignas(max_align_t) byte_t bytes[GUEST_MEMORY_PAGE_SIZE];
};

static pthread_mutex_t identity_lock = PTHREAD_MUTEX_INITIALIZER;
static qword_t last_identity;

static void check_pthread(int result) {
    if (result != 0)
        abort();
}

static struct guest_page_backing *sync_backing(void *opaque) {
    assert(opaque != NULL);
    return opaque;
}

static void page_sync_read_lock(void *opaque) {
    check_pthread(pthread_rwlock_rdlock(&sync_backing(opaque)->lock));
}

static void page_sync_read_unlock(void *opaque) {
    check_pthread(pthread_rwlock_unlock(&sync_backing(opaque)->lock));
}

static void page_sync_write_lock(void *opaque) {
    check_pthread(pthread_rwlock_wrlock(&sync_backing(opaque)->lock));
}

static void page_sync_write_unlock(void *opaque) {
    check_pthread(pthread_rwlock_unlock(&sync_backing(opaque)->lock));
}

static size_t exclusive_granule(size_t page_offset) {
    return page_offset & ~((size_t) GUEST_MEMORY_EXCLUSIVE_GRANULE_SIZE - 1);
}

static struct guest_page_backing_exclusive_record *find_exclusive_record(
        struct guest_page_backing *backing, size_t granule_offset) {
    for (unsigned index = 0;
            index < GUEST_PAGE_BACKING_EXCLUSIVE_RECORD_COUNT; index++) {
        struct guest_page_backing_exclusive_record *record =
                &backing->exclusive_records[index];
        if (record->generation != 0 &&
                record->granule_offset == granule_offset)
            return record;
    }
    return NULL;
}

static qword_t next_exclusive_generation(
        struct guest_page_backing *backing) {
    if (backing->exclusive_sequence == UINT64_MAX)
        abort();
    return ++backing->exclusive_sequence;
}

static qword_t page_sync_track_exclusive(void *opaque, size_t page_offset) {
    assert(page_offset < GUEST_MEMORY_PAGE_SIZE);
    struct guest_page_backing *backing = sync_backing(opaque);
    size_t granule_offset = exclusive_granule(page_offset);
    struct guest_page_backing_exclusive_record *record =
            find_exclusive_record(backing, granule_offset);
    if (record == NULL) {
        for (unsigned index = 0;
                index < GUEST_PAGE_BACKING_EXCLUSIVE_RECORD_COUNT; index++) {
            if (backing->exclusive_records[index].generation == 0) {
                record = &backing->exclusive_records[index];
                break;
            }
        }
        if (record == NULL) {
            record = &backing->exclusive_records[
                    backing->exclusive_next_record];
            backing->exclusive_next_record =
                    (backing->exclusive_next_record + 1) %
                    GUEST_PAGE_BACKING_EXCLUSIVE_RECORD_COUNT;
        }
        *record = (struct guest_page_backing_exclusive_record) {
            .granule_offset = granule_offset,
            .generation = next_exclusive_generation(backing),
        };
    }
    return record->generation;
}

static bool page_sync_exclusive_matches(void *opaque, size_t page_offset,
        qword_t generation) {
    assert(page_offset < GUEST_MEMORY_PAGE_SIZE);
    struct guest_page_backing_exclusive_record *record =
            find_exclusive_record(sync_backing(opaque),
                    exclusive_granule(page_offset));
    return generation != 0 && record != NULL &&
            record->generation == generation;
}

static void page_sync_written(void *opaque, size_t page_offset, size_t size) {
    assert(page_offset < GUEST_MEMORY_PAGE_SIZE);
    assert(size != 0 && size <= GUEST_MEMORY_PAGE_SIZE - page_offset);
    struct guest_page_backing *backing = sync_backing(opaque);
    size_t first_granule = exclusive_granule(page_offset);
    size_t last_granule = exclusive_granule(page_offset + size - 1);
    for (unsigned index = 0;
            index < GUEST_PAGE_BACKING_EXCLUSIVE_RECORD_COUNT; index++) {
        struct guest_page_backing_exclusive_record *record =
                &backing->exclusive_records[index];
        if (record->generation != 0 &&
                record->granule_offset >= first_granule &&
                record->granule_offset <= last_granule) {
            record->generation = next_exclusive_generation(backing);
        }
    }
}

static const struct guest_page_sync_ops page_sync_ops = {
    .read_lock = page_sync_read_lock,
    .read_unlock = page_sync_read_unlock,
    .write_lock = page_sync_write_lock,
    .write_unlock = page_sync_write_unlock,
    .track_exclusive = page_sync_track_exclusive,
    .exclusive_matches = page_sync_exclusive_matches,
    .written = page_sync_written,
};

static qword_t allocate_identity(void) {
    check_pthread(pthread_mutex_lock(&identity_lock));
    if (last_identity == UINT64_MAX)
        abort();
    qword_t identity = ++last_identity;
    check_pthread(pthread_mutex_unlock(&identity_lock));
    return identity;
}

#if defined(GUEST_PAGE_BACKING_TESTING)
static size_t allocation_fail_at = SIZE_MAX;
static size_t allocation_count;
static atomic_uint live_count;

void guest_page_backing_test_fail_allocation_at(size_t index) {
    allocation_fail_at = index;
    allocation_count = 0;
}

size_t guest_page_backing_test_allocation_count(void) {
    return allocation_count;
}

unsigned guest_page_backing_test_live_count(void) {
    return atomic_load_explicit(&live_count, memory_order_relaxed);
}

unsigned guest_page_backing_test_reference_count(
        const struct guest_page_backing *backing) {
    assert(backing != NULL);
    return atomic_load_explicit(&backing->references, memory_order_relaxed);
}

static bool allocation_fails(void) {
    return allocation_count++ == allocation_fail_at;
}
#endif

static struct guest_page_backing *allocate_backing(void) {
#if defined(GUEST_PAGE_BACKING_TESTING)
    if (allocation_fails())
        return NULL;
#endif
    struct guest_page_backing *backing = calloc(1, sizeof(*backing));
    if (backing == NULL)
        return NULL;
    if (pthread_rwlock_init(&backing->lock, NULL) != 0) {
        free(backing);
        return NULL;
    }
    atomic_init(&backing->references, 1);
    backing->sync = (struct guest_page_sync) {
        .ops = &page_sync_ops,
        .opaque = backing,
        .identity = allocate_identity(),
    };
#if defined(GUEST_PAGE_BACKING_TESTING)
    unsigned previous = atomic_fetch_add_explicit(
            &live_count, 1, memory_order_relaxed);
    assert(previous != UINT_MAX);
#endif
    return backing;
}

struct guest_page_backing *guest_page_backing_create(void) {
    return allocate_backing();
}

struct guest_page_backing *guest_page_backing_clone(
        const struct guest_page_backing *source) {
    assert(source != NULL);
    struct guest_page_backing *copy = allocate_backing();
    if (copy != NULL) {
        source->sync.ops->read_lock(source->sync.opaque);
        memcpy(copy->bytes, source->bytes, sizeof(copy->bytes));
        source->sync.ops->read_unlock(source->sync.opaque);
    }
    return copy;
}

void guest_page_backing_retain(struct guest_page_backing *backing) {
    assert(backing != NULL);
    unsigned previous = atomic_fetch_add_explicit(
            &backing->references, 1, memory_order_relaxed);
    assert(previous != 0 && previous != UINT_MAX);
}

void guest_page_backing_release(struct guest_page_backing *backing) {
    assert(backing != NULL);
    unsigned previous = atomic_fetch_sub_explicit(
            &backing->references, 1, memory_order_acq_rel);
    assert(previous != 0);
    if (previous != 1)
        return;
#if defined(GUEST_PAGE_BACKING_TESTING)
    unsigned live_previous = atomic_fetch_sub_explicit(
            &live_count, 1, memory_order_relaxed);
    assert(live_previous != 0);
#endif
    check_pthread(pthread_rwlock_destroy(&backing->lock));
    free(backing);
}

byte_t *guest_page_backing_bytes(struct guest_page_backing *backing) {
    assert(backing != NULL);
    return backing->bytes;
}

const struct guest_page_sync *guest_page_backing_sync(
        const struct guest_page_backing *backing) {
    assert(backing != NULL);
    return &backing->sync;
}
