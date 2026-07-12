#include <assert.h>
#include <limits.h>
#include <string.h>

#include "guest/aarch64/elf64.h"
#include "guest/aarch64/linux-process.h"
#include "guest/aarch64/linux-runtime.h"
#include "guest/memory/address-space.h"
#include "guest/memory/page-table.h"

#define TEST_FILE_SIZE 0x500
#define PROGRAM_OFFSET 0x300
#define DATA_FILE_OFFSET 0x200
#define TEXT_BASE UINT64_C(0x400000)
#define DATA_ADDRESS UINT64_C(0x500200)
#define START_BRK UINT64_C(0x501000)
#define GROWN_BRK UINT64_C(0x502000)
#define BRK_LIMIT UINT64_C(0x600000)
#define FIRST_MMAP BRK_LIMIT
#define SECOND_MMAP (FIRST_MMAP + GUEST_MEMORY_PAGE_SIZE)
#define STACK_TOP UINT64_C(0x00007fff00000000)
#define SIGNAL_TRAMPOLINE UINT64_C(0x00007ffe00000000)
#define PARENT_TID 101
#define CHILD_TID 202

static const char *const process_arguments[] = {"fork-test"};
static const byte_t process_random[AARCH64_LINUX_PROCESS_RANDOM_SIZE] = {
    0, 1, 2, 3, 4, 5, 6, 7,
    8, 9, 10, 11, 12, 13, 14, 15,
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

static void make_test_elf(byte_t file[TEST_FILE_SIZE],
        const dword_t *program, size_t instruction_count) {
    assert(PROGRAM_OFFSET + instruction_count * sizeof(*program) <=
            TEST_FILE_SIZE);
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
    put_u64(file + 24, TEXT_BASE + PROGRAM_OFFSET);
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
            1, 5, 0, TEXT_BASE, TEST_FILE_SIZE,
            TEST_FILE_SIZE, GUEST_MEMORY_PAGE_SIZE);
    put_program_header(headers + 2 * AARCH64_ELF64_PROGRAM_HEADER_SIZE,
            1, 6, DATA_FILE_OFFSET, DATA_ADDRESS,
            sizeof(qword_t), sizeof(qword_t), GUEST_MEMORY_PAGE_SIZE);

    for (size_t i = 0; i < instruction_count; i++)
        put_u32(file + PROGRAM_OFFSET + i * sizeof(*program), program[i]);
    put_u64(file + DATA_FILE_OFFSET, UINT64_C(0x11));
}

static struct aarch64_linux_process_config make_config(
        const byte_t file[TEST_FILE_SIZE], pid_t_ tid, void *task_opaque,
        const struct guest_linux_syscall_service *syscalls,
        const struct guest_linux_signal_service *signals) {
    return (struct aarch64_linux_process_config) {
        .elf_data = file,
        .elf_size = TEST_FILE_SIZE,
        .stack_top = STACK_TOP,
        .stack_size = UINT64_C(0x10000),
        .signal_trampoline_page = SIGNAL_TRAMPOLINE,
        .brk_limit = BRK_LIMIT,
        .executable = "/bin/fork-test",
        .arguments = process_arguments,
        .argument_count = 1,
        .random = process_random,
        .tid = tid,
        .task_opaque = task_opaque,
        .syscalls = syscalls,
        .signals = signals,
    };
}

static qword_t read_user_qword(
        const struct guest_linux_syscall_context *context,
        guest_addr_t address, struct guest_linux_user_fault *fault) {
    byte_t bytes[8];
    assert(context->user.read(context->user.opaque,
            address, bytes, sizeof(bytes), fault));
    qword_t value = 0;
    for (byte_t i = 0; i < sizeof(bytes); i++)
        value |= (qword_t) bytes[i] << (i * 8);
    return value;
}

static void run_to_exit(struct aarch64_linux_process *process,
        dword_t expected_status) {
    for (unsigned i = 0; i < 256; i++) {
        struct aarch64_linux_process_result result =
                aarch64_linux_process_run_one(process);
        if (result.status == AARCH64_LINUX_PROCESS_EXIT) {
            assert(result.exit_status == expected_status);
            return;
        }
        assert(result.status == AARCH64_LINUX_PROCESS_RUNNABLE);
    }
    assert(false);
}

struct fork_fixture {
    struct aarch64_linux_process *parent;
    struct aarch64_linux_process *child;
    const struct guest_linux_syscall_service *expected_syscalls;
    const struct guest_linux_signal_service *expected_signals;
    int parent_task;
    int child_task;
    guest_addr_t fork_stack_pointer;
    unsigned fork_calls;
    unsigned memory_reports[2];
    unsigned state_reports[2];
    unsigned vector_reports[2];
    unsigned signal_polls[2];
};

static unsigned fixture_task_index(const struct fork_fixture *fixture,
        const void *task_opaque) {
    if (task_opaque == &fixture->parent_task)
        return 0;
    assert(task_opaque == &fixture->child_task);
    return 1;
}

static void exercise_late_clone_failure(struct fork_fixture *fixture,
        const struct aarch64_linux_process_fork_config *config) {
    guest_page_table_test_fail_clone_allocation_at(SIZE_MAX);
    struct aarch64_linux_process *probe =
            aarch64_linux_process_fork(fixture->parent, config, NULL);
    assert(probe != NULL);
    size_t allocation_count =
            guest_page_table_test_clone_allocation_count();
    assert(allocation_count > 1);
    aarch64_linux_process_destroy(probe);

    guest_page_table_test_fail_clone_allocation_at(allocation_count - 1);
    struct aarch64_linux_process_error error = {
        .stage = UINT32_MAX,
        .detail = UINT32_MAX,
    };
    assert(aarch64_linux_process_fork(
            fixture->parent, config, &error) == NULL);
    assert(error.stage == AARCH64_LINUX_PROCESS_ERROR_ALLOCATION &&
            error.detail == 0);
    assert(aarch64_linux_process_uses_services(fixture->parent,
            PARENT_TID, &fixture->parent_task,
            fixture->expected_syscalls, fixture->expected_signals));
    assert(aarch64_linux_process_test_has_owned_state(
            fixture->parent, PARENT_TID, &fixture->parent_task,
            FIRST_MMAP, SIGNAL_TRAMPOLINE, false));
    guest_page_table_test_fail_clone_allocation_at(SIZE_MAX);
}

static qword_t dispatch_fork_fixture(
        const struct guest_linux_syscall_context *context,
        const struct guest_linux_syscall *syscall,
        struct guest_linux_user_fault *fault) {
    struct fork_fixture *fixture = context->runtime_opaque;
    unsigned task_index = fixture_task_index(
            fixture, context->task_opaque);
    assert(context->completion != NULL);

    if (syscall->number == 500) {
        assert(task_index == 0 && fixture->fork_calls == 0 &&
                fixture->parent != NULL);
        fixture->fork_stack_pointer = context->stack_pointer;
        const struct aarch64_linux_process_fork_config config = {
            .tid = CHILD_TID,
            .task_opaque = &fixture->child_task,
        };
        exercise_late_clone_failure(fixture, &config);
        struct aarch64_linux_process_error error = {
            .stage = UINT32_MAX,
            .detail = UINT32_MAX,
        };
        fixture->child = aarch64_linux_process_fork(
                fixture->parent, &config, &error);
        assert(fixture->child != NULL &&
                error.stage == AARCH64_LINUX_PROCESS_ERROR_NONE &&
                error.detail == 0);
        assert(aarch64_linux_process_uses_services(fixture->child,
                CHILD_TID, &fixture->child_task,
                fixture->expected_syscalls, fixture->expected_signals));
        assert(aarch64_linux_process_test_has_owned_state(
                fixture->child, CHILD_TID, &fixture->child_task,
                0, SIGNAL_TRAMPOLINE, true));
        fixture->fork_calls++;
        return CHILD_TID;
    }

    assert(context->stack_pointer == fixture->fork_stack_pointer);
    qword_t path = task_index + 1;
    if (syscall->number == 501) {
        assert(syscall->arguments[0] == path);
        assert(syscall->arguments[1] == GROWN_BRK &&
                syscall->arguments[2] == GROWN_BRK);
        assert(syscall->arguments[3] == FIRST_MMAP &&
                syscall->arguments[4] == SECOND_MMAP);
        assert(syscall->arguments[5] ==
                (task_index == 0 ? PARENT_TID : CHILD_TID));
        byte_t marker;
        assert(context->user.read(context->user.opaque,
                FIRST_MMAP, &marker, sizeof(marker), fault));
        assert(marker == (task_index == 0 ? 'P' : 'C'));
        fixture->memory_reports[task_index]++;
        return 0;
    }

    if (syscall->number == 502) {
        assert(syscall->arguments[0] == UINT64_C(0x2345));
        assert(syscall->arguments[1] == UINT64_C(0x5678));
        assert(syscall->arguments[2] == 1 &&
                syscall->arguments[3] == 7);
        assert(syscall->arguments[4] ==
                (task_index == 0 ? CHILD_TID : 0));
        assert(syscall->arguments[5] == path);
        fixture->state_reports[task_index]++;
        return 0;
    }

    assert(syscall->number == 503);
    assert(syscall->arguments[0] == UINT64_C(0x6789));
    assert(syscall->arguments[1] == path);
    fixture->vector_reports[task_index]++;
    return 0;
}

static struct guest_linux_signal_poll_result poll_fork_fixture(
        const struct guest_linux_signal_context *context,
        guest_linux_signal_installer installer,
        void *installer_opaque) {
    (void) installer;
    (void) installer_opaque;
    struct fork_fixture *fixture = context->runtime_opaque;
    unsigned task_index = fixture_task_index(
            fixture, context->task_opaque);
    fixture->signal_polls[task_index]++;
    return (struct guest_linux_signal_poll_result) {
        .status = GUEST_LINUX_SIGNAL_POLL_IDLE,
    };
}

static void test_invalid_fork_arguments(struct fork_fixture *fixture) {
    const struct aarch64_linux_process_fork_config valid = {
        .tid = CHILD_TID,
        .task_opaque = &fixture->child_task,
    };
    struct aarch64_linux_process_error error = {
        .stage = UINT32_MAX,
        .detail = UINT32_MAX,
    };
    assert(aarch64_linux_process_fork(NULL, &valid, &error) == NULL);
    assert(error.stage == AARCH64_LINUX_PROCESS_ERROR_ARGUMENT &&
            error.detail == 0);
    assert(aarch64_linux_process_fork(
            fixture->parent, NULL, &error) == NULL);
    assert(error.stage == AARCH64_LINUX_PROCESS_ERROR_ARGUMENT);

    struct aarch64_linux_process_fork_config bad = valid;
    bad.tid = 0;
    assert(aarch64_linux_process_fork(
            fixture->parent, &bad, &error) == NULL);
    bad.tid = -1;
    assert(aarch64_linux_process_fork(
            fixture->parent, &bad, &error) == NULL);
    bad.tid = AARCH64_LINUX_MAX_TID + 1;
    assert(aarch64_linux_process_fork(
            fixture->parent, &bad, &error) == NULL);
    assert(error.stage == AARCH64_LINUX_PROCESS_ERROR_ARGUMENT);
    assert(aarch64_linux_process_fork(NULL, &valid, NULL) == NULL);

    const struct aarch64_linux_process_fork_config nullable = {
        .tid = CHILD_TID + 1,
        .task_opaque = NULL,
    };
    struct aarch64_linux_process *temporary =
            aarch64_linux_process_fork(
                    fixture->parent, &nullable, NULL);
    assert(temporary != NULL);
    assert(aarch64_linux_process_uses_services(temporary,
            CHILD_TID + 1, NULL, fixture->expected_syscalls,
            fixture->expected_signals));
    assert(aarch64_linux_process_test_has_owned_state(
            temporary, CHILD_TID + 1, NULL, 0,
            SIGNAL_TRAMPOLINE, true));
    aarch64_linux_process_destroy(temporary);
}

static void test_post_svc_fork(void) {
    // guest 先建立内存与架构状态，再由 syscall 500 的真实 SVC
    // 安全点分叉。
    static const dword_t program[] = {
        UINT32_C(0xd2800000), UINT32_C(0xd2801ac8),
        UINT32_C(0xd4000001), UINT32_C(0xaa0003f3),
        UINT32_C(0x91400660), UINT32_C(0xd2801ac8),
        UINT32_C(0xd4000001), UINT32_C(0xaa0003f3),
        UINT32_C(0xd2800000), UINT32_C(0xd2820001),
        UINT32_C(0xd2800062), UINT32_C(0xd2800443),
        UINT32_C(0xd2800004), UINT32_C(0xd2800005),
        UINT32_C(0xd2801bc8), UINT32_C(0xd4000001),
        UINT32_C(0xaa0003f4), UINT32_C(0xd2800838),
        UINT32_C(0x39000298), UINT32_C(0xaa1403e0),
        UINT32_C(0xd2800c08), UINT32_C(0xd4000001),
        UINT32_C(0xd28468b5), UINT32_C(0xd51bd055),
        UINT32_C(0xd28acf16), UINT32_C(0x9e6702c0),
        UINT32_C(0xd28cf136), UINT32_C(0x9eaf02c0),
        UINT32_C(0xd28000f7), UINT32_C(0xeb1702ff),
        UINT32_C(0xd2800c7d), UINT32_C(0xd2803e88),
        UINT32_C(0xd4000001),
        UINT32_C(0xaa0003fd), UINT32_C(0xb4000080),
        UINT32_C(0xd2800a18), UINT32_C(0xd2800039),
        UINT32_C(0x14000003), UINT32_C(0xd2800878),
        UINT32_C(0xd2800059), UINT32_C(0x39000298),
        UINT32_C(0xd2801648), UINT32_C(0xd4000001),
        UINT32_C(0xaa0003fc), UINT32_C(0xd2800000),
        UINT32_C(0xd2801ac8), UINT32_C(0xd4000001),
        UINT32_C(0xaa0003fa), UINT32_C(0xd2800000),
        UINT32_C(0xd2820001), UINT32_C(0xd2800062),
        UINT32_C(0xd2800443), UINT32_C(0xd2800004),
        UINT32_C(0xd2800005), UINT32_C(0xd2801bc8),
        UINT32_C(0xd4000001), UINT32_C(0xaa0003fb),
        UINT32_C(0xaa1903e0), UINT32_C(0xaa1303e1),
        UINT32_C(0xaa1a03e2), UINT32_C(0xaa1403e3),
        UINT32_C(0xaa1b03e4), UINT32_C(0xaa1c03e5),
        UINT32_C(0xd2803ea8), UINT32_C(0xd4000001),
        UINT32_C(0xd53bd040), UINT32_C(0x9e660001),
        UINT32_C(0xd2800002), UINT32_C(0x54000041),
        UINT32_C(0xd2800022), UINT32_C(0xaa1703e3),
        UINT32_C(0xaa1d03e4), UINT32_C(0xaa1903e5),
        UINT32_C(0xd2803ec8), UINT32_C(0xd4000001),
        UINT32_C(0x9eae0000), UINT32_C(0xaa1903e1),
        UINT32_C(0xd2803ee8), UINT32_C(0xd4000001),
        UINT32_C(0xf100073f), UINT32_C(0x54000081),
        UINT32_C(0xd1400660), UINT32_C(0xd2801ac8),
        UINT32_C(0xd4000001), UINT32_C(0xaa1403e0),
        UINT32_C(0xd2820001), UINT32_C(0xd2801ae8),
        UINT32_C(0xd4000001), UINT32_C(0xaa1903e0),
        UINT32_C(0xd2800ba8), UINT32_C(0xd4000001),
    };
    byte_t file[TEST_FILE_SIZE];
    make_test_elf(file, program, sizeof(program) / sizeof(program[0]));

    struct fork_fixture fixture = {0};
    const struct guest_linux_syscall_service expected_syscalls = {
        .runtime_opaque = &fixture,
        .dispatch = dispatch_fork_fixture,
    };
    const struct guest_linux_signal_service expected_signals = {
        .runtime_opaque = &fixture,
        .poll = poll_fork_fixture,
    };
    struct guest_linux_syscall_service configured_syscalls =
            expected_syscalls;
    struct guest_linux_signal_service configured_signals =
            expected_signals;
    fixture.expected_syscalls = &expected_syscalls;
    fixture.expected_signals = &expected_signals;
    struct aarch64_linux_process_config config = make_config(
            file, PARENT_TID, &fixture.parent_task,
            &configured_syscalls, &configured_signals);
    fixture.parent = aarch64_linux_process_create(&config, NULL);
    assert(fixture.parent != NULL);
    configured_syscalls = (struct guest_linux_syscall_service) {0};
    configured_signals = (struct guest_linux_signal_service) {0};
    assert(aarch64_linux_process_uses_services(fixture.parent,
            PARENT_TID, &fixture.parent_task,
            &expected_syscalls, &expected_signals));
    assert(aarch64_linux_process_test_has_owned_state(
            fixture.parent, PARENT_TID, &fixture.parent_task,
            0, SIGNAL_TRAMPOLINE, true));
    test_invalid_fork_arguments(&fixture);

    run_to_exit(fixture.parent, 1);
    assert(fixture.fork_calls == 1 && fixture.child != NULL);
    assert(fixture.memory_reports[0] == 1 &&
            fixture.state_reports[0] == 1 &&
            fixture.vector_reports[0] == 1);
    aarch64_linux_process_destroy(fixture.parent);
    fixture.parent = NULL;

    run_to_exit(fixture.child, 2);
    assert(fixture.memory_reports[1] == 1 &&
            fixture.state_reports[1] == 1 &&
            fixture.vector_reports[1] == 1);
    assert(fixture.signal_polls[0] > 0 &&
            fixture.signal_polls[1] > 0);
    aarch64_linux_process_destroy(fixture.child);
    guest_page_table_test_fail_clone_allocation_at(SIZE_MAX);
}

struct exclusive_fixture {
    int parent_task;
    int child_task;
    unsigned reports[2];
};

static qword_t dispatch_exclusive_fixture(
        const struct guest_linux_syscall_context *context,
        const struct guest_linux_syscall *syscall,
        struct guest_linux_user_fault *fault) {
    struct exclusive_fixture *fixture = context->runtime_opaque;
    unsigned task_index;
    if (context->task_opaque == &fixture->parent_task)
        task_index = 0;
    else {
        assert(context->task_opaque == &fixture->child_task);
        task_index = 1;
    }
    assert(syscall->number == 600);
    assert(syscall->arguments[0] == task_index);
    qword_t value = read_user_qword(context, DATA_ADDRESS, fault);
    assert(value == (task_index == 0 ? UINT64_C(0x55) :
            UINT64_C(0x11)));
    fixture->reports[task_index]++;
    return 0;
}

static void test_exclusive_monitor_reset(void) {
    // fork 位于 LDXR 与 STXR 之间：父写入成功，子因监视器重置而失败。
    static const dword_t program[] = {
        UINT32_C(0xd2804004), UINT32_C(0xf2a00a04),
        UINT32_C(0xd2800aa3), UINT32_C(0xc85f7c81),
        UINT32_C(0xc8027c83), UINT32_C(0xaa0203e0),
        UINT32_C(0xd2804b08), UINT32_C(0xd4000001),
        UINT32_C(0xd2800000), UINT32_C(0xd2800ba8),
        UINT32_C(0xd4000001),
    };
    byte_t file[TEST_FILE_SIZE];
    make_test_elf(file, program, sizeof(program) / sizeof(program[0]));
    struct exclusive_fixture fixture = {0};
    const struct guest_linux_syscall_service expected_syscalls = {
        .runtime_opaque = &fixture,
        .dispatch = dispatch_exclusive_fixture,
    };
    struct guest_linux_syscall_service configured_syscalls =
            expected_syscalls;
    struct aarch64_linux_process_config config = make_config(
            file, PARENT_TID, &fixture.parent_task,
            &configured_syscalls, NULL);
    struct aarch64_linux_process *parent =
            aarch64_linux_process_create(&config, NULL);
    assert(parent != NULL);
    configured_syscalls = (struct guest_linux_syscall_service) {0};

    // LDXR 已完成但 STXR 尚未执行，父监视器保留，子监视器失效。
    for (unsigned i = 0; i < 4; i++) {
        assert(aarch64_linux_process_run_one(parent).status ==
                AARCH64_LINUX_PROCESS_RUNNABLE);
    }
    const struct aarch64_linux_process_fork_config fork_config = {
        .tid = CHILD_TID,
        .task_opaque = &fixture.child_task,
    };
    struct aarch64_linux_process_error error = {
        .stage = UINT32_MAX,
        .detail = UINT32_MAX,
    };
    struct aarch64_linux_process *child =
            aarch64_linux_process_fork(parent, &fork_config, &error);
    assert(child != NULL &&
            error.stage == AARCH64_LINUX_PROCESS_ERROR_NONE);
    assert(aarch64_linux_process_test_has_owned_state(
            parent, PARENT_TID, &fixture.parent_task,
            0, SIGNAL_TRAMPOLINE, false));
    assert(aarch64_linux_process_test_has_owned_state(
            child, CHILD_TID, &fixture.child_task,
            0, SIGNAL_TRAMPOLINE, true));

    run_to_exit(parent, 0);
    assert(fixture.reports[0] == 1);
    aarch64_linux_process_destroy(parent);
    run_to_exit(child, 0);
    assert(fixture.reports[1] == 1);
    aarch64_linux_process_destroy(child);
}

struct thread_fixture {
    int parent_task;
    int child_task;
    unsigned parent_calls;
    unsigned child_calls;
};

static qword_t dispatch_thread_fixture(
        const struct guest_linux_syscall_context *context,
        const struct guest_linux_syscall *syscall,
        struct guest_linux_user_fault *fault) {
    struct thread_fixture *fixture = context->runtime_opaque;
    assert(syscall->number == 600);
    if (context->task_opaque == &fixture->parent_task) {
        const byte_t marker = 0x7b;
        assert(context->user.write(context->user.opaque,
                DATA_ADDRESS, &marker, sizeof(marker), fault));
        fixture->parent_calls++;
    } else {
        assert(context->task_opaque == &fixture->child_task);
        byte_t marker;
        assert(context->user.read(context->user.opaque,
                DATA_ADDRESS, &marker, sizeof(marker), fault));
        assert(marker == 0x7b);
        fixture->child_calls++;
    }
    return 0;
}

static void test_thread_shared_memory(void) {
    static const dword_t program[] = {
        UINT32_C(0xd2804b08), UINT32_C(0xd4000001),
        UINT32_C(0xd2800000), UINT32_C(0xd2800ba8),
        UINT32_C(0xd4000001),
    };
    byte_t file[TEST_FILE_SIZE];
    make_test_elf(file, program, sizeof(program) / sizeof(program[0]));
    struct thread_fixture fixture = {0};
    const struct guest_linux_syscall_service syscall_service = {
        .runtime_opaque = &fixture,
        .dispatch = dispatch_thread_fixture,
    };
    struct aarch64_linux_process_config config = make_config(
            file, PARENT_TID, &fixture.parent_task,
            &syscall_service, NULL);
    struct aarch64_linux_process *parent =
            aarch64_linux_process_create(&config, NULL);
    assert(parent != NULL);

    const guest_addr_t child_stack = STACK_TOP - UINT64_C(0x8000);
    const qword_t child_tls = UINT64_C(0x123456789abcdef0);
    const guest_addr_t clear_child_tid = DATA_ADDRESS + 4;
    const struct aarch64_linux_process_thread_config thread_config = {
        .tid = CHILD_TID,
        .set_tls = 1,
        .task_opaque = &fixture.child_task,
        .stack_pointer = child_stack,
        .tls = child_tls,
        .clear_child_tid = clear_child_tid,
    };
    struct aarch64_linux_process_error error = {
        .stage = UINT32_MAX,
        .detail = UINT32_MAX,
    };
    struct aarch64_linux_process_thread_config invalid = thread_config;
    invalid.tid = 0;
    assert(aarch64_linux_process_clone_thread(
            parent, &invalid, &error) == NULL);
    assert(error.stage == AARCH64_LINUX_PROCESS_ERROR_ARGUMENT);
    invalid = thread_config;
    invalid.set_tls = 2;
    assert(aarch64_linux_process_clone_thread(
            parent, &invalid, &error) == NULL);
    assert(error.stage == AARCH64_LINUX_PROCESS_ERROR_ARGUMENT);

    struct aarch64_linux_process *child =
            aarch64_linux_process_clone_thread(
                    parent, &thread_config, &error);
    assert(child != NULL &&
            error.stage == AARCH64_LINUX_PROCESS_ERROR_NONE &&
            error.detail == 0);
    assert(aarch64_linux_process_test_has_owned_state(
            child, CHILD_TID, &fixture.child_task,
            clear_child_tid, SIGNAL_TRAMPOLINE, true));
    assert(aarch64_linux_process_test_has_thread_state(
            child, child_stack, child_tls, clear_child_tid));

    run_to_exit(parent, 0);
    assert(fixture.parent_calls == 1);
    aarch64_linux_process_destroy(parent);
    run_to_exit(child, 0);
    assert(fixture.child_calls == 1);
    aarch64_linux_process_destroy(child);

    assert(aarch64_linux_process_clone_thread(
            NULL, &thread_config, &error) == NULL);
    assert(error.stage == AARCH64_LINUX_PROCESS_ERROR_ARGUMENT);
    assert(aarch64_linux_process_clone_thread(NULL, NULL, NULL) == NULL);
}

int main(void) {
    test_post_svc_fork();
    test_exclusive_monitor_reset();
    test_thread_shared_memory();
    return 0;
}
