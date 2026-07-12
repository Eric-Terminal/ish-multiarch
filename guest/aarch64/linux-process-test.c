#include <assert.h>
#include <string.h>

#include "guest/aarch64/elf64.h"
#include "guest/aarch64/elf64-loader.h"
#include "guest/aarch64/linux-process.h"
#include "guest/aarch64/linux-signal-abi.h"
#include "guest/aarch64/linux-stack.h"
#include "guest/linux/auxv.h"
#include "guest/memory/address-space.h"
#include "guest/memory/page-table.h"

#define TEST_FILE_SIZE 768
#define TEXT_BASE UINT64_C(0x400000)
#define DATA_ADDRESS UINT64_C(0x500200)
#define BRK_LIMIT UINT64_C(0x600000)
#define STACK_TOP UINT64_C(0x00007fff00000000)
#define SIGNAL_TRAMPOLINE UINT64_C(0x00007ffe00000000)
#define PIE_LOAD_BIAS UINT64_C(0x0000400000000000)
#define SIGNAL_HANDLER (TEXT_BASE + UINT64_C(0x140))

static const char process_interpreter_path[] =
        "/lib/ld-musl-aarch64.so.1";

struct test_sink {
    void *expected_task_opaque;
    byte_t data[3];
    unsigned calls;
    bool saw_initial_stack;
};

struct signal_probe {
    void *expected_task_opaque;
    dword_t status;
    sdword_t signal;
    unsigned calls;
};

struct pie_probe {
    unsigned calls;
};

struct signal_closure {
    void *expected_task_opaque;
    guest_addr_t interrupted_sp;
    unsigned dispatches;
    unsigned polls;
    unsigned restores;
    unsigned bad_frames;
    bool pending;
    bool delivered;
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
    put_u64(file + 24, TEXT_BASE + 0x100);
    put_u64(file + 32, AARCH64_ELF64_HEADER_SIZE);
    put_u16(file + 52, AARCH64_ELF64_HEADER_SIZE);
    put_u16(file + 54, AARCH64_ELF64_PROGRAM_HEADER_SIZE);
    put_u16(file + 56, 3);

    byte_t *headers = file + AARCH64_ELF64_HEADER_SIZE;
    put_program_header(headers, 6, 4,
            AARCH64_ELF64_HEADER_SIZE,
            TEXT_BASE + AARCH64_ELF64_HEADER_SIZE,
            3 * AARCH64_ELF64_PROGRAM_HEADER_SIZE,
            3 * AARCH64_ELF64_PROGRAM_HEADER_SIZE, 8);
    put_program_header(headers + AARCH64_ELF64_PROGRAM_HEADER_SIZE,
            1, 5, 0, TEXT_BASE, 0x180, 0x180, 0x1000);
    put_program_header(headers + 2 * AARCH64_ELF64_PROGRAM_HEADER_SIZE,
            1, 4, 0x200, DATA_ADDRESS, 3, 3, 0x1000);

    const dword_t program[] = {
        UINT32_C(0xd2800020), UINT32_C(0xd2804001),
        UINT32_C(0xf2a00a01), UINT32_C(0xd2800062),
        UINT32_C(0xd2800808), UINT32_C(0xd4000001),
        UINT32_C(0xd2800540), UINT32_C(0xd2800ba8),
        UINT32_C(0xd4000001),
    };
    for (size_t i = 0; i < array_size(program); i++)
        put_u32(file + 0x100 + i * 4, program[i]);
    put_u32(file + 0x140, UINT32_C(0xd65f03c0));
    memcpy(file + 0x200, "hi\n", 3);
}

static void make_interpreted_test_elf(byte_t file[TEST_FILE_SIZE]) {
    make_test_elf(file);
    put_program_header(file + AARCH64_ELF64_HEADER_SIZE,
            3, 4, 0x180, 0, sizeof(process_interpreter_path),
            sizeof(process_interpreter_path), 1);
    memcpy(file + 0x180, process_interpreter_path,
            sizeof(process_interpreter_path));
}

static qword_t read_user_qword(
        const struct guest_linux_syscall_context *context,
        guest_addr_t address) {
    byte_t bytes[8];
    struct guest_linux_user_fault fault;
    assert(context->user.read(context->user.opaque,
            address, bytes, sizeof(bytes), &fault));
    qword_t value = 0;
    for (byte_t i = 0; i < sizeof(bytes); i++)
        value |= (qword_t) bytes[i] << (i * 8);
    return value;
}

static void assert_user_string(
        const struct guest_linux_syscall_context *context,
        guest_addr_t address, const char *expected) {
    struct guest_linux_user_fault fault;
    for (size_t i = 0;; i++) {
        char value;
        assert(context->user.read(context->user.opaque,
                address + i, &value, 1, &fault));
        assert(value == expected[i]);
        if (value == '\0')
            return;
    }
}

static void assert_initial_stack(
        const struct guest_linux_syscall_context *context) {
    assert((context->stack_pointer & 0xf) == 0);
    guest_addr_t cursor = context->stack_pointer;
    assert(read_user_qword(context, cursor) == 2);
    guest_addr_t argv0 = read_user_qword(context, cursor + 8);
    guest_addr_t argv1 = read_user_qword(context, cursor + 16);
    assert(read_user_qword(context, cursor + 24) == 0);
    assert_user_string(context, argv0, "process-test");
    assert_user_string(context, argv1, "one");

    cursor += 32;
    guest_addr_t environment = read_user_qword(context, cursor);
    assert(read_user_qword(context, cursor + 8) == 0);
    assert_user_string(context, environment, "A=B");
    cursor += 16;

    guest_addr_t random_address = 0;
    guest_addr_t executable_address = 0;
    bool saw_base = false;
    bool saw_null = false;
    for (unsigned i = 0; i < 32; i++, cursor += 16) {
        qword_t type = read_user_qword(context, cursor);
        qword_t value = read_user_qword(context, cursor + 8);
        if (type == GUEST_AT_BASE) {
            assert(value == 0);
            saw_base = true;
        } else if (type == GUEST_AT_RANDOM) {
            random_address = value;
        } else if (type == GUEST_AT_EXECFN) {
            executable_address = value;
        } else if (type == GUEST_AT_NULL) {
            saw_null = true;
            break;
        }
    }
    assert(saw_base && saw_null && random_address != 0 &&
            executable_address != 0);
    assert_user_string(context, executable_address, "/bin/process-test");
    byte_t random[AARCH64_LINUX_PROCESS_RANDOM_SIZE];
    struct guest_linux_user_fault fault;
    assert(context->user.read(context->user.opaque,
            random_address, random, sizeof(random), &fault));
    for (byte_t i = 0; i < sizeof(random); i++)
        assert(random[i] == i);
}

static qword_t read_auxv_value(
        const struct guest_linux_syscall_context *context,
        qword_t wanted_type, bool *found) {
    guest_addr_t cursor = context->stack_pointer;
    qword_t argument_count = read_user_qword(context, cursor);
    assert(argument_count <= 64);
    cursor += 8 + (argument_count + 1) * 8;
    for (unsigned i = 0; i < 64; i++, cursor += 8) {
        if (read_user_qword(context, cursor) == 0) {
            cursor += 8;
            break;
        }
        assert(i != 63);
    }
    for (unsigned i = 0; i < 64; i++, cursor += 16) {
        qword_t type = read_user_qword(context, cursor);
        qword_t value = read_user_qword(context, cursor + 8);
        if (type == wanted_type) {
            *found = true;
            return value;
        }
        if (type == GUEST_AT_NULL)
            break;
    }
    *found = false;
    return 0;
}

static qword_t dispatch_sink(
        const struct guest_linux_syscall_context *context,
        const struct guest_linux_syscall *syscall,
        struct guest_linux_user_fault *fault) {
    struct test_sink *sink = context->runtime_opaque;
    assert(context->task_opaque == sink->expected_task_opaque);
    assert(syscall->number == 64);
    assert(syscall->arguments[0] == 1);
    assert(syscall->arguments[1] == DATA_ADDRESS);
    assert(syscall->arguments[2] == 3);
    assert(context->user.read(context->user.opaque,
            DATA_ADDRESS, sink->data, sizeof(sink->data), fault));
    assert_initial_stack(context);
    sink->saw_initial_stack = true;
    sink->calls++;
    return sizeof(sink->data);
}

static struct guest_linux_signal_poll_result poll_signal(
        const struct guest_linux_signal_context *context,
        guest_linux_signal_installer installer,
        void *installer_opaque) {
    struct signal_probe *probe = context->runtime_opaque;
    assert(context->task_opaque == probe->expected_task_opaque);
    assert(installer != NULL && installer_opaque != NULL);
    probe->calls++;
    return (struct guest_linux_signal_poll_result) {
        .status = probe->status,
        .signal = probe->signal,
    };
}

static qword_t dispatch_pie_probe(
        const struct guest_linux_syscall_context *context,
        const struct guest_linux_syscall *syscall,
        struct guest_linux_user_fault *fault) {
    use(fault);
    struct pie_probe *probe = context->runtime_opaque;
    assert(context->task_opaque == NULL && syscall->number == 64);
    bool found;
    assert(read_auxv_value(context, GUEST_AT_ENTRY, &found) ==
            PIE_LOAD_BIAS + TEXT_BASE + 0x100 && found);
    assert(read_auxv_value(context, GUEST_AT_PHDR, &found) ==
            PIE_LOAD_BIAS + TEXT_BASE + AARCH64_ELF64_HEADER_SIZE && found);
    assert(read_auxv_value(context, GUEST_AT_BASE, &found) == 0 && found);
    probe->calls++;
    return 0;
}

static struct aarch64_linux_process_config make_config(
        const byte_t *file, size_t file_size,
        const char *executable, const char *const *arguments,
        size_t argument_count, const char *const *environment,
        size_t environment_count, const byte_t *random) {
    return (struct aarch64_linux_process_config) {
        .elf_data = file,
        .elf_size = file_size,
        .stack_top = STACK_TOP,
        .stack_size = 2 * GUEST_MEMORY_PAGE_SIZE,
        .signal_trampoline_page = SIGNAL_TRAMPOLINE,
        .brk_limit = BRK_LIMIT,
        .executable = executable,
        .arguments = arguments,
        .argument_count = argument_count,
        .environment = environment,
        .environment_count = environment_count,
        .random = random,
        .tid = 1234,
    };
}

static void make_default_inputs(
        const char *arguments[2], const char *environment[1],
        byte_t random[AARCH64_LINUX_PROCESS_RANDOM_SIZE]) {
    arguments[0] = "process-test";
    arguments[1] = "one";
    environment[0] = "A=B";
    for (byte_t i = 0; i < AARCH64_LINUX_PROCESS_RANDOM_SIZE; i++)
        random[i] = i;
}

static void assert_empty_result_fields(
        const struct aarch64_linux_process_result *result) {
    assert(result->signal == 0 && result->exit_status == 0);
    assert(result->fault.address == 0 && result->fault.access == 0 &&
            result->fault.kind == 0);
}

static void test_load_run_and_ownership(void) {
    byte_t file[TEST_FILE_SIZE];
    make_test_elf(file);
    char executable[] = "/bin/process-test";
    char argument0[] = "process-test";
    char argument1[] = "one";
    char environment0[] = "A=B";
    const char *arguments[] = {argument0, argument1};
    const char *environment[] = {environment0};
    byte_t random[AARCH64_LINUX_PROCESS_RANDOM_SIZE];
    for (byte_t i = 0; i < sizeof(random); i++)
        random[i] = i;

    int task_opaque;
    struct test_sink sink = {
        .expected_task_opaque = &task_opaque,
    };
    struct signal_probe signal = {
        .expected_task_opaque = &task_opaque,
        .status = GUEST_LINUX_SIGNAL_POLL_IDLE,
    };
    struct guest_linux_syscall_service syscall_service = {
        .runtime_opaque = &sink,
        .dispatch = dispatch_sink,
    };
    struct guest_linux_signal_service signal_service = {
        .runtime_opaque = &signal,
        .poll = poll_signal,
    };
    struct aarch64_linux_process_config config = make_config(
            file, sizeof(file), executable, arguments,
            array_size(arguments), environment,
            array_size(environment), random);
    config.task_opaque = &task_opaque;
    config.syscalls = &syscall_service;
    config.signals = &signal_service;
    struct aarch64_linux_process_error error = {
        .stage = UINT32_MAX,
        .detail = UINT32_MAX,
    };
    struct aarch64_linux_process *process =
            aarch64_linux_process_create(&config, &error);
    assert(process != NULL);
    assert(error.stage == AARCH64_LINUX_PROCESS_ERROR_NONE &&
            error.detail == 0);

    // 成功返回后，process 不得再借用这些初始化输入与描述符。
    memset(file, 0xa5, sizeof(file));
    memset(executable, 'x', sizeof(executable));
    memset(argument0, 'x', sizeof(argument0));
    memset(argument1, 'x', sizeof(argument1));
    memset(environment0, 'x', sizeof(environment0));
    arguments[0] = NULL;
    arguments[1] = NULL;
    environment[0] = NULL;
    memset(random, 0xff, sizeof(random));
    syscall_service = (struct guest_linux_syscall_service) {0};
    signal_service = (struct guest_linux_signal_service) {0};
    config = (struct aarch64_linux_process_config) {0};

    unsigned steps = 0;
    struct aarch64_linux_process_result result;
    do {
        result = aarch64_linux_process_run_one(process);
        steps++;
        if (result.status == AARCH64_LINUX_PROCESS_RUNNABLE)
            assert_empty_result_fields(&result);
    } while (result.status == AARCH64_LINUX_PROCESS_RUNNABLE);
    assert(result.status == AARCH64_LINUX_PROCESS_EXIT);
    assert(result.exit_status == 42 && result.signal == 0);
    assert(result.fault.address == 0 && result.fault.access == 0 &&
            result.fault.kind == 0);
    assert(steps == 9);
    assert(sink.calls == 1 && sink.saw_initial_stack);
    assert(memcmp(sink.data, "hi\n", 3) == 0);
    assert(signal.calls == 8);
    aarch64_linux_process_destroy(process);
    aarch64_linux_process_destroy(NULL);
}

static void test_static_pie(void) {
    byte_t file[TEST_FILE_SIZE];
    make_test_elf(file);
    struct aarch64_linux_executable_info info =
            aarch64_linux_inspect_executable(file, sizeof(file));
    assert(info.status == AARCH64_LINUX_EXECUTABLE_VALID &&
            info.elf_error == 0 && info.position_independent == 0);
    info = aarch64_linux_inspect_executable(NULL, sizeof(file));
    assert(info.status == AARCH64_LINUX_EXECUTABLE_BAD_ELF &&
            info.elf_error == AARCH64_ELF64_BAD_IDENTIFICATION);
    put_u16(file + 16, 3);
    info = aarch64_linux_inspect_executable(file, sizeof(file));
    assert(info.status == AARCH64_LINUX_EXECUTABLE_VALID &&
            info.elf_error == 0 && info.position_independent == 1);
    const dword_t program[] = {
        UINT32_C(0xd2800808), UINT32_C(0xd4000001),
        UINT32_C(0xd2800540), UINT32_C(0xd2800ba8),
        UINT32_C(0xd4000001),
    };
    for (size_t i = 0; i < array_size(program); i++)
        put_u32(file + 0x100 + i * 4, program[i]);
    const char *arguments[2];
    const char *environment[1];
    byte_t random[AARCH64_LINUX_PROCESS_RANDOM_SIZE];
    make_default_inputs(arguments, environment, random);
    struct aarch64_linux_process_config config = make_config(
            file, sizeof(file), "/bin/process-test", arguments,
            array_size(arguments), environment,
            array_size(environment), random);
    config.load_bias = PIE_LOAD_BIAS;
    config.brk_limit = PIE_LOAD_BIAS + BRK_LIMIT;
    struct pie_probe probe = {0};
    const struct guest_linux_syscall_service syscalls = {
        .runtime_opaque = &probe,
        .dispatch = dispatch_pie_probe,
    };
    config.syscalls = &syscalls;
    struct aarch64_linux_process *process =
            aarch64_linux_process_create(&config, NULL);
    assert(process != NULL);
    unsigned steps = 0;
    struct aarch64_linux_process_result result;
    do {
        result = aarch64_linux_process_run_one(process);
        steps++;
    } while (result.status == AARCH64_LINUX_PROCESS_RUNNABLE);
    assert(result.status == AARCH64_LINUX_PROCESS_EXIT);
    assert(result.exit_status == 42 && steps == 5 && probe.calls == 1);
    aarch64_linux_process_destroy(process);
}

static void assert_fresh_create_succeeds(
        const byte_t file[TEST_FILE_SIZE]) {
    const char *arguments[2];
    const char *environment[1];
    byte_t random[AARCH64_LINUX_PROCESS_RANDOM_SIZE];
    make_default_inputs(arguments, environment, random);
    struct aarch64_linux_process_config config = make_config(
            file, TEST_FILE_SIZE, "/bin/process-test", arguments,
            array_size(arguments), environment,
            array_size(environment), random);
    struct aarch64_linux_process *process =
            aarch64_linux_process_create(&config, NULL);
    assert(process != NULL);
    aarch64_linux_process_destroy(process);
}

static void assert_create_fails(
        const struct aarch64_linux_process_config *config,
        enum aarch64_linux_process_error_stage expected_stage,
        dword_t expected_detail) {
    struct aarch64_linux_process_error error = {
        .stage = UINT32_MAX,
        .detail = UINT32_MAX,
    };
    assert(aarch64_linux_process_create(config, &error) == NULL);
    assert(error.stage == (dword_t) expected_stage);
    assert(error.detail == expected_detail);
}

static void test_interpreter_path_query(void) {
    byte_t file[TEST_FILE_SIZE];
    make_interpreted_test_elf(file);
    struct aarch64_linux_interpreter_path_result result =
            aarch64_linux_copy_interpreter_path(
                    file, sizeof(file), NULL, 0);
    assert(result.status ==
            AARCH64_LINUX_INTERPRETER_PATH_BUFFER_TOO_SMALL);
    assert(result.elf_error == 0 &&
            result.required_size == sizeof(process_interpreter_path));

    char destination[64];
    char unchanged[sizeof(destination)];
    memset(destination, 0xa5, sizeof(destination));
    memcpy(unchanged, destination, sizeof(unchanged));
    result = aarch64_linux_copy_interpreter_path(
            file, sizeof(file), destination,
            sizeof(process_interpreter_path) - 1);
    assert(result.status ==
            AARCH64_LINUX_INTERPRETER_PATH_BUFFER_TOO_SMALL);
    assert(result.required_size == sizeof(process_interpreter_path));
    assert(memcmp(destination, unchanged, sizeof(destination)) == 0);

    result = aarch64_linux_copy_interpreter_path(
            file, sizeof(file), destination, sizeof(destination));
    assert(result.status == AARCH64_LINUX_INTERPRETER_PATH_COPIED);
    assert(result.elf_error == 0 &&
            result.required_size == sizeof(process_interpreter_path));
    assert(strcmp(destination, process_interpreter_path) == 0);

    make_test_elf(file);
    result = aarch64_linux_copy_interpreter_path(
            file, sizeof(file), destination, sizeof(destination));
    assert(result.status == AARCH64_LINUX_INTERPRETER_PATH_NONE);
    assert(result.elf_error == 0 && result.required_size == 0);

    file[0] = 0;
    result = aarch64_linux_copy_interpreter_path(
            file, sizeof(file), destination, sizeof(destination));
    assert(result.status == AARCH64_LINUX_INTERPRETER_PATH_BAD_ELF);
    assert(result.elf_error == AARCH64_ELF64_BAD_IDENTIFICATION &&
            result.required_size == 0);
    result = aarch64_linux_copy_interpreter_path(
            NULL, sizeof(file), destination, sizeof(destination));
    assert(result.status == AARCH64_LINUX_INTERPRETER_PATH_BAD_ELF);
    assert(result.elf_error == AARCH64_ELF64_BAD_IDENTIFICATION);
}

static void test_failure_transactions(void) {
    byte_t file[TEST_FILE_SIZE];
    make_test_elf(file);
    const char *arguments[2];
    const char *environment[1];
    byte_t random[AARCH64_LINUX_PROCESS_RANDOM_SIZE];
    make_default_inputs(arguments, environment, random);
    struct aarch64_linux_process_config config = make_config(
            file, sizeof(file), "/bin/process-test", arguments,
            array_size(arguments), environment,
            array_size(environment), random);

    config.tid = 0;
    assert_create_fails(&config,
            AARCH64_LINUX_PROCESS_ERROR_ARGUMENT, 0);
    assert_fresh_create_succeeds(file);

    config = make_config(file, sizeof(file), "/bin/process-test",
            arguments, array_size(arguments), environment,
            array_size(environment), random);
    config.brk_limit = UINT64_C(1) << 48;
    assert_create_fails(&config,
            AARCH64_LINUX_PROCESS_ERROR_ARGUMENT, 0);
    assert_fresh_create_succeeds(file);

    const char *invalid_arguments[] = {"process-test", NULL};
    config = make_config(file, sizeof(file), "/bin/process-test",
            invalid_arguments, array_size(invalid_arguments), environment,
            array_size(environment), random);
    assert_create_fails(&config,
            AARCH64_LINUX_PROCESS_ERROR_ARGUMENT, 0);
    assert_fresh_create_succeeds(file);

    const char *invalid_environment[] = {NULL};
    config = make_config(file, sizeof(file), "/bin/process-test",
            arguments, array_size(arguments), invalid_environment,
            array_size(invalid_environment), random);
    assert_create_fails(&config,
            AARCH64_LINUX_PROCESS_ERROR_ARGUMENT, 0);
    assert_fresh_create_succeeds(file);

    byte_t invalid_elf[TEST_FILE_SIZE];
    memcpy(invalid_elf, file, sizeof(invalid_elf));
    invalid_elf[0] = 0;
    config = make_config(invalid_elf, sizeof(invalid_elf),
            "/bin/process-test", arguments, array_size(arguments),
            environment, array_size(environment), random);
    assert_create_fails(&config, AARCH64_LINUX_PROCESS_ERROR_ELF,
            AARCH64_ELF64_BAD_IDENTIFICATION);
    assert_fresh_create_succeeds(file);

    byte_t interpreted[TEST_FILE_SIZE];
    make_interpreted_test_elf(interpreted);
    config = make_config(interpreted, sizeof(interpreted),
            "/bin/process-test", arguments, array_size(arguments),
            environment, array_size(environment), random);
    assert_create_fails(&config,
            AARCH64_LINUX_PROCESS_ERROR_INTERPRETER,
            AARCH64_LINUX_INTERPRETER_CONFIG_REQUIRED);
    assert_fresh_create_succeeds(file);

    byte_t pie[TEST_FILE_SIZE];
    memcpy(pie, file, sizeof(pie));
    put_u16(pie + 16, 3);
    config = make_config(pie, sizeof(pie), "/bin/process-test",
            arguments, array_size(arguments), environment,
            array_size(environment), random);
    assert_create_fails(&config, AARCH64_LINUX_PROCESS_ERROR_LOAD,
            AARCH64_ELF64_LOAD_UNSUPPORTED_LAYOUT);
    assert_fresh_create_succeeds(file);

    config = make_config(file, sizeof(file), "/bin/process-test",
            arguments, array_size(arguments), environment,
            array_size(environment), random);
    config.signal_trampoline_page = TEXT_BASE;
    assert_create_fails(&config,
            AARCH64_LINUX_PROCESS_ERROR_TRAMPOLINE,
            GUEST_PAGE_TABLE_ALREADY_MAPPED);
    assert_fresh_create_succeeds(file);

    config = make_config(file, sizeof(file), "/bin/process-test",
            arguments, array_size(arguments), environment,
            array_size(environment), random);
    config.stack_top = TEXT_BASE + GUEST_MEMORY_PAGE_SIZE;
    config.stack_size = 2 * GUEST_MEMORY_PAGE_SIZE;
    assert_create_fails(&config, AARCH64_LINUX_PROCESS_ERROR_STACK,
            AARCH64_LINUX_STACK_MAPPING_CONFLICT);
    assert_fresh_create_succeeds(file);

    char huge_executable[2 * GUEST_MEMORY_PAGE_SIZE];
    memset(huge_executable, 'x', sizeof(huge_executable) - 1);
    huge_executable[sizeof(huge_executable) - 1] = '\0';
    config = make_config(file, sizeof(file), huge_executable,
            arguments, array_size(arguments), environment,
            array_size(environment), random);
    config.stack_size = GUEST_MEMORY_PAGE_SIZE;
    assert_create_fails(&config, AARCH64_LINUX_PROCESS_ERROR_STACK,
            AARCH64_LINUX_STACK_OVERFLOW);
    assert_fresh_create_succeeds(file);

    config = make_config(file, sizeof(file), "/bin/process-test",
            arguments, array_size(arguments), environment,
            array_size(environment), random);
    config.brk_limit = SIGNAL_TRAMPOLINE + GUEST_MEMORY_PAGE_SIZE;
    assert_create_fails(&config,
            AARCH64_LINUX_PROCESS_ERROR_LAYOUT, 0);
    assert_fresh_create_succeeds(file);

    // brk 末端与 trampoline 相邻不构成重叠。
    config.brk_limit = SIGNAL_TRAMPOLINE;
    struct aarch64_linux_process *adjacent =
            aarch64_linux_process_create(&config, NULL);
    assert(adjacent != NULL);
    aarch64_linux_process_destroy(adjacent);

    assert_create_fails(NULL,
            AARCH64_LINUX_PROCESS_ERROR_ARGUMENT, 0);
}

static void test_poll_events(void) {
    byte_t file[TEST_FILE_SIZE];
    make_test_elf(file);
    const char *arguments[2];
    const char *environment[1];
    byte_t random[AARCH64_LINUX_PROCESS_RANDOM_SIZE];
    make_default_inputs(arguments, environment, random);
    int task_opaque;
    struct signal_probe probe = {
        .expected_task_opaque = &task_opaque,
        .status = GUEST_LINUX_SIGNAL_POLL_STOP,
        .signal = 19,
    };
    const struct guest_linux_signal_service service = {
        .runtime_opaque = &probe,
        .poll = poll_signal,
    };
    struct aarch64_linux_process_config config = make_config(
            file, sizeof(file), "/bin/process-test", arguments,
            array_size(arguments), environment,
            array_size(environment), random);
    config.task_opaque = &task_opaque;
    config.signals = &service;
    struct aarch64_linux_process *process =
            aarch64_linux_process_create(&config, NULL);
    assert(process != NULL);

    struct aarch64_linux_process_result result =
            aarch64_linux_process_poll_signals(process);
    assert(result.status == AARCH64_LINUX_PROCESS_STOP);
    assert(result.signal == 19 && result.exit_status == 0 &&
            result.instruction == 0 && result.fault.kind == 0);
    probe.status = GUEST_LINUX_SIGNAL_POLL_TERMINATE;
    probe.signal = 9;
    result = aarch64_linux_process_poll_signals(process);
    assert(result.status == AARCH64_LINUX_PROCESS_TERMINATE);
    assert(result.signal == 9 && result.exit_status == 0 &&
            result.instruction == 0 && result.fault.kind == 0);
    probe.status = GUEST_LINUX_SIGNAL_POLL_IDLE;
    probe.signal = 0;
    result = aarch64_linux_process_poll_signals(process);
    assert(result.status == AARCH64_LINUX_PROCESS_RUNNABLE);
    assert_empty_result_fields(&result);
    assert(probe.calls == 3);
    aarch64_linux_process_destroy(process);
}

static struct aarch64_linux_process *create_fault_process(
        byte_t file[TEST_FILE_SIZE], dword_t instruction) {
    make_test_elf(file);
    put_u32(file + 0x100, instruction);
    const char *arguments[2];
    const char *environment[1];
    byte_t random[AARCH64_LINUX_PROCESS_RANDOM_SIZE];
    make_default_inputs(arguments, environment, random);
    struct aarch64_linux_process_config config = make_config(
            file, TEST_FILE_SIZE, "/bin/process-test", arguments,
            array_size(arguments), environment,
            array_size(environment), random);
    return aarch64_linux_process_create(&config, NULL);
}

static void test_fault_events(void) {
    byte_t file[TEST_FILE_SIZE];
    struct aarch64_linux_process *process = create_fault_process(
            file, UINT32_C(0xf9400001));
    assert(process != NULL);
    for (unsigned i = 0; i < 2; i++) {
        struct aarch64_linux_process_result result =
                aarch64_linux_process_run_one(process);
        assert(result.status == AARCH64_LINUX_PROCESS_DATA_FAULT);
        assert(result.instruction == UINT32_C(0xf9400001));
        assert(result.fault.address == 0);
        assert(result.fault.access == GUEST_MEMORY_READ);
        assert(result.fault.kind == GUEST_MEMORY_FAULT_UNMAPPED);
        assert(result.signal == 0 && result.exit_status == 0);
    }
    aarch64_linux_process_destroy(process);

    process = create_fault_process(file, UINT32_C(0xf9000001));
    assert(process != NULL);
    struct aarch64_linux_process_result result =
            aarch64_linux_process_run_one(process);
    assert(result.status == AARCH64_LINUX_PROCESS_DATA_FAULT);
    assert(result.instruction == UINT32_C(0xf9000001));
    assert(result.fault.address == 0);
    assert(result.fault.access == GUEST_MEMORY_WRITE);
    assert(result.fault.kind == GUEST_MEMORY_FAULT_UNMAPPED);
    assert(result.signal == 0 && result.exit_status == 0);
    aarch64_linux_process_destroy(process);

    process = create_fault_process(file, UINT32_C(0xffffffff));
    assert(process != NULL);
    for (unsigned i = 0; i < 2; i++) {
        result = aarch64_linux_process_run_one(process);
        assert(result.status == AARCH64_LINUX_PROCESS_UNDEFINED);
        assert(result.instruction == UINT32_C(0xffffffff));
        assert(result.fault.address == TEXT_BASE + 0x100);
        assert(result.fault.access == 0 && result.fault.kind == 0);
        assert(result.signal == 0 && result.exit_status == 0);
    }
    aarch64_linux_process_destroy(process);

    process = create_fault_process(file, UINT32_C(0x14000400));
    assert(process != NULL);
    result = aarch64_linux_process_run_one(process);
    assert(result.status == AARCH64_LINUX_PROCESS_RUNNABLE);
    assert_empty_result_fields(&result);
    result = aarch64_linux_process_run_one(process);
    assert(result.status == AARCH64_LINUX_PROCESS_FETCH_FAULT);
    assert(result.instruction == 0);
    assert(result.fault.address == TEXT_BASE + 0x1100);
    assert(result.fault.access == GUEST_MEMORY_EXECUTE);
    assert(result.fault.kind == GUEST_MEMORY_FAULT_UNMAPPED);
    assert(result.signal == 0 && result.exit_status == 0);
    aarch64_linux_process_destroy(process);
}

static void test_exit_group_event(void) {
    byte_t file[TEST_FILE_SIZE];
    make_test_elf(file);
    put_u32(file + 0x100, UINT32_C(0xd2803fe0));
    put_u32(file + 0x104, UINT32_C(0xd2800bc8));
    put_u32(file + 0x108, UINT32_C(0xd4000001));
    const char *arguments[2];
    const char *environment[1];
    byte_t random[AARCH64_LINUX_PROCESS_RANDOM_SIZE];
    make_default_inputs(arguments, environment, random);
    struct aarch64_linux_process_config config = make_config(
            file, sizeof(file), "/bin/process-test", arguments,
            array_size(arguments), environment,
            array_size(environment), random);
    struct aarch64_linux_process *process =
            aarch64_linux_process_create(&config, NULL);
    assert(process != NULL);
    assert(aarch64_linux_process_run_one(process).status ==
            AARCH64_LINUX_PROCESS_RUNNABLE);
    assert(aarch64_linux_process_run_one(process).status ==
            AARCH64_LINUX_PROCESS_RUNNABLE);
    struct aarch64_linux_process_result result =
            aarch64_linux_process_run_one(process);
    assert(result.status == AARCH64_LINUX_PROCESS_EXIT_GROUP);
    assert(result.exit_status == UINT32_C(0xff));
    assert(result.instruction == UINT32_C(0xd4000001));
    assert(result.signal == 0 && result.fault.address == 0 &&
            result.fault.access == 0 && result.fault.kind == 0);
    aarch64_linux_process_destroy(process);
}

static qword_t closure_dispatch(
        const struct guest_linux_syscall_context *context,
        const struct guest_linux_syscall *syscall,
        struct guest_linux_user_fault *fault) {
    use(fault);
    struct signal_closure *closure = context->runtime_opaque;
    assert(context->task_opaque == closure->expected_task_opaque);
    assert(syscall->number == 64);
    assert(closure->dispatches == 0 && !closure->pending);
    closure->interrupted_sp = context->stack_pointer;
    closure->pending = true;
    closure->dispatches++;
    return UINT64_C(0x123);
}

static struct guest_linux_signal_poll_result closure_poll(
        const struct guest_linux_signal_context *context,
        guest_linux_signal_installer installer,
        void *installer_opaque) {
    struct signal_closure *closure = context->runtime_opaque;
    assert(context->task_opaque == closure->expected_task_opaque);
    closure->polls++;
    if (!closure->pending) {
        return (struct guest_linux_signal_poll_result) {
            .status = GUEST_LINUX_SIGNAL_POLL_IDLE,
        };
    }
    assert(!closure->delivered);
    const struct guest_linux_signal_delivery delivery = {
        .info = {
            .signal = 10,
            .payload_kind = GUEST_LINUX_SIGNAL_PAYLOAD_NONE,
        },
        .action = {
            .handler = SIGNAL_HANDLER,
            .flags = AARCH64_LINUX_SA_SIGINFO,
        },
        .blocked_mask = UINT64_C(0x40),
    };
    assert(installer(installer_opaque, &delivery) ==
            GUEST_LINUX_SIGNAL_INSTALL_COMPLETE);
    closure->pending = false;
    closure->delivered = true;
    return (struct guest_linux_signal_poll_result) {
        .status = GUEST_LINUX_SIGNAL_POLL_HANDLER,
        .signal = 10,
    };
}

static void closure_restore(
        const struct guest_linux_signal_context *context,
        const struct guest_linux_signal_restore_request *request) {
    struct signal_closure *closure = context->runtime_opaque;
    assert(context->task_opaque == closure->expected_task_opaque);
    if (closure->interrupted_sp != 0)
        assert(request->stack_pointer == closure->interrupted_sp);
    else
        closure->interrupted_sp = request->stack_pointer;
    assert(request->blocked_mask == UINT64_C(0x40));
    assert(request->altstack.base == 0 && request->altstack.size == 0);
    closure->restores++;
}

static void closure_bad_frame(
        const struct guest_linux_signal_context *context,
        qword_t frame_address) {
    use(frame_address);
    struct signal_closure *closure = context->runtime_opaque;
    closure->bad_frames++;
    assert(false);
}

static void test_independent_handler_poll(void) {
    byte_t file[TEST_FILE_SIZE];
    make_test_elf(file);
    const dword_t program[] = {
        UINT32_C(0xd2800540), UINT32_C(0xd2800ba8),
        UINT32_C(0xd4000001),
    };
    for (size_t i = 0; i < array_size(program); i++)
        put_u32(file + 0x100 + i * 4, program[i]);

    const char *arguments[2];
    const char *environment[1];
    byte_t random[AARCH64_LINUX_PROCESS_RANDOM_SIZE];
    make_default_inputs(arguments, environment, random);
    int task_opaque;
    struct signal_closure closure = {
        .expected_task_opaque = &task_opaque,
        .pending = true,
    };
    const struct guest_linux_signal_service signals = {
        .runtime_opaque = &closure,
        .poll = closure_poll,
        .restore = closure_restore,
        .bad_frame = closure_bad_frame,
    };
    struct aarch64_linux_process_config config = make_config(
            file, sizeof(file), "/bin/process-test", arguments,
            array_size(arguments), environment,
            array_size(environment), random);
    config.task_opaque = &task_opaque;
    config.signals = &signals;
    struct aarch64_linux_process *process =
            aarch64_linux_process_create(&config, NULL);
    assert(process != NULL);

    struct aarch64_linux_process_result result =
            aarch64_linux_process_poll_signals(process);
    assert(result.status == AARCH64_LINUX_PROCESS_RUNNABLE);
    assert(result.signal == 10 && closure.delivered);
    for (unsigned i = 0; i < 3; i++) {
        result = aarch64_linux_process_run_one(process);
        assert(result.status == AARCH64_LINUX_PROCESS_RUNNABLE);
    }
    assert(closure.restores == 1 && closure.bad_frames == 0);
    assert(closure.interrupted_sp != 0);

    assert(aarch64_linux_process_run_one(process).status ==
            AARCH64_LINUX_PROCESS_RUNNABLE);
    assert(aarch64_linux_process_run_one(process).status ==
            AARCH64_LINUX_PROCESS_RUNNABLE);
    result = aarch64_linux_process_run_one(process);
    assert(result.status == AARCH64_LINUX_PROCESS_EXIT);
    assert(result.exit_status == 42);
    assert(closure.dispatches == 0 && closure.polls == 6);
    aarch64_linux_process_destroy(process);
}

static void test_signal_trampoline_closure(void) {
    byte_t file[TEST_FILE_SIZE];
    make_test_elf(file);
    const dword_t program[] = {
        UINT32_C(0xd2800808), UINT32_C(0xd4000001),
        UINT32_C(0xd2800ba8), UINT32_C(0xd4000001),
    };
    for (size_t i = 0; i < array_size(program); i++)
        put_u32(file + 0x100 + i * 4, program[i]);

    const char *arguments[2];
    const char *environment[1];
    byte_t random[AARCH64_LINUX_PROCESS_RANDOM_SIZE];
    make_default_inputs(arguments, environment, random);
    int task_opaque;
    struct signal_closure closure = {
        .expected_task_opaque = &task_opaque,
    };
    const struct guest_linux_syscall_service syscalls = {
        .runtime_opaque = &closure,
        .dispatch = closure_dispatch,
    };
    const struct guest_linux_signal_service signals = {
        .runtime_opaque = &closure,
        .poll = closure_poll,
        .restore = closure_restore,
        .bad_frame = closure_bad_frame,
    };
    struct aarch64_linux_process_config config = make_config(
            file, sizeof(file), "/bin/process-test", arguments,
            array_size(arguments), environment,
            array_size(environment), random);
    config.task_opaque = &task_opaque;
    config.syscalls = &syscalls;
    config.signals = &signals;
    struct aarch64_linux_process *process =
            aarch64_linux_process_create(&config, NULL);
    assert(process != NULL);
    assert(aarch64_linux_process_uses_services(process,
            config.tid, &task_opaque, &syscalls, &signals));
    assert(!aarch64_linux_process_uses_services(process,
            config.tid + 1, &task_opaque, &syscalls, &signals));
    int wrong_task_opaque = 0;
    assert(!aarch64_linux_process_uses_services(process,
            config.tid, &wrong_task_opaque, &syscalls, &signals));
    struct guest_linux_signal_service wrong_signals = signals;
    wrong_signals.bad_frame = NULL;
    assert(!aarch64_linux_process_uses_services(process,
            config.tid, &task_opaque, &syscalls, &wrong_signals));

    struct aarch64_linux_process_result result =
            aarch64_linux_process_poll_signals(process);
    assert(result.status == AARCH64_LINUX_PROCESS_RUNNABLE);
    assert_empty_result_fields(&result);
    result = aarch64_linux_process_run_one(process);
    assert(result.status == AARCH64_LINUX_PROCESS_RUNNABLE &&
            result.signal == 0);
    result = aarch64_linux_process_run_one(process);
    assert(result.status == AARCH64_LINUX_PROCESS_RUNNABLE &&
            result.signal == 10 && closure.delivered);

    // handler RET 后执行内部 RX trampoline，再由 syscall 139 恢复原状态。
    for (unsigned i = 0; i < 3; i++) {
        result = aarch64_linux_process_run_one(process);
        assert(result.status == AARCH64_LINUX_PROCESS_RUNNABLE);
    }
    assert(closure.restores == 1 && closure.bad_frames == 0);
    assert(closure.dispatches == 1);

    result = aarch64_linux_process_run_one(process);
    assert(result.status == AARCH64_LINUX_PROCESS_RUNNABLE);
    result = aarch64_linux_process_run_one(process);
    assert(result.status == AARCH64_LINUX_PROCESS_EXIT);
    assert(result.exit_status == UINT32_C(0x23));
    assert(closure.dispatches == 1 && closure.restores == 1);
    assert(closure.polls == 7);
    aarch64_linux_process_destroy(process);
}

struct exec_completion_probe {
    unsigned dispatches;
    unsigned polls;
};

static qword_t dispatch_exec_completion(
        const struct guest_linux_syscall_context *context,
        const struct guest_linux_syscall *syscall,
        struct guest_linux_user_fault *fault) {
    use(fault);
    struct exec_completion_probe *probe = context->runtime_opaque;
    assert(syscall->number == 64 && context->completion != NULL);
    context->completion->disposition =
            GUEST_LINUX_SYSCALL_REPLACED_IMAGE;
    probe->dispatches++;
    return UINT64_C(0xfeedfacecafebeef);
}

static struct guest_linux_signal_poll_result poll_exec_completion(
        const struct guest_linux_signal_context *context,
        guest_linux_signal_installer installer,
        void *installer_opaque) {
    use(installer, installer_opaque);
    struct exec_completion_probe *probe = context->runtime_opaque;
    probe->polls++;
    return (struct guest_linux_signal_poll_result) {
        .status = GUEST_LINUX_SIGNAL_POLL_IDLE,
    };
}

static void test_exec_completion_event(void) {
    byte_t file[TEST_FILE_SIZE];
    make_test_elf(file);
    put_u32(file + 0x100, UINT32_C(0xd2800808));
    put_u32(file + 0x104, UINT32_C(0xd4000001));

    const char *arguments[2];
    const char *environment[1];
    byte_t random[AARCH64_LINUX_PROCESS_RANDOM_SIZE];
    make_default_inputs(arguments, environment, random);
    struct exec_completion_probe probe = {0};
    const struct guest_linux_syscall_service syscalls = {
        .runtime_opaque = &probe,
        .dispatch = dispatch_exec_completion,
    };
    const struct guest_linux_signal_service signals = {
        .runtime_opaque = &probe,
        .poll = poll_exec_completion,
    };
    struct aarch64_linux_process_config config = make_config(
            file, sizeof(file), "/bin/process-test", arguments,
            array_size(arguments), environment,
            array_size(environment), random);
    config.syscalls = &syscalls;
    config.signals = &signals;
    struct aarch64_linux_process *process =
            aarch64_linux_process_create(&config, NULL);
    assert(process != NULL);
    assert(aarch64_linux_process_run_one(process).status ==
            AARCH64_LINUX_PROCESS_RUNNABLE);
    struct aarch64_linux_process_result result =
            aarch64_linux_process_run_one(process);
    assert(result.status == AARCH64_LINUX_PROCESS_EXEC &&
            result.instruction == UINT32_C(0xd4000001) &&
            probe.dispatches == 1 && probe.polls == 1);
    aarch64_linux_process_destroy(process);
}

int main(void) {
    test_load_run_and_ownership();
    test_static_pie();
    test_interpreter_path_query();
    test_failure_transactions();
    test_poll_events();
    test_fault_events();
    test_exit_group_event();
    test_independent_handler_poll();
    test_signal_trampoline_closure();
    test_exec_completion_event();
    return 0;
}
