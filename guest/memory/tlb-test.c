#include <assert.h>
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
    return 0;
}
