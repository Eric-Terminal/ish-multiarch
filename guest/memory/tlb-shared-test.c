#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <string.h>

#include "guest/memory/page-backing.h"
#include "guest/memory/tlb.h"

#define TEST_PAGE UINT64_C(0x0000000020000000)
#define TEST_NEXT_PAGE (TEST_PAGE + GUEST_MEMORY_PAGE_SIZE)
#define TEST_ALIAS_PAGE (TEST_PAGE + 4 * GUEST_MEMORY_PAGE_SIZE)
#define TEST_PAIR_OFFSET 128

struct shared_test_page {
    guest_addr_t address;
    struct guest_page_backing *backing;
};

struct shared_test_space {
    pthread_rwlock_t lock;
    struct guest_address_space address_space;
    struct shared_test_page pages[2];
    unsigned page_count;
};

static bool shared_read_lock(void *opaque) {
    struct shared_test_space *space = opaque;
    assert(pthread_rwlock_rdlock(&space->lock) == 0);
    return true;
}

static void shared_read_unlock(void *opaque, bool locked) {
    struct shared_test_space *space = opaque;
    assert(locked);
    assert(pthread_rwlock_unlock(&space->lock) == 0);
}

static bool shared_write_lock(void *opaque) {
    struct shared_test_space *space = opaque;
    assert(pthread_rwlock_wrlock(&space->lock) == 0);
    return true;
}

static void shared_write_unlock(void *opaque, bool locked) {
    struct shared_test_space *space = opaque;
    assert(locked);
    assert(pthread_rwlock_unlock(&space->lock) == 0);
}

static enum guest_memory_fault_kind resolve_shared_page(void *opaque,
        guest_addr_t page_base, enum guest_memory_access access,
        struct guest_page_view *view) {
    struct shared_test_space *space = opaque;
    (void) access;
    for (unsigned index = 0; index < space->page_count; index++) {
        if (space->pages[index].address != page_base)
            continue;
        view->host_page = guest_page_backing_bytes(
                space->pages[index].backing);
        view->permissions = GUEST_MEMORY_READ | GUEST_MEMORY_WRITE;
        view->sync = guest_page_backing_sync(space->pages[index].backing);
        return GUEST_MEMORY_FAULT_NONE;
    }
    return GUEST_MEMORY_FAULT_UNMAPPED;
}

static const struct guest_address_space_ops shared_test_ops = {
    .read_lock = shared_read_lock,
    .read_unlock = shared_read_unlock,
    .write_lock = shared_write_lock,
    .write_unlock = shared_write_unlock,
    .resolve_page = resolve_shared_page,
};

static void shared_test_space_init(struct shared_test_space *space,
        const struct shared_test_page *pages, unsigned page_count) {
    assert(page_count <= 2);
    *space = (struct shared_test_space) {0};
    assert(pthread_rwlock_init(&space->lock, NULL) == 0);
    memcpy(space->pages, pages, page_count * sizeof(*pages));
    space->page_count = page_count;
    guest_address_space_init(
            &space->address_space, &shared_test_ops, space, 48);
}

static void shared_test_space_destroy(struct shared_test_space *space) {
    assert(pthread_rwlock_destroy(&space->lock) == 0);
}

static void encode_pair(byte_t pair[16], qword_t value) {
    qword_t complement = ~value;
    memcpy(pair, &value, sizeof(value));
    memcpy(pair + sizeof(value), &complement, sizeof(complement));
}

static qword_t decode_pair(const byte_t pair[16]) {
    qword_t value;
    qword_t complement;
    memcpy(&value, pair, sizeof(value));
    memcpy(&complement, pair + sizeof(value), sizeof(complement));
    assert(complement == ~value);
    return value;
}

static void test_visibility_and_exclusive_reservation(void) {
    struct guest_page_backing *backing = guest_page_backing_create();
    assert(backing != NULL);
    const struct shared_test_page first_pages[] = {
        {TEST_PAGE, backing},
        {TEST_ALIAS_PAGE, backing},
    };
    const struct shared_test_page second_pages[] = {
        {TEST_PAGE, backing},
    };
    struct shared_test_space first;
    struct shared_test_space second;
    shared_test_space_init(&first, first_pages, 2);
    shared_test_space_init(&second, second_pages, 1);
    struct guest_tlb first_tlb;
    struct guest_tlb second_tlb;
    guest_tlb_init(&first_tlb, &first.address_space);
    guest_tlb_init(&second_tlb, &second.address_space);

    const guest_addr_t address = TEST_PAGE + 64;
    qword_t initial = UINT64_C(0x1020304050607080);
    struct guest_memory_fault fault;
    assert(guest_tlb_write(&first_tlb, address,
            &initial, sizeof(initial), &fault));
    qword_t observed;
    assert(guest_tlb_read(&second_tlb, address,
            &observed, sizeof(observed), GUEST_MEMORY_READ, &fault));
    assert(observed == initial);

    struct guest_tlb_exclusive_token token;
    assert(guest_tlb_load_exclusive(&first_tlb, address,
            &observed, sizeof(observed), &token, &fault));
    assert(observed == initial);
    assert(token.sync_identity == guest_page_sync_identity(
            guest_page_backing_sync(backing)));
    qword_t temporary = UINT64_C(0xa1a2a3a4a5a6a7a8);
    // 值恢复原状也不能恢复已经被另一个 address space 打破的保留。
    assert(guest_tlb_write(&second_tlb, address,
            &temporary, sizeof(temporary), &fault));
    assert(guest_tlb_write(&second_tlb, address,
            &initial, sizeof(initial), &fault));
    qword_t replacement = UINT64_C(0xb1b2b3b4b5b6b7b8);
    assert(guest_tlb_store_exclusive(&first_tlb, address,
            &initial, &replacement, sizeof(replacement), token, &fault) ==
            GUEST_TLB_EXCLUSIVE_FAILED);

    assert(guest_tlb_load_exclusive(&first_tlb, address,
            &observed, sizeof(observed), &token, &fault));
    byte_t unrelated = 0x5a;
    assert(guest_tlb_write(&second_tlb, TEST_PAGE + 96,
            &unrelated, sizeof(unrelated), &fault));
    assert(guest_tlb_store_exclusive(&first_tlb, address,
            &observed, &replacement, sizeof(replacement), token, &fault) ==
            GUEST_TLB_EXCLUSIVE_STORED);

    assert(guest_tlb_load_exclusive(&first_tlb, address,
            &observed, sizeof(observed), &token, &fault));
    qword_t wrong = 0;
    assert(guest_tlb_compare_exchange(&second_tlb, address,
            &wrong, &initial, &wrong, sizeof(wrong), &fault) ==
            GUEST_TLB_COMPARE_EXCHANGE_MISMATCH);
    assert(guest_tlb_store_exclusive(&first_tlb, TEST_ALIAS_PAGE + 64,
            &observed, &initial, sizeof(initial), token, &fault) ==
            GUEST_TLB_EXCLUSIVE_STORED);
    assert(guest_tlb_read(&second_tlb, address,
            &observed, sizeof(observed), GUEST_MEMORY_READ, &fault));
    assert(observed == initial);

    assert(guest_tlb_load_exclusive(&first_tlb, address,
            &observed, sizeof(observed), &token, &fault));
    assert(guest_tlb_write(&first_tlb, TEST_ALIAS_PAGE + 64,
            &temporary, sizeof(temporary), &fault));
    assert(guest_tlb_write(&first_tlb, TEST_ALIAS_PAGE + 64,
            &initial, sizeof(initial), &fault));
    assert(guest_tlb_store_exclusive(&first_tlb, address,
            &initial, &replacement, sizeof(replacement), token, &fault) ==
            GUEST_TLB_EXCLUSIVE_FAILED);

    shared_test_space_destroy(&second);
    shared_test_space_destroy(&first);
    guest_page_backing_release(backing);
}

struct cas_thread_context {
    struct guest_tlb tlb;
    atomic_bool *start;
    guest_addr_t address;
    byte_t replacement[16];
    enum guest_tlb_compare_exchange_result result;
};

static void *cas_once(void *opaque) {
    struct cas_thread_context *context = opaque;
    while (!atomic_load_explicit(context->start, memory_order_acquire))
        sched_yield();
    byte_t expected[16];
    byte_t observed[16];
    encode_pair(expected, 0);
    struct guest_memory_fault fault;
    context->result = guest_tlb_compare_exchange(&context->tlb,
            context->address, expected, context->replacement,
            observed, sizeof(observed), &fault);
    assert(context->result != GUEST_TLB_COMPARE_EXCHANGE_FAULT);
    decode_pair(observed);
    return NULL;
}

static void test_cross_space_compare_exchange(void) {
    struct guest_page_backing *backing = guest_page_backing_create();
    assert(backing != NULL);
    const struct shared_test_page page = {TEST_PAGE, backing};
    struct shared_test_space spaces[2];
    for (unsigned index = 0; index < 2; index++)
        shared_test_space_init(&spaces[index], &page, 1);

    struct guest_tlb initializer;
    guest_tlb_init(&initializer, &spaces[0].address_space);
    byte_t initial[16];
    encode_pair(initial, 0);
    struct guest_memory_fault fault;
    assert(guest_tlb_write(&initializer, TEST_PAGE + TEST_PAIR_OFFSET,
            initial, sizeof(initial), &fault));

    atomic_bool start;
    atomic_init(&start, false);
    struct cas_thread_context contexts[2] = {
        {.start = &start, .address = TEST_PAGE + TEST_PAIR_OFFSET},
        {.start = &start, .address = TEST_PAGE + TEST_PAIR_OFFSET},
    };
    pthread_t threads[2];
    for (unsigned index = 0; index < 2; index++) {
        guest_tlb_init(&contexts[index].tlb, &spaces[index].address_space);
        encode_pair(contexts[index].replacement, index + 1);
        assert(pthread_create(
                &threads[index], NULL, cas_once, &contexts[index]) == 0);
    }
    atomic_store_explicit(&start, true, memory_order_release);
    for (unsigned index = 0; index < 2; index++)
        assert(pthread_join(threads[index], NULL) == 0);
    assert((contexts[0].result == GUEST_TLB_COMPARE_EXCHANGE_EXCHANGED) !=
            (contexts[1].result == GUEST_TLB_COMPARE_EXCHANGE_EXCHANGED));

    byte_t final[16];
    assert(guest_tlb_read(&initializer, TEST_PAGE + TEST_PAIR_OFFSET,
            final, sizeof(final), GUEST_MEMORY_READ, &fault));
    qword_t final_value = decode_pair(final);
    assert(final_value == 1 || final_value == 2);
    for (unsigned index = 0; index < 2; index++)
        shared_test_space_destroy(&spaces[index]);
    guest_page_backing_release(backing);
}

struct pair_writer_context {
    struct guest_tlb tlb;
    atomic_bool *start;
    atomic_bool *done;
    guest_addr_t address;
    unsigned iterations;
};

static void *write_pairs(void *opaque) {
    struct pair_writer_context *context = opaque;
    while (!atomic_load_explicit(context->start, memory_order_acquire))
        sched_yield();
    struct guest_memory_fault fault;
    byte_t pair[16];
    for (unsigned iteration = 1; iteration <= context->iterations;
            iteration++) {
        encode_pair(pair, iteration);
        assert(guest_tlb_write(&context->tlb, context->address,
                pair, sizeof(pair), &fault));
    }
    atomic_store_explicit(context->done, true, memory_order_release);
    return NULL;
}

struct pair_reader_context {
    struct guest_tlb tlb;
    atomic_bool *start;
    atomic_bool *done;
    guest_addr_t address;
};

static void *read_pairs(void *opaque) {
    struct pair_reader_context *context = opaque;
    while (!atomic_load_explicit(context->start, memory_order_acquire))
        sched_yield();
    struct guest_memory_fault fault;
    byte_t pair[16];
    do {
        assert(guest_tlb_read(&context->tlb, context->address,
                pair, sizeof(pair), GUEST_MEMORY_READ, &fault));
        decode_pair(pair);
        sched_yield();
    } while (!atomic_load_explicit(context->done, memory_order_acquire));
    return NULL;
}

static void test_cross_space_reads_are_not_torn(void) {
    struct guest_page_backing *backing = guest_page_backing_create();
    assert(backing != NULL);
    const struct shared_test_page page = {TEST_PAGE, backing};
    struct shared_test_space spaces[2];
    for (unsigned index = 0; index < 2; index++)
        shared_test_space_init(&spaces[index], &page, 1);
    struct guest_tlb initializer;
    guest_tlb_init(&initializer, &spaces[0].address_space);
    byte_t initial[16];
    encode_pair(initial, 0);
    struct guest_memory_fault fault;
    assert(guest_tlb_write(&initializer, TEST_PAGE + TEST_PAIR_OFFSET,
            initial, sizeof(initial), &fault));

    atomic_bool start;
    atomic_bool done;
    atomic_init(&start, false);
    atomic_init(&done, false);
    struct pair_writer_context writer = {
        .start = &start,
        .done = &done,
        .address = TEST_PAGE + TEST_PAIR_OFFSET,
        .iterations = 20000,
    };
    struct pair_reader_context reader = {
        .start = &start,
        .done = &done,
        .address = TEST_PAGE + TEST_PAIR_OFFSET,
    };
    guest_tlb_init(&writer.tlb, &spaces[0].address_space);
    guest_tlb_init(&reader.tlb, &spaces[1].address_space);
    pthread_t writer_thread;
    pthread_t reader_thread;
    assert(pthread_create(&writer_thread, NULL, write_pairs, &writer) == 0);
    assert(pthread_create(&reader_thread, NULL, read_pairs, &reader) == 0);
    atomic_store_explicit(&start, true, memory_order_release);
    assert(pthread_join(writer_thread, NULL) == 0);
    assert(pthread_join(reader_thread, NULL) == 0);

    for (unsigned index = 0; index < 2; index++)
        shared_test_space_destroy(&spaces[index]);
    guest_page_backing_release(backing);
}

struct repeated_write_context {
    struct guest_tlb tlb;
    atomic_bool *start;
    guest_addr_t address;
    byte_t value[16];
    unsigned iterations;
};

static void *write_repeatedly(void *opaque) {
    struct repeated_write_context *context = opaque;
    while (!atomic_load_explicit(context->start, memory_order_acquire))
        sched_yield();
    struct guest_memory_fault fault;
    for (unsigned iteration = 0; iteration < context->iterations;
            iteration++) {
        assert(guest_tlb_write(&context->tlb, context->address,
                context->value, sizeof(context->value), &fault));
    }
    return NULL;
}

static void test_reversed_cross_page_lock_order(void) {
    struct guest_page_backing *first_backing = guest_page_backing_create();
    struct guest_page_backing *second_backing = guest_page_backing_create();
    assert(first_backing != NULL && second_backing != NULL);
    assert(guest_page_sync_identity(guest_page_backing_sync(first_backing)) !=
            guest_page_sync_identity(guest_page_backing_sync(second_backing)));
    // 两个 address space 故意以相反顺序解析同一对后备页。
    const struct shared_test_page first_pages[] = {
        {TEST_PAGE, first_backing},
        {TEST_NEXT_PAGE, second_backing},
    };
    const struct shared_test_page second_pages[] = {
        {TEST_PAGE, second_backing},
        {TEST_NEXT_PAGE, first_backing},
    };
    struct shared_test_space spaces[2];
    shared_test_space_init(&spaces[0], first_pages, 2);
    shared_test_space_init(&spaces[1], second_pages, 2);
    atomic_bool start;
    atomic_init(&start, false);
    struct repeated_write_context contexts[2] = {
        {.start = &start, .address = TEST_NEXT_PAGE - 8,
                .iterations = 10000},
        {.start = &start, .address = TEST_NEXT_PAGE - 8,
                .iterations = 10000},
    };
    pthread_t threads[2];
    for (unsigned index = 0; index < 2; index++) {
        memset(contexts[index].value,
                (int) (0x30 + index), sizeof(contexts[index].value));
        guest_tlb_init(&contexts[index].tlb, &spaces[index].address_space);
        assert(pthread_create(&threads[index], NULL,
                write_repeatedly, &contexts[index]) == 0);
    }
    atomic_store_explicit(&start, true, memory_order_release);
    for (unsigned index = 0; index < 2; index++)
        assert(pthread_join(threads[index], NULL) == 0);

    for (unsigned index = 0; index < 2; index++)
        shared_test_space_destroy(&spaces[index]);
    guest_page_backing_release(second_backing);
    guest_page_backing_release(first_backing);
}

static void test_repeated_sync_is_locked_once(void) {
    struct guest_page_backing *backing = guest_page_backing_create();
    assert(backing != NULL);
    const struct shared_test_page pages[] = {
        {TEST_PAGE, backing},
        {TEST_NEXT_PAGE, backing},
    };
    struct shared_test_space space;
    shared_test_space_init(&space, pages, 2);
    struct guest_tlb tlb;
    guest_tlb_init(&tlb, &space.address_space);

    byte_t written[16];
    for (unsigned index = 0; index < sizeof(written); index++)
        written[index] = (byte_t) (index + 1);
    struct guest_memory_fault fault;
    assert(guest_tlb_write(&tlb, TEST_NEXT_PAGE - 8,
            written, sizeof(written), &fault));
    byte_t observed[16];
    assert(guest_tlb_read(&tlb, TEST_NEXT_PAGE - 8,
            observed, sizeof(observed), GUEST_MEMORY_READ, &fault));
    assert(memcmp(observed, written, sizeof(observed)) == 0);

    shared_test_space_destroy(&space);
    guest_page_backing_release(backing);
}

struct clone_context {
    struct guest_page_backing *source;
    atomic_bool *start;
    size_t offset;
    unsigned iterations;
};

static void *clone_repeatedly(void *opaque) {
    struct clone_context *context = opaque;
    while (!atomic_load_explicit(context->start, memory_order_acquire))
        sched_yield();
    for (unsigned iteration = 0; iteration < context->iterations;
            iteration++) {
        struct guest_page_backing *copy =
                guest_page_backing_clone(context->source);
        assert(copy != NULL);
        byte_t pair[16];
        memcpy(pair, guest_page_backing_bytes(copy) + context->offset,
                sizeof(pair));
        decode_pair(pair);
        guest_page_backing_release(copy);
    }
    return NULL;
}

static void test_clone_observes_consistent_page(void) {
    struct guest_page_backing *backing = guest_page_backing_create();
    assert(backing != NULL);
    const struct shared_test_page page = {TEST_PAGE, backing};
    struct shared_test_space space;
    shared_test_space_init(&space, &page, 1);
    struct guest_tlb initializer;
    guest_tlb_init(&initializer, &space.address_space);
    byte_t initial[16];
    encode_pair(initial, 0);
    struct guest_memory_fault fault;
    assert(guest_tlb_write(&initializer, TEST_PAGE + TEST_PAIR_OFFSET,
            initial, sizeof(initial), &fault));

    atomic_bool start;
    atomic_bool done;
    atomic_init(&start, false);
    atomic_init(&done, false);
    struct pair_writer_context writer = {
        .start = &start,
        .done = &done,
        .address = TEST_PAGE + TEST_PAIR_OFFSET,
        .iterations = 10000,
    };
    guest_tlb_init(&writer.tlb, &space.address_space);
    struct clone_context cloner = {
        .source = backing,
        .start = &start,
        .offset = TEST_PAIR_OFFSET,
        .iterations = 2000,
    };
    pthread_t writer_thread;
    pthread_t clone_thread;
    assert(pthread_create(&writer_thread, NULL, write_pairs, &writer) == 0);
    assert(pthread_create(&clone_thread, NULL,
            clone_repeatedly, &cloner) == 0);
    atomic_store_explicit(&start, true, memory_order_release);
    assert(pthread_join(writer_thread, NULL) == 0);
    assert(pthread_join(clone_thread, NULL) == 0);

    shared_test_space_destroy(&space);
    guest_page_backing_release(backing);
}

int main(void) {
    test_visibility_and_exclusive_reservation();
    test_cross_space_compare_exchange();
    test_cross_space_reads_are_not_torn();
    test_reversed_cross_page_lock_order();
    test_repeated_sync_is_locked_once();
    test_clone_observes_consistent_page();
    return 0;
}
