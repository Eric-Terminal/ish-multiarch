#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include "guest/memory/file-pager.h"

struct provider_probe {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    enum guest_file_page_result result;
    dword_t valid_bytes;
    qword_t last_offset;
    unsigned reads;
    unsigned releases;
    bool block;
    bool entered;
    bool allow_read;
    bool retain_failed_during_release;
};

static void check_pthread(int result) {
    assert(result == 0);
}

static void probe_init(struct provider_probe *probe) {
    *probe = (struct provider_probe) {
        .result = GUEST_FILE_PAGE_OK,
        .valid_bytes = (dword_t) GUEST_MEMORY_PAGE_SIZE,
    };
    check_pthread(pthread_mutex_init(&probe->lock, NULL));
    check_pthread(pthread_cond_init(&probe->changed, NULL));
}

static void probe_destroy(struct provider_probe *probe) {
    check_pthread(pthread_cond_destroy(&probe->changed));
    check_pthread(pthread_mutex_destroy(&probe->lock));
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

static void probe_release(struct guest_file_pager *pager, void *opaque) {
    struct provider_probe *probe = opaque;
    probe->retain_failed_during_release =
            !guest_file_pager_try_retain(pager);
    probe->releases++;
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

int main(void) {
    test_cache_and_tail_zero();
    test_failures_are_not_cached();
    test_allocation_failures_retry();
    test_concurrent_single_flight();
    assert(guest_page_backing_test_live_count() == 0);
    return 0;
}
