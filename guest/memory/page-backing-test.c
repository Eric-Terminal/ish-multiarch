#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <string.h>

#include "guest/memory/page-backing.h"

#define RETAIN_THREAD_COUNT 4
#define RETAIN_ITERATIONS 20000
#define CLONE_SNAPSHOT_ITERATIONS 256

struct clone_writer_context {
    struct guest_page_backing *source;
    atomic_bool stop;
    atomic_uint writes;
};

static void lock_sync_write(const struct guest_page_sync *sync) {
    sync->ops->write_lock(sync->opaque);
}

static void unlock_sync_write(const struct guest_page_sync *sync) {
    sync->ops->write_unlock(sync->opaque);
}

static void *retain_repeatedly(void *opaque) {
    struct guest_page_backing *backing = opaque;
    for (unsigned iteration = 0; iteration < RETAIN_ITERATIONS; iteration++) {
        guest_page_backing_retain(backing);
        guest_page_backing_release(backing);
    }
    return NULL;
}

static void *write_clone_source(void *opaque) {
    struct clone_writer_context *context = opaque;
    const struct guest_page_sync *sync =
            guest_page_backing_sync(context->source);
    byte_t *bytes = guest_page_backing_bytes(context->source);
    byte_t pattern = UINT8_C(0x55);
    while (!atomic_load_explicit(&context->stop, memory_order_acquire)) {
        lock_sync_write(sync);
        memset(bytes, pattern, GUEST_MEMORY_PAGE_SIZE / 2);
        sched_yield();
        memset(bytes + GUEST_MEMORY_PAGE_SIZE / 2, pattern,
                GUEST_MEMORY_PAGE_SIZE / 2);
        sync->ops->written(sync->opaque, 0, GUEST_MEMORY_PAGE_SIZE);
        unlock_sync_write(sync);
        atomic_fetch_add_explicit(&context->writes, 1,
                memory_order_release);
        pattern ^= UINT8_C(0xff);
        sched_yield();
    }
    return NULL;
}

static void test_zeroed_clone_and_lifecycle(void) {
    assert(guest_page_backing_test_live_count() == 0);
    guest_page_backing_test_fail_allocation_at(0);
    assert(guest_page_backing_create() == NULL);
    assert(guest_page_backing_test_allocation_count() == 1);
    assert(guest_page_backing_test_live_count() == 0);

    guest_page_backing_test_fail_allocation_at(SIZE_MAX);
    struct guest_page_backing *source = guest_page_backing_create();
    assert(source != NULL);
    byte_t *source_bytes = guest_page_backing_bytes(source);
    for (size_t index = 0; index < GUEST_MEMORY_PAGE_SIZE; index++)
        assert(source_bytes[index] == 0);
    for (size_t index = 0; index < GUEST_MEMORY_PAGE_SIZE; index++)
        source_bytes[index] = (byte_t) (index * 29 + 7);

    guest_page_backing_test_fail_allocation_at(0);
    assert(guest_page_backing_clone(source) == NULL);
    assert(guest_page_backing_test_reference_count(source) == 1);
    assert(guest_page_backing_test_live_count() == 1);

    guest_page_backing_test_fail_allocation_at(SIZE_MAX);
    struct guest_page_backing *copy = guest_page_backing_clone(source);
    assert(copy != NULL && copy != source);
    byte_t *copy_bytes = guest_page_backing_bytes(copy);
    assert(copy_bytes != source_bytes);
    assert(memcmp(copy_bytes, source_bytes, GUEST_MEMORY_PAGE_SIZE) == 0);
    copy_bytes[0] ^= UINT8_C(0xff);
    assert(copy_bytes[0] != source_bytes[0]);
    assert(guest_page_backing_test_live_count() == 2);

    guest_page_backing_retain(source);
    assert(guest_page_backing_test_reference_count(source) == 2);
    guest_page_backing_release(source);
    assert(guest_page_backing_test_reference_count(source) == 1);
    guest_page_backing_release(source);
    assert(guest_page_backing_test_live_count() == 1);
    guest_page_backing_release(copy);
    assert(guest_page_backing_test_live_count() == 0);
}

static void test_concurrent_reference_updates(void) {
    guest_page_backing_test_fail_allocation_at(SIZE_MAX);
    struct guest_page_backing *backing = guest_page_backing_create();
    assert(backing != NULL);
    pthread_t threads[RETAIN_THREAD_COUNT];
    for (unsigned index = 0; index < RETAIN_THREAD_COUNT; index++) {
        assert(pthread_create(&threads[index], NULL,
                retain_repeatedly, backing) == 0);
    }
    for (unsigned index = 0; index < RETAIN_THREAD_COUNT; index++)
        assert(pthread_join(threads[index], NULL) == 0);
    assert(guest_page_backing_test_reference_count(backing) == 1);
    assert(guest_page_backing_test_live_count() == 1);
    guest_page_backing_release(backing);
    assert(guest_page_backing_test_live_count() == 0);
}

static void test_clone_consistent_during_shared_writes(void) {
    guest_page_backing_test_fail_allocation_at(SIZE_MAX);
    struct guest_page_backing *source = guest_page_backing_create();
    assert(source != NULL);
    struct clone_writer_context context = {
        .source = source,
    };
    atomic_init(&context.stop, false);
    atomic_init(&context.writes, 0);
    pthread_t writer;
    assert(pthread_create(&writer, NULL, write_clone_source, &context) == 0);
    while (atomic_load_explicit(&context.writes, memory_order_acquire) == 0)
        sched_yield();

    for (unsigned iteration = 0;
            iteration < CLONE_SNAPSHOT_ITERATIONS; iteration++) {
        struct guest_page_backing *copy = guest_page_backing_clone(source);
        assert(copy != NULL);
        byte_t *bytes = guest_page_backing_bytes(copy);
        for (size_t index = 1; index < GUEST_MEMORY_PAGE_SIZE; index++)
            assert(bytes[index] == bytes[0]);
        assert(guest_page_backing_sync(copy)->identity !=
                guest_page_backing_sync(source)->identity);
        guest_page_backing_release(copy);
        sched_yield();
    }

    atomic_store_explicit(&context.stop, true, memory_order_release);
    assert(pthread_join(writer, NULL) == 0);
    guest_page_backing_release(source);
    assert(guest_page_backing_test_live_count() == 0);
}

static void test_sync_identity_lifecycle(void) {
    guest_page_backing_test_fail_allocation_at(SIZE_MAX);
    struct guest_page_backing *first = guest_page_backing_create();
    struct guest_page_backing *second = guest_page_backing_create();
    assert(first != NULL && second != NULL);
    const struct guest_page_sync *first_sync =
            guest_page_backing_sync(first);
    const struct guest_page_sync *second_sync =
            guest_page_backing_sync(second);
    assert(first_sync == guest_page_backing_sync(first));
    assert(first_sync->identity != 0);
    assert(second_sync->identity != 0);
    assert(first_sync->identity != second_sync->identity);
    qword_t released_identity = first_sync->identity;
    guest_page_backing_release(first);

    struct guest_page_backing *third = guest_page_backing_create();
    assert(third != NULL);
    const struct guest_page_sync *third_sync =
            guest_page_backing_sync(third);
    assert(third_sync->identity != 0);
    assert(third_sync->identity != released_identity);
    assert(third_sync->identity != second_sync->identity);
    guest_page_backing_release(second);
    guest_page_backing_release(third);
    assert(guest_page_backing_test_live_count() == 0);
}

static void test_sync_exclusive_granules(void) {
    guest_page_backing_test_fail_allocation_at(SIZE_MAX);
    struct guest_page_backing *backing = guest_page_backing_create();
    assert(backing != NULL);
    const struct guest_page_sync *sync = guest_page_backing_sync(backing);

    lock_sync_write(sync);
    qword_t first = sync->ops->track_exclusive(sync->opaque, 3);
    qword_t adjacent = sync->ops->track_exclusive(sync->opaque,
            GUEST_MEMORY_EXCLUSIVE_GRANULE_SIZE);
    assert(first != 0 && adjacent != 0 && first != adjacent);
    assert(sync->ops->exclusive_matches(sync->opaque, 15, first));
    assert(sync->ops->exclusive_matches(sync->opaque, 17, adjacent));

    sync->ops->written(sync->opaque, 5, 2);
    assert(!sync->ops->exclusive_matches(sync->opaque, 0, first));
    assert(sync->ops->exclusive_matches(sync->opaque, 16, adjacent));

    qword_t renewed = sync->ops->track_exclusive(sync->opaque, 0);
    sync->ops->written(sync->opaque, 15, 2);
    assert(!sync->ops->exclusive_matches(sync->opaque, 0, renewed));
    assert(!sync->ops->exclusive_matches(sync->opaque, 16, adjacent));

    qword_t eviction_candidate = sync->ops->track_exclusive(
            sync->opaque, 0);
    qword_t latest = 0;
    for (size_t granule = 2; granule <= 8; granule++) {
        latest = sync->ops->track_exclusive(sync->opaque,
                granule * GUEST_MEMORY_EXCLUSIVE_GRANULE_SIZE);
    }
    assert(!sync->ops->exclusive_matches(
            sync->opaque, 0, eviction_candidate));
    assert(sync->ops->exclusive_matches(sync->opaque,
            8 * GUEST_MEMORY_EXCLUSIVE_GRANULE_SIZE, latest));
    unlock_sync_write(sync);

    guest_page_backing_release(backing);
    assert(guest_page_backing_test_live_count() == 0);
}

static void test_file_dirty_generation(void) {
    guest_page_backing_test_fail_allocation_at(SIZE_MAX);
    struct guest_page_backing *backing = guest_page_backing_create();
    assert(backing != NULL);
    byte_t snapshot[GUEST_MEMORY_PAGE_SIZE];
    qword_t generation = UINT64_MAX;
    assert(!guest_page_backing_copy_dirty(
            backing, snapshot, &generation) && generation == 0);

    guest_page_backing_track_file_writes(backing);
    assert(!guest_page_backing_copy_dirty(
            backing, snapshot, &generation) && generation == 0);

    const struct guest_page_sync *sync =
            guest_page_backing_sync(backing);
    lock_sync_write(sync);
    guest_page_backing_bytes(backing)[17] = UINT8_C(0xa5);
    sync->ops->written(sync->opaque, 17, 1);
    unlock_sync_write(sync);

    qword_t first_generation;
    assert(guest_page_backing_copy_dirty(
            backing, snapshot, &first_generation));
    assert(first_generation != 0 && snapshot[17] == UINT8_C(0xa5));

    lock_sync_write(sync);
    guest_page_backing_bytes(backing)[17] = UINT8_C(0x5a);
    sync->ops->written(sync->opaque, 17, 1);
    unlock_sync_write(sync);
    guest_page_backing_finish_writeback(backing, first_generation);

    qword_t second_generation;
    assert(guest_page_backing_copy_dirty(
            backing, snapshot, &second_generation));
    assert(second_generation != first_generation &&
            snapshot[17] == UINT8_C(0x5a));
    guest_page_backing_finish_writeback(backing, second_generation);
    assert(!guest_page_backing_copy_dirty(
            backing, snapshot, &generation) && generation == 0);

    lock_sync_write(sync);
    guest_page_backing_bytes(backing)[23] = UINT8_C(0x7c);
    sync->ops->written(sync->opaque, 23, 1);
    unlock_sync_write(sync);
    struct guest_page_backing *copy = guest_page_backing_clone(backing);
    assert(copy != NULL);
    assert(guest_page_backing_bytes(copy)[23] == UINT8_C(0x7c));
    assert(!guest_page_backing_copy_dirty(
            copy, snapshot, &generation) && generation == 0);

    guest_page_backing_release(copy);
    guest_page_backing_release(backing);
    assert(guest_page_backing_test_live_count() == 0);
}

int main(void) {
    test_zeroed_clone_and_lifecycle();
    test_concurrent_reference_updates();
    test_clone_consistent_during_shared_writes();
    test_sync_identity_lifecycle();
    test_sync_exclusive_granules();
    test_file_dirty_generation();
    guest_page_backing_test_fail_allocation_at(SIZE_MAX);
    return 0;
}
