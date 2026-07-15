#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <string.h>

#include "guest/memory/tlb.h"

#define PAGE_A UINT64_C(0x0000123456789000)
#define PAGE_B (PAGE_A + (GUEST_TLB_SIZE * GUEST_MEMORY_PAGE_SIZE))
#define PAGE_NEXT (PAGE_A + GUEST_MEMORY_PAGE_SIZE)

struct test_page {
    guest_addr_t address;
    byte_t *host_page;
    unsigned permissions;
};

struct test_memory {
    struct test_page pages[3];
    byte_t first[GUEST_MEMORY_PAGE_SIZE];
    byte_t replacement[GUEST_MEMORY_PAGE_SIZE];
    byte_t next[GUEST_MEMORY_PAGE_SIZE];
    byte_t collision[GUEST_MEMORY_PAGE_SIZE];
    unsigned resolutions;
    unsigned read_locks;
    unsigned read_unlocks;
    unsigned read_lock_depth;
    unsigned write_locks;
    unsigned write_unlocks;
    unsigned write_lock_depth;
};

static bool read_lock(void *opaque) {
    struct test_memory *memory = opaque;
    assert(memory->read_lock_depth == 0);
    memory->read_lock_depth = 1;
    memory->read_locks++;
    return true;
}

static void read_unlock(void *opaque, bool locked) {
    struct test_memory *memory = opaque;
    assert(locked);
    assert(memory->read_lock_depth == 1);
    memory->read_lock_depth = 0;
    memory->read_unlocks++;
}

static bool write_lock(void *opaque) {
    struct test_memory *memory = opaque;
    assert(memory->read_lock_depth == 0 && memory->write_lock_depth == 0);
    memory->write_lock_depth = 1;
    memory->write_locks++;
    return true;
}

static void write_unlock(void *opaque, bool locked) {
    struct test_memory *memory = opaque;
    assert(locked);
    assert(memory->write_lock_depth == 1);
    memory->write_lock_depth = 0;
    memory->write_unlocks++;
}

static enum guest_memory_fault_kind resolve_test_page(void *opaque,
        guest_addr_t page_base, enum guest_memory_access access,
        struct guest_page_view *view) {
    struct test_memory *memory = opaque;
    use(access);
    assert(memory->read_lock_depth == 1 || memory->write_lock_depth == 1);
    memory->resolutions++;
    for (unsigned i = 0; i < array_size(memory->pages); i++) {
        if (memory->pages[i].address == page_base) {
            *view = (struct guest_page_view) {
                .host_page = memory->pages[i].host_page,
                .permissions = memory->pages[i].permissions,
            };
            return GUEST_MEMORY_FAULT_NONE;
        }
    }
    return GUEST_MEMORY_FAULT_UNMAPPED;
}

static const struct guest_address_space_ops test_ops = {
    .read_lock = read_lock,
    .read_unlock = read_unlock,
    .write_lock = write_lock,
    .write_unlock = write_unlock,
    .resolve_page = resolve_test_page,
};

static void test_exclusive_token_address_space_identity(void) {
    struct test_memory memories[2] = {
        {.pages = {{PAGE_A, NULL,
                GUEST_MEMORY_READ | GUEST_MEMORY_WRITE}}},
        {.pages = {{PAGE_A, NULL,
                GUEST_MEMORY_READ | GUEST_MEMORY_WRITE}}},
    };
    struct guest_address_space spaces[2];
    struct guest_tlb tlbs[2];
    struct guest_tlb_exclusive_token tokens[2];
    struct guest_memory_fault fault;
    byte_t values[2];
    for (unsigned index = 0; index < array_size(memories); index++) {
        memories[index].pages[0].host_page = memories[index].first;
        memories[index].first[32] = 0x31;
        guest_address_space_init(
                &spaces[index], &test_ops, &memories[index], 48);
        guest_tlb_init(&tlbs[index], &spaces[index]);
        assert(guest_tlb_load_exclusive(&tlbs[index], PAGE_A + 32,
                &values[index], 1, &tokens[index], &fault));
    }
    assert(tokens[0].mapping_generation == tokens[1].mapping_generation);
    assert(tokens[0].write_generation == tokens[1].write_generation);

    byte_t replacement = 0x42;
    assert(guest_tlb_store_exclusive(&tlbs[1], PAGE_A + 32,
            &values[0], &replacement, 1, tokens[0], &fault) ==
            GUEST_TLB_EXCLUSIVE_FAILED);
    assert(memories[1].first[32] == values[1]);
    assert(guest_tlb_store_exclusive(&tlbs[1], PAGE_A + 32,
            &values[1], &replacement, 1, tokens[1], &fault) ==
            GUEST_TLB_EXCLUSIVE_STORED);
    assert(memories[1].first[32] == replacement);

    const byte_t initial_pair[8] = {
        0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87,
    };
    const byte_t replacement_pair[8] = {
        0x89, 0x9a, 0xab, 0xbc, 0xcd, 0xde, 0xef, 0xf0,
    };
    struct guest_tlb_exclusive_token pair_tokens[2];
    byte_t pair_values[2][sizeof(initial_pair)];
    for (unsigned index = 0; index < array_size(memories); index++) {
        memcpy(memories[index].first + 64,
                initial_pair, sizeof(initial_pair));
        assert(guest_tlb_load_exclusive(&tlbs[index], PAGE_A + 64,
                pair_values[index], sizeof(pair_values[index]),
                &pair_tokens[index], &fault));
    }

    byte_t observed[sizeof(initial_pair)];
    assert(guest_tlb_compare_exchange(&tlbs[1], PAGE_A + 64,
            initial_pair, replacement_pair, observed, sizeof(observed),
            &fault) == GUEST_TLB_COMPARE_EXCHANGE_EXCHANGED);
    assert(memcmp(observed, initial_pair, sizeof(observed)) == 0);
    assert(memcmp(memories[0].first + 64,
            initial_pair, sizeof(initial_pair)) == 0);
    assert(memcmp(memories[1].first + 64,
            replacement_pair, sizeof(replacement_pair)) == 0);
    assert(guest_address_space_exclusive_matches(&spaces[0],
            PAGE_A + 64, pair_tokens[0].write_generation));
    assert(!guest_address_space_exclusive_matches(&spaces[1],
            PAGE_A + 64, pair_tokens[1].write_generation));
}

static void test_compare_exchange_transaction(struct test_memory *memory,
        struct guest_address_space *space, struct guest_tlb *tlb) {
    byte_t *first = memory->pages[0].host_page;
    const guest_addr_t address = PAGE_A + 128;
    const byte_t initial[8] = {
        0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
    };
    const byte_t replacement[8] = {
        0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10,
    };
    memcpy(first + 128, initial, sizeof(initial));

    struct guest_memory_fault fault;
    struct guest_tlb_exclusive_token token;
    byte_t expected[sizeof(initial)];
    assert(guest_tlb_load_exclusive(tlb, address, expected,
            sizeof(expected), &token, &fault));
    unsigned write_locks = memory->write_locks;
    byte_t observed[16];
    memset(observed, 0xa5, sizeof(observed));
    assert(guest_tlb_compare_exchange(tlb, address,
            expected, replacement, observed, sizeof(expected), &fault) ==
            GUEST_TLB_COMPARE_EXCHANGE_EXCHANGED);
    assert(memory->write_locks == write_locks + 1 &&
            memory->write_unlocks == memory->write_locks);
    assert(memcmp(observed, initial, sizeof(initial)) == 0);
    assert(memcmp(first + 128, replacement, sizeof(replacement)) == 0);
    assert(!guest_address_space_exclusive_matches(
            space, address, token.write_generation));

    assert(guest_tlb_load_exclusive(tlb, address, expected,
            sizeof(expected), &token, &fault));
    byte_t wrong_expected[sizeof(expected)];
    memset(wrong_expected, 0x3c, sizeof(wrong_expected));
    memset(observed, 0xa5, sizeof(observed));
    assert(guest_tlb_compare_exchange(tlb, address,
            wrong_expected, initial, observed, sizeof(expected), &fault) ==
            GUEST_TLB_COMPARE_EXCHANGE_MISMATCH);
    assert(fault.kind == GUEST_MEMORY_FAULT_NONE &&
            fault.access == GUEST_MEMORY_WRITE);
    assert(memcmp(observed, replacement, sizeof(replacement)) == 0);
    assert(memcmp(first + 128, replacement, sizeof(replacement)) == 0);
    assert(guest_address_space_exclusive_matches(
            space, address, token.write_generation));

    assert(guest_tlb_compare_exchange(tlb, address,
            replacement, replacement, observed, sizeof(replacement),
            &fault) == GUEST_TLB_COMPARE_EXCHANGE_EXCHANGED);
    assert(memcmp(observed, replacement, sizeof(replacement)) == 0);
    assert(memcmp(first + 128, replacement, sizeof(replacement)) == 0);
    assert(!guest_address_space_exclusive_matches(
            space, address, token.write_generation));

    // CAS/CASP 会把旧内存值写回期望寄存器，因此输入与输出允许重叠。
    memcpy(expected, replacement, sizeof(expected));
    assert(guest_tlb_compare_exchange(tlb, address,
            expected, initial, expected, sizeof(expected), &fault) ==
            GUEST_TLB_COMPARE_EXCHANGE_EXCHANGED);
    assert(memcmp(expected, replacement, sizeof(expected)) == 0);
    assert(memcmp(first + 128, initial, sizeof(initial)) == 0);

    const byte_t observed_sentinel[16] = {
        0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
        0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf,
    };
    memcpy(observed, observed_sentinel, sizeof(observed));
    byte_t readonly_before[8];
    memcpy(readonly_before, memory->collision, sizeof(readonly_before));
    assert(guest_tlb_compare_exchange(tlb, PAGE_B,
            initial, replacement, observed, 8, &fault) ==
            GUEST_TLB_COMPARE_EXCHANGE_FAULT);
    assert(fault.kind == GUEST_MEMORY_FAULT_PERMISSION &&
            fault.address == PAGE_B && fault.access == GUEST_MEMORY_WRITE);
    assert(memcmp(observed, observed_sentinel, sizeof(observed)) == 0);
    assert(memcmp(memory->collision,
            readonly_before, sizeof(readonly_before)) == 0);

    memcpy(observed, observed_sentinel, sizeof(observed));
    assert(guest_tlb_compare_exchange(tlb,
            PAGE_B + GUEST_MEMORY_PAGE_SIZE,
            initial, replacement, observed, 8, &fault) ==
            GUEST_TLB_COMPARE_EXCHANGE_FAULT);
    assert(fault.kind == GUEST_MEMORY_FAULT_UNMAPPED &&
            fault.access == GUEST_MEMORY_WRITE);
    assert(memcmp(observed, observed_sentinel, sizeof(observed)) == 0);

    memcpy(observed, observed_sentinel, sizeof(observed));
    assert(guest_tlb_compare_exchange(tlb,
            (UINT64_C(1) << 48) - 8,
            observed_sentinel, observed_sentinel, observed, 16, &fault) ==
            GUEST_TLB_COMPARE_EXCHANGE_FAULT);
    assert(fault.kind == GUEST_MEMORY_FAULT_ADDRESS_SIZE &&
            fault.address == (UINT64_C(1) << 48) &&
            fault.access == GUEST_MEMORY_WRITE);
    assert(memcmp(observed, observed_sentinel, sizeof(observed)) == 0);

    const byte_t cross_initial[16] = {
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
        0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
    };
    const byte_t cross_replacement[16] = {
        0xff, 0xee, 0xdd, 0xcc, 0xbb, 0xaa, 0x99, 0x88,
        0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x00,
    };
    memcpy(first + GUEST_MEMORY_PAGE_SIZE - 8, cross_initial, 8);
    memcpy(memory->next, cross_initial + 8, 8);
    unsigned next_permissions = memory->pages[1].permissions;
    memory->pages[1].permissions = GUEST_MEMORY_READ;
    guest_address_space_changed(space);
    struct guest_tlb_exclusive_token cross_fault_tokens[2];
    byte_t reserved_bytes[2];
    assert(guest_tlb_load_exclusive(tlb, PAGE_NEXT - 8,
            &reserved_bytes[0], 1, &cross_fault_tokens[0], &fault));
    assert(guest_tlb_load_exclusive(tlb, PAGE_NEXT,
            &reserved_bytes[1], 1, &cross_fault_tokens[1], &fault));
    memcpy(observed, observed_sentinel, sizeof(observed));
    assert(guest_tlb_compare_exchange(tlb, PAGE_NEXT - 8,
            cross_initial, cross_replacement, observed, sizeof(observed),
            &fault) == GUEST_TLB_COMPARE_EXCHANGE_FAULT);
    assert(fault.kind == GUEST_MEMORY_FAULT_PERMISSION &&
            fault.address == PAGE_NEXT && fault.access == GUEST_MEMORY_WRITE);
    assert(memcmp(observed, observed_sentinel, sizeof(observed)) == 0);
    assert(memcmp(first + GUEST_MEMORY_PAGE_SIZE - 8,
            cross_initial, 8) == 0);
    assert(memcmp(memory->next, cross_initial + 8, 8) == 0);
    assert(guest_address_space_exclusive_matches(space,
            PAGE_NEXT - 8, cross_fault_tokens[0].write_generation));
    assert(guest_address_space_exclusive_matches(space,
            PAGE_NEXT, cross_fault_tokens[1].write_generation));

    // 比较交换只要求写权限，第二页无需额外开放普通读权限。
    memory->pages[1].permissions = GUEST_MEMORY_WRITE;
    guest_address_space_changed(space);
    assert(guest_tlb_compare_exchange(tlb, PAGE_NEXT - 8,
            cross_initial, cross_replacement, observed, sizeof(observed),
            &fault) == GUEST_TLB_COMPARE_EXCHANGE_EXCHANGED);
    assert(memcmp(observed, cross_initial, sizeof(observed)) == 0);
    assert(memcmp(first + GUEST_MEMORY_PAGE_SIZE - 8,
            cross_replacement, 8) == 0);
    assert(memcmp(memory->next, cross_replacement + 8, 8) == 0);

    memcpy(first + GUEST_MEMORY_PAGE_SIZE - 8, cross_initial, 8);
    memcpy(memory->next, cross_initial + 8, 8);
    memory->pages[1].permissions =
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE;
    guest_address_space_changed(space);
    struct guest_tlb_exclusive_token cross_success_tokens[2];
    assert(guest_tlb_load_exclusive(tlb, PAGE_NEXT - 8,
            &reserved_bytes[0], 1, &cross_success_tokens[0], &fault));
    assert(guest_tlb_load_exclusive(tlb, PAGE_NEXT,
            &reserved_bytes[1], 1, &cross_success_tokens[1], &fault));
    assert(guest_tlb_compare_exchange(tlb, PAGE_NEXT - 8,
            cross_initial, cross_replacement, observed, sizeof(observed),
            &fault) == GUEST_TLB_COMPARE_EXCHANGE_EXCHANGED);
    assert(!guest_address_space_exclusive_matches(space,
            PAGE_NEXT - 8, cross_success_tokens[0].write_generation));
    assert(!guest_address_space_exclusive_matches(space,
            PAGE_NEXT, cross_success_tokens[1].write_generation));
    memory->pages[1].permissions = next_permissions;
    guest_address_space_changed(space);
}

struct concurrent_test_memory {
    byte_t first[GUEST_MEMORY_PAGE_SIZE];
    byte_t next[GUEST_MEMORY_PAGE_SIZE];
    pthread_rwlock_t lock;
    struct guest_address_space space;
};

static bool concurrent_read_lock(void *opaque) {
    struct concurrent_test_memory *memory = opaque;
    assert(pthread_rwlock_rdlock(&memory->lock) == 0);
    return true;
}

static void concurrent_read_unlock(void *opaque, bool locked) {
    struct concurrent_test_memory *memory = opaque;
    assert(locked);
    assert(pthread_rwlock_unlock(&memory->lock) == 0);
}

static bool concurrent_write_lock(void *opaque) {
    struct concurrent_test_memory *memory = opaque;
    assert(pthread_rwlock_wrlock(&memory->lock) == 0);
    return true;
}

static void concurrent_write_unlock(void *opaque, bool locked) {
    struct concurrent_test_memory *memory = opaque;
    assert(locked);
    assert(pthread_rwlock_unlock(&memory->lock) == 0);
}

static enum guest_memory_fault_kind resolve_concurrent_page(void *opaque,
        guest_addr_t page_base, enum guest_memory_access access,
        struct guest_page_view *view) {
    struct concurrent_test_memory *memory = opaque;
    use(access);
    if (page_base == PAGE_A) {
        view->host_page = memory->first;
    } else if (page_base == PAGE_NEXT) {
        view->host_page = memory->next;
    } else {
        return GUEST_MEMORY_FAULT_UNMAPPED;
    }
    view->permissions = GUEST_MEMORY_READ | GUEST_MEMORY_WRITE;
    return GUEST_MEMORY_FAULT_NONE;
}

static const struct guest_address_space_ops concurrent_test_ops = {
    .read_lock = concurrent_read_lock,
    .read_unlock = concurrent_read_unlock,
    .write_lock = concurrent_write_lock,
    .write_unlock = concurrent_write_unlock,
    .resolve_page = resolve_concurrent_page,
};

static void encode_consistent_pair(byte_t pair[16], qword_t value) {
    qword_t complement = ~value;
    memcpy(pair, &value, sizeof(value));
    memcpy(pair + sizeof(value), &complement, sizeof(complement));
}

static qword_t consistent_pair_value(const byte_t pair[16]) {
    qword_t value;
    qword_t complement;
    memcpy(&value, pair, sizeof(value));
    memcpy(&complement, pair + sizeof(value), sizeof(complement));
    assert(complement == ~value);
    return value;
}

struct compare_exchange_writer_context {
    struct guest_tlb tlb;
    atomic_bool *start;
    atomic_bool *reader_ready;
    atomic_uint *first_reads;
    atomic_uint *finished;
    atomic_uint *mismatches;
    guest_addr_t address;
    unsigned writers;
    unsigned iterations;
};

static void *compare_exchange_writer(void *opaque) {
    struct compare_exchange_writer_context *context = opaque;
    while (!atomic_load_explicit(context->start, memory_order_acquire))
        sched_yield();

    struct guest_memory_fault fault;
    byte_t expected[16];
    byte_t replacement[16];
    for (unsigned iteration = 0; iteration < context->iterations;
            iteration++) {
        assert(guest_tlb_read(&context->tlb, context->address,
                expected, sizeof(expected), GUEST_MEMORY_READ, &fault));
        if (iteration == 0) {
            atomic_fetch_add_explicit(context->first_reads, 1,
                    memory_order_release);
            while (atomic_load_explicit(context->first_reads,
                    memory_order_acquire) != context->writers)
                sched_yield();
            while (!atomic_load_explicit(
                    context->reader_ready, memory_order_acquire))
                sched_yield();
        }

        for (;;) {
            qword_t value = consistent_pair_value(expected);
            encode_consistent_pair(replacement, value + 1);
            enum guest_tlb_compare_exchange_result result =
                    guest_tlb_compare_exchange(&context->tlb,
                            context->address, expected, replacement,
                            expected, sizeof(expected), &fault);
            assert(result != GUEST_TLB_COMPARE_EXCHANGE_FAULT);
            consistent_pair_value(expected);
            if (result == GUEST_TLB_COMPARE_EXCHANGE_EXCHANGED)
                break;
            assert(result == GUEST_TLB_COMPARE_EXCHANGE_MISMATCH);
            atomic_fetch_add_explicit(context->mismatches, 1,
                    memory_order_relaxed);
            sched_yield();
        }
    }
    atomic_fetch_add_explicit(context->finished, 1, memory_order_release);
    return NULL;
}

struct compare_exchange_reader_context {
    struct guest_tlb tlb;
    atomic_bool *start;
    atomic_bool *reader_ready;
    atomic_uint *finished;
    atomic_uint *reads;
    guest_addr_t address;
    unsigned writers;
};

static void *compare_exchange_reader(void *opaque) {
    struct compare_exchange_reader_context *context = opaque;
    while (!atomic_load_explicit(context->start, memory_order_acquire))
        sched_yield();

    struct guest_memory_fault fault;
    byte_t observed[16];
    assert(guest_tlb_read(&context->tlb, context->address,
            observed, sizeof(observed), GUEST_MEMORY_READ, &fault));
    consistent_pair_value(observed);
    atomic_fetch_add_explicit(context->reads, 1, memory_order_relaxed);
    atomic_store_explicit(context->reader_ready, true, memory_order_release);
    while (atomic_load_explicit(context->finished,
            memory_order_acquire) != context->writers) {
        assert(guest_tlb_read(&context->tlb, context->address,
                observed, sizeof(observed), GUEST_MEMORY_READ, &fault));
        consistent_pair_value(observed);
        atomic_fetch_add_explicit(context->reads, 1, memory_order_relaxed);
        sched_yield();
    }
    return NULL;
}

static void test_concurrent_compare_exchange_pair(void) {
    enum { writer_count = 2, iterations = 5000 };
    struct concurrent_test_memory memory = {0};
    assert(pthread_rwlock_init(&memory.lock, NULL) == 0);
    guest_address_space_init(
            &memory.space, &concurrent_test_ops, &memory, 48);
    const guest_addr_t address = PAGE_NEXT - 8;
    byte_t initial[16];
    encode_consistent_pair(initial, 0);
    memcpy(memory.first + GUEST_MEMORY_PAGE_SIZE - 8, initial, 8);
    memcpy(memory.next, initial + 8, 8);

    atomic_bool start;
    atomic_bool reader_ready;
    atomic_uint first_reads;
    atomic_uint finished;
    atomic_uint mismatches;
    atomic_uint reads;
    atomic_init(&start, false);
    atomic_init(&reader_ready, false);
    atomic_init(&first_reads, 0);
    atomic_init(&finished, 0);
    atomic_init(&mismatches, 0);
    atomic_init(&reads, 0);

    struct compare_exchange_writer_context writers[writer_count];
    pthread_t writer_threads[writer_count];
    for (unsigned index = 0; index < writer_count; index++) {
        writers[index] = (struct compare_exchange_writer_context) {
            .start = &start,
            .reader_ready = &reader_ready,
            .first_reads = &first_reads,
            .finished = &finished,
            .mismatches = &mismatches,
            .address = address,
            .writers = writer_count,
            .iterations = iterations,
        };
        guest_tlb_init(&writers[index].tlb, &memory.space);
        assert(pthread_create(&writer_threads[index], NULL,
                compare_exchange_writer, &writers[index]) == 0);
    }

    struct compare_exchange_reader_context reader = {
        .start = &start,
        .reader_ready = &reader_ready,
        .finished = &finished,
        .reads = &reads,
        .address = address,
        .writers = writer_count,
    };
    guest_tlb_init(&reader.tlb, &memory.space);
    pthread_t reader_thread;
    assert(pthread_create(&reader_thread, NULL,
            compare_exchange_reader, &reader) == 0);
    atomic_store_explicit(&start, true, memory_order_release);

    for (unsigned index = 0; index < writer_count; index++)
        assert(pthread_join(writer_threads[index], NULL) == 0);
    assert(pthread_join(reader_thread, NULL) == 0);
    byte_t final[16];
    struct guest_memory_fault fault;
    assert(guest_tlb_read(&reader.tlb, address, final,
            sizeof(final), GUEST_MEMORY_READ, &fault));
    assert(consistent_pair_value(final) == writer_count * iterations);
    assert(atomic_load_explicit(&mismatches, memory_order_relaxed) != 0);
    assert(atomic_load_explicit(&reads, memory_order_relaxed) != 0);
    assert(pthread_rwlock_destroy(&memory.lock) == 0);
}

int main(void) {
    struct test_memory memory = {
        .pages = {
            {PAGE_A, NULL, GUEST_MEMORY_READ | GUEST_MEMORY_WRITE |
                    GUEST_MEMORY_EXECUTE},
            {PAGE_NEXT, NULL, GUEST_MEMORY_READ},
            {PAGE_B, NULL, GUEST_MEMORY_READ},
        },
    };
    memory.pages[0].host_page = memory.first;
    memory.pages[1].host_page = memory.next;
    memory.pages[2].host_page = memory.collision;
    memory.first[0] = 0x11;
    memory.collision[0] = 0x22;

    struct guest_address_space space;
    guest_address_space_init(&space, &test_ops, &memory, 48);
    struct guest_tlb tlb;
    guest_tlb_init(&tlb, &space);
    assert(space.exclusive_sequence == 0);
    struct guest_memory_fault fault;
    byte_t value;

    assert(guest_tlb_read(&tlb, PAGE_A, &value, 1,
            GUEST_MEMORY_READ, &fault));
    assert(value == 0x11);

    struct test_memory other_memory = {
        .pages = {
            {PAGE_A, NULL, GUEST_MEMORY_READ},
        },
    };
    other_memory.pages[0].host_page = other_memory.first;
    other_memory.first[0] = 0x44;
    struct guest_address_space other_space;
    guest_address_space_init(&other_space, &test_ops, &other_memory, 48);
    assert(space.generation == other_space.generation);
    guest_tlb_bind(&tlb, &other_space);
    assert(guest_tlb_read(&tlb, PAGE_A, &value, 1,
            GUEST_MEMORY_READ, &fault));
    assert(value == 0x44);
    guest_tlb_bind(&tlb, &space);
    assert(guest_tlb_read(&tlb, PAGE_A, &value, 1,
            GUEST_MEMORY_READ, &fault));
    assert(value == 0x11);

    unsigned resolutions = memory.resolutions;
    guest_tlb_bind(&tlb, &space);
    assert(guest_tlb_read(&tlb, PAGE_A, &value, 1,
            GUEST_MEMORY_READ, &fault));
    assert(memory.resolutions == resolutions + 1);
    assert(guest_tlb_read(&tlb, PAGE_A, &value, 1,
            GUEST_MEMORY_EXECUTE, &fault));
    assert(value == 0x11);
    assert(guest_tlb_read(&tlb, PAGE_B, &value, 1,
            GUEST_MEMORY_READ, &fault));
    assert(value == 0x22);
    assert(guest_tlb_read(&tlb, PAGE_A, &value, 1,
            GUEST_MEMORY_READ, &fault));
    assert(value == 0x11);

    memory.replacement[0] = 0x33;
    memory.pages[0].host_page = memory.replacement;
    guest_address_space_changed(&space);
    assert(guest_tlb_read(&tlb, PAGE_A, &value, 1,
            GUEST_MEMORY_READ, &fault));
    assert(value == 0x33);

    struct guest_tlb_exclusive_token failed_first;
    struct guest_tlb_exclusive_token failed_next;
    byte_t failed_first_value;
    byte_t failed_next_value;
    assert(guest_tlb_load_exclusive(&tlb, PAGE_NEXT - 2,
            &failed_first_value, 1, &failed_first, &fault));
    assert(guest_tlb_load_exclusive(&tlb, PAGE_NEXT,
            &failed_next_value, 1, &failed_next, &fault));
    assert(!guest_tlb_write(&tlb, PAGE_NEXT, &value, 1, &fault));
    assert(guest_address_space_exclusive_matches(&space,
            PAGE_NEXT - 2, failed_first.write_generation));
    assert(guest_address_space_exclusive_matches(&space,
            PAGE_NEXT, failed_next.write_generation));
    assert(fault.kind == GUEST_MEMORY_FAULT_PERMISSION);
    assert(fault.address == PAGE_NEXT);
    assert(fault.access == GUEST_MEMORY_WRITE);
    assert(!guest_tlb_read(&tlb, PAGE_NEXT, &value, 1,
            GUEST_MEMORY_EXECUTE, &fault));
    assert(fault.kind == GUEST_MEMORY_FAULT_PERMISSION);
    assert(!guest_tlb_read(&tlb, PAGE_B + GUEST_MEMORY_PAGE_SIZE,
            &value, 1, GUEST_MEMORY_READ, &fault));
    assert(fault.kind == GUEST_MEMORY_FAULT_UNMAPPED);
    assert(!guest_tlb_read(&tlb, UINT64_C(1) << 48,
            &value, 1, GUEST_MEMORY_READ, &fault));
    assert(fault.kind == GUEST_MEMORY_FAULT_ADDRESS_SIZE);
    byte_t boundary[4];
    assert(!guest_tlb_read(&tlb, (UINT64_C(1) << 48) - 2,
            boundary, sizeof(boundary), GUEST_MEMORY_READ, &fault));
    assert(fault.kind == GUEST_MEMORY_FAULT_ADDRESS_SIZE);
    assert(fault.address == (UINT64_C(1) << 48));

    const byte_t cross_page[] = {0xa1, 0xb2, 0xc3, 0xd4};
    byte_t output[sizeof(cross_page)] = {0};
    memcpy(&memory.replacement[GUEST_MEMORY_PAGE_SIZE - 2], cross_page, 2);
    memcpy(memory.next, cross_page + 2, 2);
    assert(guest_tlb_read(&tlb, PAGE_NEXT - 2, output, sizeof(output),
            GUEST_MEMORY_READ, &fault));
    assert(memcmp(output, cross_page, sizeof(output)) == 0);

    const byte_t before_first[] = {
        memory.replacement[GUEST_MEMORY_PAGE_SIZE - 2],
        memory.replacement[GUEST_MEMORY_PAGE_SIZE - 1],
    };
    const byte_t before_next[] = {memory.next[0], memory.next[1]};
    const byte_t write_value[] = {0x10, 0x20, 0x30, 0x40};
    assert(!guest_tlb_write(&tlb, PAGE_NEXT - 2,
            write_value, sizeof(write_value), &fault));
    assert(fault.kind == GUEST_MEMORY_FAULT_PERMISSION);
    assert(fault.address == PAGE_NEXT);
    assert(memcmp(&memory.replacement[GUEST_MEMORY_PAGE_SIZE - 2],
            before_first, sizeof(before_first)) == 0);
    assert(memcmp(memory.next, before_next, sizeof(before_next)) == 0);
    assert(guest_address_space_exclusive_matches(&space,
            PAGE_NEXT - 2, failed_first.write_generation));
    assert(guest_address_space_exclusive_matches(&space,
            PAGE_NEXT, failed_next.write_generation));

    memory.pages[1].permissions |= GUEST_MEMORY_WRITE;
    guest_address_space_changed(&space);
    struct guest_tlb_exclusive_token written_first;
    struct guest_tlb_exclusive_token written_next;
    assert(guest_tlb_load_exclusive(&tlb, PAGE_NEXT - 2,
            &failed_first_value, 1, &written_first, &fault));
    assert(guest_tlb_load_exclusive(&tlb, PAGE_NEXT,
            &failed_next_value, 1, &written_next, &fault));
    assert(guest_tlb_write(&tlb, PAGE_NEXT - 2,
            write_value, sizeof(write_value), &fault));
    assert(!guest_address_space_exclusive_matches(&space,
            PAGE_NEXT - 2, written_first.write_generation));
    assert(!guest_address_space_exclusive_matches(&space,
            PAGE_NEXT, written_next.write_generation));
    memset(output, 0, sizeof(output));
    assert(guest_tlb_read(&tlb, PAGE_NEXT - 2, output, sizeof(output),
            GUEST_MEMORY_READ, &fault));
    assert(memcmp(output, write_value, sizeof(output)) == 0);

    struct guest_tlb_exclusive_token span_tokens[3];
    byte_t span_values[3];
    for (unsigned index = 0; index < array_size(span_tokens); index++) {
        assert(guest_tlb_load_exclusive(&tlb,
                PAGE_A + index * GUEST_MEMORY_EXCLUSIVE_GRANULE_SIZE,
                &span_values[index], 1, &span_tokens[index], &fault));
    }
    byte_t span_write[GUEST_TLB_MAX_ACCESS_SIZE];
    memset(span_write, 0xa5, sizeof(span_write));
    assert(guest_tlb_write(&tlb,
            PAGE_A + GUEST_MEMORY_EXCLUSIVE_GRANULE_SIZE - 1,
            span_write, sizeof(span_write), &fault));
    for (unsigned index = 0; index < array_size(span_tokens); index++) {
        assert(!guest_address_space_exclusive_matches(&space,
                PAGE_A + index * GUEST_MEMORY_EXCLUSIVE_GRANULE_SIZE,
                span_tokens[index].write_generation));
    }

    memory.replacement[64] = 0x5a;
    struct guest_tlb_exclusive_token token;
    assert(guest_tlb_load_exclusive(&tlb, PAGE_A + 64,
            &value, 1, &token, &fault));
    assert(value == 0x5a && token.address_space == &space);
    byte_t unrelated = 0x9d;
    assert(guest_tlb_write(&tlb, PAGE_A + 96,
            &unrelated, 1, &fault));
    byte_t replacement = 0x6b;
    assert(guest_tlb_store_exclusive(&tlb, PAGE_A + 64,
            &value, &replacement, 1, token, &fault) ==
            GUEST_TLB_EXCLUSIVE_STORED);
    assert(memory.replacement[64] == replacement);

    byte_t stale_replacement = 0x7c;
    assert(guest_tlb_store_exclusive(&tlb, PAGE_A + 64,
            &replacement, &stale_replacement, 1, token, &fault) ==
            GUEST_TLB_EXCLUSIVE_FAILED);
    assert(memory.replacement[64] == replacement);

    assert(guest_tlb_load_exclusive(&tlb, PAGE_A + 64,
            &value, 1, &token, &fault));
    byte_t wrong_expected = 0xff;
    assert(guest_tlb_store_exclusive(&tlb, PAGE_A + 64,
            &wrong_expected, &stale_replacement, 1,
            token, &fault) == GUEST_TLB_EXCLUSIVE_FAILED);
    assert(guest_address_space_exclusive_matches(&space,
            PAGE_A + 64, token.write_generation));

    struct guest_tlb_exclusive_token readonly_token;
    assert(guest_tlb_load_exclusive(&tlb, PAGE_B,
            &value, 1, &readonly_token, &fault));
    assert(guest_tlb_store_exclusive(&tlb, PAGE_B,
            &value, &stale_replacement, 1, readonly_token, &fault) ==
            GUEST_TLB_EXCLUSIVE_FAULT);
    assert(fault.kind == GUEST_MEMORY_FAULT_PERMISSION);
    assert(fault.address == PAGE_B && fault.access == GUEST_MEMORY_WRITE);

    assert(guest_tlb_load_exclusive(&tlb, PAGE_A + 64,
            &value, 1, &token, &fault));
    assert(guest_tlb_write(&tlb, PAGE_A, &replacement, 0, &fault));
    assert(guest_tlb_store_exclusive(&tlb, PAGE_A + 64,
            &value, &stale_replacement, 1, token, &fault) ==
            GUEST_TLB_EXCLUSIVE_STORED);

    assert(guest_tlb_load_exclusive(&tlb, PAGE_A + 64,
            &value, 1, &token, &fault));
    byte_t *mapped_page = memory.pages[0].host_page;
    memory.pages[0].host_page = NULL;
    guest_address_space_changed(&space);
    unsigned stale_resolutions = memory.resolutions;
    assert(guest_tlb_store_exclusive(&tlb, PAGE_A + 64,
            &value, &replacement, 1, token, &fault) ==
            GUEST_TLB_EXCLUSIVE_FAILED);
    assert(memory.resolutions == stale_resolutions &&
            mapped_page[64] == value);
    memory.pages[0].host_page = mapped_page;
    guest_address_space_changed(&space);
    test_compare_exchange_transaction(&memory, &space, &tlb);
    assert(memory.read_lock_depth == 0 &&
            memory.read_locks != 0 &&
            memory.read_locks == memory.read_unlocks &&
            memory.write_lock_depth == 0 &&
            memory.write_locks != 0 &&
            memory.write_locks == memory.write_unlocks &&
            other_memory.read_lock_depth == 0 &&
            other_memory.read_locks == other_memory.read_unlocks &&
            other_memory.write_lock_depth == 0 &&
            other_memory.write_locks == other_memory.write_unlocks);
    test_exclusive_token_address_space_identity();
    test_concurrent_compare_exchange_pair();
    return 0;
}
