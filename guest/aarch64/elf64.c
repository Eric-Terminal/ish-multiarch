#include <assert.h>
#include <string.h>

#include "guest/aarch64/elf64.h"

#define ELF64_CLASS 2
#define ELF_LITTLE_ENDIAN 1
#define ELF_CURRENT_VERSION 1
#define ELF_OSABI_SYSTEM_V 0
#define ELF_OSABI_LINUX 3
#define ELF_EXECUTABLE 2
#define ELF_SHARED_OBJECT 3
#define ELF_MACHINE_AARCH64 183
#define ELF_PT_LOAD 1
#define ELF_PT_INTERP 3
#define ELF_PF_EXECUTE UINT32_C(1)
#define ELF_PF_WRITE UINT32_C(2)
#define ELF_PF_READ UINT32_C(4)
#define ELF_PF_MASK (ELF_PF_EXECUTE | ELF_PF_WRITE | ELF_PF_READ)

static word_t read_u16(const byte_t *bytes) {
    return (word_t) ((word_t) bytes[0] | (word_t) bytes[1] << 8);
}

static dword_t read_u32(const byte_t *bytes) {
    return (dword_t) bytes[0] |
            (dword_t) bytes[1] << 8 |
            (dword_t) bytes[2] << 16 |
            (dword_t) bytes[3] << 24;
}

static qword_t read_u64(const byte_t *bytes) {
    qword_t low = read_u32(bytes);
    qword_t high = read_u32(bytes + 4);
    return low | high << 32;
}

static unsigned elf_permissions(dword_t flags) {
    unsigned permissions = 0;
    if (flags & ELF_PF_READ)
        permissions |= GUEST_MEMORY_READ;
    if (flags & ELF_PF_WRITE)
        permissions |= GUEST_MEMORY_WRITE;
    if (flags & ELF_PF_EXECUTE)
        permissions |= GUEST_MEMORY_EXECUTE;
    return permissions;
}

bool aarch64_elf64_program_header(const struct aarch64_elf64_image *image,
        word_t index, struct aarch64_elf64_program_header *header) {
    if (index >= image->program_header_count)
        return false;
    qword_t offset = image->program_header_offset +
            (qword_t) index * AARCH64_ELF64_PROGRAM_HEADER_SIZE;
    const byte_t *bytes = image->data + (size_t) offset;
    dword_t flags = read_u32(bytes + 4);
    *header = (struct aarch64_elf64_program_header) {
        .type = read_u32(bytes),
        .flags = flags,
        .file_offset = read_u64(bytes + 8),
        .virtual_address = read_u64(bytes + 16),
        .file_size = read_u64(bytes + 32),
        .memory_size = read_u64(bytes + 40),
        .alignment = read_u64(bytes + 48),
        .permissions = elf_permissions(flags),
    };
    return true;
}

static bool valid_file_range(const struct aarch64_elf64_image *image,
        qword_t offset, qword_t size) {
    return offset <= (qword_t) image->size &&
            size <= (qword_t) image->size - offset;
}

static bool valid_guest_range(guest_addr_t address, qword_t size) {
    const qword_t limit = UINT64_C(1) << 48;
    return address < limit && size <= limit - address;
}

static bool is_power_of_two(qword_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}

static enum aarch64_elf64_error validate_segments(
        struct aarch64_elf64_image *image,
        bool inspect_interpreter_path) {
    bool entry_in_executable_segment = false;
    bool have_previous_load = false;
    guest_addr_t previous_load_address = 0;

    for (word_t i = 0; i < image->program_header_count; i++) {
        struct aarch64_elf64_program_header header;
        aarch64_elf64_program_header(image, i, &header);
        if (header.type == ELF_PT_INTERP) {
            if (!inspect_interpreter_path)
                continue;
            // Linux 只采用首个 PT_INTERP，后续条目不再参与校验。
            if (image->interpreter_path != NULL)
                continue;
            if (header.file_size < 2 ||
                    header.file_size > AARCH64_ELF64_INTERPRETER_PATH_MAX ||
                    !valid_file_range(image,
                            header.file_offset, header.file_size))
                return AARCH64_ELF64_BAD_SEGMENT;
            const byte_t *path = image->data +
                    (size_t) header.file_offset;
            if (path[(size_t) header.file_size - 1] != '\0')
                return AARCH64_ELF64_BAD_SEGMENT;
            const byte_t *terminator = memchr(
                    path, '\0', (size_t) header.file_size);
            assert(terminator != NULL);
            image->interpreter_path = (const char *) path;
            image->interpreter_path_length =
                    (size_t) (terminator - path);
            continue;
        }
        if (header.type != ELF_PT_LOAD)
            continue;

        image->load_segment_count++;
        if (header.flags & ~ELF_PF_MASK)
            return AARCH64_ELF64_BAD_SEGMENT;
        if (header.file_size > header.memory_size ||
                !valid_file_range(image, header.file_offset, header.file_size) ||
                !valid_guest_range(header.virtual_address, header.memory_size))
            return AARCH64_ELF64_BAD_SEGMENT;
        if ((header.virtual_address & GUEST_MEMORY_PAGE_MASK) !=
                (header.file_offset & GUEST_MEMORY_PAGE_MASK))
            return AARCH64_ELF64_BAD_SEGMENT;
        if (header.alignment > 1 &&
                (!is_power_of_two(header.alignment) ||
                (header.virtual_address & (header.alignment - 1)) !=
                (header.file_offset & (header.alignment - 1))))
            return AARCH64_ELF64_BAD_SEGMENT;
        if (have_previous_load &&
                header.virtual_address < previous_load_address)
            return AARCH64_ELF64_BAD_SEGMENT;
        previous_load_address = header.virtual_address;
        have_previous_load = true;

        if ((header.flags & ELF_PF_EXECUTE) && header.memory_size != 0 &&
                image->entry >= header.virtual_address &&
                image->entry - header.virtual_address < header.memory_size)
            entry_in_executable_segment = true;
    }

    if (image->load_segment_count == 0 || !entry_in_executable_segment)
        return AARCH64_ELF64_BAD_ENTRY;
    return AARCH64_ELF64_OK;
}

static enum aarch64_elf64_error parse_elf64(const void *data,
        size_t size, struct aarch64_elf64_image *image,
        bool inspect_interpreter_path) {
    *image = (struct aarch64_elf64_image) {0};
    if (size < AARCH64_ELF64_HEADER_SIZE)
        return AARCH64_ELF64_TRUNCATED;
    const byte_t *bytes = data;
    static const byte_t magic[] = {0x7f, 'E', 'L', 'F'};
    if (memcmp(bytes, magic, sizeof(magic)) != 0 ||
            bytes[4] != ELF64_CLASS || bytes[5] != ELF_LITTLE_ENDIAN ||
            bytes[6] != ELF_CURRENT_VERSION ||
            (bytes[7] != ELF_OSABI_SYSTEM_V && bytes[7] != ELF_OSABI_LINUX) ||
            bytes[8] != 0)
        return AARCH64_ELF64_BAD_IDENTIFICATION;
    word_t type = read_u16(bytes + 16);
    if (type != ELF_EXECUTABLE && type != ELF_SHARED_OBJECT)
        return AARCH64_ELF64_UNSUPPORTED_TYPE;
    if (read_u16(bytes + 18) != ELF_MACHINE_AARCH64)
        return AARCH64_ELF64_UNSUPPORTED_MACHINE;
    if (read_u32(bytes + 20) != ELF_CURRENT_VERSION ||
            read_u32(bytes + 48) != 0 ||
            read_u16(bytes + 52) != AARCH64_ELF64_HEADER_SIZE ||
            read_u16(bytes + 54) != AARCH64_ELF64_PROGRAM_HEADER_SIZE ||
            read_u16(bytes + 56) == 0 || read_u16(bytes + 56) == UINT16_MAX)
        return AARCH64_ELF64_BAD_HEADER;

    guest_addr_t entry = read_u64(bytes + 24);
    qword_t program_header_offset = read_u64(bytes + 32);
    word_t program_header_count = read_u16(bytes + 56);
    if ((entry & 3) != 0 || !valid_guest_range(entry, 1))
        return AARCH64_ELF64_BAD_ENTRY;
    if (program_header_offset > (qword_t) size ||
            (qword_t) program_header_count >
            ((qword_t) size - program_header_offset) /
                    AARCH64_ELF64_PROGRAM_HEADER_SIZE)
        return AARCH64_ELF64_TRUNCATED;

    *image = (struct aarch64_elf64_image) {
        .data = bytes,
        .size = size,
        .position_independent = type == ELF_SHARED_OBJECT,
        .entry = entry,
        .program_header_offset = program_header_offset,
        .program_header_count = program_header_count,
    };
    return validate_segments(image, inspect_interpreter_path);
}

enum aarch64_elf64_error aarch64_elf64_parse(const void *data,
        size_t size, struct aarch64_elf64_image *image) {
    return parse_elf64(data, size, image, true);
}

enum aarch64_elf64_error aarch64_elf64_parse_as_interpreter(
        const void *data, size_t size,
        struct aarch64_elf64_image *image) {
    return parse_elf64(data, size, image, false);
}
