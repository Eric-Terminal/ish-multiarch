#include <assert.h>
#include <pthread.h>
#include <string.h>

#include "guest/aarch64/decode.h"
#include "guest/aarch64/execute.h"
#include "guest/memory/page-backing.h"

#define TEST_PAGE UINT64_C(0x0000000030000000)

struct shared_test_space {
    pthread_rwlock_t lock;
    struct guest_page_backing *backing;
    struct guest_address_space address_space;
    struct guest_tlb tlb;
};

static bool read_lock(void *opaque) {
    struct shared_test_space *space = opaque;
    assert(pthread_rwlock_rdlock(&space->lock) == 0);
    return true;
}

static void read_unlock(void *opaque, bool locked) {
    struct shared_test_space *space = opaque;
    assert(locked);
    assert(pthread_rwlock_unlock(&space->lock) == 0);
}

static bool write_lock(void *opaque) {
    struct shared_test_space *space = opaque;
    assert(pthread_rwlock_wrlock(&space->lock) == 0);
    return true;
}

static void write_unlock(void *opaque, bool locked) {
    struct shared_test_space *space = opaque;
    assert(locked);
    assert(pthread_rwlock_unlock(&space->lock) == 0);
}

static enum guest_memory_fault_kind resolve_page(void *opaque,
        guest_addr_t page_base, enum guest_memory_access access,
        struct guest_page_view *view) {
    struct shared_test_space *space = opaque;
    use(access);
    if (page_base != TEST_PAGE)
        return GUEST_MEMORY_FAULT_UNMAPPED;
    *view = (struct guest_page_view) {
        .host_page = guest_page_backing_bytes(space->backing),
        .permissions = GUEST_MEMORY_READ | GUEST_MEMORY_WRITE,
        .sync = guest_page_backing_sync(space->backing),
    };
    return GUEST_MEMORY_FAULT_NONE;
}

static const struct guest_address_space_ops test_ops = {
    .read_lock = read_lock,
    .read_unlock = read_unlock,
    .write_lock = write_lock,
    .write_unlock = write_unlock,
    .resolve_page = resolve_page,
};

static void init_space(struct shared_test_space *space,
        struct guest_page_backing *backing) {
    *space = (struct shared_test_space) {.backing = backing};
    assert(pthread_rwlock_init(&space->lock, NULL) == 0);
    guest_address_space_init(&space->address_space,
            &test_ops, space, 48);
    guest_tlb_init(&space->tlb, &space->address_space);
}

static void destroy_space(struct shared_test_space *space) {
    assert(pthread_rwlock_destroy(&space->lock) == 0);
}

static dword_t encode_load(unsigned size_shift,
        byte_t rt, byte_t rn) {
    return UINT32_C(0x085f7c00) |
            (dword_t) size_shift << 30 |
            (dword_t) rn << 5 |
            rt;
}

static dword_t encode_store(unsigned size_shift,
        byte_t rs, byte_t rt, byte_t rn) {
    return UINT32_C(0x08007c00) |
            (dword_t) size_shift << 30 |
            (dword_t) rs << 16 |
            (dword_t) rn << 5 |
            rt;
}

static dword_t encode_pair_load(byte_t rt, byte_t rt2, byte_t rn) {
    return UINT32_C(0xc87f0000) |
            (dword_t) rt2 << 10 |
            (dword_t) rn << 5 |
            rt;
}

static dword_t encode_pair_store(
        byte_t rs, byte_t rt, byte_t rt2, byte_t rn) {
    return UINT32_C(0xc8200000) |
            (dword_t) rs << 16 |
            (dword_t) rt2 << 10 |
            (dword_t) rn << 5 |
            rt;
}

static struct aarch64_execute_result execute_word(
        struct shared_test_space *space, struct cpu_state *cpu,
        dword_t word) {
    struct aarch64_decoded instruction;
    assert(aarch64_decode(word, &instruction));
    return aarch64_execute(cpu, &space->tlb, &instruction);
}

static void assert_retired(struct aarch64_execute_result result) {
    assert(result.stop == AARCH64_EXECUTE_RETIRED);
    assert(result.fault.kind == GUEST_MEMORY_FAULT_NONE);
}

static void test_single_monitor_crosses_address_spaces(
        struct shared_test_space *first,
        struct shared_test_space *second) {
    guest_addr_t address = TEST_PAGE + 64;
    qword_t initial = UINT64_C(0x1020304050607080);
    struct guest_memory_fault fault;
    assert(guest_tlb_write(&first->tlb, address,
            &initial, sizeof(initial), &fault));

    struct cpu_state cpu = {.pc = UINT64_C(0x1000)};
    cpu.x[1] = address;
    assert_retired(execute_word(first, &cpu,
            encode_load(3, 2, 1)));
    assert(cpu.x[2] == initial);
    assert(cpu.exclusive.sync_identity == guest_page_sync_identity(
            guest_page_backing_sync(first->backing)));

    qword_t temporary = UINT64_C(0xa1a2a3a4a5a6a7a8);
    assert(guest_tlb_write(&second->tlb, address,
            &temporary, sizeof(temporary), &fault));
    assert(guest_tlb_write(&second->tlb, address,
            &initial, sizeof(initial), &fault));
    cpu.x[3] = UINT64_C(0xb1b2b3b4b5b6b7b8);
    assert_retired(execute_word(first, &cpu,
            encode_store(3, 4, 3, 1)));
    assert(cpu.x[4] == 1);

    assert_retired(execute_word(first, &cpu,
            encode_load(3, 2, 1)));
    byte_t unrelated = 0x5a;
    assert(guest_tlb_write(&second->tlb, TEST_PAGE + 96,
            &unrelated, sizeof(unrelated), &fault));
    assert_retired(execute_word(first, &cpu,
            encode_store(3, 4, 3, 1)));
    assert(cpu.x[4] == 0);
    qword_t observed;
    assert(guest_tlb_read(&second->tlb, address,
            &observed, sizeof(observed), GUEST_MEMORY_READ, &fault));
    assert(observed == cpu.x[3]);
}

static void test_pair_monitor_crosses_address_spaces(
        struct shared_test_space *first,
        struct shared_test_space *second) {
    guest_addr_t address = TEST_PAGE + 128;
    const qword_t initial[2] = {
        UINT64_C(0x0123456789abcdef),
        UINT64_C(0xfedcba9876543210),
    };
    struct guest_memory_fault fault;
    assert(guest_tlb_write(&first->tlb, address,
            initial, sizeof(initial), &fault));

    struct cpu_state cpu = {.pc = UINT64_C(0x2000)};
    cpu.x[1] = address;
    assert_retired(execute_word(first, &cpu,
            encode_pair_load(2, 3, 1)));
    assert(cpu.x[2] == initial[0] && cpu.x[3] == initial[1]);
    assert(cpu.exclusive.sync_identity != 0);

    // 同值写也必须推进物理写世代，避免 ABA 后错误提交。
    assert(guest_tlb_write(&second->tlb, address,
            initial, sizeof(initial), &fault));
    cpu.x[6] = UINT64_C(0x1111222233334444);
    cpu.x[7] = UINT64_C(0xaaaabbbbccccdddd);
    assert_retired(execute_word(first, &cpu,
            encode_pair_store(4, 6, 7, 1)));
    assert(cpu.x[4] == 1);

    assert_retired(execute_word(first, &cpu,
            encode_pair_load(2, 3, 1)));
    byte_t unrelated = 0xa5;
    assert(guest_tlb_write(&second->tlb, TEST_PAGE + 160,
            &unrelated, sizeof(unrelated), &fault));
    assert_retired(execute_word(first, &cpu,
            encode_pair_store(4, 6, 7, 1)));
    assert(cpu.x[4] == 0);
    qword_t observed[2];
    assert(guest_tlb_read(&second->tlb, address,
            observed, sizeof(observed), GUEST_MEMORY_READ, &fault));
    assert(observed[0] == cpu.x[6] && observed[1] == cpu.x[7]);
}

int main(void) {
    struct guest_page_backing *backing = guest_page_backing_create();
    assert(backing != NULL);
    struct shared_test_space spaces[2];
    init_space(&spaces[0], backing);
    init_space(&spaces[1], backing);
    test_single_monitor_crosses_address_spaces(&spaces[0], &spaces[1]);
    test_pair_monitor_crosses_address_spaces(&spaces[0], &spaces[1]);
    destroy_space(&spaces[1]);
    destroy_space(&spaces[0]);
    guest_page_backing_release(backing);
    return 0;
}
