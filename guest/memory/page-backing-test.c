#include <assert.h>
#include <pthread.h>
#include <string.h>

#include "guest/memory/page-backing.h"

#define RETAIN_THREAD_COUNT 4
#define RETAIN_ITERATIONS 20000

static void *retain_repeatedly(void *opaque) {
    struct guest_page_backing *backing = opaque;
    for (unsigned iteration = 0; iteration < RETAIN_ITERATIONS; iteration++) {
        guest_page_backing_retain(backing);
        guest_page_backing_release(backing);
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

int main(void) {
    test_zeroed_clone_and_lifecycle();
    test_concurrent_reference_updates();
    guest_page_backing_test_fail_allocation_at(SIZE_MAX);
    return 0;
}
