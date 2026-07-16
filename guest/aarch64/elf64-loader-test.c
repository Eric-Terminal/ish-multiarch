#include <assert.h>
#include <string.h>

#include "guest/aarch64/elf64-loader.h"
#include "guest/aarch64/linux-syscall.h"
#include "guest/aarch64/runner.h"

#define TEST_FILE_SIZE 8192
#define TEST_BASE UINT64_C(0x400000)
#define TEST_PIE_LOAD_BIAS UINT64_C(0x0000400000000000)
#define TEST_FILE_IDENTITY UINT64_C(0x1020304050607080)

static struct guest_file_source *test_file_source;

static enum aarch64_elf64_load_error load_test_image(
        const struct aarch64_elf64_image *image,
        struct guest_page_table *table, guest_addr_t load_bias,
        struct aarch64_elf64_load_result *result) {
    return aarch64_elf64_load(
            image, test_file_source, table, load_bias, result);
}

static void put_u16(byte_t *bytes, word_t value) {
    bytes[0] = (byte_t) value;
    bytes[1] = (byte_t) (value >> 8);
}

static void put_u32(byte_t *bytes, dword_t value) {
    for (byte_t i = 0; i < 4; i++)
        bytes[i] = (byte_t) (value >> (i * 8));
}

static void put_u64(byte_t *bytes, qword_t value) {
    for (byte_t i = 0; i < 8; i++)
        bytes[i] = (byte_t) (value >> (i * 8));
}

static void put_program_header(byte_t *bytes, dword_t type, dword_t flags,
        qword_t offset, qword_t address, qword_t file_size,
        qword_t memory_size, qword_t alignment) {
    put_u32(bytes, type);
    put_u32(bytes + 4, flags);
    put_u64(bytes + 8, offset);
    put_u64(bytes + 16, address);
    put_u64(bytes + 32, file_size);
    put_u64(bytes + 40, memory_size);
    put_u64(bytes + 48, alignment);
}

static void make_test_elf(byte_t file[TEST_FILE_SIZE]) {
    memset(file, 0, TEST_FILE_SIZE);
    file[0] = 0x7f;
    file[1] = 'E';
    file[2] = 'L';
    file[3] = 'F';
    file[4] = 2;
    file[5] = 1;
    file[6] = 1;
    put_u16(file + 16, 2);
    put_u16(file + 18, 183);
    put_u32(file + 20, 1);
    put_u64(file + 24, TEST_BASE + 0x100);
    put_u64(file + 32, AARCH64_ELF64_HEADER_SIZE);
    put_u16(file + 52, AARCH64_ELF64_HEADER_SIZE);
    put_u16(file + 54, AARCH64_ELF64_PROGRAM_HEADER_SIZE);
    put_u16(file + 56, 3);

    byte_t *headers = file + AARCH64_ELF64_HEADER_SIZE;
    put_program_header(headers, 6, 4,
            AARCH64_ELF64_HEADER_SIZE, TEST_BASE + AARCH64_ELF64_HEADER_SIZE,
            3 * AARCH64_ELF64_PROGRAM_HEADER_SIZE,
            3 * AARCH64_ELF64_PROGRAM_HEADER_SIZE, 8);
    put_program_header(headers + AARCH64_ELF64_PROGRAM_HEADER_SIZE,
            1, 5, 0, TEST_BASE, 0x180, 0x180, 0x1000);
    put_program_header(headers + 2 * AARCH64_ELF64_PROGRAM_HEADER_SIZE,
            1, 7, 0x180, TEST_BASE + 0x180, 0x20, 0x100, 0x1000);

    put_u32(file + 0x100, UINT32_C(0xd2800540));
    put_u32(file + 0x104, UINT32_C(0xd2800ba8));
    put_u32(file + 0x108, UINT32_C(0xd4000001));
    for (byte_t i = 0; i < 0x20; i++)
        file[0x180 + i] = (byte_t) (0x80 + i);
}

static void make_test_pie(byte_t file[TEST_FILE_SIZE]) {
    make_test_elf(file);
    put_u16(file + 16, 3);
    put_u64(file + 24, 0x100);
    byte_t *headers = file + AARCH64_ELF64_HEADER_SIZE;
    put_program_header(headers, 2, 6, 0x180, 0x180, 0x20, 0x20, 8);
    put_u64(headers + AARCH64_ELF64_PROGRAM_HEADER_SIZE + 16, 0);
    put_u64(headers + AARCH64_ELF64_PROGRAM_HEADER_SIZE + 48, 0x10000);
    put_u64(headers + 2 * AARCH64_ELF64_PROGRAM_HEADER_SIZE + 16, 0x180);
    put_u64(headers + 2 * AARCH64_ELF64_PROGRAM_HEADER_SIZE + 48, 0x10000);
    put_u64(file + 0x180, UINT64_C(0x4ac));
}

static enum guest_page_origin resolve_page_origin(
        struct guest_page_table *table, guest_addr_t page) {
    struct guest_page_view view;
    assert(guest_address_space_resolve_page(&table->address_space, page,
            GUEST_MEMORY_READ, &view) == GUEST_MEMORY_FAULT_NONE);
    return view.origin;
}

static struct guest_page_view resolve_page_view(
        struct guest_page_table *table, guest_addr_t page) {
    struct guest_page_view view;
    assert(guest_address_space_resolve_page(&table->address_space, page,
            GUEST_MEMORY_READ, &view) == GUEST_MEMORY_FAULT_NONE);
    return view;
}

static void test_load_and_run(const byte_t file[TEST_FILE_SIZE],
        guest_addr_t load_bias) {
    struct aarch64_elf64_image image;
    assert(aarch64_elf64_parse(file, TEST_FILE_SIZE, &image) ==
            AARCH64_ELF64_OK);
    struct guest_page_table table;
    assert(guest_page_table_init(&table, 48));
    struct aarch64_elf64_load_result loaded;
    assert(load_test_image(&image, &table, load_bias, &loaded) ==
            AARCH64_ELF64_LOAD_OK);
    assert(loaded.load_bias == load_bias);
    guest_addr_t image_base = image.position_independent ? load_bias : TEST_BASE;
    assert(loaded.entry == image_base + 0x100);
    assert(loaded.program_headers ==
            image_base + AARCH64_ELF64_HEADER_SIZE);
    assert(loaded.program_header_count == 3);
    assert(loaded.brk_end == image_base + GUEST_MEMORY_PAGE_SIZE);

    byte_t *host_page;
    unsigned permissions;
    assert(guest_page_table_lookup(&table, image_base,
            &host_page, &permissions) == GUEST_PAGE_TABLE_OK);
    assert(permissions == GUEST_MEMORY_PERMISSION_MASK);
    assert(resolve_page_origin(&table, image_base) ==
            GUEST_PAGE_ORIGIN_ANONYMOUS);
    assert(memcmp(host_page + 0x100, file + 0x100, 12) == 0);
    assert(memcmp(host_page + 0x180, file + 0x180, 0x20) == 0);
    if (image.position_independent) {
        // 重定位由 static PIE 自启动代码完成，loader 只复制链接期值。
        assert(host_page[0x180] == 0xac);
        assert(host_page[0x181] == 0x04);
        for (size_t i = 0x182; i < 0x188; i++)
            assert(host_page[i] == 0);
    }
    for (size_t i = 0x1a0; i < 0x280; i++)
        assert(host_page[i] == 0);
    if (load_bias != 0) {
        assert(guest_page_table_lookup(&table, 0,
                &host_page, &permissions) == GUEST_PAGE_TABLE_NOT_MAPPED);
    }

    struct guest_tlb tlb;
    guest_tlb_init(&tlb, &table.address_space);
    struct aarch64_runner runner;
    aarch64_runner_init(&runner, &tlb);
    struct cpu_state cpu = {.pc = loaded.entry};
    assert(aarch64_run_one(&runner, &cpu).stop == AARCH64_STEP_RETIRED);
    assert(aarch64_run_one(&runner, &cpu).stop == AARCH64_STEP_RETIRED);
    assert(aarch64_run_one(&runner, &cpu).stop == AARCH64_STEP_SYSCALL);
    struct guest_linux_syscall syscall;
    aarch64_linux_read_syscall(&cpu, &syscall);
    assert(syscall.number == 93);
    assert(syscall.arguments[0] == 42);
    guest_page_table_destroy(&table);
}

static void test_load_page_origins(void) {
    byte_t file[TEST_FILE_SIZE];
    make_test_elf(file);
    put_u16(file + 56, 8);
    byte_t *headers = file + AARCH64_ELF64_HEADER_SIZE;
    qword_t header_size = 8 * AARCH64_ELF64_PROGRAM_HEADER_SIZE;
    put_u64(headers + 32, header_size);
    put_u64(headers + 40, header_size);
    qword_t first_load_size = AARCH64_ELF64_HEADER_SIZE + header_size;
    put_u64(headers + AARCH64_ELF64_PROGRAM_HEADER_SIZE + 32,
            first_load_size);
    put_u64(headers + AARCH64_ELF64_PROGRAM_HEADER_SIZE + 40,
            first_load_size);
    put_program_header(headers + 2 * AARCH64_ELF64_PROGRAM_HEADER_SIZE,
            1, 4, 0, TEST_BASE + UINT64_C(0x1000),
            0, UINT64_C(0x1000), UINT64_C(0x1000));
    put_program_header(headers + 3 * AARCH64_ELF64_PROGRAM_HEADER_SIZE,
            1, 6, 0, TEST_BASE + UINT64_C(0x2000),
            0, UINT64_C(0x300), UINT64_C(0x1000));
    put_program_header(headers + 4 * AARCH64_ELF64_PROGRAM_HEADER_SIZE,
            1, 4, UINT64_C(0x200),
            TEST_BASE + UINT64_C(0x2200),
            4, 4, UINT64_C(0x1000));
    put_program_header(headers + 5 * AARCH64_ELF64_PROGRAM_HEADER_SIZE,
            1, 6, 0, TEST_BASE + UINT64_C(0x3000),
            UINT64_C(0x100), UINT64_C(0x100), UINT64_C(0x1000));
    // 只读段的页内补零故障会被忽略，因此页面仍保留文件来源。
    put_program_header(headers + 6 * AARCH64_ELF64_PROGRAM_HEADER_SIZE,
            1, 4, UINT64_C(0x200),
            TEST_BASE + UINT64_C(0x4200),
            4, 8, UINT64_C(0x1000));
    put_program_header(headers + 7 * AARCH64_ELF64_PROGRAM_HEADER_SIZE,
            1, 4, UINT64_C(0x1200),
            TEST_BASE + UINT64_C(0x5200),
            4, 4, UINT64_C(0x1000));
    const byte_t marker[4] = {0x31, 0x41, 0x59, 0x26};
    const byte_t read_only_tail[4] = {0x92, 0x65, 0x35, 0x89};
    const byte_t second_marker[4] = {0x53, 0x58, 0x27, 0x97};
    memcpy(file + 0x200, marker, sizeof(marker));
    memcpy(file + 0x204, read_only_tail, sizeof(read_only_tail));
    memcpy(file + 0x1200, second_marker, sizeof(second_marker));

    struct aarch64_elf64_image image;
    assert(aarch64_elf64_parse(file, sizeof(file), &image) ==
            AARCH64_ELF64_OK);
    struct guest_page_table table;
    assert(guest_page_table_init(&table, 48));
    struct aarch64_elf64_load_result loaded;
    assert(load_test_image(&image, &table, 0, &loaded) ==
            AARCH64_ELF64_LOAD_OK);
    byte_t *host_page;
    unsigned permissions;
    struct guest_page_view view = resolve_page_view(&table, TEST_BASE);
    assert(view.origin == GUEST_PAGE_ORIGIN_FILE &&
            view.file_identity == TEST_FILE_IDENTITY &&
            view.file_offset == 0);
    view = resolve_page_view(&table, TEST_BASE + UINT64_C(0x1000));
    assert(view.origin == GUEST_PAGE_ORIGIN_ANONYMOUS &&
            view.file_identity == 0 && view.file_offset == 0);
    assert(guest_page_table_lookup(&table,
            TEST_BASE + UINT64_C(0x1000),
            &host_page, &permissions) == GUEST_PAGE_TABLE_OK);
    assert(permissions == (GUEST_MEMORY_READ | GUEST_MEMORY_WRITE));
    assert(resolve_page_origin(&table, TEST_BASE + UINT64_C(0x2000)) ==
            GUEST_PAGE_ORIGIN_FILE);
    assert(resolve_page_origin(&table, TEST_BASE + UINT64_C(0x3000)) ==
            GUEST_PAGE_ORIGIN_FILE);
    assert(resolve_page_origin(&table, TEST_BASE + UINT64_C(0x4000)) ==
            GUEST_PAGE_ORIGIN_FILE);
    view = resolve_page_view(&table, TEST_BASE + UINT64_C(0x5000));
    assert(view.origin == GUEST_PAGE_ORIGIN_FILE &&
            view.file_identity == TEST_FILE_IDENTITY &&
            view.file_offset == UINT64_C(0x1000));
    assert(guest_page_table_lookup(&table,
            TEST_BASE + UINT64_C(0x2000),
            &host_page, &permissions) == GUEST_PAGE_TABLE_OK);
    assert(memcmp(host_page + 0x200, marker, sizeof(marker)) == 0);
    assert(memcmp(host_page + 0x204,
            read_only_tail, sizeof(read_only_tail)) == 0);
    assert(guest_page_table_lookup(&table,
            TEST_BASE + UINT64_C(0x5000),
            &host_page, &permissions) == GUEST_PAGE_TABLE_OK);
    assert(memcmp(host_page + 0x200,
            second_marker, sizeof(second_marker)) == 0);
    guest_page_table_destroy(&table);
}

int main(void) {
    test_file_source = guest_file_source_create(
            TEST_FILE_IDENTITY, NULL, NULL);
    assert(test_file_source != NULL);
    byte_t file[TEST_FILE_SIZE];
    make_test_elf(file);
    test_load_and_run(file, 0);

    byte_t without_phdr[TEST_FILE_SIZE];
    memcpy(without_phdr, file, sizeof(without_phdr));
    put_u32(without_phdr + AARCH64_ELF64_HEADER_SIZE, 0);
    test_load_and_run(without_phdr, 0);

    byte_t pie[TEST_FILE_SIZE];
    make_test_pie(pie);
    test_load_and_run(pie, TEST_PIE_LOAD_BIAS);
    test_load_page_origins();

    byte_t overlapping_bss[TEST_FILE_SIZE];
    make_test_elf(overlapping_bss);
    byte_t *overlap_segment = overlapping_bss +
            AARCH64_ELF64_HEADER_SIZE +
            2 * AARCH64_ELF64_PROGRAM_HEADER_SIZE;
    put_u64(overlap_segment + 8, 0x170);
    put_u64(overlap_segment + 16, TEST_BASE + 0x170);
    put_u64(overlap_segment + 32, 4);
    put_u64(overlap_segment + 40, 0x30);
    for (byte_t i = 0; i < 0x10; i++)
        overlapping_bss[0x170 + i] = (byte_t) (0xd0 + i);
    struct aarch64_elf64_image image;
    assert(aarch64_elf64_parse(overlapping_bss,
            sizeof(overlapping_bss), &image) == AARCH64_ELF64_OK);
    struct guest_page_table overlap;
    assert(guest_page_table_init(&overlap, 48));
    struct aarch64_elf64_load_result result;
    assert(load_test_image(&image, &overlap, 0, &result) ==
            AARCH64_ELF64_LOAD_OK);
    byte_t *host_page;
    unsigned permissions;
    assert(guest_page_table_lookup(&overlap, TEST_BASE,
            &host_page, &permissions) == GUEST_PAGE_TABLE_OK);
    assert(memcmp(host_page + 0x170, overlapping_bss + 0x170, 4) == 0);
    for (size_t i = 0x174; i < 0x1a0; i++)
        assert(host_page[i] == 0);
    struct guest_page_view overlap_view =
            resolve_page_view(&overlap, TEST_BASE);
    assert(overlap_view.origin == GUEST_PAGE_ORIGIN_ANONYMOUS &&
            overlap_view.file_identity == 0 &&
            overlap_view.file_offset == 0);
    guest_page_table_destroy(&overlap);

    byte_t overlapping_permissions[TEST_FILE_SIZE];
    make_test_elf(overlapping_permissions);
    byte_t *last_segment = overlapping_permissions +
            AARCH64_ELF64_HEADER_SIZE +
            2 * AARCH64_ELF64_PROGRAM_HEADER_SIZE;
    put_u32(last_segment + 4, 4);
    assert(aarch64_elf64_parse(overlapping_permissions,
            sizeof(overlapping_permissions), &image) == AARCH64_ELF64_OK);
    struct guest_page_table permission_overlap;
    assert(guest_page_table_init(&permission_overlap, 48));
    assert(load_test_image(&image, &permission_overlap, 0, &result) ==
            AARCH64_ELF64_LOAD_OK);
    assert(guest_page_table_lookup(&permission_overlap, TEST_BASE,
            &host_page, &permissions) == GUEST_PAGE_TABLE_OK);
    assert(permissions == GUEST_MEMORY_READ);
    struct guest_page_view permission_view =
            resolve_page_view(&permission_overlap, TEST_BASE);
    assert(permission_view.origin == GUEST_PAGE_ORIGIN_FILE &&
            permission_view.file_identity == TEST_FILE_IDENTITY &&
            permission_view.file_offset == 0);
    guest_page_table_destroy(&permission_overlap);

    assert(aarch64_elf64_parse(file, sizeof(file), &image) ==
            AARCH64_ELF64_OK);
    struct guest_page_table conflict;
    assert(guest_page_table_init(&conflict, 48));
    assert(guest_page_table_map(&conflict, TEST_BASE,
            GUEST_MEMORY_READ, &host_page) == GUEST_PAGE_TABLE_OK);
    assert(load_test_image(&image, &conflict, 0, &result) ==
            AARCH64_ELF64_LOAD_MAPPING_CONFLICT);
    guest_page_table_destroy(&conflict);

    byte_t missing_phdr[TEST_FILE_SIZE];
    memcpy(missing_phdr, without_phdr, sizeof(missing_phdr));
    byte_t *first_load = missing_phdr + AARCH64_ELF64_HEADER_SIZE +
            AARCH64_ELF64_PROGRAM_HEADER_SIZE;
    put_u64(first_load + 8, 0x100);
    put_u64(first_load + 16, TEST_BASE + 0x100);
    put_u64(first_load + 32, 0x80);
    put_u64(first_load + 40, 0x80);
    assert(aarch64_elf64_parse(missing_phdr, sizeof(missing_phdr), &image) ==
            AARCH64_ELF64_OK);
    struct guest_page_table unsupported;
    assert(guest_page_table_init(&unsupported, 48));
    assert(load_test_image(&image, &unsupported, 0, &result) ==
            AARCH64_ELF64_LOAD_UNSUPPORTED_LAYOUT);
    guest_page_table_destroy(&unsupported);

    byte_t mismatched_phdr[TEST_FILE_SIZE];
    make_test_elf(mismatched_phdr);
    put_u64(mismatched_phdr + AARCH64_ELF64_HEADER_SIZE + 16,
            TEST_BASE + 0x80);
    assert(aarch64_elf64_parse(mismatched_phdr,
            sizeof(mismatched_phdr), &image) == AARCH64_ELF64_OK);
    struct guest_page_table bad_phdr;
    assert(guest_page_table_init(&bad_phdr, 48));
    assert(load_test_image(&image, &bad_phdr, 0, &result) ==
            AARCH64_ELF64_LOAD_UNSUPPORTED_LAYOUT);
    assert(guest_page_table_lookup(&bad_phdr, TEST_BASE,
            &host_page, &permissions) == GUEST_PAGE_TABLE_NOT_MAPPED);
    guest_page_table_destroy(&bad_phdr);

    assert(aarch64_elf64_parse(pie, sizeof(pie), &image) ==
            AARCH64_ELF64_OK);
    struct guest_page_table invalid_bias;
    assert(guest_page_table_init(&invalid_bias, 48));
    assert(load_test_image(&image, &invalid_bias, 0, &result) ==
            AARCH64_ELF64_LOAD_UNSUPPORTED_LAYOUT);
    assert(load_test_image(&image, &invalid_bias,
            TEST_PIE_LOAD_BIAS + 1, &result) ==
            AARCH64_ELF64_LOAD_UNSUPPORTED_LAYOUT);
    assert(load_test_image(&image, &invalid_bias,
            TEST_PIE_LOAD_BIAS + GUEST_MEMORY_PAGE_SIZE,
            &result) == AARCH64_ELF64_LOAD_UNSUPPORTED_LAYOUT);
    guest_page_table_destroy(&invalid_bias);

    assert(aarch64_elf64_parse(file, sizeof(file), &image) ==
            AARCH64_ELF64_OK);
    struct guest_page_table fixed_rebase;
    assert(guest_page_table_init(&fixed_rebase, 48));
    assert(load_test_image(&image, &fixed_rebase,
            TEST_PIE_LOAD_BIAS, &result) ==
            AARCH64_ELF64_LOAD_UNSUPPORTED_LAYOUT);
    guest_page_table_destroy(&fixed_rebase);

    byte_t overflowing_pie[TEST_FILE_SIZE];
    make_test_pie(overflowing_pie);
    guest_addr_t overflow_base = (UINT64_C(1) << 48) - UINT64_C(0x10000);
    put_u64(overflowing_pie + 24, overflow_base + 0x100);
    byte_t *overflow_headers =
            overflowing_pie + AARCH64_ELF64_HEADER_SIZE;
    put_u64(overflow_headers + AARCH64_ELF64_PROGRAM_HEADER_SIZE + 16,
            overflow_base);
    put_u64(overflow_headers +
            2 * AARCH64_ELF64_PROGRAM_HEADER_SIZE + 16,
            overflow_base + 0x180);
    assert(aarch64_elf64_parse(overflowing_pie,
            sizeof(overflowing_pie), &image) == AARCH64_ELF64_OK);
    struct guest_page_table overflow;
    assert(guest_page_table_init(&overflow, 48));
    assert(load_test_image(&image, &overflow,
            TEST_PIE_LOAD_BIAS, &result) ==
            AARCH64_ELF64_LOAD_UNSUPPORTED_LAYOUT);
    guest_page_table_destroy(&overflow);

    byte_t brk_boundary_file[TEST_FILE_SIZE];
    make_test_elf(brk_boundary_file);
    guest_addr_t high_base = (UINT64_C(1) << 48) - GUEST_MEMORY_PAGE_SIZE;
    byte_t *boundary_headers =
            brk_boundary_file + AARCH64_ELF64_HEADER_SIZE;
    put_u64(brk_boundary_file + 24, high_base + 0x100);
    put_u64(boundary_headers + 16,
            high_base + AARCH64_ELF64_HEADER_SIZE);
    put_u64(boundary_headers + AARCH64_ELF64_PROGRAM_HEADER_SIZE + 16,
            high_base);
    put_u64(boundary_headers +
            2 * AARCH64_ELF64_PROGRAM_HEADER_SIZE + 16,
            high_base + 0x180);
    assert(aarch64_elf64_parse(brk_boundary_file,
            sizeof(brk_boundary_file), &image) == AARCH64_ELF64_OK);
    struct guest_page_table brk_boundary;
    assert(guest_page_table_init(&brk_boundary, 48));
    assert(load_test_image(&image, &brk_boundary, 0, &result) ==
            AARCH64_ELF64_LOAD_UNSUPPORTED_LAYOUT);
    assert(guest_page_table_lookup(&brk_boundary, high_base,
            &host_page, &permissions) == GUEST_PAGE_TABLE_NOT_MAPPED);
    guest_page_table_destroy(&brk_boundary);

    byte_t late_conflict_file[TEST_FILE_SIZE];
    make_test_elf(late_conflict_file);
    byte_t *late_segment = late_conflict_file +
            AARCH64_ELF64_HEADER_SIZE +
            2 * AARCH64_ELF64_PROGRAM_HEADER_SIZE;
    put_u64(late_segment + 16, TEST_BASE + GUEST_MEMORY_PAGE_SIZE + 0x180);
    put_u64(late_segment + 32, 0);
    put_u64(late_segment + 40, 0x100);
    assert(aarch64_elf64_parse(late_conflict_file,
            sizeof(late_conflict_file), &image) == AARCH64_ELF64_OK);
    struct guest_page_table late_conflict;
    assert(guest_page_table_init(&late_conflict, 48));
    byte_t *sentinel_page;
    assert(guest_page_table_map(&late_conflict,
            TEST_BASE + GUEST_MEMORY_PAGE_SIZE,
            GUEST_MEMORY_READ, &sentinel_page) == GUEST_PAGE_TABLE_OK);
    sentinel_page[0] = 0x7d;
    qword_t generation = late_conflict.address_space.generation;
    assert(load_test_image(&image, &late_conflict, 0, &result) ==
            AARCH64_ELF64_LOAD_MAPPING_CONFLICT);
    assert(late_conflict.address_space.generation == generation);
    assert(guest_page_table_lookup(&late_conflict, TEST_BASE,
            &host_page, &permissions) == GUEST_PAGE_TABLE_NOT_MAPPED);
    assert(sentinel_page[0] == 0x7d);
    assert(guest_page_table_lookup(&late_conflict,
            TEST_BASE + GUEST_MEMORY_PAGE_SIZE,
            &host_page, &permissions) == GUEST_PAGE_TABLE_OK);
    assert(permissions == GUEST_MEMORY_READ);
    guest_page_table_destroy(&late_conflict);
    guest_file_source_release(test_file_source);
    return 0;
}
