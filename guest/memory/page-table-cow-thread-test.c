#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <string.h>

#include "guest/memory/page-backing.h"
#include "guest/memory/page-table.h"
#include "guest/memory/tlb.h"

#define TEST_PAGE UINT64_C(0x0000000040000000)

struct release_probe {
    atomic_uint calls;
};

struct cow_thread_context {
    struct guest_page_table *table;
    atomic_bool *start;
    atomic_uint *ready;
    dword_t expected;
    dword_t replacement;
    dword_t observed;
    bool compare_exchange;
    bool success;
    struct guest_memory_fault fault;
};

static void release_source(void *opaque) {
    struct release_probe *probe = opaque;
    atomic_fetch_add_explicit(&probe->calls, 1, memory_order_release);
}

static struct guest_page_view resolve_view(
        struct guest_page_table *table) {
    struct guest_page_view view;
    assert(guest_address_space_resolve_page(&table->address_space,
            TEST_PAGE, GUEST_MEMORY_READ, &view) ==
            GUEST_MEMORY_FAULT_NONE);
    return view;
}

static void *run_cow(void *opaque) {
    struct cow_thread_context *context = opaque;
    struct guest_tlb tlb;
    guest_tlb_init(&tlb, &context->table->address_space);
    atomic_fetch_add_explicit(context->ready, 1, memory_order_release);
    while (!atomic_load_explicit(context->start, memory_order_acquire))
        sched_yield();
    if (context->compare_exchange) {
        context->success = guest_tlb_compare_exchange(&tlb, TEST_PAGE,
                &context->expected, &context->replacement,
                &context->observed, sizeof(context->observed),
                &context->fault) == GUEST_TLB_COMPARE_EXCHANGE_EXCHANGED;
    } else {
        context->success = guest_tlb_write(&tlb, TEST_PAGE,
                &context->replacement, sizeof(context->replacement),
                &context->fault);
    }
    return NULL;
}

int main(void) {
    unsigned live_baseline = guest_page_backing_test_live_count();
    struct guest_page_table parent;
    assert(guest_page_table_init(&parent, 48));
    struct release_probe probe;
    atomic_init(&probe.calls, 0);
    struct guest_file_source *source = guest_file_source_create(
            UINT64_C(0x18374652), &probe, release_source);
    assert(source != NULL);
    byte_t *page;
    assert(guest_page_table_map_file(&parent, TEST_PAGE,
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE,
            source, 0, &page) == GUEST_PAGE_TABLE_OK);
    const dword_t initial = UINT32_C(0x11223344);
    memcpy(page, &initial, sizeof(initial));
    guest_file_source_release(source);

    struct guest_page_table child;
    assert(guest_page_table_clone(&child, &parent));
    struct guest_page_view parent_before = resolve_view(&parent);
    struct guest_page_view child_before = resolve_view(&child);
    assert(parent_before.host_page == child_before.host_page &&
            parent_before.sync == child_before.sync &&
            parent_before.copy_on_write && child_before.copy_on_write);

    atomic_bool start;
    atomic_init(&start, false);
    atomic_uint ready;
    atomic_init(&ready, 0);
    struct cow_thread_context parent_context = {
        .table = &parent,
        .start = &start,
        .ready = &ready,
        .replacement = UINT32_C(0xa1a2a3a4),
    };
    struct cow_thread_context child_context = {
        .table = &child,
        .start = &start,
        .ready = &ready,
        .expected = initial,
        .replacement = UINT32_C(0xb1b2b3b4),
        .observed = UINT32_C(0xfeedface),
        .compare_exchange = true,
    };
    pthread_t parent_thread;
    pthread_t child_thread;
    assert(pthread_create(&parent_thread, NULL,
            run_cow, &parent_context) == 0);
    assert(pthread_create(&child_thread, NULL,
            run_cow, &child_context) == 0);
    while (atomic_load_explicit(&ready, memory_order_acquire) != 2)
        sched_yield();
    atomic_store_explicit(&start, true, memory_order_release);
    assert(pthread_join(parent_thread, NULL) == 0);
    assert(pthread_join(child_thread, NULL) == 0);
    assert(parent_context.success && child_context.success &&
            child_context.observed == initial);

    struct guest_page_view parent_after = resolve_view(&parent);
    struct guest_page_view child_after = resolve_view(&child);
    dword_t parent_value;
    dword_t child_value;
    memcpy(&parent_value, parent_after.host_page, sizeof(parent_value));
    memcpy(&child_value, child_after.host_page, sizeof(child_value));
    assert(parent_after.host_page != child_after.host_page &&
            parent_after.origin == GUEST_PAGE_ORIGIN_ANONYMOUS &&
            child_after.origin == GUEST_PAGE_ORIGIN_ANONYMOUS &&
            !parent_after.copy_on_write && !child_after.copy_on_write &&
            parent_after.sync == NULL && child_after.sync == NULL &&
            parent_value == parent_context.replacement &&
            child_value == child_context.replacement &&
            atomic_load_explicit(&probe.calls, memory_order_acquire) == 1);

    guest_page_table_destroy(&parent);
    guest_page_table_destroy(&child);
    assert(atomic_load_explicit(&probe.calls, memory_order_acquire) == 1 &&
            guest_page_backing_test_live_count() == live_baseline);
    return 0;
}
