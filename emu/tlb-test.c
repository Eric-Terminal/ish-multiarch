#include <assert.h>
#include <stdint.h>

#include "emu/tlb.h"

struct test_mmu {
    struct mmu mmu;
    uint8_t pages[2][PAGE_SIZE];
    unsigned translations;
    int translation_types[8];
};

static void *test_translate(struct mmu *mmu, addr_t addr, int type) {
    struct test_mmu *memory = container_of(mmu, struct test_mmu, mmu);
    assert(PGOFFSET(addr) == 0);
    memory->translation_types[memory->translations] = type;
    memory->translations++;
    if (addr == 0x1000)
        return memory->pages[0];
    if (addr == 0x2000)
        return memory->pages[1];
    return NULL;
}

static struct mmu_ops test_mmu_ops = {
    .translate = test_translate,
};

static void test_pointer_reconstruction(void) {
    const uint64_t host = UINT64_C(0x00123000);
    const uint64_t high_guest_page = UINT64_C(0xf0000000);
    const uint64_t low_guest_page = UINT64_C(0x00001000);

    assert((uint32_t) (host - high_guest_page + high_guest_page) == host);
    assert((uint32_t) (host - low_guest_page + low_guest_page) == host);
}

int main(void) {
    struct test_mmu memory = {
        .mmu = {.ops = &test_mmu_ops},
    };
    struct tlb tlb = {0};
    const uint8_t initial[] = {0x12, 0x34, 0x56, 0x78};
    const uint8_t replacement[] = {0x87, 0x65, 0x43, 0x21};
    const uint8_t cross_page[] = {0xa1, 0xb2, 0xc3, 0xd4};
    uint8_t result[sizeof(cross_page)] = {0};

    test_pointer_reconstruction();
    tlb_refresh(&tlb, &memory.mmu);

    memcpy(&memory.pages[0][4], initial, sizeof(initial));
    assert(tlb_read(&tlb, 0x1004, result, sizeof(result)));
    assert(memory.translations == 1);
    assert(memory.translation_types[0] == MEM_READ);
    assert(memcmp(result, initial, sizeof(initial)) == 0);

    assert(tlb_write(&tlb, 0x1004, replacement, sizeof(replacement)));
    assert(memory.translations == 2);
    assert(memory.translation_types[1] == MEM_WRITE);
    assert(memcmp(&memory.pages[0][4], replacement, sizeof(replacement)) == 0);
    assert(tlb_read(&tlb, 0x1004, result, sizeof(result)));
    assert(memcmp(result, replacement, sizeof(replacement)) == 0);
    assert(memory.translations == 2);

    assert(tlb_write(&tlb, 0x1ffe, cross_page, sizeof(cross_page)));
    assert(memory.translations == 3);
    assert(memory.translation_types[2] == MEM_WRITE);
    assert(memcmp(&memory.pages[0][PAGE_SIZE - 2], cross_page, 2) == 0);
    assert(memcmp(&memory.pages[1][0], cross_page + 2, 2) == 0);
    memset(result, 0, sizeof(result));
    assert(tlb_read(&tlb, 0x1ffe, result, sizeof(result)));
    assert(memcmp(result, cross_page, sizeof(cross_page)) == 0);
    assert(memory.translations == 3);

    assert(!tlb_read(&tlb, 0x2ffe, result, sizeof(result)));
    assert(memory.translations == 4);
    assert(tlb.segfault_addr == 0x3000);

    memory.mmu.changes++;
    tlb_refresh(&tlb, &memory.mmu);
    assert(tlb_read(&tlb, 0x1004, result, sizeof(result)));
    assert(memory.translations == 5);
    assert(memory.translation_types[4] == MEM_READ);
    assert(memcmp(result, replacement, sizeof(replacement)) == 0);

    assert(tlb_read(&tlb, 0x1004, result, sizeof(result)));
    assert(memory.translations == 5);
    return 0;
}
