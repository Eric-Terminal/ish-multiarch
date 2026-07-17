#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>

#include "guest/linux/errno.h"
#include "guest/linux/memory.h"
#include "guest/linux/mman.h"
#include "guest/memory/file-pager.h"
#include "guest/memory/tlb.h"

#define TEST_BRK UINT64_C(0x00100000)
#define TEST_BRK_LIMIT UINT64_C(0x00200000)

static qword_t linux_error(unsigned error) {
    return (qword_t) -(sqword_t) error;
}

struct file_probe {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    byte_t data[GUEST_MEMORY_PAGE_SIZE * 3];
    qword_t size;
    enum guest_file_page_result forced_result;
    unsigned reads;
    unsigned releases;
    bool block;
    bool entered;
    bool allow_read;
    bool read_outside_page_table_lock;
    bool release_outside_page_table_lock;
    struct guest_page_table *table;
};

static void check_pthread(int result) {
    assert(result == 0);
}

static bool page_table_write_lock_is_free(struct guest_page_table *table) {
    int result = pthread_rwlock_trywrlock(&table->lock);
    if (result == 0) {
        check_pthread(pthread_rwlock_unlock(&table->lock));
        return true;
    }
    assert(result == EBUSY || result == EDEADLK);
    return false;
}

static void probe_init(struct file_probe *probe,
        struct guest_page_table *table, qword_t size) {
    assert(size <= sizeof(probe->data));
    *probe = (struct file_probe) {
        .size = size,
        .forced_result = GUEST_FILE_PAGE_OK,
        .read_outside_page_table_lock = true,
        .release_outside_page_table_lock = true,
        .table = table,
    };
    for (qword_t index = 0; index < sizeof(probe->data); index++)
        probe->data[index] = (byte_t) ((index + 1) & UINT64_C(0xff));
    check_pthread(pthread_mutex_init(&probe->lock, NULL));
    check_pthread(pthread_cond_init(&probe->changed, NULL));
}

static void probe_destroy(struct file_probe *probe) {
    check_pthread(pthread_cond_destroy(&probe->changed));
    check_pthread(pthread_mutex_destroy(&probe->lock));
}

static enum guest_file_page_result probe_read_page(void *opaque,
        qword_t file_offset, byte_t *page, dword_t *valid_bytes) {
    struct file_probe *probe = opaque;
    bool outside_lock = page_table_write_lock_is_free(probe->table);
    check_pthread(pthread_mutex_lock(&probe->lock));
    probe->read_outside_page_table_lock &= outside_lock;
    probe->reads++;
    probe->entered = true;
    check_pthread(pthread_cond_broadcast(&probe->changed));
    while (probe->block && !probe->allow_read)
        check_pthread(pthread_cond_wait(&probe->changed, &probe->lock));
    enum guest_file_page_result result = probe->forced_result;
    qword_t size = probe->size;
    check_pthread(pthread_mutex_unlock(&probe->lock));

    if (result != GUEST_FILE_PAGE_OK)
        return result;
    if (file_offset >= size)
        return GUEST_FILE_PAGE_END_OF_FILE;
    qword_t remaining = size - file_offset;
    dword_t count = remaining < GUEST_MEMORY_PAGE_SIZE ?
            (dword_t) remaining : (dword_t) GUEST_MEMORY_PAGE_SIZE;
    memcpy(page, probe->data + file_offset, count);
    *valid_bytes = count;
    return GUEST_FILE_PAGE_OK;
}

static void probe_release(
        struct guest_file_pager *pager, void *opaque) {
    struct file_probe *probe = opaque;
    assert(!guest_file_pager_try_retain(pager));
    probe->release_outside_page_table_lock &=
            page_table_write_lock_is_free(probe->table);
    probe->releases++;
}

static struct guest_file_pager *probe_pager(
        struct file_probe *probe, qword_t identity) {
    return guest_file_pager_create(identity,
            (struct guest_file_pager_provider) {
                .opaque = probe,
                .read_page = probe_read_page,
                .release = probe_release,
            });
}

struct memory_fixture {
    struct guest_page_table table;
    struct guest_linux_mm memory;
    struct guest_tlb tlb;
};

static void fixture_init(struct memory_fixture *fixture) {
    memset(fixture, 0, sizeof(*fixture));
    assert(guest_page_table_init(&fixture->table, 48));
    guest_linux_mm_init(&fixture->memory, &fixture->table,
            TEST_BRK, TEST_BRK_LIMIT);
    guest_page_table_enable_concurrency(&fixture->table);
    guest_tlb_init(&fixture->tlb, &fixture->table.address_space);
}

static void fixture_destroy(struct memory_fixture *fixture) {
    guest_linux_mm_destroy(&fixture->memory);
    guest_page_table_destroy(&fixture->table);
}

static qword_t map_private(struct memory_fixture *fixture,
        struct guest_file_pager *pager, guest_addr_t address,
        qword_t length, qword_t protection,
        qword_t maximum_protection, qword_t flags, qword_t offset) {
    return guest_linux_mmap_file_private(&fixture->memory,
            address, length, protection, maximum_protection,
            flags, pager, offset);
}

static struct guest_page_view resolve_view(
        struct guest_page_table *table, guest_addr_t page,
        enum guest_memory_access access) {
    bool locked = guest_page_table_read_lock(table);
    struct guest_page_view view;
    assert(guest_address_space_resolve_page(
            &table->address_space, page, access, &view) ==
            GUEST_MEMORY_FAULT_NONE);
    guest_page_table_read_unlock(table, locked);
    return view;
}

static void assert_not_resident(
        struct guest_page_table *table, guest_addr_t page) {
    bool locked = guest_page_table_read_lock(table);
    byte_t *host;
    unsigned permissions;
    assert(guest_page_table_lookup(table, page, &host, &permissions) ==
            GUEST_PAGE_TABLE_NOT_MAPPED);
    guest_page_table_read_unlock(table, locked);
}

static void test_lazy_alias_and_private_cow(void) {
    unsigned baseline = guest_page_backing_test_live_count();
    struct memory_fixture fixture;
    fixture_init(&fixture);
    struct file_probe probe;
    probe_init(&probe, &fixture.table, GUEST_MEMORY_PAGE_SIZE * 2);
    struct guest_file_pager *pager = probe_pager(&probe, 101);
    assert(pager != NULL);

    qword_t flags = GUEST_LINUX_MAP_PRIVATE;
    qword_t protection = GUEST_LINUX_PROT_READ |
            GUEST_LINUX_PROT_WRITE;
    qword_t first_result = map_private(&fixture, pager, 0,
            GUEST_MEMORY_PAGE_SIZE, protection,
            GUEST_LINUX_PROT_MASK, flags, 0);
    assert((sqword_t) first_result >= 0);
    guest_addr_t first = (guest_addr_t) first_result;
    qword_t second_result = map_private(&fixture, pager,
            first + GUEST_MEMORY_PAGE_SIZE * 4,
            GUEST_MEMORY_PAGE_SIZE, protection,
            GUEST_LINUX_PROT_MASK, flags, 0);
    assert((sqword_t) second_result >= 0);
    guest_addr_t second = (guest_addr_t) second_result;
    assert(second != first);
    assert_not_resident(&fixture.table, first);
    assert_not_resident(&fixture.table, second);
    assert(probe.reads == 0);

    dword_t observed = UINT32_C(0xfeedface);
    struct guest_tlb_mapping_snapshot snapshot = {
        .shared_identity = UINT64_MAX,
        .file_identity = UINT64_MAX,
        .page_offset = UINT64_MAX,
        .file_offset = UINT64_MAX,
    };
    struct guest_memory_fault no_page_in_fault;
    assert(guest_tlb_compare_exchange_u32_resolved_no_page_in(
            &fixture.tlb, first, 0, 1, &observed,
            &snapshot, &no_page_in_fault) ==
                    GUEST_TLB_COMPARE_EXCHANGE_FAULT);
    assert(no_page_in_fault.address == first &&
            no_page_in_fault.access == GUEST_MEMORY_WRITE &&
            no_page_in_fault.kind == GUEST_MEMORY_FAULT_UNMAPPED &&
            observed == UINT32_C(0xfeedface) &&
            snapshot.shared_identity == UINT64_MAX &&
            probe.reads == 0);

    byte_t first_byte;
    byte_t second_byte;
    struct guest_memory_fault fault;
    assert(guest_tlb_read(&fixture.tlb, first, &first_byte, 1,
            GUEST_MEMORY_READ, &fault));
    assert(guest_tlb_read(&fixture.tlb, second, &second_byte, 1,
            GUEST_MEMORY_READ, &fault));
    assert(first_byte == 1 && second_byte == 1 && probe.reads == 1);
    struct guest_page_view first_view = resolve_view(
            &fixture.table, first, GUEST_MEMORY_READ);
    struct guest_page_view second_view = resolve_view(
            &fixture.table, second, GUEST_MEMORY_READ);
    assert(first_view.origin == GUEST_PAGE_ORIGIN_FILE &&
            second_view.origin == GUEST_PAGE_ORIGIN_FILE);
    assert(first_view.sync == second_view.sync &&
            first_view.backing_identity == second_view.backing_identity);

    byte_t replacement = UINT8_C(0xa7);
    assert(guest_tlb_write(
            &fixture.tlb, first, &replacement, 1, &fault));
    assert(guest_tlb_read(&fixture.tlb, first, &first_byte, 1,
            GUEST_MEMORY_READ, &fault));
    assert(guest_tlb_read(&fixture.tlb, second, &second_byte, 1,
            GUEST_MEMORY_READ, &fault));
    assert(first_byte == replacement && second_byte == 1);
    first_view = resolve_view(&fixture.table, first, GUEST_MEMORY_READ);
    second_view = resolve_view(&fixture.table, second, GUEST_MEMORY_READ);
    assert(first_view.origin == GUEST_PAGE_ORIGIN_ANONYMOUS &&
            first_view.sync == NULL && !first_view.copy_on_write);
    assert(second_view.origin == GUEST_PAGE_ORIGIN_FILE &&
            second_view.copy_on_write);

    guest_file_pager_release(pager);
    fixture_destroy(&fixture);
    assert(probe.read_outside_page_table_lock &&
            probe.release_outside_page_table_lock && probe.releases == 1);
    probe_destroy(&probe);
    assert(guest_page_backing_test_live_count() == baseline);
}

static void test_tail_eof_permissions_and_errors(void) {
    struct memory_fixture fixture;
    fixture_init(&fixture);
    struct file_probe probe;
    probe_init(&probe, &fixture.table, GUEST_MEMORY_PAGE_SIZE + 37);
    struct guest_file_pager *pager = probe_pager(&probe, 102);
    assert(pager != NULL);

    qword_t mapped_result = map_private(&fixture, pager, 0,
            GUEST_MEMORY_PAGE_SIZE * 3, GUEST_LINUX_PROT_READ,
            GUEST_LINUX_PROT_READ, GUEST_LINUX_MAP_PRIVATE, 0);
    assert((sqword_t) mapped_result >= 0);
    guest_addr_t mapped = (guest_addr_t) mapped_result;
    byte_t value;
    struct guest_memory_fault fault;
    assert(guest_tlb_read(&fixture.tlb,
            mapped + GUEST_MEMORY_PAGE_SIZE + 36,
            &value, 1, GUEST_MEMORY_READ, &fault));
    assert(value == probe.data[GUEST_MEMORY_PAGE_SIZE + 36]);
    assert(guest_tlb_read(&fixture.tlb,
            mapped + GUEST_MEMORY_PAGE_SIZE + 37,
            &value, 1, GUEST_MEMORY_READ, &fault));
    assert(value == 0);

    value = UINT8_C(0xee);
    assert(!guest_tlb_read(&fixture.tlb,
            mapped + GUEST_MEMORY_PAGE_SIZE * 2 + 17,
            &value, 1, GUEST_MEMORY_READ, &fault));
    assert(value == UINT8_C(0xee));
    assert(fault.address == mapped + GUEST_MEMORY_PAGE_SIZE * 2 + 17 &&
            fault.access == GUEST_MEMORY_READ &&
            fault.kind == GUEST_MEMORY_FAULT_BUS_ADDRESS);

    unsigned reads = probe.reads;
    value = 1;
    assert(!guest_tlb_write(&fixture.tlb,
            mapped + GUEST_MEMORY_PAGE_SIZE * 2,
            &value, 1, &fault));
    assert(fault.kind == GUEST_MEMORY_FAULT_PERMISSION &&
            probe.reads == reads);

    probe.forced_result = GUEST_FILE_PAGE_IO_ERROR;
    qword_t error_result = map_private(&fixture, pager, 0,
            GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_PROT_READ,
            GUEST_LINUX_PROT_READ, GUEST_LINUX_MAP_PRIVATE,
            GUEST_MEMORY_PAGE_SIZE * 2);
    assert((sqword_t) error_result >= 0);
    assert(!guest_tlb_read(&fixture.tlb, (guest_addr_t) error_result,
            &value, 1, GUEST_MEMORY_READ, &fault));
    assert(fault.kind == GUEST_MEMORY_FAULT_BUS_ADDRESS);

    guest_file_pager_release(pager);
    fixture_destroy(&fixture);
    assert(probe.releases == 1);
    probe_destroy(&probe);
}

static void test_lazy_mprotect_holes_and_fixed_replace(void) {
    struct memory_fixture fixture;
    fixture_init(&fixture);
    struct file_probe probe;
    probe_init(&probe, &fixture.table, GUEST_MEMORY_PAGE_SIZE);
    struct guest_file_pager *pager = probe_pager(&probe, 103);
    assert(pager != NULL);

    qword_t mapped_result = map_private(&fixture, pager, 0,
            GUEST_MEMORY_PAGE_SIZE, 0, GUEST_LINUX_PROT_READ,
            GUEST_LINUX_MAP_PRIVATE, 0);
    assert((sqword_t) mapped_result >= 0);
    guest_addr_t mapped = (guest_addr_t) mapped_result;
    qword_t generation = fixture.table.address_space.generation;
    assert(guest_linux_mprotect(&fixture.memory, mapped,
            GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_PROT_READ) == 0);
    assert(fixture.table.address_space.generation == generation + 1 &&
            probe.reads == 0);
    assert(guest_linux_mprotect(&fixture.memory, mapped,
            GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_PROT_WRITE) ==
            linux_error(GUEST_LINUX_EACCES));

    assert(map_private(&fixture, pager, mapped,
            GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_PROT_READ,
            GUEST_LINUX_PROT_READ,
            GUEST_LINUX_MAP_PRIVATE |
                    GUEST_LINUX_MAP_FIXED_NOREPLACE,
            0) == linux_error(GUEST_LINUX_EEXIST));
    qword_t hinted = map_private(&fixture, pager, mapped,
            GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_PROT_READ,
            GUEST_LINUX_PROT_READ, GUEST_LINUX_MAP_PRIVATE, 0);
    assert((sqword_t) hinted >= 0 && hinted != mapped);

    // 固定匿名映射必须覆盖尚未 fault 的文件 VMA。
    assert(guest_linux_mmap(&fixture.memory, mapped,
            GUEST_MEMORY_PAGE_SIZE,
            GUEST_LINUX_PROT_READ | GUEST_LINUX_PROT_WRITE,
            GUEST_LINUX_MAP_PRIVATE | GUEST_LINUX_MAP_ANONYMOUS |
                    GUEST_LINUX_MAP_FIXED,
            0, 0) == mapped);
    byte_t value = UINT8_C(0xff);
    struct guest_memory_fault fault;
    assert(guest_tlb_read(&fixture.tlb, mapped, &value, 1,
            GUEST_MEMORY_READ, &fault));
    assert(value == 0 && probe.reads == 0);

    guest_file_pager_release(pager);
    fixture_destroy(&fixture);
    assert(probe.releases == 1);
    probe_destroy(&probe);
}

static void test_fixed_file_mapping_replaces_offset(void) {
    struct memory_fixture fixture;
    fixture_init(&fixture);
    struct file_probe probe;
    probe_init(&probe, &fixture.table, GUEST_MEMORY_PAGE_SIZE * 3);
    probe.data[GUEST_MEMORY_PAGE_SIZE] = UINT8_C(0x22);
    probe.data[GUEST_MEMORY_PAGE_SIZE * 2] = UINT8_C(0x33);
    struct guest_file_pager *pager = probe_pager(&probe, 109);
    assert(pager != NULL);

    qword_t mapped_result = map_private(&fixture, pager, 0,
            GUEST_MEMORY_PAGE_SIZE * 2, GUEST_LINUX_PROT_READ,
            GUEST_LINUX_PROT_READ, GUEST_LINUX_MAP_PRIVATE, 0);
    assert((sqword_t) mapped_result >= 0);
    guest_addr_t second = (guest_addr_t) mapped_result +
            GUEST_MEMORY_PAGE_SIZE;
    byte_t value;
    struct guest_memory_fault fault;
    assert(guest_tlb_read(&fixture.tlb, second, &value, 1,
            GUEST_MEMORY_READ, &fault) && value == UINT8_C(0x22));

    assert(map_private(&fixture, pager, second,
            GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_PROT_READ,
            GUEST_LINUX_PROT_READ,
            GUEST_LINUX_MAP_PRIVATE | GUEST_LINUX_MAP_FIXED,
            GUEST_MEMORY_PAGE_SIZE * 2) == second);
    assert_not_resident(&fixture.table, second);
    assert(guest_tlb_read(&fixture.tlb, second, &value, 1,
            GUEST_MEMORY_READ, &fault) && value == UINT8_C(0x33));
    struct guest_page_view view = resolve_view(
            &fixture.table, second, GUEST_MEMORY_READ);
    assert(view.file_offset == GUEST_MEMORY_PAGE_SIZE * 2 &&
            probe.reads == 2);

    guest_file_pager_release(pager);
    fixture_destroy(&fixture);
    assert(probe.releases == 1);
    probe_destroy(&probe);
}

static void test_fork_before_fault_and_exclusive_cow(void) {
    struct memory_fixture parent;
    fixture_init(&parent);
    struct file_probe probe;
    probe_init(&probe, &parent.table, GUEST_MEMORY_PAGE_SIZE);
    struct guest_file_pager *pager = probe_pager(&probe, 104);
    assert(pager != NULL);
    qword_t mapped_result = map_private(&parent, pager, 0,
            GUEST_MEMORY_PAGE_SIZE,
            GUEST_LINUX_PROT_READ | GUEST_LINUX_PROT_WRITE,
            GUEST_LINUX_PROT_MASK, GUEST_LINUX_MAP_PRIVATE, 0);
    assert((sqword_t) mapped_result >= 0);
    guest_addr_t mapped = (guest_addr_t) mapped_result;

    struct guest_page_table child_table;
    struct guest_linux_mm child_memory;
    assert(guest_linux_mm_clone(
            &child_memory, &child_table, &parent.memory));
    guest_page_table_enable_concurrency(&child_table);
    struct guest_tlb child_tlb;
    guest_tlb_init(&child_tlb, &child_table.address_space);

    byte_t parent_value;
    byte_t child_value;
    struct guest_memory_fault fault;
    assert(guest_tlb_read(&child_tlb, mapped, &child_value, 1,
            GUEST_MEMORY_READ, &fault));
    assert(guest_tlb_read(&parent.tlb, mapped, &parent_value, 1,
            GUEST_MEMORY_READ, &fault));
    assert(parent_value == 1 && child_value == 1 && probe.reads == 1);
    struct guest_page_view parent_view = resolve_view(
            &parent.table, mapped, GUEST_MEMORY_READ);
    struct guest_page_view child_view = resolve_view(
            &child_table, mapped, GUEST_MEMORY_READ);
    assert(parent_view.backing_identity == child_view.backing_identity);

    qword_t expected;
    memcpy(&expected, probe.data, sizeof(expected));
    struct guest_tlb_exclusive_token token;
    assert(guest_tlb_load_exclusive(&child_tlb, mapped,
            &child_value, 1, &token, &fault));
    byte_t replacement = UINT8_C(0x5a);
    assert(guest_tlb_store_exclusive(&child_tlb, mapped,
            &child_value, &replacement, 1, token, &fault) ==
            GUEST_TLB_EXCLUSIVE_FAILED);
    assert(guest_tlb_load_exclusive(&child_tlb, mapped,
            &child_value, 1, &token, &fault));
    assert(guest_tlb_store_exclusive(&child_tlb, mapped,
            &child_value, &replacement, 1, token, &fault) ==
            GUEST_TLB_EXCLUSIVE_STORED);
    assert(guest_tlb_read(&parent.tlb, mapped, &parent_value, 1,
            GUEST_MEMORY_READ, &fault));
    assert(parent_value == 1);

    // CAS mismatch 也必须先完成 private-file 写 fault COW。
    qword_t other_result = map_private(&parent, pager,
            mapped + GUEST_MEMORY_PAGE_SIZE * 4,
            GUEST_MEMORY_PAGE_SIZE,
            GUEST_LINUX_PROT_READ | GUEST_LINUX_PROT_WRITE,
            GUEST_LINUX_PROT_MASK, GUEST_LINUX_MAP_PRIVATE, 0);
    assert((sqword_t) other_result >= 0);
    qword_t wrong = expected ^ UINT64_MAX;
    qword_t replacement_word = UINT64_C(0x1122334455667788);
    qword_t observed = UINT64_C(0xdeadbeef);
    assert(guest_tlb_compare_exchange(&parent.tlb,
            (guest_addr_t) other_result,
            &wrong, &replacement_word, &observed,
            sizeof(observed), &fault) ==
            GUEST_TLB_COMPARE_EXCHANGE_MISMATCH);
    assert(observed == expected);
    struct guest_page_view other_view = resolve_view(&parent.table,
            (guest_addr_t) other_result, GUEST_MEMORY_READ);
    assert(other_view.origin == GUEST_PAGE_ORIGIN_ANONYMOUS &&
            !other_view.copy_on_write);

    guest_linux_mm_destroy(&child_memory);
    guest_page_table_destroy(&child_table);
    guest_file_pager_release(pager);
    fixture_destroy(&parent);
    assert(probe.releases == 1);
    probe_destroy(&probe);
}

struct blocked_read {
    struct guest_tlb tlb;
    guest_addr_t address;
    bool succeeded;
    byte_t value;
    struct guest_memory_fault fault;
};

static void *blocked_read_thread(void *opaque) {
    struct blocked_read *read = opaque;
    read->succeeded = guest_tlb_read(&read->tlb,
            read->address, &read->value, 1,
            GUEST_MEMORY_READ, &read->fault);
    return NULL;
}

static void test_fault_racing_munmap_drops_stale_page(void) {
    struct memory_fixture fixture;
    fixture_init(&fixture);
    struct file_probe probe;
    probe_init(&probe, &fixture.table, GUEST_MEMORY_PAGE_SIZE);
    probe.block = true;
    struct guest_file_pager *pager = probe_pager(&probe, 105);
    assert(pager != NULL);
    qword_t mapped_result = map_private(&fixture, pager, 0,
            GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_PROT_READ,
            GUEST_LINUX_PROT_READ, GUEST_LINUX_MAP_PRIVATE, 0);
    assert((sqword_t) mapped_result >= 0);
    guest_addr_t mapped = (guest_addr_t) mapped_result;
    // 此后 VMA 或正在进行的 page-in 是 pager 的唯一所有者。
    guest_file_pager_release(pager);

    struct blocked_read read = {.address = mapped};
    guest_tlb_init(&read.tlb, &fixture.table.address_space);
    pthread_t thread;
    check_pthread(pthread_create(
            &thread, NULL, blocked_read_thread, &read));
    check_pthread(pthread_mutex_lock(&probe.lock));
    while (!probe.entered)
        check_pthread(pthread_cond_wait(&probe.changed, &probe.lock));
    check_pthread(pthread_mutex_unlock(&probe.lock));

    assert(guest_linux_munmap(&fixture.memory,
            mapped, GUEST_MEMORY_PAGE_SIZE) == 0);
    assert(probe.releases == 0);
    check_pthread(pthread_mutex_lock(&probe.lock));
    probe.allow_read = true;
    check_pthread(pthread_cond_broadcast(&probe.changed));
    check_pthread(pthread_mutex_unlock(&probe.lock));
    check_pthread(pthread_join(thread, NULL));
    assert(!read.succeeded &&
            read.fault.kind == GUEST_MEMORY_FAULT_UNMAPPED);
    assert_not_resident(&fixture.table, mapped);
    assert(probe.releases == 1 &&
            probe.read_outside_page_table_lock &&
            probe.release_outside_page_table_lock);

    fixture_destroy(&fixture);
    probe_destroy(&probe);
}

static void test_argument_and_cache_oom_errors(void) {
    struct memory_fixture fixture;
    fixture_init(&fixture);
    struct file_probe probe;
    probe_init(&probe, &fixture.table, GUEST_MEMORY_PAGE_SIZE);
    struct guest_file_pager *pager = probe_pager(&probe, 106);
    assert(pager != NULL);
    assert(map_private(&fixture, pager, 0, 0,
            GUEST_LINUX_PROT_READ, GUEST_LINUX_PROT_READ,
            GUEST_LINUX_MAP_PRIVATE, 0) ==
            linux_error(GUEST_LINUX_EINVAL));
    assert(map_private(&fixture, pager, 0, GUEST_MEMORY_PAGE_SIZE,
            GUEST_LINUX_PROT_WRITE, GUEST_LINUX_PROT_READ,
            GUEST_LINUX_MAP_PRIVATE, 0) ==
            linux_error(GUEST_LINUX_EACCES));
    assert(map_private(&fixture, pager, 0, GUEST_MEMORY_PAGE_SIZE,
            GUEST_LINUX_PROT_READ, GUEST_LINUX_PROT_READ,
            GUEST_LINUX_MAP_PRIVATE, 1) ==
            linux_error(GUEST_LINUX_EINVAL));
    assert(map_private(&fixture, pager, 0, GUEST_MEMORY_PAGE_SIZE,
            GUEST_LINUX_PROT_READ, GUEST_LINUX_PROT_READ,
            GUEST_LINUX_MAP_PRIVATE,
            UINT64_MAX & ~GUEST_MEMORY_PAGE_MASK) ==
            linux_error(GUEST_LINUX_EOVERFLOW));

    qword_t mapped_result = map_private(&fixture, pager, 0,
            GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_PROT_READ,
            GUEST_LINUX_PROT_READ, GUEST_LINUX_MAP_PRIVATE, 0);
    assert((sqword_t) mapped_result >= 0);
    guest_file_pager_test_fail_allocation_at(0);
    byte_t value = 0;
    struct guest_memory_fault fault;
    assert(!guest_tlb_read(&fixture.tlb,
            (guest_addr_t) mapped_result, &value, 1,
            GUEST_MEMORY_READ, &fault));
    assert(fault.kind == GUEST_MEMORY_FAULT_OUT_OF_MEMORY &&
            probe.reads == 0);
    guest_file_pager_test_fail_allocation_at(SIZE_MAX);
    assert(guest_tlb_read(&fixture.tlb,
            (guest_addr_t) mapped_result, &value, 1,
            GUEST_MEMORY_READ, &fault));
    assert(value == 1 && probe.reads == 1);

    guest_file_pager_release(pager);
    fixture_destroy(&fixture);
    probe_destroy(&probe);
}

int main(void) {
    test_lazy_alias_and_private_cow();
    test_tail_eof_permissions_and_errors();
    test_lazy_mprotect_holes_and_fixed_replace();
    test_fixed_file_mapping_replaces_offset();
    test_fork_before_fault_and_exclusive_cow();
    test_fault_racing_munmap_drops_stale_page();
    test_argument_and_cache_oom_errors();
    assert(guest_page_backing_test_live_count() == 0);
    return 0;
}
