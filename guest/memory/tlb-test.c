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
};

static enum guest_memory_fault_kind resolve_test_page(void *opaque,
        guest_addr_t page_base, enum guest_memory_access access,
        struct guest_page_view *view) {
    struct test_memory *memory = opaque;
    use(access);
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
    .resolve_page = resolve_test_page,
};

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

    assert(!guest_tlb_write(&tlb, PAGE_NEXT, &value, 1, &fault));
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

    memory.pages[1].permissions |= GUEST_MEMORY_WRITE;
    guest_address_space_changed(&space);
    assert(guest_tlb_write(&tlb, PAGE_NEXT - 2,
            write_value, sizeof(write_value), &fault));
    memset(output, 0, sizeof(output));
    assert(guest_tlb_read(&tlb, PAGE_NEXT - 2, output, sizeof(output),
            GUEST_MEMORY_READ, &fault));
    assert(memcmp(output, write_value, sizeof(output)) == 0);
    return 0;
}
