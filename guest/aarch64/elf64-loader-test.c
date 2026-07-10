#include <assert.h>
#include <string.h>

#include "guest/aarch64/elf64-loader.h"
#include "guest/aarch64/linux-syscall.h"
#include "guest/aarch64/runner.h"

#define TEST_FILE_SIZE 1024
#define TEST_BASE UINT64_C(0x400000)

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
            1, 6, 0x180, TEST_BASE + 0x180, 0x20, 0x100, 0x1000);

    put_u32(file + 0x100, UINT32_C(0xd2800540));
    put_u32(file + 0x104, UINT32_C(0xd2800ba8));
    put_u32(file + 0x108, UINT32_C(0xd4000001));
    for (byte_t i = 0; i < 0x20; i++)
        file[0x180 + i] = (byte_t) (0x80 + i);
}

static void test_load_and_run(const byte_t file[TEST_FILE_SIZE]) {
    struct aarch64_elf64_image image;
    assert(aarch64_elf64_parse(file, TEST_FILE_SIZE, &image) ==
            AARCH64_ELF64_OK);
    struct guest_page_table table;
    assert(guest_page_table_init(&table, 48));
    struct aarch64_elf64_load_result loaded;
    assert(aarch64_elf64_load(&image, &table, &loaded) ==
            AARCH64_ELF64_LOAD_OK);
    assert(loaded.entry == TEST_BASE + 0x100);
    assert(loaded.program_headers == TEST_BASE + AARCH64_ELF64_HEADER_SIZE);
    assert(loaded.program_header_count == 3);
    assert(loaded.brk_end == TEST_BASE + GUEST_MEMORY_PAGE_SIZE);

    byte_t *host_page;
    unsigned permissions;
    assert(guest_page_table_lookup(&table, TEST_BASE,
            &host_page, &permissions) == GUEST_PAGE_TABLE_OK);
    assert(permissions == GUEST_MEMORY_PERMISSION_MASK);
    assert(memcmp(host_page + 0x100, file + 0x100, 12) == 0);
    assert(memcmp(host_page + 0x180, file + 0x180, 0x20) == 0);
    for (size_t i = 0x1a0; i < 0x280; i++)
        assert(host_page[i] == 0);

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

int main(void) {
    byte_t file[TEST_FILE_SIZE];
    make_test_elf(file);
    test_load_and_run(file);

    byte_t without_phdr[TEST_FILE_SIZE];
    memcpy(without_phdr, file, sizeof(without_phdr));
    put_u32(without_phdr + AARCH64_ELF64_HEADER_SIZE, 0);
    test_load_and_run(without_phdr);

    struct aarch64_elf64_image image;
    assert(aarch64_elf64_parse(file, sizeof(file), &image) ==
            AARCH64_ELF64_OK);
    struct guest_page_table conflict;
    assert(guest_page_table_init(&conflict, 48));
    byte_t *host_page;
    assert(guest_page_table_map(&conflict, TEST_BASE,
            GUEST_MEMORY_READ, &host_page) == GUEST_PAGE_TABLE_OK);
    struct aarch64_elf64_load_result result;
    assert(aarch64_elf64_load(&image, &conflict, &result) ==
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
    assert(aarch64_elf64_load(&image, &unsupported, &result) ==
            AARCH64_ELF64_LOAD_UNSUPPORTED_LAYOUT);
    guest_page_table_destroy(&unsupported);
    return 0;
}
