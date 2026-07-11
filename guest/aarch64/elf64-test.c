#include <assert.h>
#include <string.h>

#include "guest/aarch64/elf64.h"

#define TEST_FILE_SIZE 512
#define TEST_SEGMENT_OFFSET 256
#define TEST_SEGMENT_ADDRESS UINT64_C(0x400100)
#define TEST_INTERPRETER_OFFSET 384

static const char test_interpreter[] = "/lib/ld-musl-aarch64.so.1";

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

static void make_interpreted_image(byte_t file[TEST_FILE_SIZE]) {
    make_valid_image(file);
    byte_t *headers = file + AARCH64_ELF64_HEADER_SIZE;
    memmove(headers + AARCH64_ELF64_PROGRAM_HEADER_SIZE,
            headers, AARCH64_ELF64_PROGRAM_HEADER_SIZE);
    memset(headers, 0, AARCH64_ELF64_PROGRAM_HEADER_SIZE);
    put_u32(headers, 3);
    put_u32(headers + 4, 4);
    put_u64(headers + 8, TEST_INTERPRETER_OFFSET);
    put_u64(headers + 32, sizeof(test_interpreter));
    put_u64(headers + 40, sizeof(test_interpreter));
    put_u64(headers + 48, 1);
    put_u16(file + 56, 2);
    memcpy(file + TEST_INTERPRETER_OFFSET,
            test_interpreter, sizeof(test_interpreter));
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
    assert(!image.position_independent);
    assert(image.program_header_count == 1);
    assert(image.load_segment_count == 1);
    assert(image.interpreter_path == NULL);
    assert(image.interpreter_path_length == 0);

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
    assert(aarch64_elf64_parse(file, sizeof(file), &image) ==
            AARCH64_ELF64_OK);
    assert(image.position_independent);
    make_valid_image(file);
    put_u16(file + 16, 1);
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

    make_interpreted_image(file);
    assert(aarch64_elf64_parse(file, sizeof(file), &image) ==
            AARCH64_ELF64_OK);
    assert(image.program_header_count == 2);
    assert(image.load_segment_count == 1);
    assert(image.interpreter_path_length ==
            sizeof(test_interpreter) - 1);
    assert(memcmp(image.interpreter_path, test_interpreter,
            sizeof(test_interpreter)) == 0);

    make_interpreted_image(file);
    put_u16(file + 16, 3);
    assert(aarch64_elf64_parse(file, sizeof(file), &image) ==
            AARCH64_ELF64_OK);
    assert(image.position_independent);
    assert(strcmp(image.interpreter_path, test_interpreter) == 0);

    make_interpreted_image(file);
    put_u64(file + AARCH64_ELF64_HEADER_SIZE + 32, 2);
    file[TEST_INTERPRETER_OFFSET] = 'x';
    file[TEST_INTERPRETER_OFFSET + 1] = '\0';
    assert(aarch64_elf64_parse(file, sizeof(file), &image) ==
            AARCH64_ELF64_OK);
    assert(image.interpreter_path_length == 1);
    assert(strcmp(image.interpreter_path, "x") == 0);

    make_interpreted_image(file);
    put_u64(file + AARCH64_ELF64_HEADER_SIZE + 32, 2);
    file[TEST_INTERPRETER_OFFSET] = '\0';
    file[TEST_INTERPRETER_OFFSET + 1] = '\0';
    assert(aarch64_elf64_parse(file, sizeof(file), &image) ==
            AARCH64_ELF64_OK);
    assert(image.interpreter_path != NULL &&
            image.interpreter_path_length == 0);

    make_interpreted_image(file);
    put_u64(file + AARCH64_ELF64_HEADER_SIZE + 32, 1);
    file[TEST_INTERPRETER_OFFSET] = '\0';
    assert(parse(file) == AARCH64_ELF64_BAD_SEGMENT);
    make_interpreted_image(file);
    put_u64(file + AARCH64_ELF64_HEADER_SIZE + 32, 3);
    memcpy(file + TEST_INTERPRETER_OFFSET, "abc", 3);
    assert(parse(file) == AARCH64_ELF64_BAD_SEGMENT);
    make_interpreted_image(file);
    put_u64(file + AARCH64_ELF64_HEADER_SIZE + 32, 4);
    file[TEST_INTERPRETER_OFFSET] = 'a';
    file[TEST_INTERPRETER_OFFSET + 1] = '\0';
    file[TEST_INTERPRETER_OFFSET + 2] = 'b';
    file[TEST_INTERPRETER_OFFSET + 3] = '\0';
    assert(aarch64_elf64_parse(file, sizeof(file), &image) ==
            AARCH64_ELF64_OK);
    assert(image.interpreter_path_length == 1);
    assert(strcmp(image.interpreter_path, "a") == 0);
    make_interpreted_image(file);
    put_u64(file + AARCH64_ELF64_HEADER_SIZE + 8,
            TEST_FILE_SIZE - 1);
    put_u64(file + AARCH64_ELF64_HEADER_SIZE + 32, 2);
    assert(parse(file) == AARCH64_ELF64_BAD_SEGMENT);

    make_interpreted_image(file);
    byte_t *headers = file + AARCH64_ELF64_HEADER_SIZE;
    memmove(headers + 2 * AARCH64_ELF64_PROGRAM_HEADER_SIZE,
            headers + AARCH64_ELF64_PROGRAM_HEADER_SIZE,
            AARCH64_ELF64_PROGRAM_HEADER_SIZE);
    memcpy(headers + AARCH64_ELF64_PROGRAM_HEADER_SIZE,
            headers, AARCH64_ELF64_PROGRAM_HEADER_SIZE);
    put_u64(headers + AARCH64_ELF64_PROGRAM_HEADER_SIZE + 32, 0);
    put_u16(file + 56, 3);
    assert(aarch64_elf64_parse(file, sizeof(file), &image) ==
            AARCH64_ELF64_OK);
    assert(strcmp(image.interpreter_path, test_interpreter) == 0);

    make_interpreted_image(file);
    byte_t swapped[AARCH64_ELF64_PROGRAM_HEADER_SIZE];
    memcpy(swapped, headers, sizeof(swapped));
    memcpy(headers, headers + AARCH64_ELF64_PROGRAM_HEADER_SIZE,
            AARCH64_ELF64_PROGRAM_HEADER_SIZE);
    memcpy(headers + AARCH64_ELF64_PROGRAM_HEADER_SIZE,
            swapped, sizeof(swapped));
    assert(aarch64_elf64_parse(file, sizeof(file), &image) ==
            AARCH64_ELF64_OK);
    assert(strcmp(image.interpreter_path, test_interpreter) == 0);

    byte_t large[5000];
    memset(large, 0, sizeof(large));
    make_interpreted_image(file);
    memcpy(large, file, sizeof(file));
    headers = large + AARCH64_ELF64_HEADER_SIZE;
    put_u64(headers + 8, TEST_FILE_SIZE);
    put_u64(headers + 32, AARCH64_ELF64_INTERPRETER_PATH_MAX);
    memset(large + TEST_FILE_SIZE, 'a',
            AARCH64_ELF64_INTERPRETER_PATH_MAX - 1);
    large[TEST_FILE_SIZE + AARCH64_ELF64_INTERPRETER_PATH_MAX - 1] = '\0';
    assert(aarch64_elf64_parse(large,
            TEST_FILE_SIZE + AARCH64_ELF64_INTERPRETER_PATH_MAX,
            &image) ==
            AARCH64_ELF64_OK);
    assert(image.interpreter_path_length ==
            AARCH64_ELF64_INTERPRETER_PATH_MAX - 1);
    put_u64(headers + 32, AARCH64_ELF64_INTERPRETER_PATH_MAX + 1);
    large[TEST_FILE_SIZE + AARCH64_ELF64_INTERPRETER_PATH_MAX] = '\0';
    assert(aarch64_elf64_parse(large,
            TEST_FILE_SIZE + AARCH64_ELF64_INTERPRETER_PATH_MAX + 1,
            &image) ==
            AARCH64_ELF64_BAD_SEGMENT);

    make_valid_image(file);
    put_u16(file + 16, 3);
    put_u16(file + 56, 2);
    byte_t *dynamic = file + AARCH64_ELF64_HEADER_SIZE +
            AARCH64_ELF64_PROGRAM_HEADER_SIZE;
    put_u32(dynamic, 2);
    assert(parse(file) == AARCH64_ELF64_OK);
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
