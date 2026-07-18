#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "guest/memory/file-pager.h"

struct provider_probe {
    pthread_mutex_t lock;
    pthread_mutex_t io_lock;
    pthread_cond_t changed;
    enum guest_file_page_result result;
    enum guest_file_sync_result write_result;
    enum guest_file_sync_result sync_result;
    dword_t valid_bytes;
    qword_t last_offset;
    qword_t write_offsets[8];
    qword_t sync_offset;
    qword_t sync_length;
    byte_t written_pages[8][GUEST_MEMORY_PAGE_SIZE];
    unsigned reads;
    unsigned writes;
    unsigned syncs;
    unsigned releases;
    unsigned writes_at_release;
    unsigned syncs_at_release;
    unsigned drain_failures;
    bool block;
    bool entered;
    bool allow_read;
    bool block_write;
    bool write_entered;
    bool allow_write;
    bool retain_failed_during_release;
};

static void check_pthread(int result) {
    assert(result == 0);
}

static void probe_init(struct provider_probe *probe) {
    *probe = (struct provider_probe) {
        .result = GUEST_FILE_PAGE_OK,
        .write_result = GUEST_FILE_SYNC_OK,
        .sync_result = GUEST_FILE_SYNC_OK,
        .valid_bytes = (dword_t) GUEST_MEMORY_PAGE_SIZE,
    };
    check_pthread(pthread_mutex_init(&probe->lock, NULL));
    check_pthread(pthread_mutex_init(&probe->io_lock, NULL));
    check_pthread(pthread_cond_init(&probe->changed, NULL));
}

static void probe_destroy(struct provider_probe *probe) {
    check_pthread(pthread_cond_destroy(&probe->changed));
    check_pthread(pthread_mutex_destroy(&probe->io_lock));
    check_pthread(pthread_mutex_destroy(&probe->lock));
}

static void probe_begin_io(void *opaque) {
    struct provider_probe *probe = opaque;
    check_pthread(pthread_mutex_lock(&probe->io_lock));
}

static void probe_end_io(void *opaque) {
    struct provider_probe *probe = opaque;
    check_pthread(pthread_mutex_unlock(&probe->io_lock));
}

static enum guest_file_page_result probe_read_page(void *opaque,
        qword_t file_offset, byte_t *page, dword_t *valid_bytes) {
    struct provider_probe *probe = opaque;
    check_pthread(pthread_mutex_lock(&probe->lock));
    probe->reads++;
    probe->last_offset = file_offset;
    probe->entered = true;
    check_pthread(pthread_cond_broadcast(&probe->changed));
    while (probe->block && !probe->allow_read)
        check_pthread(pthread_cond_wait(&probe->changed, &probe->lock));
    enum guest_file_page_result result = probe->result;
    dword_t count = probe->valid_bytes;
    check_pthread(pthread_mutex_unlock(&probe->lock));

    if (result == GUEST_FILE_PAGE_OK) {
        for (dword_t index = 0;
                index < count && index < GUEST_MEMORY_PAGE_SIZE; index++) {
            page[index] = (byte_t) ((file_offset + index + 1) & UINT64_C(0xff));
        }
        *valid_bytes = count;
    }
    return result;
}

static enum guest_file_sync_result probe_write_page(void *opaque,
        qword_t file_offset, const byte_t *page) {
    struct provider_probe *probe = opaque;
    check_pthread(pthread_mutex_lock(&probe->lock));
    assert(probe->writes < 8);
    unsigned write = probe->writes++;
    probe->write_offsets[write] = file_offset;
    memcpy(probe->written_pages[write], page, GUEST_MEMORY_PAGE_SIZE);
    probe->write_entered = true;
    check_pthread(pthread_cond_broadcast(&probe->changed));
    while (probe->block_write && !probe->allow_write)
        check_pthread(pthread_cond_wait(&probe->changed, &probe->lock));
    enum guest_file_sync_result result = probe->write_result;
    check_pthread(pthread_mutex_unlock(&probe->lock));
    return result;
}

static enum guest_file_sync_result probe_sync_range(void *opaque,
        qword_t file_offset, qword_t length) {
    struct provider_probe *probe = opaque;
    check_pthread(pthread_mutex_lock(&probe->lock));
    probe->syncs++;
    probe->sync_offset = file_offset;
    probe->sync_length = length;
    enum guest_file_sync_result result = probe->sync_result;
    check_pthread(pthread_mutex_unlock(&probe->lock));
    return result;
}

static void probe_release(struct guest_file_pager *pager, void *opaque) {
    struct provider_probe *probe = opaque;
    probe->writes_at_release = probe->writes;
    probe->syncs_at_release = probe->syncs;
    probe->retain_failed_during_release =
            !guest_file_pager_try_retain(pager);
    probe->releases++;
}

static void probe_drain_failed(
        struct guest_file_pager *pager, void *opaque) {
    struct provider_probe *probe = opaque;
    assert(pager != NULL);
    check_pthread(pthread_mutex_lock(&probe->lock));
    probe->drain_failures++;
    check_pthread(pthread_mutex_unlock(&probe->lock));
}

static struct guest_file_pager *create_pager(
        qword_t identity, struct provider_probe *probe) {
    return guest_file_pager_create(identity,
            (struct guest_file_pager_provider) {
                .opaque = probe,
                .read_page = probe_read_page,
                .release = probe_release,
            });
}

static struct guest_file_pager *create_writable_pager(
        qword_t identity, struct provider_probe *probe,
        bool serialize_io) {
    return guest_file_pager_create(identity,
            (struct guest_file_pager_provider) {
                .opaque = probe,
                .begin_io = serialize_io ? probe_begin_io : NULL,
                .end_io = serialize_io ? probe_end_io : NULL,
                .read_page = probe_read_page,
                .write_page = probe_write_page,
                .sync_range = probe_sync_range,
                .drain_failed = probe_drain_failed,
                .release = probe_release,
            });
}

static void write_backing_byte(struct guest_page_backing *backing,
        size_t page_offset, byte_t value) {
    const struct guest_page_sync *sync = guest_page_backing_sync(backing);
    sync->ops->write_lock(sync->opaque);
    guest_page_backing_bytes(backing)[page_offset] = value;
    sync->ops->written(sync->opaque, page_offset, 1);
    sync->ops->write_unlock(sync->opaque);
}

static bool backing_is_dirty(struct guest_page_backing *backing,
        byte_t *snapshot) {
    qword_t generation;
    return guest_page_backing_copy_dirty(backing, snapshot, &generation);
}

static void assert_page_prefix(const struct guest_page_backing *backing,
        qword_t file_offset, dword_t valid_bytes) {
    const byte_t *page = guest_page_backing_bytes(
            (struct guest_page_backing *) backing);
    for (dword_t index = 0; index < valid_bytes; index++) {
        assert(page[index] ==
                (byte_t) ((file_offset + index + 1) & UINT64_C(0xff)));
    }
    for (qword_t index = valid_bytes;
            index < GUEST_MEMORY_PAGE_SIZE; index++)
        assert(page[index] == 0);
}

static struct guest_page_backing *load_page(
        struct guest_file_pager *pager, qword_t file_offset) {
    struct guest_page_backing *backing = NULL;
    assert(guest_file_pager_get_page(pager, file_offset, &backing) ==
            GUEST_FILE_PAGE_OK);
    assert(backing != NULL);
    return backing;
}

static void test_cache_and_tail_zero(void) {
    unsigned baseline = guest_page_backing_test_live_count();
    struct provider_probe probe;
    probe_init(&probe);
    struct guest_file_pager *pager = create_pager(UINT64_C(0x1234), &probe);
    assert(pager != NULL);
    assert(guest_file_source_identity(
            guest_file_pager_file_source(pager)) == UINT64_C(0x1234));
    assert(guest_file_pager_test_reference_count(pager) == 1);
    assert(guest_file_pager_retain(pager) == pager);
    assert(guest_file_pager_test_reference_count(pager) == 2);
    guest_file_pager_release(pager);

    struct guest_page_backing *first;
    assert(guest_file_pager_get_page(pager, 0, &first) ==
            GUEST_FILE_PAGE_OK);
    assert(first != NULL);
    assert_page_prefix(first, 0, (dword_t) GUEST_MEMORY_PAGE_SIZE);

    struct guest_page_backing *alias;
    assert(guest_file_pager_get_page(pager, 0, &alias) ==
            GUEST_FILE_PAGE_OK);
    assert(alias == first);
    assert(guest_page_sync_identity(guest_page_backing_sync(alias)) ==
            guest_page_sync_identity(guest_page_backing_sync(first)));
    assert(probe.reads == 1);
    guest_page_backing_release(alias);
    guest_page_backing_release(first);

    probe.valid_bytes = 37;
    struct guest_page_backing *tail;
    assert(guest_file_pager_get_page(
            pager, GUEST_MEMORY_PAGE_SIZE, &tail) == GUEST_FILE_PAGE_OK);
    assert_page_prefix(tail, GUEST_MEMORY_PAGE_SIZE, 37);
    assert(probe.reads == 2 &&
            probe.last_offset == GUEST_MEMORY_PAGE_SIZE);
    guest_page_backing_release(tail);

    guest_file_pager_release(pager);
    assert(probe.releases == 1 && probe.retain_failed_during_release);
    assert(guest_page_backing_test_live_count() == baseline);
    probe_destroy(&probe);
}

static void test_failures_are_not_cached(void) {
    unsigned baseline = guest_page_backing_test_live_count();
    struct provider_probe probe;
    probe_init(&probe);
    struct guest_file_pager *pager = create_pager(UINT64_C(0x2234), &probe);
    assert(pager != NULL);

    struct guest_page_backing *backing = (void *) (uintptr_t) 1;
    probe.result = GUEST_FILE_PAGE_END_OF_FILE;
    assert(guest_file_pager_get_page(pager, 0, &backing) ==
            GUEST_FILE_PAGE_END_OF_FILE && backing == NULL);
    assert(guest_file_pager_get_page(pager, 0, &backing) ==
            GUEST_FILE_PAGE_END_OF_FILE && backing == NULL);
    assert(probe.reads == 2);

    probe.result = GUEST_FILE_PAGE_IO_ERROR;
    assert(guest_file_pager_get_page(pager, 0, &backing) ==
            GUEST_FILE_PAGE_IO_ERROR && backing == NULL);
    probe.result = GUEST_FILE_PAGE_OUT_OF_MEMORY;
    assert(guest_file_pager_get_page(pager, 0, &backing) ==
            GUEST_FILE_PAGE_OUT_OF_MEMORY && backing == NULL);

    probe.result = GUEST_FILE_PAGE_OK;
    probe.valid_bytes = 0;
    assert(guest_file_pager_get_page(pager, 0, &backing) ==
            GUEST_FILE_PAGE_IO_ERROR && backing == NULL);
    probe.valid_bytes = (dword_t) GUEST_MEMORY_PAGE_SIZE + 1;
    assert(guest_file_pager_get_page(pager, 0, &backing) ==
            GUEST_FILE_PAGE_IO_ERROR && backing == NULL);

    probe.valid_bytes = 19;
    assert(guest_file_pager_get_page(pager, 0, &backing) ==
            GUEST_FILE_PAGE_OK && backing != NULL);
    assert_page_prefix(backing, 0, 19);
    guest_page_backing_release(backing);
    guest_file_pager_release(pager);
    assert(probe.releases == 1);
    assert(guest_page_backing_test_live_count() == baseline);
    probe_destroy(&probe);
}

static void test_allocation_failures_retry(void) {
    struct provider_probe probe;
    probe_init(&probe);

    guest_file_pager_test_fail_allocation_at(0);
    assert(create_pager(UINT64_C(0x3234), &probe) == NULL);
    guest_file_pager_test_fail_allocation_at(SIZE_MAX);
    struct guest_file_pager *pager = create_pager(UINT64_C(0x3234), &probe);
    assert(pager != NULL);

    struct guest_page_backing *backing;
    guest_file_pager_test_fail_allocation_at(0);
    assert(guest_file_pager_get_page(pager, 0, &backing) ==
            GUEST_FILE_PAGE_OUT_OF_MEMORY && backing == NULL);
    assert(probe.reads == 0);

    guest_file_pager_test_fail_allocation_at(SIZE_MAX);
    guest_page_backing_test_fail_allocation_at(0);
    assert(guest_file_pager_get_page(pager, 0, &backing) ==
            GUEST_FILE_PAGE_OUT_OF_MEMORY && backing == NULL);
    assert(probe.reads == 0);

    guest_page_backing_test_fail_allocation_at(SIZE_MAX);
    assert(guest_file_pager_get_page(pager, 0, &backing) ==
            GUEST_FILE_PAGE_OK && backing != NULL);
    assert(probe.reads == 1);
    guest_page_backing_release(backing);

    guest_file_pager_release(pager);
    guest_file_pager_test_fail_allocation_at(SIZE_MAX);
    guest_page_backing_test_fail_allocation_at(SIZE_MAX);
    probe_destroy(&probe);
}

struct page_thread {
    struct guest_file_pager *pager;
    struct guest_page_backing *backing;
    enum guest_file_page_result result;
};

static void *get_page_thread(void *opaque) {
    struct page_thread *thread = opaque;
    thread->result = guest_file_pager_get_page(
            thread->pager, GUEST_MEMORY_PAGE_SIZE, &thread->backing);
    return NULL;
}

static void test_concurrent_single_flight(void) {
    struct provider_probe probe;
    probe_init(&probe);
    probe.block = true;
    struct guest_file_pager *pager = create_pager(UINT64_C(0x4234), &probe);
    assert(pager != NULL);

    struct page_thread left = {.pager = pager};
    struct page_thread right = {.pager = pager};
    pthread_t left_thread;
    pthread_t right_thread;
    check_pthread(pthread_create(
            &left_thread, NULL, get_page_thread, &left));

    check_pthread(pthread_mutex_lock(&probe.lock));
    while (!probe.entered)
        check_pthread(pthread_cond_wait(&probe.changed, &probe.lock));
    check_pthread(pthread_mutex_unlock(&probe.lock));

    check_pthread(pthread_create(
            &right_thread, NULL, get_page_thread, &right));
    check_pthread(pthread_mutex_lock(&probe.lock));
    probe.allow_read = true;
    check_pthread(pthread_cond_broadcast(&probe.changed));
    check_pthread(pthread_mutex_unlock(&probe.lock));

    check_pthread(pthread_join(left_thread, NULL));
    check_pthread(pthread_join(right_thread, NULL));
    assert(left.result == GUEST_FILE_PAGE_OK &&
            right.result == GUEST_FILE_PAGE_OK);
    assert(left.backing != NULL && left.backing == right.backing);
    assert(probe.reads == 1);
    guest_page_backing_release(right.backing);
    guest_page_backing_release(left.backing);
    guest_file_pager_release(pager);
    probe_destroy(&probe);
}

static void test_writeback_range_and_retry(void) {
    unsigned baseline = guest_page_backing_test_live_count();
    struct provider_probe probe;
    probe_init(&probe);
    struct guest_file_pager *pager = create_writable_pager(
            UINT64_C(0x5234), &probe, true);
    assert(pager != NULL);

    struct guest_page_backing *first = load_page(pager, 0);
    struct guest_page_backing *middle = load_page(
            pager, GUEST_MEMORY_PAGE_SIZE);
    struct guest_page_backing *last = load_page(
            pager, 2 * GUEST_MEMORY_PAGE_SIZE);
    write_backing_byte(first, 17, UINT8_C(0xa1));
    write_backing_byte(last, 29, UINT8_C(0xc3));

    assert(guest_file_pager_sync_range(pager, 17, 37) ==
            GUEST_FILE_SYNC_OK);
    assert(probe.writes == 1 && probe.write_offsets[0] == 0);
    assert(probe.written_pages[0][17] == UINT8_C(0xa1));
    assert(probe.syncs == 1 && probe.sync_offset == 17 &&
            probe.sync_length == 37);

    byte_t snapshot[GUEST_MEMORY_PAGE_SIZE];
    assert(!backing_is_dirty(first, snapshot));
    assert(!backing_is_dirty(middle, snapshot));
    assert(backing_is_dirty(last, snapshot));
    assert(guest_file_pager_sync_range(pager, 17, 37) ==
            GUEST_FILE_SYNC_OK);
    assert(probe.writes == 1 && probe.syncs == 2);

    probe.write_result = GUEST_FILE_SYNC_IO_ERROR;
    assert(guest_file_pager_sync_range(pager,
            2 * GUEST_MEMORY_PAGE_SIZE + 29, 1) ==
            GUEST_FILE_SYNC_IO_ERROR);
    assert(probe.writes == 2 && probe.syncs == 2);
    assert(backing_is_dirty(last, snapshot));

    probe.write_result = GUEST_FILE_SYNC_OK;
    assert(guest_file_pager_sync_range(pager,
            2 * GUEST_MEMORY_PAGE_SIZE,
            GUEST_MEMORY_PAGE_SIZE) == GUEST_FILE_SYNC_OK);
    assert(probe.writes == 3 && probe.syncs == 3);
    assert(probe.write_offsets[2] == 2 * GUEST_MEMORY_PAGE_SIZE);
    assert(probe.written_pages[2][29] == UINT8_C(0xc3));
    assert(!backing_is_dirty(last, snapshot));

    write_backing_byte(middle, 31, UINT8_C(0xb2));
    probe.sync_result = GUEST_FILE_SYNC_IO_ERROR;
    assert(guest_file_pager_sync_range(pager,
            GUEST_MEMORY_PAGE_SIZE, GUEST_MEMORY_PAGE_SIZE) ==
            GUEST_FILE_SYNC_IO_ERROR);
    assert(probe.writes == 4 && probe.syncs == 4);
    assert(probe.write_offsets[3] == GUEST_MEMORY_PAGE_SIZE &&
            probe.written_pages[3][31] == UINT8_C(0xb2));
    assert(!backing_is_dirty(middle, snapshot));
    probe.sync_result = GUEST_FILE_SYNC_OK;
    assert(guest_file_pager_sync_range(pager,
            GUEST_MEMORY_PAGE_SIZE, GUEST_MEMORY_PAGE_SIZE) ==
            GUEST_FILE_SYNC_OK);
    assert(probe.writes == 4 && probe.syncs == 5);

    guest_page_backing_release(last);
    guest_page_backing_release(middle);
    guest_page_backing_release(first);
    guest_file_pager_release(pager);
    assert(probe.releases == 1 && probe.retain_failed_during_release);
    probe_destroy(&probe);

    struct provider_probe read_only_probe;
    probe_init(&read_only_probe);
    pager = create_pager(UINT64_C(0x5235), &read_only_probe);
    assert(pager != NULL);
    assert(guest_file_pager_sync_range(pager, 0, 0) ==
            GUEST_FILE_SYNC_OK);
    assert(guest_file_pager_sync_range(
            pager, 0, GUEST_MEMORY_PAGE_SIZE) ==
            GUEST_FILE_SYNC_UNSUPPORTED);
    guest_file_pager_release(pager);
    probe_destroy(&read_only_probe);
    assert(guest_page_backing_test_live_count() == baseline);
}

struct sync_thread {
    struct guest_file_pager *pager;
    qword_t file_offset;
    qword_t length;
    atomic_bool started;
    enum guest_file_sync_result result;
};

static void *sync_range_thread(void *opaque) {
    struct sync_thread *thread = opaque;
    atomic_store_explicit(&thread->started, true, memory_order_release);
    thread->result = guest_file_pager_sync_range(
            thread->pager, thread->file_offset, thread->length);
    return NULL;
}

static void wait_for_write(struct provider_probe *probe) {
    check_pthread(pthread_mutex_lock(&probe->lock));
    while (!probe->write_entered)
        check_pthread(pthread_cond_wait(&probe->changed, &probe->lock));
    check_pthread(pthread_mutex_unlock(&probe->lock));
}

static struct timespec deadline_after_milliseconds(long milliseconds) {
    struct timespec deadline;
    assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_sec += milliseconds / 1000;
    deadline.tv_nsec += (milliseconds % 1000) * 1000 * 1000;
    if (deadline.tv_nsec >= 1000 * 1000 * 1000) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000 * 1000 * 1000;
    }
    return deadline;
}

static bool wait_for_write_timeout(
        struct provider_probe *probe, long milliseconds) {
    struct timespec deadline = deadline_after_milliseconds(milliseconds);
    check_pthread(pthread_mutex_lock(&probe->lock));
    while (!probe->write_entered) {
        int result = pthread_cond_timedwait(
                &probe->changed, &probe->lock, &deadline);
        if (result == ETIMEDOUT)
            break;
        check_pthread(result);
    }
    bool entered = probe->write_entered;
    check_pthread(pthread_mutex_unlock(&probe->lock));
    return entered;
}

static void assert_second_write_stays_blocked(
        struct provider_probe *probe) {
    struct timespec deadline = deadline_after_milliseconds(100);

    check_pthread(pthread_mutex_lock(&probe->lock));
    while (probe->writes == 1) {
        int result = pthread_cond_timedwait(
                &probe->changed, &probe->lock, &deadline);
        if (result == ETIMEDOUT)
            break;
        check_pthread(result);
    }
    assert(probe->writes == 1);
    check_pthread(pthread_mutex_unlock(&probe->lock));
}

static void test_writeback_single_flight(void) {
    unsigned baseline = guest_page_backing_test_live_count();
    struct provider_probe probe;
    probe_init(&probe);
    probe.block_write = true;
    struct guest_file_pager *pager = create_writable_pager(
            UINT64_C(0x6233), &probe, false);
    assert(pager != NULL);
    struct guest_page_backing *backing = load_page(pager, 0);
    write_backing_byte(backing, 39, UINT8_C(0x51));

    struct sync_thread first = {
        .pager = pager,
        .length = GUEST_MEMORY_PAGE_SIZE,
        .started = ATOMIC_VAR_INIT(false),
    };
    struct sync_thread second = {
        .pager = pager,
        .length = GUEST_MEMORY_PAGE_SIZE,
        .started = ATOMIC_VAR_INIT(false),
    };
    pthread_t first_thread;
    pthread_t second_thread;
    check_pthread(pthread_create(
            &first_thread, NULL, sync_range_thread, &first));
    wait_for_write(&probe);
    check_pthread(pthread_create(
            &second_thread, NULL, sync_range_thread, &second));
    while (!atomic_load_explicit(&second.started, memory_order_acquire))
        sched_yield();
    assert_second_write_stays_blocked(&probe);

    check_pthread(pthread_mutex_lock(&probe.lock));
    probe.allow_write = true;
    check_pthread(pthread_cond_broadcast(&probe.changed));
    check_pthread(pthread_mutex_unlock(&probe.lock));
    check_pthread(pthread_join(first_thread, NULL));
    check_pthread(pthread_join(second_thread, NULL));
    assert(first.result == GUEST_FILE_SYNC_OK &&
            second.result == GUEST_FILE_SYNC_OK);
    assert(probe.writes == 1 && probe.syncs == 2);
    assert(probe.written_pages[0][39] == UINT8_C(0x51));
    byte_t snapshot[GUEST_MEMORY_PAGE_SIZE];
    assert(!backing_is_dirty(backing, snapshot));

    guest_page_backing_release(backing);
    guest_file_pager_release(pager);
    probe_destroy(&probe);
    assert(guest_page_backing_test_live_count() == baseline);
}

static void test_writeback_generation_and_isolation(void) {
    unsigned baseline = guest_page_backing_test_live_count();
    struct provider_probe blocked_probe;
    struct provider_probe other_probe;
    probe_init(&blocked_probe);
    probe_init(&other_probe);
    blocked_probe.block_write = true;

    struct guest_file_pager *blocked = create_writable_pager(
            UINT64_C(0x6234), &blocked_probe, false);
    struct guest_file_pager *other = create_writable_pager(
            UINT64_C(0x6235), &other_probe, true);
    assert(blocked != NULL && other != NULL);
    struct guest_page_backing *blocked_backing = load_page(blocked, 0);
    struct guest_page_backing *other_backing = load_page(other, 0);
    write_backing_byte(blocked_backing, 41, UINT8_C(0x11));
    write_backing_byte(other_backing, 43, UINT8_C(0x33));

    struct sync_thread first = {
        .pager = blocked,
        .length = GUEST_MEMORY_PAGE_SIZE,
        .started = ATOMIC_VAR_INIT(false),
    };
    pthread_t first_thread;
    check_pthread(pthread_create(
            &first_thread, NULL, sync_range_thread, &first));
    wait_for_write(&blocked_probe);
    assert(blocked_probe.writes == 1 &&
            blocked_probe.written_pages[0][41] == UINT8_C(0x11));

    write_backing_byte(blocked_backing, 41, UINT8_C(0x22));
    struct sync_thread other_sync = {
        .pager = other,
        .length = GUEST_MEMORY_PAGE_SIZE,
        .started = ATOMIC_VAR_INIT(false),
    };
    pthread_t other_thread;
    check_pthread(pthread_create(
            &other_thread, NULL, sync_range_thread, &other_sync));
    bool other_entered = wait_for_write_timeout(&other_probe, 3000);
    if (!other_entered) {
        check_pthread(pthread_mutex_lock(&blocked_probe.lock));
        blocked_probe.allow_write = true;
        check_pthread(pthread_cond_broadcast(&blocked_probe.changed));
        check_pthread(pthread_mutex_unlock(&blocked_probe.lock));
        check_pthread(pthread_join(first_thread, NULL));
        check_pthread(pthread_join(other_thread, NULL));
        guest_page_backing_release(other_backing);
        guest_page_backing_release(blocked_backing);
        guest_file_pager_release(other);
        guest_file_pager_release(blocked);
        probe_destroy(&other_probe);
        probe_destroy(&blocked_probe);
        assert(guest_page_backing_test_live_count() == baseline);
        assert(other_entered);
        return;
    }
    check_pthread(pthread_join(other_thread, NULL));
    assert(other_sync.result == GUEST_FILE_SYNC_OK);
    assert(other_probe.writes == 1 && other_probe.syncs == 1 &&
            other_probe.written_pages[0][43] == UINT8_C(0x33));

    check_pthread(pthread_mutex_lock(&blocked_probe.lock));
    blocked_probe.allow_write = true;
    check_pthread(pthread_cond_broadcast(&blocked_probe.changed));
    check_pthread(pthread_mutex_unlock(&blocked_probe.lock));
    check_pthread(pthread_join(first_thread, NULL));
    assert(first.result == GUEST_FILE_SYNC_OK);
    assert(blocked_probe.writes == 1 && blocked_probe.syncs == 1);
    byte_t snapshot[GUEST_MEMORY_PAGE_SIZE];
    assert(backing_is_dirty(blocked_backing, snapshot));
    assert(snapshot[41] == UINT8_C(0x22));
    assert(guest_file_pager_sync_range(
            blocked, 0, GUEST_MEMORY_PAGE_SIZE) == GUEST_FILE_SYNC_OK);
    assert(blocked_probe.writes == 2 && blocked_probe.syncs == 2);
    assert(blocked_probe.written_pages[0][41] == UINT8_C(0x11));
    assert(blocked_probe.written_pages[1][41] == UINT8_C(0x22));
    assert(!backing_is_dirty(blocked_backing, snapshot));
    assert(!backing_is_dirty(other_backing, snapshot));

    guest_page_backing_release(other_backing);
    guest_page_backing_release(blocked_backing);
    guest_file_pager_release(other);
    guest_file_pager_release(blocked);
    probe_destroy(&other_probe);
    probe_destroy(&blocked_probe);
    assert(guest_page_backing_test_live_count() == baseline);
}

static void test_release_drains_dirty_pages(void) {
    unsigned baseline = guest_page_backing_test_live_count();
    assert(guest_file_pager_orphan_count() == 0);

    struct provider_probe clean_failure;
    probe_init(&clean_failure);
    clean_failure.sync_result = GUEST_FILE_SYNC_IO_ERROR;
    struct guest_file_pager *pager = create_writable_pager(
            UINT64_C(0x7233), &clean_failure, true);
    assert(pager != NULL);
    struct guest_page_backing *first = load_page(pager, 0);
    guest_page_backing_release(first);
    guest_file_pager_release(pager);
    assert(clean_failure.writes == 0 && clean_failure.syncs == 0 &&
            clean_failure.drain_failures == 0 &&
            clean_failure.releases == 1 &&
            guest_file_pager_orphan_count() == 0);
    probe_destroy(&clean_failure);
    assert(guest_page_backing_test_live_count() == baseline);

    struct provider_probe probe;
    probe_init(&probe);
    pager = create_writable_pager(
            UINT64_C(0x7234), &probe, true);
    assert(pager != NULL);
    first = load_page(pager, 0);
    struct guest_page_backing *second = load_page(
            pager, 2 * GUEST_MEMORY_PAGE_SIZE);
    write_backing_byte(first, 53, UINT8_C(0xd7));
    write_backing_byte(second, 59, UINT8_C(0xe9));
    guest_page_backing_release(second);
    guest_page_backing_release(first);

    guest_file_pager_release(pager);
    assert(probe.writes == 2 && probe.write_offsets[0] == 0 &&
            probe.write_offsets[1] == 2 * GUEST_MEMORY_PAGE_SIZE);
    assert(probe.written_pages[0][53] == UINT8_C(0xd7));
    assert(probe.written_pages[1][59] == UINT8_C(0xe9));
    assert(probe.syncs == 1 && probe.sync_offset == 0 &&
            probe.sync_length == UINT64_MAX);
    assert(probe.releases == 1 && probe.retain_failed_during_release &&
            probe.writes_at_release == 2 && probe.syncs_at_release == 1);
    probe_destroy(&probe);
    assert(guest_page_backing_test_live_count() == baseline);

    struct provider_probe write_failure;
    probe_init(&write_failure);
    write_failure.write_result = GUEST_FILE_SYNC_IO_ERROR;
    pager = create_writable_pager(
            UINT64_C(0x7235), &write_failure, true);
    assert(pager != NULL);
    first = load_page(pager, 0);
    write_backing_byte(first, 61, UINT8_C(0xf1));
    guest_page_backing_release(first);
    guest_file_pager_release(pager);
    assert(write_failure.writes == 1 && write_failure.syncs == 0 &&
            write_failure.releases == 0 &&
            write_failure.drain_failures == 1 &&
            guest_file_pager_orphan_count() == 1 &&
            guest_page_backing_test_live_count() == baseline + 1);
    assert(guest_file_pager_retry_orphans() == 1 &&
            guest_file_pager_orphan_count() == 1 &&
            write_failure.writes == 2 &&
            write_failure.drain_failures == 2 &&
            write_failure.releases == 0);
    write_failure.write_result = GUEST_FILE_SYNC_OK;
    assert(guest_file_pager_retry_orphans() == 1 &&
            guest_file_pager_orphan_count() == 0);
    assert(write_failure.writes == 3 && write_failure.syncs == 1 &&
            write_failure.releases == 1 &&
            write_failure.writes_at_release == 3 &&
            write_failure.syncs_at_release == 1 &&
            write_failure.retain_failed_during_release);
    probe_destroy(&write_failure);
    assert(guest_page_backing_test_live_count() == baseline);

    struct provider_probe sync_failure;
    probe_init(&sync_failure);
    sync_failure.sync_result = GUEST_FILE_SYNC_IO_ERROR;
    pager = create_writable_pager(
            UINT64_C(0x7236), &sync_failure, true);
    assert(pager != NULL);
    first = load_page(pager, 0);
    write_backing_byte(first, 67, UINT8_C(0xf3));
    guest_page_backing_release(first);
    guest_file_pager_release(pager);
    assert(sync_failure.writes == 1 && sync_failure.syncs == 1 &&
            sync_failure.releases == 0 &&
            sync_failure.drain_failures == 1 &&
            guest_file_pager_orphan_count() == 1);
    sync_failure.sync_result = GUEST_FILE_SYNC_OK;
    assert(guest_file_pager_retry_orphans() == 1 &&
            guest_file_pager_orphan_count() == 0);
    assert(sync_failure.writes == 1 && sync_failure.syncs == 2 &&
            sync_failure.releases == 1 &&
            sync_failure.writes_at_release == 1 &&
            sync_failure.syncs_at_release == 2 &&
            sync_failure.retain_failed_during_release);
    probe_destroy(&sync_failure);
    assert(guest_page_backing_test_live_count() == baseline);
}

struct release_thread {
    struct guest_file_pager *pager;
    atomic_bool finished;
};

static void *release_pager_thread(void *opaque) {
    struct release_thread *thread = opaque;
    guest_file_pager_release(thread->pager);
    atomic_store_explicit(&thread->finished, true, memory_order_release);
    return NULL;
}

static void test_failed_drain_retain_reclaims_orphan(void) {
    unsigned baseline = guest_page_backing_test_live_count();
    assert(guest_file_pager_orphan_count() == 0);
    struct provider_probe probe;
    probe_init(&probe);
    probe.block_write = true;
    probe.write_result = GUEST_FILE_SYNC_IO_ERROR;
    struct guest_file_pager *pager = create_writable_pager(
            UINT64_C(0x7237), &probe, true);
    assert(pager != NULL);
    struct guest_page_backing *backing = load_page(pager, 0);
    write_backing_byte(backing, 71, UINT8_C(0xf5));
    guest_page_backing_release(backing);

    struct release_thread release = {
        .pager = pager,
        .finished = ATOMIC_VAR_INIT(false),
    };
    pthread_t thread;
    check_pthread(pthread_create(
            &thread, NULL, release_pager_thread, &release));
    wait_for_write(&probe);
    assert(!guest_file_pager_try_retain(pager));

    check_pthread(pthread_mutex_lock(&probe.lock));
    probe.allow_write = true;
    check_pthread(pthread_cond_broadcast(&probe.changed));
    check_pthread(pthread_mutex_unlock(&probe.lock));
    check_pthread(pthread_join(thread, NULL));
    assert(atomic_load_explicit(
            &release.finished, memory_order_acquire));
    assert(probe.writes == 1 && probe.syncs == 0 &&
            probe.releases == 0 && probe.drain_failures == 1 &&
            guest_file_pager_orphan_count() == 1 &&
            guest_file_pager_test_reference_count(pager) == 1);

    assert(guest_file_pager_try_retain(pager));
    assert(guest_file_pager_orphan_count() == 0 &&
            guest_file_pager_test_reference_count(pager) == 1);
    probe.write_result = GUEST_FILE_SYNC_OK;
    guest_file_pager_release(pager);
    assert(probe.writes == 2 && probe.syncs == 1 &&
            probe.releases == 1 && probe.drain_failures == 1 &&
            probe.retain_failed_during_release &&
            guest_file_pager_orphan_count() == 0);
    probe_destroy(&probe);
    assert(guest_page_backing_test_live_count() == baseline);
}

struct retry_thread {
    size_t attempted;
    atomic_bool started;
    atomic_bool finished;
};

static void *retry_orphans_thread(void *opaque) {
    struct retry_thread *thread = opaque;
    atomic_store_explicit(&thread->started, true, memory_order_release);
    thread->attempted = guest_file_pager_retry_orphans();
    atomic_store_explicit(&thread->finished, true, memory_order_release);
    return NULL;
}

static void test_orphan_retry_callers_are_serialized(void) {
    unsigned baseline = guest_page_backing_test_live_count();
    assert(guest_file_pager_orphan_count() == 0);
    struct provider_probe probe;
    probe_init(&probe);
    probe.write_result = GUEST_FILE_SYNC_IO_ERROR;
    struct guest_file_pager *pager = create_writable_pager(
            UINT64_C(0x7238), &probe, true);
    assert(pager != NULL);
    struct guest_page_backing *backing = load_page(pager, 0);
    write_backing_byte(backing, 73, UINT8_C(0xf7));
    guest_page_backing_release(backing);
    guest_file_pager_release(pager);
    assert(guest_file_pager_orphan_count() == 1 &&
            probe.writes == 1 && probe.drain_failures == 1);

    check_pthread(pthread_mutex_lock(&probe.lock));
    probe.write_result = GUEST_FILE_SYNC_OK;
    probe.block_write = true;
    probe.write_entered = false;
    probe.allow_write = false;
    check_pthread(pthread_mutex_unlock(&probe.lock));

    struct retry_thread first = {
        .started = ATOMIC_VAR_INIT(false),
        .finished = ATOMIC_VAR_INIT(false),
    };
    pthread_t first_thread;
    check_pthread(pthread_create(
            &first_thread, NULL, retry_orphans_thread, &first));
    wait_for_write(&probe);

    struct retry_thread second = {
        .started = ATOMIC_VAR_INIT(false),
        .finished = ATOMIC_VAR_INIT(false),
    };
    pthread_t second_thread;
    check_pthread(pthread_create(
            &second_thread, NULL, retry_orphans_thread, &second));
    while (!atomic_load_explicit(&second.started, memory_order_acquire))
        sched_yield();
    assert(!atomic_load_explicit(&second.finished, memory_order_acquire));

    check_pthread(pthread_mutex_lock(&probe.lock));
    probe.allow_write = true;
    check_pthread(pthread_cond_broadcast(&probe.changed));
    check_pthread(pthread_mutex_unlock(&probe.lock));
    check_pthread(pthread_join(first_thread, NULL));
    check_pthread(pthread_join(second_thread, NULL));
    assert(first.attempted == 1 && second.attempted == 0 &&
            atomic_load_explicit(&first.finished, memory_order_acquire) &&
            atomic_load_explicit(&second.finished, memory_order_acquire));
    assert(probe.writes == 2 && probe.syncs == 1 &&
            probe.releases == 1 && probe.drain_failures == 1 &&
            guest_file_pager_orphan_count() == 0);
    probe_destroy(&probe);
    assert(guest_page_backing_test_live_count() == baseline);
}

static void test_resident_file_io_merge(void) {
    unsigned baseline = guest_page_backing_test_live_count();
    struct provider_probe probe;
    probe_init(&probe);
    struct guest_file_pager *pager = create_writable_pager(
            UINT64_C(0x8234), &probe, true);
    assert(pager != NULL);
    struct guest_page_backing *first = load_page(pager, 0);
    struct guest_page_backing *third = load_page(
            pager, 2 * GUEST_MEMORY_PAGE_SIZE);

    byte_t read_buffer[GUEST_MEMORY_PAGE_SIZE + 4];
    memset(read_buffer, UINT8_C(0xee), sizeof(read_buffer));
    probe_begin_io(&probe);
    guest_file_pager_read_resident(pager,
            GUEST_MEMORY_PAGE_SIZE - 2,
            read_buffer, sizeof(read_buffer));
    probe_end_io(&probe);
    assert(read_buffer[0] == UINT8_C(0xff) &&
            read_buffer[1] == UINT8_C(0x00));
    assert(read_buffer[2] == UINT8_C(0xee) &&
            read_buffer[GUEST_MEMORY_PAGE_SIZE + 1] == UINT8_C(0xee));
    assert(read_buffer[GUEST_MEMORY_PAGE_SIZE + 2] == UINT8_C(0x01) &&
            read_buffer[GUEST_MEMORY_PAGE_SIZE + 3] == UINT8_C(0x02));

    byte_t spanning_write[GUEST_MEMORY_PAGE_SIZE + 4];
    memset(spanning_write, UINT8_C(0x8c), sizeof(spanning_write));
    probe_begin_io(&probe);
    guest_file_pager_commit_file_write(pager,
            GUEST_MEMORY_PAGE_SIZE - 2,
            spanning_write, sizeof(spanning_write));
    guest_file_pager_read_resident(
            pager, UINT64_MAX, NULL, 0);
    guest_file_pager_commit_file_write(
            pager, UINT64_MAX, NULL, 0);
    probe_end_io(&probe);
    byte_t snapshot[GUEST_MEMORY_PAGE_SIZE];
    assert(!backing_is_dirty(first, snapshot) &&
            !backing_is_dirty(third, snapshot));
    assert(guest_page_backing_bytes(first)
                    [GUEST_MEMORY_PAGE_SIZE - 2] == UINT8_C(0x8c) &&
            guest_page_backing_bytes(third)[0] == UINT8_C(0x8c) &&
            guest_page_backing_bytes(third)[1] == UINT8_C(0x8c) &&
            probe.reads == 2);

    const byte_t clean_write[] = {
        UINT8_C(0xa1), UINT8_C(0xa2), UINT8_C(0xa3),
    };
    probe_begin_io(&probe);
    guest_file_pager_commit_file_write(
            pager, 20, clean_write, sizeof(clean_write));
    probe_end_io(&probe);
    assert(!backing_is_dirty(first, snapshot));
    assert(guest_page_backing_bytes(first)[20] == UINT8_C(0xa1) &&
            guest_page_backing_bytes(first)[22] == UINT8_C(0xa3));

    write_backing_byte(first, 41, UINT8_C(0xb1));
    const byte_t concurrent_write[] = {UINT8_C(0xc1), UINT8_C(0xc2)};
    probe_begin_io(&probe);
    guest_file_pager_commit_file_write(
            pager, 21, concurrent_write, sizeof(concurrent_write));
    probe_end_io(&probe);
    assert(backing_is_dirty(first, snapshot));
    assert(snapshot[20] == UINT8_C(0xa1) &&
            snapshot[21] == UINT8_C(0xc1) &&
            snapshot[22] == UINT8_C(0xc2) &&
            snapshot[41] == UINT8_C(0xb1));

    byte_t complete_page[GUEST_MEMORY_PAGE_SIZE];
    memset(complete_page, UINT8_C(0xd4), sizeof(complete_page));
    probe_begin_io(&probe);
    guest_file_pager_commit_file_write(
            pager, 0, complete_page, sizeof(complete_page));
    probe_end_io(&probe);
    assert(!backing_is_dirty(first, snapshot));
    assert(guest_page_backing_bytes(first)[41] == UINT8_C(0xd4));

    guest_page_backing_release(third);
    guest_page_backing_release(first);
    guest_file_pager_release(pager);
    assert(probe.writes == 0 && probe.syncs == 0 &&
            probe.releases == 1);
    probe_destroy(&probe);
    assert(guest_page_backing_test_live_count() == baseline);
}

int main(void) {
    test_cache_and_tail_zero();
    test_failures_are_not_cached();
    test_allocation_failures_retry();
    test_concurrent_single_flight();
    test_writeback_range_and_retry();
    test_writeback_single_flight();
    test_writeback_generation_and_isolation();
    test_release_drains_dirty_pages();
    test_failed_drain_retain_reclaims_orphan();
    test_orphan_retry_callers_are_serialized();
    test_resident_file_io_merge();
    assert(guest_file_pager_orphan_count() == 0);
    assert(guest_page_backing_test_live_count() == 0);
    return 0;
}
