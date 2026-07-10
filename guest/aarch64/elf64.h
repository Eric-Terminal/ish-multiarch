#ifndef GUEST_AARCH64_ELF64_H
#define GUEST_AARCH64_ELF64_H

#include "guest/memory/address-space.h"

#define AARCH64_ELF64_HEADER_SIZE 64
#define AARCH64_ELF64_PROGRAM_HEADER_SIZE 56

enum aarch64_elf64_error {
    AARCH64_ELF64_OK,
    AARCH64_ELF64_TRUNCATED,
    AARCH64_ELF64_BAD_IDENTIFICATION,
    AARCH64_ELF64_UNSUPPORTED_TYPE,
    AARCH64_ELF64_UNSUPPORTED_MACHINE,
    AARCH64_ELF64_BAD_HEADER,
    AARCH64_ELF64_UNSUPPORTED_DYNAMIC_LINKING,
    AARCH64_ELF64_BAD_SEGMENT,
    AARCH64_ELF64_BAD_ENTRY,
};

struct aarch64_elf64_image {
    const byte_t *data;
    size_t size;
    bool position_independent;
    guest_addr_t entry;
    qword_t program_header_offset;
    word_t program_header_count;
    word_t load_segment_count;
};

struct aarch64_elf64_program_header {
    dword_t type;
    dword_t flags;
    qword_t file_offset;
    guest_addr_t virtual_address;
    qword_t file_size;
    qword_t memory_size;
    qword_t alignment;
    unsigned permissions;
};

enum aarch64_elf64_error aarch64_elf64_parse(const void *data,
        size_t size, struct aarch64_elf64_image *image);
bool aarch64_elf64_program_header(const struct aarch64_elf64_image *image,
        word_t index, struct aarch64_elf64_program_header *header);

#endif
