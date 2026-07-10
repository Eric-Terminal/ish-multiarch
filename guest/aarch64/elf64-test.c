#include <assert.h>
#include <string.h>

#include "guest/aarch64/elf64.h"

#define TEST_FILE_SIZE 512
#define TEST_SEGMENT_OFFSET 256
#define TEST_SEGMENT_ADDRESS UINT64_C(0x400100)

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

static void make_valid_image(byte_t file[TEST_FILE_SIZE]) {
    memset(file, 0, TEST_FILE_SIZE);
    file[0] = 0x7f;
    file[1] = 'E';
    file[2] = 'L';
    file[3] = 'F';
    file[4] = 2;
    file[5] = 1;
    file[6] = 1;
    file[7] = 3;
    put_u16(file + 16, 2);
    put_u16(file + 18, 183);
    put_u32(file + 20, 1);
    put_u64(file + 24, TEST_SEGMENT_ADDRESS);
    put_u64(file + 32, AARCH64_ELF64_HEADER_SIZE);
    put_u16(file + 52, AARCH64_ELF64_HEADER_SIZE);
    put_u16(file + 54, AARCH64_ELF64_PROGRAM_HEADER_SIZE);
    put_u16(file + 56, 1);

    byte_t *program_header = file + AARCH64_ELF64_HEADER_SIZE;
    put_u32(program_header, 1);
    put_u32(program_header + 4, 5);
    put_u64(program_header + 8, TEST_SEGMENT_OFFSET);
    put_u64(program_header + 16, TEST_SEGMENT_ADDRESS);
    put_u64(program_header + 32, 12);
    put_u64(program_header + 40, 16);
    put_u64(program_header + 48, 256);
}

static enum aarch64_elf64_error parse(byte_t file[TEST_FILE_SIZE]) {
    struct aarch64_elf64_image image;
    return aarch64_elf64_parse(file, TEST_FILE_SIZE, &image);
}

int main(void) {
    byte_t file[TEST_FILE_SIZE];
    make_valid_image(file);
    struct aarch64_elf64_image image;
    assert(aarch64_elf64_parse(file, sizeof(file), &image) ==
            AARCH64_ELF64_OK);
    assert(image.entry == TEST_SEGMENT_ADDRESS);
    assert(image.program_header_count == 1);
    assert(image.load_segment_count == 1);

    struct aarch64_elf64_program_header header;
    assert(aarch64_elf64_program_header(&image, 0, &header));
    assert(header.type == 1);
    assert(header.file_offset == TEST_SEGMENT_OFFSET);
    assert(header.virtual_address == TEST_SEGMENT_ADDRESS);
    assert(header.file_size == 12);
    assert(header.memory_size == 16);
    assert(header.permissions == (GUEST_MEMORY_READ | GUEST_MEMORY_EXECUTE));
    assert(!aarch64_elf64_program_header(&image, 1, &header));

    assert(aarch64_elf64_parse(file, 63, &image) ==
            AARCH64_ELF64_TRUNCATED);
    make_valid_image(file);
    file[0] = 0;
    assert(parse(file) == AARCH64_ELF64_BAD_IDENTIFICATION);
    make_valid_image(file);
    file[4] = 1;
    assert(parse(file) == AARCH64_ELF64_BAD_IDENTIFICATION);
    make_valid_image(file);
    put_u16(file + 16, 3);
    assert(parse(file) == AARCH64_ELF64_UNSUPPORTED_TYPE);
    make_valid_image(file);
    put_u16(file + 18, 62);
    assert(parse(file) == AARCH64_ELF64_UNSUPPORTED_MACHINE);
    make_valid_image(file);
    put_u16(file + 54, 55);
    assert(parse(file) == AARCH64_ELF64_BAD_HEADER);
    make_valid_image(file);
    put_u64(file + 32, TEST_FILE_SIZE - 8);
    assert(parse(file) == AARCH64_ELF64_TRUNCATED);

    make_valid_image(file);
    put_u32(file + AARCH64_ELF64_HEADER_SIZE, 3);
    assert(parse(file) == AARCH64_ELF64_UNSUPPORTED_DYNAMIC_LINKING);
    make_valid_image(file);
    put_u64(file + AARCH64_ELF64_HEADER_SIZE + 32, 17);
    assert(parse(file) == AARCH64_ELF64_BAD_SEGMENT);
    make_valid_image(file);
    put_u64(file + AARCH64_ELF64_HEADER_SIZE + 8, TEST_FILE_SIZE - 4);
    assert(parse(file) == AARCH64_ELF64_BAD_SEGMENT);
    make_valid_image(file);
    put_u64(file + AARCH64_ELF64_HEADER_SIZE + 16,
            (UINT64_C(1) << 48) - 8);
    assert(parse(file) == AARCH64_ELF64_BAD_SEGMENT);
    make_valid_image(file);
    put_u64(file + AARCH64_ELF64_HEADER_SIZE + 48, 3);
    assert(parse(file) == AARCH64_ELF64_BAD_SEGMENT);
    make_valid_image(file);
    put_u64(file + AARCH64_ELF64_HEADER_SIZE + 8,
            TEST_SEGMENT_OFFSET + 1);
    assert(parse(file) == AARCH64_ELF64_BAD_SEGMENT);
    make_valid_image(file);
    put_u64(file + AARCH64_ELF64_HEADER_SIZE + 8, 0);
    put_u64(file + AARCH64_ELF64_HEADER_SIZE + 48, 1);
    assert(parse(file) == AARCH64_ELF64_BAD_SEGMENT);

    make_valid_image(file);
    put_u64(file + 24, TEST_SEGMENT_ADDRESS + 2);
    assert(parse(file) == AARCH64_ELF64_BAD_ENTRY);
    make_valid_image(file);
    put_u64(file + 24, TEST_SEGMENT_ADDRESS + 32);
    assert(parse(file) == AARCH64_ELF64_BAD_ENTRY);
    make_valid_image(file);
    put_u32(file + AARCH64_ELF64_HEADER_SIZE + 4, 4);
    assert(parse(file) == AARCH64_ELF64_BAD_ENTRY);
    return 0;
}
