#include <assert.h>
#include <string.h>

#include "guest/aarch64/elf64-loader.h"
#include "guest/aarch64/elf64.h"
#include "guest/aarch64/linux-process.h"
#include "guest/linux/auxv.h"
#include "guest/memory/address-space.h"

#define IMAGE_SIZE 1024
#define IMAGE_FILE_SIZE UINT64_C(0x300)
#define IMAGE_ALIGNMENT UINT64_C(0x10000)
#define INTERPRETER_PATH_OFFSET UINT64_C(0x180)
#define ENTRY_OFFSET UINT64_C(0x200)
#define MAIN_LOAD_BIAS UINT64_C(0x0000400000000000)
#define INTERPRETER_LOAD_BIAS UINT64_C(0x0000500000000000)
#define FIXED_MAIN_BASE UINT64_C(0x200000)
#define MAIN_BRK_LIMIT (MAIN_LOAD_BIAS + UINT64_C(0x100000))
#define STACK_TOP UINT64_C(0x00007fff00000000)
#define SIGNAL_TRAMPOLINE UINT64_C(0x00007ffe00000000)
#define MAIN_FILE_IDENTITY UINT64_C(0x1000000000000003)
#define INTERPRETER_FILE_IDENTITY UINT64_C(0x1000000000000004)

#define AARCH64_MOV_X0_42 UINT32_C(0xd2800540)
#define AARCH64_MOV_X8_WRITE UINT32_C(0xd2800808)
#define AARCH64_MOV_X8_EXIT UINT32_C(0xd2800ba8)
#define AARCH64_SVC_0 UINT32_C(0xd4000001)
#define AARCH64_BR_X0 UINT32_C(0xd61f0000)

static const char interpreter_path[] =
        "/lib/ld-musl-aarch64.so.1";

struct dynamic_probe {
    void *expected_task_opaque;
    guest_addr_t expected_main_entry;
    guest_addr_t expected_main_phdr;
    guest_addr_t expected_interpreter_base;
    unsigned calls;
};

struct dynamic_fixture {
    byte_t main_file[IMAGE_SIZE];
    byte_t interpreter_file[IMAGE_SIZE];
    const char *arguments[1];
    const char *environment[1];
    byte_t random[AARCH64_LINUX_PROCESS_RANDOM_SIZE];
    int task_opaque;
    struct dynamic_probe probe;
    struct guest_linux_syscall_service syscall_service;
    struct aarch64_linux_process_config config;
    struct aarch64_linux_interpreter_image interpreter;
};

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

static void put_program_header(byte_t *bytes, dword_t type,
        dword_t flags, qword_t offset, qword_t address,
        qword_t file_size, qword_t memory_size, qword_t alignment) {
    put_u32(bytes, type);
    put_u32(bytes + 4, flags);
    put_u64(bytes + 8, offset);
    put_u64(bytes + 16, address);
    put_u64(bytes + 32, file_size);
    put_u64(bytes + 40, memory_size);
    put_u64(bytes + 48, alignment);
}

static void make_dynamic_image(byte_t file[IMAGE_SIZE],
        bool has_interpreter, const dword_t *program,
        size_t instruction_count) {
    memset(file, 0, IMAGE_SIZE);
    file[0] = 0x7f;
    file[1] = 'E';
    file[2] = 'L';
    file[3] = 'F';
    file[4] = 2;
    file[5] = 1;
    file[6] = 1;
    put_u16(file + 16, 3);
    put_u16(file + 18, 183);
    put_u32(file + 20, 1);
    put_u64(file + 24, ENTRY_OFFSET);
    put_u64(file + 32, AARCH64_ELF64_HEADER_SIZE);
    put_u16(file + 52, AARCH64_ELF64_HEADER_SIZE);
    put_u16(file + 54, AARCH64_ELF64_PROGRAM_HEADER_SIZE);

    word_t header_count = has_interpreter ? 3 : 2;
    put_u16(file + 56, header_count);
    byte_t *headers = file + AARCH64_ELF64_HEADER_SIZE;
    qword_t header_size =
            (qword_t) header_count * AARCH64_ELF64_PROGRAM_HEADER_SIZE;
    put_program_header(headers, 6, 4,
            AARCH64_ELF64_HEADER_SIZE, AARCH64_ELF64_HEADER_SIZE,
            header_size, header_size, 8);

    byte_t *load_header =
            headers + AARCH64_ELF64_PROGRAM_HEADER_SIZE;
    if (has_interpreter) {
        put_program_header(load_header, 3, 4,
                INTERPRETER_PATH_OFFSET, INTERPRETER_PATH_OFFSET,
                sizeof(interpreter_path), sizeof(interpreter_path), 1);
        memcpy(file + INTERPRETER_PATH_OFFSET,
                interpreter_path, sizeof(interpreter_path));
        load_header += AARCH64_ELF64_PROGRAM_HEADER_SIZE;
    }
    put_program_header(load_header, 1, 5, 0, 0,
            IMAGE_FILE_SIZE, IMAGE_FILE_SIZE, IMAGE_ALIGNMENT);

    assert(instruction_count * 4 <=
            IMAGE_FILE_SIZE - ENTRY_OFFSET);
    for (size_t i = 0; i < instruction_count; i++)
        put_u32(file + ENTRY_OFFSET + i * 4, program[i]);
}

static void make_main_image(
        byte_t file[IMAGE_SIZE], bool has_interpreter) {
    const dword_t program[] = {
        AARCH64_MOV_X0_42,
        AARCH64_MOV_X8_EXIT,
        AARCH64_SVC_0,
    };
    make_dynamic_image(file, has_interpreter,
            program, array_size(program));
}

static void make_fixed_main_image(byte_t file[IMAGE_SIZE]) {
    make_main_image(file, true);
    put_u16(file + 16, 2);
    put_u64(file + 24, FIXED_MAIN_BASE + ENTRY_OFFSET);
    byte_t *headers = file + AARCH64_ELF64_HEADER_SIZE;
    put_u64(headers + 16,
            FIXED_MAIN_BASE + AARCH64_ELF64_HEADER_SIZE);
    put_u64(headers + AARCH64_ELF64_PROGRAM_HEADER_SIZE + 16,
            FIXED_MAIN_BASE + INTERPRETER_PATH_OFFSET);
    put_u64(headers + 2 * AARCH64_ELF64_PROGRAM_HEADER_SIZE + 16,
            FIXED_MAIN_BASE);
}

static void make_interpreter_image(
        byte_t file[IMAGE_SIZE], bool has_nested_interpreter) {
    const dword_t program[] = {
        AARCH64_MOV_X8_WRITE,
        AARCH64_SVC_0,
        AARCH64_BR_X0,
    };
    make_dynamic_image(file, has_nested_interpreter,
            program, array_size(program));
}

static qword_t read_user_qword(
        const struct guest_linux_syscall_context *context,
        guest_addr_t address) {
    byte_t bytes[8];
    struct guest_linux_user_fault fault = {0};
    assert(context->user.read(context->user.opaque,
            address, bytes, sizeof(bytes), &fault));
    qword_t value = 0;
    for (byte_t i = 0; i < sizeof(bytes); i++)
        value |= (qword_t) bytes[i] << (i * 8);
    return value;
}

static void assert_dynamic_auxv(
        const struct guest_linux_syscall_context *context,
        const struct dynamic_probe *probe) {
    assert((context->stack_pointer & 0xf) == 0);
    guest_addr_t cursor = context->stack_pointer;
    qword_t argument_count = read_user_qword(context, cursor);
    assert(argument_count == 1);
    cursor += 8 + (argument_count + 1) * 8;

    for (unsigned i = 0; i < 64; i++, cursor += 8) {
        if (read_user_qword(context, cursor) == 0) {
            cursor += 8;
            break;
        }
        assert(i != 63);
    }

    bool saw_entry = false;
    bool saw_phdr = false;
    bool saw_phent = false;
    bool saw_phnum = false;
    bool saw_base = false;
    bool saw_null = false;
    for (unsigned i = 0; i < 64; i++, cursor += 16) {
        qword_t type = read_user_qword(context, cursor);
        qword_t value = read_user_qword(context, cursor + 8);
        if (type == GUEST_AT_ENTRY) {
            assert(value == probe->expected_main_entry);
            saw_entry = true;
        } else if (type == GUEST_AT_PHDR) {
            assert(value == probe->expected_main_phdr);
            saw_phdr = true;
        } else if (type == GUEST_AT_PHENT) {
            assert(value == AARCH64_ELF64_PROGRAM_HEADER_SIZE);
            saw_phent = true;
        } else if (type == GUEST_AT_PHNUM) {
            assert(value == 3);
            saw_phnum = true;
        } else if (type == GUEST_AT_BASE) {
            assert(value == probe->expected_interpreter_base);
            saw_base = true;
        } else if (type == GUEST_AT_NULL) {
            assert(value == 0);
            saw_null = true;
            break;
        }
    }
    assert(saw_entry && saw_phdr && saw_phent && saw_phnum &&
            saw_base && saw_null);
}

static qword_t dispatch_from_interpreter(
        const struct guest_linux_syscall_context *context,
        const struct guest_linux_syscall *syscall,
        struct guest_linux_user_fault *fault) {
    use(fault);
    struct dynamic_probe *probe = context->runtime_opaque;
    assert(context->task_opaque == probe->expected_task_opaque);
    assert(probe->calls == 0);
    assert(syscall->number == 64);
    assert(syscall->arguments[0] == 0);
    assert_dynamic_auxv(context, probe);
    probe->calls++;
    return probe->expected_main_entry;
}

static void init_fixture(struct dynamic_fixture *fixture,
        bool main_has_interpreter, bool interpreter_has_interpreter) {
    memset(fixture, 0, sizeof(*fixture));
    make_main_image(fixture->main_file, main_has_interpreter);
    make_interpreter_image(fixture->interpreter_file,
            interpreter_has_interpreter);
    fixture->arguments[0] = "dynamic-test";
    fixture->environment[0] = "A=B";
    for (byte_t i = 0; i < sizeof(fixture->random); i++)
        fixture->random[i] = i;

    fixture->probe = (struct dynamic_probe) {
        .expected_task_opaque = &fixture->task_opaque,
        .expected_main_entry = MAIN_LOAD_BIAS + ENTRY_OFFSET,
        .expected_main_phdr =
                MAIN_LOAD_BIAS + AARCH64_ELF64_HEADER_SIZE,
        .expected_interpreter_base = INTERPRETER_LOAD_BIAS,
    };
    fixture->syscall_service = (struct guest_linux_syscall_service) {
        .runtime_opaque = &fixture->probe,
        .dispatch = dispatch_from_interpreter,
    };
    fixture->config = (struct aarch64_linux_process_config) {
        .elf_data = fixture->main_file,
        .elf_size = sizeof(fixture->main_file),
        .elf_file_source = guest_file_source_create(
                MAIN_FILE_IDENTITY, NULL, NULL),
        .load_bias = MAIN_LOAD_BIAS,
        .stack_top = STACK_TOP,
        .stack_size = 2 * GUEST_MEMORY_PAGE_SIZE,
        .signal_trampoline_page = SIGNAL_TRAMPOLINE,
        .brk_limit = MAIN_BRK_LIMIT,
        .executable = "/bin/dynamic-test",
        .arguments = fixture->arguments,
        .argument_count = array_size(fixture->arguments),
        .environment = fixture->environment,
        .environment_count = array_size(fixture->environment),
        .random = fixture->random,
        .tid = 1234,
        .task_opaque = &fixture->task_opaque,
        .syscalls = &fixture->syscall_service,
    };
    fixture->interpreter = (struct aarch64_linux_interpreter_image) {
        .data = fixture->interpreter_file,
        .size = sizeof(fixture->interpreter_file),
        .file_source = guest_file_source_create(
                INTERPRETER_FILE_IDENTITY, NULL, NULL),
        .load_bias = INTERPRETER_LOAD_BIAS,
    };
    assert(fixture->config.elf_file_source != NULL &&
            fixture->interpreter.file_source != NULL);
}

static struct aarch64_linux_process *create_dynamic_process(
        struct aarch64_linux_process_config *config,
        struct aarch64_linux_interpreter_image *interpreter,
        struct aarch64_linux_process_error *error) {
    assert(config->elf_file_source != NULL &&
            interpreter->file_source != NULL);
    struct aarch64_linux_process *process =
            aarch64_linux_process_create_with_interpreter(
                    config, interpreter, error);
    guest_file_source_release(config->elf_file_source);
    guest_file_source_release(interpreter->file_source);
    config->elf_file_source = NULL;
    interpreter->file_source = NULL;
    return process;
}

static struct aarch64_linux_process *create_main_process(
        struct aarch64_linux_process_config *config,
        struct aarch64_linux_process_error *error) {
    assert(config->elf_file_source != NULL);
    struct aarch64_linux_process *process =
            aarch64_linux_process_create(config, error);
    guest_file_source_release(config->elf_file_source);
    config->elf_file_source = NULL;
    return process;
}

static void run_dynamic_process(struct aarch64_linux_process *process,
        const struct dynamic_probe *probe) {
    const dword_t expected_instructions[] = {
        AARCH64_MOV_X8_WRITE,
        AARCH64_SVC_0,
        AARCH64_BR_X0,
        AARCH64_MOV_X0_42,
        AARCH64_MOV_X8_EXIT,
        AARCH64_SVC_0,
    };
    struct aarch64_linux_process_result result;
    for (size_t i = 0; i < array_size(expected_instructions); i++) {
        result = aarch64_linux_process_run_one(process);
        assert(result.instruction == expected_instructions[i]);
        if (i + 1 != array_size(expected_instructions)) {
            assert(result.status == AARCH64_LINUX_PROCESS_RUNNABLE);
            assert(result.signal == 0 && result.exit_status == 0 &&
                    result.fault.kind == GUEST_MEMORY_FAULT_NONE);
        }
    }
    assert(result.status == AARCH64_LINUX_PROCESS_EXIT);
    assert(result.exit_status == 42 && result.signal == 0);
    assert(result.fault.address == 0 && result.fault.access == 0 &&
            result.fault.kind == GUEST_MEMORY_FAULT_NONE);
    assert(probe->calls == 1);
}

static void assert_create_error(
        struct aarch64_linux_process_config *config,
        struct aarch64_linux_interpreter_image *interpreter,
        enum aarch64_linux_process_error_stage expected_stage,
        dword_t expected_detail) {
    struct aarch64_linux_process_error error = {
        .stage = UINT32_MAX,
        .detail = UINT32_MAX,
    };
    assert(create_dynamic_process(config, interpreter, &error) == NULL);
    assert(error.stage == (dword_t) expected_stage);
    assert(error.detail == expected_detail);
}

static void test_dynamic_run_and_ownership(void) {
    struct dynamic_fixture fixture;
    init_fixture(&fixture, true, false);
    struct aarch64_linux_process_error error = {
        .stage = UINT32_MAX,
        .detail = UINT32_MAX,
    };
    struct aarch64_linux_process *process =
            create_dynamic_process(
                    &fixture.config, &fixture.interpreter, &error);
    assert(process != NULL);
    assert(error.stage == AARCH64_LINUX_PROCESS_ERROR_NONE &&
            error.detail == 0);

    // 成功返回后，两份映像和所有输入描述符都不再由 process 借用。
    memset(fixture.main_file, 0xa5, sizeof(fixture.main_file));
    memset(fixture.interpreter_file, 0x5a,
            sizeof(fixture.interpreter_file));
    fixture.arguments[0] = NULL;
    fixture.environment[0] = NULL;
    fixture.syscall_service = (struct guest_linux_syscall_service) {0};
    fixture.interpreter = (struct aarch64_linux_interpreter_image) {0};
    fixture.config = (struct aarch64_linux_process_config) {0};

    run_dynamic_process(process, &fixture.probe);
    aarch64_linux_process_destroy(process);
}

static void test_fixed_main_with_pie_interpreter(void) {
    struct dynamic_fixture fixture;
    init_fixture(&fixture, true, false);
    make_fixed_main_image(fixture.main_file);
    fixture.config.load_bias = 0;
    fixture.config.brk_limit = FIXED_MAIN_BASE + UINT64_C(0x100000);
    fixture.probe.expected_main_entry = FIXED_MAIN_BASE + ENTRY_OFFSET;
    fixture.probe.expected_main_phdr =
            FIXED_MAIN_BASE + AARCH64_ELF64_HEADER_SIZE;
    struct aarch64_linux_process *process =
            create_dynamic_process(
                    &fixture.config, &fixture.interpreter, NULL);
    assert(process != NULL);
    run_dynamic_process(process, &fixture.probe);
    aarch64_linux_process_destroy(process);
}

static void test_interpreter_configuration_errors(void) {
    struct dynamic_fixture fixture;
    init_fixture(&fixture, true, false);
    struct aarch64_linux_process_error error;
    assert(create_main_process(&fixture.config, &error) == NULL);
    guest_file_source_release(fixture.interpreter.file_source);
    fixture.interpreter.file_source = NULL;
    assert(error.stage == AARCH64_LINUX_PROCESS_ERROR_INTERPRETER);
    assert(error.detail == AARCH64_LINUX_INTERPRETER_CONFIG_REQUIRED);

    init_fixture(&fixture, false, false);
    assert_create_error(&fixture.config, &fixture.interpreter,
            AARCH64_LINUX_PROCESS_ERROR_INTERPRETER,
            AARCH64_LINUX_INTERPRETER_CONFIG_UNEXPECTED);

    init_fixture(&fixture, true, false);
    struct aarch64_linux_interpreter_image invalid =
            fixture.interpreter;
    fixture.interpreter.file_source = NULL;
    invalid.data = NULL;
    assert_create_error(&fixture.config, &invalid,
            AARCH64_LINUX_PROCESS_ERROR_INTERPRETER,
            AARCH64_LINUX_INTERPRETER_CONFIG_INVALID);
    init_fixture(&fixture, true, false);
    invalid = fixture.interpreter;
    fixture.interpreter.file_source = NULL;
    invalid.size = 0;
    assert_create_error(&fixture.config, &invalid,
            AARCH64_LINUX_PROCESS_ERROR_INTERPRETER,
            AARCH64_LINUX_INTERPRETER_CONFIG_INVALID);
}

static void test_nested_interpreter_is_not_followed(void) {
    struct dynamic_fixture fixture;
    init_fixture(&fixture, true, true);
    // 即使解释器自身的 PT_INTERP 已损坏，Linux 也不会递归解析它。
    put_u64(fixture.interpreter_file + AARCH64_ELF64_HEADER_SIZE +
            AARCH64_ELF64_PROGRAM_HEADER_SIZE + 32, 0);
    struct aarch64_linux_process *process =
            create_dynamic_process(
                    &fixture.config, &fixture.interpreter, NULL);
    assert(process != NULL);
    run_dynamic_process(process, &fixture.probe);
    aarch64_linux_process_destroy(process);
}

static void test_interpreter_elf_error(void) {
    struct dynamic_fixture fixture;
    init_fixture(&fixture, true, false);
    fixture.interpreter_file[0] = 0;
    assert_create_error(&fixture.config, &fixture.interpreter,
            AARCH64_LINUX_PROCESS_ERROR_INTERPRETER_ELF,
            AARCH64_ELF64_BAD_IDENTIFICATION);
}

static void test_load_bias_errors(void) {
    struct dynamic_fixture fixture;
    init_fixture(&fixture, true, false);
    fixture.config.load_bias += GUEST_MEMORY_PAGE_SIZE;
    assert_create_error(&fixture.config, &fixture.interpreter,
            AARCH64_LINUX_PROCESS_ERROR_LOAD,
            AARCH64_ELF64_LOAD_UNSUPPORTED_LAYOUT);

    init_fixture(&fixture, true, false);
    fixture.interpreter.load_bias += GUEST_MEMORY_PAGE_SIZE;
    assert_create_error(&fixture.config, &fixture.interpreter,
            AARCH64_LINUX_PROCESS_ERROR_INTERPRETER_LOAD,
            AARCH64_ELF64_LOAD_UNSUPPORTED_LAYOUT);
}

static void test_image_mapping_conflict(void) {
    struct dynamic_fixture fixture;
    init_fixture(&fixture, true, false);
    fixture.interpreter.load_bias = MAIN_LOAD_BIAS;
    assert_create_error(&fixture.config, &fixture.interpreter,
            AARCH64_LINUX_PROCESS_ERROR_INTERPRETER_LOAD,
            AARCH64_ELF64_LOAD_MAPPING_CONFLICT);
}

static void test_interpreter_overlaps_main_brk_reservation(void) {
    struct dynamic_fixture fixture;
    init_fixture(&fixture, true, false);
    fixture.interpreter.load_bias =
            MAIN_LOAD_BIAS + IMAGE_ALIGNMENT;
    fixture.probe.expected_interpreter_base =
            fixture.interpreter.load_bias;
    assert_create_error(&fixture.config, &fixture.interpreter,
            AARCH64_LINUX_PROCESS_ERROR_LAYOUT, 0);
}

static void test_interpreter_adjacent_to_main_brk_reservation(void) {
    struct dynamic_fixture fixture;
    init_fixture(&fixture, true, false);
    fixture.interpreter.load_bias = MAIN_BRK_LIMIT;
    fixture.probe.expected_interpreter_base = MAIN_BRK_LIMIT;
    struct aarch64_linux_process *process =
            create_dynamic_process(
                    &fixture.config, &fixture.interpreter, NULL);
    assert(process != NULL);
    run_dynamic_process(process, &fixture.probe);
    aarch64_linux_process_destroy(process);
}

static void test_empty_main_brk_reservation(void) {
    struct dynamic_fixture fixture;
    init_fixture(&fixture, true, false);
    fixture.config.brk_limit =
            MAIN_LOAD_BIAS + GUEST_MEMORY_PAGE_SIZE;
    struct aarch64_linux_process *process =
            create_dynamic_process(
                    &fixture.config, &fixture.interpreter, NULL);
    assert(process != NULL);
    run_dynamic_process(process, &fixture.probe);
    aarch64_linux_process_destroy(process);
}

int main(void) {
    test_dynamic_run_and_ownership();
    test_fixed_main_with_pie_interpreter();
    test_interpreter_configuration_errors();
    test_nested_interpreter_is_not_followed();
    test_interpreter_elf_error();
    test_load_bias_errors();
    test_image_mapping_conflict();
    test_interpreter_overlaps_main_brk_reservation();
    test_interpreter_adjacent_to_main_brk_reservation();
    test_empty_main_brk_reservation();
    return 0;
}
