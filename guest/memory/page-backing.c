#include <assert.h>
#include <limits.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "guest/memory/page-backing.h"

struct guest_page_backing {
    atomic_uint references;
    // 数据内嵌于对象，引用存续期间地址不会变化，并保留宿主自然对齐。
    alignas(max_align_t) byte_t bytes[GUEST_MEMORY_PAGE_SIZE];
};

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
    atomic_init(&backing->references, 1);
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
    if (copy != NULL)
        memcpy(copy->bytes, source->bytes, sizeof(copy->bytes));
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
    free(backing);
}

byte_t *guest_page_backing_bytes(struct guest_page_backing *backing) {
    assert(backing != NULL);
    return backing->bytes;
}
