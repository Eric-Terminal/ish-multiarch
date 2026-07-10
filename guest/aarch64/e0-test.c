#include <assert.h>
#include <string.h>

#include "guest/aarch64/elf64-loader.h"
#include "guest/aarch64/linux-runtime.h"
#include "guest/aarch64/linux-stack.h"
#include "guest/aarch64/runner.h"

#define TEST_FILE_SIZE 768
#define TEXT_BASE UINT64_C(0x400000)
#define DATA_ADDRESS UINT64_C(0x500200)
#define STACK_TOP UINT64_C(0x00007fff00000000)

struct test_sink {
    byte_t data[16];
    size_t size;
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
    put_u64(file + 24, TEXT_BASE + 0x100);
    put_u64(file + 32, AARCH64_ELF64_HEADER_SIZE);
    put_u16(file + 52, AARCH64_ELF64_HEADER_SIZE);
    put_u16(file + 54, AARCH64_ELF64_PROGRAM_HEADER_SIZE);
    put_u16(file + 56, 3);

    byte_t *headers = file + AARCH64_ELF64_HEADER_SIZE;
    put_program_header(headers, 6, 4,
            AARCH64_ELF64_HEADER_SIZE, TEXT_BASE + AARCH64_ELF64_HEADER_SIZE,
            3 * AARCH64_ELF64_PROGRAM_HEADER_SIZE,
            3 * AARCH64_ELF64_PROGRAM_HEADER_SIZE, 8);
    put_program_header(headers + AARCH64_ELF64_PROGRAM_HEADER_SIZE,
            1, 5, 0, TEXT_BASE, 0x124, 0x124, 0x1000);
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
    memcpy(file + 0x200, "hi\n", 3);
}

static sqword_t write_sink(void *opaque, qword_t fd,
        const byte_t *data, size_t size) {
    struct test_sink *sink = opaque;
    assert(fd == 1);
    assert(sink->size + size <= sizeof(sink->data));
    memcpy(sink->data + sink->size, data, size);
    sink->size += size;
    return (sqword_t) size;
}

static struct aarch64_linux_syscall_result run_to_syscall(
        struct aarch64_runner *runner, struct cpu_state *cpu,
        struct guest_tlb *tlb, struct aarch64_linux_runtime *runtime,
        struct aarch64_linux_task *task) {
    while (true) {
        struct aarch64_step_result step = aarch64_run_one(runner, cpu);
        if (step.stop == AARCH64_STEP_RETIRED)
            continue;
        assert(step.stop == AARCH64_STEP_SYSCALL);
        return aarch64_linux_dispatch_syscall(cpu, tlb, runtime, task);
    }
}

int main(void) {
    byte_t file[TEST_FILE_SIZE];
    make_test_elf(file);
    struct aarch64_elf64_image image;
    assert(aarch64_elf64_parse(file, sizeof(file), &image) ==
            AARCH64_ELF64_OK);
    struct guest_page_table table;
    assert(guest_page_table_init(&table, 48));
    struct aarch64_elf64_load_result loaded;
    assert(aarch64_elf64_load(&image, &table, &loaded) ==
            AARCH64_ELF64_LOAD_OK);

    const char *arguments[] = {"e0"};
    byte_t random[AARCH64_LINUX_RANDOM_SIZE] = {0};
    const struct aarch64_linux_stack_config stack_config = {
        .stack_top = STACK_TOP,
        .stack_size = GUEST_MEMORY_PAGE_SIZE,
        .executable = "/bin/e0",
        .arguments = arguments,
        .argument_count = array_size(arguments),
        .random = random,
    };
    struct aarch64_linux_stack_result stack;
    assert(aarch64_linux_build_initial_stack(&table, &loaded,
            &stack_config, &stack) == AARCH64_LINUX_STACK_OK);

    struct guest_tlb tlb;
    guest_tlb_init(&tlb, &table.address_space);
    struct aarch64_runner runner;
    aarch64_runner_init(&runner, &tlb);
    struct cpu_state cpu = {0};
    aarch64_linux_prepare_cpu(&cpu, &loaded, &stack);
    struct test_sink sink = {0};
    const struct aarch64_linux_services services = {
        .opaque = &sink,
        .write = write_sink,
    };
    struct aarch64_linux_runtime runtime;
    aarch64_linux_runtime_init(&runtime, &table, loaded.brk_end,
            loaded.brk_end + 16 * GUEST_MEMORY_PAGE_SIZE, &services);
    struct aarch64_linux_task task;
    aarch64_linux_task_init(&task, 1);

    struct aarch64_linux_syscall_result syscall =
            run_to_syscall(&runner, &cpu, &tlb, &runtime, &task);
    assert(syscall.action == AARCH64_LINUX_SYSCALL_RESUME);
    assert(cpu.x[0] == 3);
    assert(sink.size == 3);
    assert(memcmp(sink.data, "hi\n", 3) == 0);

    syscall = run_to_syscall(&runner, &cpu, &tlb, &runtime, &task);
    assert(syscall.action == AARCH64_LINUX_SYSCALL_EXIT);
    assert(syscall.exit_status == 42);
    assert(cpu.cycle == 9);
    assert(cpu.pc == loaded.entry + 9 * 4);
    guest_page_table_destroy(&table);
    return 0;
}
