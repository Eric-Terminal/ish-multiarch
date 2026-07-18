#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <string.h>

#include "guest/memory/page-backing.h"

#define RETAIN_THREAD_COUNT 4
#define RETAIN_ITERATIONS 20000
#define CLONE_SNAPSHOT_ITERATIONS 256
#define FILE_RESIZE_RACE_ITERATIONS 64
#define FILE_UNREGISTER_RACE_ITERATIONS 2000

struct clone_writer_context {
    struct guest_page_backing *source;
    atomic_bool stop;
    atomic_uint writes;
};

struct resize_clone_context {
    struct guest_file_page_domain *domain;
    struct guest_page_backing *source;
    struct guest_page_backing *copy;
    atomic_bool start;
};

struct unregister_resize_context {
    struct guest_file_page_domain *domain;
    struct guest_page_backing *source;
    atomic_bool start;
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

static void *clone_file_backing(void *opaque) {
    struct resize_clone_context *context = opaque;
    while (!atomic_load_explicit(&context->start, memory_order_acquire))
        sched_yield();
    context->copy = guest_page_backing_clone(context->source);
    return NULL;
}

static void *resize_file_domain(void *opaque) {
    struct resize_clone_context *context = opaque;
    while (!atomic_load_explicit(&context->start, memory_order_acquire))
        sched_yield();
    guest_file_page_domain_resize(
            context->domain, GUEST_MEMORY_PAGE_SIZE, 0);
    return NULL;
}

static void *clone_and_release_file_backings(void *opaque) {
    struct unregister_resize_context *context = opaque;
    while (!atomic_load_explicit(&context->start, memory_order_acquire))
        sched_yield();
    for (unsigned iteration = 0;
            iteration < FILE_UNREGISTER_RACE_ITERATIONS; iteration++) {
        struct guest_page_backing *copy =
                guest_page_backing_clone(context->source);
        assert(copy != NULL);
        guest_page_backing_release(copy);
    }
    return NULL;
}

static void *resize_partial_file_domain(void *opaque) {
    struct unregister_resize_context *context = opaque;
    while (!atomic_load_explicit(&context->start, memory_order_acquire))
        sched_yield();
    for (unsigned iteration = 0;
            iteration < FILE_UNREGISTER_RACE_ITERATIONS; iteration++) {
        qword_t old_size = (iteration & 1U) == 0 ? 127 : 257;
        qword_t new_size = (iteration & 1U) == 0 ? 257 : 127;
        guest_file_page_domain_resize(
                context->domain, old_size, new_size);
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
    struct guest_file_page_domain *domain =
            guest_file_page_domain_create();
    assert(backing != NULL && domain != NULL);
    byte_t snapshot[GUEST_MEMORY_PAGE_SIZE];
    qword_t generation = UINT64_MAX;
    assert(!guest_page_backing_copy_dirty(
            backing, snapshot, &generation) && generation == 0);

    guest_page_backing_track_file_writes(backing, domain, 0);
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
    guest_file_page_domain_release(domain);
    assert(guest_page_backing_test_live_count() == 0);
}

static void test_committed_file_write_merge(void) {
    guest_page_backing_test_fail_allocation_at(SIZE_MAX);
    struct guest_page_backing *backing = guest_page_backing_create();
    struct guest_file_page_domain *domain =
            guest_file_page_domain_create();
    assert(backing != NULL && domain != NULL);
    guest_page_backing_track_file_writes(backing, domain, 0);

    const struct guest_page_sync *sync =
            guest_page_backing_sync(backing);
    lock_sync_write(sync);
    qword_t reservation = sync->ops->track_exclusive(
            sync->opaque, 32);
    unlock_sync_write(sync);

    const byte_t clean_write[] = {UINT8_C(0x11), UINT8_C(0x22)};
    guest_page_backing_commit_file_write(
            backing, 32, clean_write, sizeof(clean_write));
    byte_t snapshot[GUEST_MEMORY_PAGE_SIZE];
    qword_t generation;
    assert(!guest_page_backing_copy_dirty(
            backing, snapshot, &generation));
    assert(guest_page_backing_bytes(backing)[32] == UINT8_C(0x11) &&
            guest_page_backing_bytes(backing)[33] == UINT8_C(0x22));
    lock_sync_write(sync);
    assert(!sync->ops->exclusive_matches(
            sync->opaque, 32, reservation));
    guest_page_backing_bytes(backing)[47] = UINT8_C(0x44);
    sync->ops->written(sync->opaque, 47, 1);
    unlock_sync_write(sync);

    const byte_t overlapping_write[] = {
        UINT8_C(0x55), UINT8_C(0x66), UINT8_C(0x77),
    };
    guest_page_backing_commit_file_write(
            backing, 33, overlapping_write, sizeof(overlapping_write));
    assert(guest_page_backing_copy_dirty(
            backing, snapshot, &generation));
    assert(snapshot[32] == UINT8_C(0x11) &&
            snapshot[33] == UINT8_C(0x55) &&
            snapshot[35] == UINT8_C(0x77) &&
            snapshot[47] == UINT8_C(0x44));

    byte_t complete_page[GUEST_MEMORY_PAGE_SIZE];
    memset(complete_page, UINT8_C(0x9a), sizeof(complete_page));
    guest_page_backing_commit_file_write(
            backing, 0, complete_page, sizeof(complete_page));
    assert(!guest_page_backing_copy_dirty(
            backing, snapshot, &generation));
    assert(guest_page_backing_bytes(backing)[0] == UINT8_C(0x9a) &&
            guest_page_backing_bytes(backing)
                    [GUEST_MEMORY_PAGE_SIZE - 1] == UINT8_C(0x9a));

    guest_page_backing_release(backing);
    guest_file_page_domain_release(domain);
    assert(guest_page_backing_test_live_count() == 0);
}

static void fill_backing(
        struct guest_page_backing *backing, byte_t value) {
    const struct guest_page_sync *sync =
            guest_page_backing_sync(backing);
    lock_sync_write(sync);
    memset(guest_page_backing_bytes(backing),
            value, GUEST_MEMORY_PAGE_SIZE);
    sync->ops->written(sync->opaque, 0, GUEST_MEMORY_PAGE_SIZE);
    unlock_sync_write(sync);
}

static void assert_byte_range(const struct guest_page_backing *backing,
        size_t first, size_t last, byte_t value) {
    const byte_t *bytes = guest_page_backing_bytes(
            (struct guest_page_backing *) backing);
    for (size_t index = first; index < last; index++)
        assert(bytes[index] == value);
}

static void test_file_resize_tail_and_invalidation(void) {
    guest_page_backing_test_fail_allocation_at(SIZE_MAX);
    struct guest_file_page_domain *domain =
            guest_file_page_domain_create();
    struct guest_page_backing *tail = guest_page_backing_create();
    struct guest_page_backing *full = guest_page_backing_create();
    assert(domain != NULL && tail != NULL && full != NULL);
    guest_page_backing_track_file_writes(tail, domain, 0);
    guest_page_backing_track_file_writes(
            full, domain, GUEST_MEMORY_PAGE_SIZE);
    fill_backing(tail, UINT8_C(0xa5));
    fill_backing(full, UINT8_C(0x5a));
    struct guest_page_backing *tail_cow =
            guest_page_backing_clone(tail);
    struct guest_page_backing *full_cow =
            guest_page_backing_clone(full);
    assert(tail_cow != NULL && full_cow != NULL);

    const struct guest_page_sync *tail_sync =
            guest_page_backing_sync(tail);
    lock_sync_write(tail_sync);
    qword_t tail_reservation = tail_sync->ops->track_exclusive(
            tail_sync->opaque, 256);
    unlock_sync_write(tail_sync);

    guest_file_page_domain_resize(domain,
            2 * GUEST_MEMORY_PAGE_SIZE, 123);
    assert(guest_page_backing_file_accessible(tail));
    assert(guest_page_backing_file_accessible(tail_cow));
    assert(!guest_page_backing_file_accessible(full));
    assert(!guest_page_backing_file_accessible(full_cow));
    assert_byte_range(tail, 0, 123, UINT8_C(0xa5));
    assert_byte_range(tail, 123, GUEST_MEMORY_PAGE_SIZE, 0);
    assert_byte_range(tail_cow, 0,
            GUEST_MEMORY_PAGE_SIZE, UINT8_C(0xa5));
    assert_byte_range(full, 0, GUEST_MEMORY_PAGE_SIZE, 0);
    assert_byte_range(full_cow, 0, GUEST_MEMORY_PAGE_SIZE, 0);

    byte_t snapshot[GUEST_MEMORY_PAGE_SIZE];
    qword_t generation;
    assert(guest_page_backing_copy_dirty(
            tail, snapshot, &generation));
    assert(!guest_page_backing_copy_dirty(
            full, snapshot, &generation));
    lock_sync_write(tail_sync);
    assert(!tail_sync->ops->exclusive_matches(
            tail_sync->opaque, 256, tail_reservation));
    unlock_sync_write(tail_sync);

    guest_file_page_domain_resize(domain, 123,
            GUEST_MEMORY_PAGE_SIZE + 17);
    assert(!guest_page_backing_file_accessible(full));
    assert(!guest_page_backing_file_accessible(full_cow));

    guest_page_backing_release(full_cow);
    guest_page_backing_release(tail_cow);
    guest_page_backing_release(full);
    guest_page_backing_release(tail);
    guest_file_page_domain_release(domain);
    assert(guest_page_backing_test_live_count() == 0);
}

static void test_file_resize_grow_and_equal_cleanup(void) {
    guest_page_backing_test_fail_allocation_at(SIZE_MAX);
    struct guest_file_page_domain *domain =
            guest_file_page_domain_create();
    struct guest_page_backing *cache = guest_page_backing_create();
    assert(domain != NULL && cache != NULL);
    guest_page_backing_track_file_writes(cache, domain, 0);
    byte_t page[GUEST_MEMORY_PAGE_SIZE];
    memset(page, UINT8_C(0x7b), sizeof(page));
    guest_page_backing_commit_file_write(
            cache, 0, page, sizeof(page));
    struct guest_page_backing *private_cow =
            guest_page_backing_clone(cache);
    assert(private_cow != NULL);

    guest_file_page_domain_resize(domain, 97, 257);
    assert_byte_range(cache, 0, 97, UINT8_C(0x7b));
    assert_byte_range(cache, 97, GUEST_MEMORY_PAGE_SIZE, 0);
    assert_byte_range(private_cow, 0,
            GUEST_MEMORY_PAGE_SIZE, UINT8_C(0x7b));
    byte_t snapshot[GUEST_MEMORY_PAGE_SIZE];
    qword_t generation;
    assert(!guest_page_backing_copy_dirty(
            cache, snapshot, &generation));

    memset(page + 257, UINT8_C(0x3c), sizeof(page) - 257);
    guest_page_backing_commit_file_write(
            cache, 257, page + 257, sizeof(page) - 257);
    guest_file_page_domain_resize(domain, 257, 257);
    assert_byte_range(cache, 257, GUEST_MEMORY_PAGE_SIZE, 0);
    assert(!guest_page_backing_copy_dirty(
            cache, snapshot, &generation));

    guest_page_backing_release(private_cow);
    guest_page_backing_release(cache);
    guest_file_page_domain_release(domain);
    assert(guest_page_backing_test_live_count() == 0);
}

static void test_concurrent_file_clone_and_resize(void) {
    guest_page_backing_test_fail_allocation_at(SIZE_MAX);
    for (unsigned iteration = 0;
            iteration < FILE_RESIZE_RACE_ITERATIONS; iteration++) {
        struct guest_file_page_domain *domain =
                guest_file_page_domain_create();
        struct guest_page_backing *source =
                guest_page_backing_create();
        assert(domain != NULL && source != NULL);
        guest_page_backing_track_file_writes(source, domain, 0);
        fill_backing(source, (byte_t) iteration);
        struct resize_clone_context context = {
            .domain = domain,
            .source = source,
            .start = ATOMIC_VAR_INIT(false),
        };
        pthread_t clone_thread;
        pthread_t resize_thread;
        assert(pthread_create(&clone_thread, NULL,
                clone_file_backing, &context) == 0);
        assert(pthread_create(&resize_thread, NULL,
                resize_file_domain, &context) == 0);
        atomic_store_explicit(&context.start, true, memory_order_release);
        assert(pthread_join(clone_thread, NULL) == 0);
        assert(pthread_join(resize_thread, NULL) == 0);
        assert(context.copy != NULL);
        assert(!guest_page_backing_file_accessible(source));
        assert(!guest_page_backing_file_accessible(context.copy));
        guest_page_backing_release(context.copy);
        guest_page_backing_release(source);
        guest_file_page_domain_release(domain);
        assert(guest_page_backing_test_live_count() == 0);
    }
}

static void test_file_domain_lifecycle_and_unregister_race(void) {
    guest_page_backing_test_fail_allocation_at(SIZE_MAX);
    struct guest_file_page_domain *owned_domain =
            guest_file_page_domain_create();
    struct guest_page_backing *owned_source =
            guest_page_backing_create();
    assert(owned_domain != NULL && owned_source != NULL);
    guest_page_backing_track_file_writes(
            owned_source, owned_domain, 0);
    guest_file_page_domain_release(owned_domain);
    struct guest_page_backing *owned_copy =
            guest_page_backing_clone(owned_source);
    assert(owned_copy != NULL);
    guest_page_backing_release(owned_copy);
    guest_page_backing_release(owned_source);
    assert(guest_page_backing_test_live_count() == 0);

    struct guest_file_page_domain *domain =
            guest_file_page_domain_create();
    struct guest_page_backing *source = guest_page_backing_create();
    assert(domain != NULL && source != NULL);
    guest_page_backing_track_file_writes(source, domain, 0);
    struct unregister_resize_context context = {
        .domain = domain,
        .source = source,
        .start = ATOMIC_VAR_INIT(false),
    };
    pthread_t clone_thread;
    pthread_t resize_thread;
    assert(pthread_create(&clone_thread, NULL,
            clone_and_release_file_backings, &context) == 0);
    assert(pthread_create(&resize_thread, NULL,
            resize_partial_file_domain, &context) == 0);
    atomic_store_explicit(&context.start, true, memory_order_release);
    assert(pthread_join(clone_thread, NULL) == 0);
    assert(pthread_join(resize_thread, NULL) == 0);
    assert(guest_page_backing_file_accessible(source));
    guest_page_backing_release(source);
    guest_file_page_domain_release(domain);
    assert(guest_page_backing_test_live_count() == 0);
}

int main(void) {
    test_zeroed_clone_and_lifecycle();
    test_concurrent_reference_updates();
    test_clone_consistent_during_shared_writes();
    test_sync_identity_lifecycle();
    test_sync_exclusive_granules();
    test_file_dirty_generation();
    test_committed_file_write_merge();
    test_file_resize_tail_and_invalidation();
    test_file_resize_grow_and_equal_cleanup();
    test_concurrent_file_clone_and_resize();
    test_file_domain_lifecycle_and_unregister_race();
    guest_page_backing_test_fail_allocation_at(SIZE_MAX);
    return 0;
}
