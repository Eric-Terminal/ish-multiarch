#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "fs/fd.h"
#include "guest/aarch64/elf64.h"
#include "guest/aarch64/linux-process-abi.h"
#include "guest/aarch64/linux-process.h"
#include "guest/aarch64/linux-signal-abi.h"
#include "guest/memory/address-space.h"
#include "kernel/aarch64-signal-service.h"
#include "kernel/aarch64-syscall-service.h"
#include "kernel/calls.h"
#include "kernel/fs.h"
#include "kernel/mm.h"
#include "kernel/resource.h"
#include "kernel/signal.h"
#include "kernel/task.h"

#define IMAGE_SIZE 1024
#define TEXT_BASE UINT64_C(0x400000)
#define ENTRY_OFFSET UINT64_C(0x200)
#define STACK_TOP UINT64_C(0x00007fff00000000)
#define SIGNAL_TRAMPOLINE UINT64_C(0x00007ffe00000000)
#define TEST_FILE_IDENTITY UINT64_C(0x1000000000000009)
#define ARGUMENT_ADDRESS UINT64_C(0x0000400012340000)
#define ARGUMENT_MEMORY_SIZE 256
#define CLONE3_SYSCALL_NUMBER UINT64_C(435)

#define PROCESS_STACK_BASE (STACK_TOP - UINT64_C(0x1800))
#define PROCESS_STACK_SIZE UINT64_C(0x0800)
#define PROCESS_STACK_POINTER \
    (PROCESS_STACK_BASE + PROCESS_STACK_SIZE)
#define THREAD_STACK_BASE (STACK_TOP - UINT64_C(0x0c00))
#define THREAD_STACK_SIZE UINT64_C(0x0400)
#define THREAD_STACK_POINTER \
    (THREAD_STACK_BASE + THREAD_STACK_SIZE)

#define TEST_CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "AArch64 clone3 测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        return false; \
    } \
} while (0)

struct argument_memory {
    byte_t bytes[ARGUMENT_MEMORY_SIZE];
    qword_t fail_read_at;
    unsigned read_calls;
    qword_t read_addresses[4];
    dword_t read_sizes[4];
};

struct clone3_fixture {
    struct task *parent;
    struct tgroup group;
    struct argument_memory arguments;
};

static const struct fd_ops metadata_fd_ops = {0};

static qword_t encoded_error(int error) {
    return (qword_t) (sqword_t) error;
}

static void put_u16(byte_t *bytes, word_t value) {
    bytes[0] = (byte_t) value;
    bytes[1] = (byte_t) (value >> 8);
}

static void put_u32(byte_t *bytes, dword_t value) {
    for (byte_t index = 0; index < 4; index++)
        bytes[index] = (byte_t) (value >> (index * 8));
}

static void put_u64(byte_t *bytes, qword_t value) {
    for (byte_t index = 0; index < 8; index++)
        bytes[index] = (byte_t) (value >> (index * 8));
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

static void make_image(byte_t file[IMAGE_SIZE]) {
    // 子任务先核对 TPIDR_EL0，再以 SP 指向的共享控制字等待退出。
    static const dword_t program[] = {
        UINT32_C(0xd53bd041), // mrs x1, tpidr_el0
        UINT32_C(0xf94007e2), // ldr x2, [sp, #8]
        UINT32_C(0xeb02003f), // cmp x1, x2
        UINT32_C(0x540000e1), // b.ne failure
        UINT32_C(0x52800023), // mov w3, #1
        UINT32_C(0xb90007e3), // str w3, [sp, #4]
        UINT32_C(0xb94003e0), // ldr w0, [sp]
        UINT32_C(0x34ffffe0), // cbz w0, wait
        UINT32_C(0xd2800000), // mov x0, #0
        UINT32_C(0x14000002), // b exit
        UINT32_C(0xd28000e0), // failure: mov x0, #7
        UINT32_C(0xd2800ba8), // exit: mov x8, #93
        UINT32_C(0xd4000001), // svc #0
    };

    memset(file, 0, IMAGE_SIZE);
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
    put_u64(file + 24, TEXT_BASE + ENTRY_OFFSET);
    put_u64(file + 32, AARCH64_ELF64_HEADER_SIZE);
    put_u16(file + 52, AARCH64_ELF64_HEADER_SIZE);
    put_u16(file + 54, AARCH64_ELF64_PROGRAM_HEADER_SIZE);
    put_u16(file + 56, 2);

    byte_t *headers = file + AARCH64_ELF64_HEADER_SIZE;
    qword_t header_size = 2 * AARCH64_ELF64_PROGRAM_HEADER_SIZE;
    put_program_header(headers, 6, 4,
            AARCH64_ELF64_HEADER_SIZE,
            TEXT_BASE + AARCH64_ELF64_HEADER_SIZE,
            header_size, header_size, 8);
    put_program_header(headers + AARCH64_ELF64_PROGRAM_HEADER_SIZE,
            1, 5, 0, TEXT_BASE, IMAGE_SIZE,
            IMAGE_SIZE, GUEST_MEMORY_PAGE_SIZE);
    for (size_t index = 0; index < array_size(program); index++)
        put_u32(file + ENTRY_OFFSET + index * sizeof(*program),
                program[index]);
}

static struct task *make_parent(struct tgroup *group) {
    struct task *parent = task_create_(NULL);
    if (parent == NULL)
        return NULL;
    *group = (struct tgroup) {0};
    list_init(&group->threads);
    list_init(&group->session);
    list_init(&group->pgroup);
    lock_init(&group->lock);
    cond_init(&group->child_exit);
    cond_init(&group->stopped_cond);
    group->leader = parent;
    group->sid = parent->pid;
    group->pgid = parent->pid;
    group->limits[RLIMIT_NOFILE_] = (struct rlimit_) {16, 16};
    parent->group = group;
    parent->tgid = parent->pid;

    struct mm *mm = mm_new();
    struct fdtable *files = fdtable_new(1);
    struct fs_info *fs = fs_info_new();
    struct sighand *sighand = sighand_new();
    struct fd *metadata = fd_create(&metadata_fd_ops);
    if (mm == NULL || IS_ERR(files) || fs == NULL ||
            sighand == NULL || metadata == NULL)
        return NULL;
    parent->files = files;
    if (f_install_task(parent, fd_retain(metadata), 0) != 0)
        return NULL;
    mm->exefile = fd_retain(metadata);
    fs->root = fd_retain(metadata);
    fs->pwd = fd_retain(metadata);
    fd_close(metadata);
    task_set_mm(parent, mm);
    parent->fs = fs;
    parent->sighand = sighand;
    task_thread_store(parent, pthread_self());
    task_publish(parent);
    current = parent;
    return parent;
}

static struct aarch64_linux_process *make_process(struct task *task) {
    byte_t file[IMAGE_SIZE];
    make_image(file);
    const char *arguments[] = {"clone3-test"};
    byte_t random[AARCH64_LINUX_PROCESS_RANDOM_SIZE] = {0};
    struct guest_file_source *file_source = guest_file_source_create(
            TEST_FILE_IDENTITY, NULL, NULL);
    if (file_source == NULL)
        return NULL;
    const struct aarch64_linux_process_config config = {
        .elf_data = file,
        .elf_size = sizeof(file),
        .elf_file_source = file_source,
        .stack_top = STACK_TOP,
        .stack_size = 2 * GUEST_MEMORY_PAGE_SIZE,
        .signal_trampoline_page = SIGNAL_TRAMPOLINE,
        .brk_limit = UINT64_C(0x600000),
        .executable = "/bin/clone3-test",
        .arguments = arguments,
        .argument_count = array_size(arguments),
        .random = random,
        .tid = task->pid,
        .task_opaque = task,
        .syscalls = &ish_aarch64_linux_syscall_service,
        .signals = &ish_aarch64_linux_signal_service,
    };
    struct aarch64_linux_process *process =
            aarch64_linux_process_create(&config, NULL);
    guest_file_source_release(file_source);
    return process;
}

static bool init_fixture(struct clone3_fixture *fixture) {
    memset(fixture, 0, sizeof(*fixture));
    fixture->arguments.fail_read_at = UINT64_MAX;
    fixture->parent = make_parent(&fixture->group);
    if (fixture->parent == NULL)
        return false;
    struct aarch64_linux_process *process =
            make_process(fixture->parent);
    return process != NULL && task_attach_aarch64_process(
            fixture->parent, process);
}

static void clear_pending_signals(struct task *task) {
    signal_flush_pending(task);
}

static void destroy_fixture(struct clone3_fixture *fixture) {
    struct task *parent = fixture->parent;
    task_release_aarch64_process(parent);
    clear_pending_signals(parent);
    mm_release(parent->mm);
    fdtable_release(parent->files);
    fs_info_release(parent->fs);
    sighand_release(parent->sighand);
    parent->mm = NULL;
    parent->files = NULL;
    parent->fs = NULL;
    parent->sighand = NULL;
    current = NULL;

    cond_destroy(&parent->pause);
    cond_destroy(&parent->ptrace.cond);
    lock(&pids_lock);
    lock(&fixture->group.lock);
    list_remove(&parent->group_links);
    list_remove(&fixture->group.session);
    list_remove(&fixture->group.pgroup);
    task_destroy(parent);
    unlock(&fixture->group.lock);
    unlock(&pids_lock);
    cond_destroy(&fixture->group.stopped_cond);
    cond_destroy(&fixture->group.child_exit);
}

static void reset_argument_access(struct argument_memory *memory) {
    memset(memory->bytes, 0, sizeof(memory->bytes));
    memory->fail_read_at = UINT64_MAX;
    memory->read_calls = 0;
    memset(memory->read_addresses, 0,
            sizeof(memory->read_addresses));
    memset(memory->read_sizes, 0, sizeof(memory->read_sizes));
}

static bool read_argument_memory(void *opaque, qword_t address,
        void *destination, dword_t size,
        struct guest_linux_user_fault *fault) {
    struct argument_memory *memory = opaque;
    unsigned call = memory->read_calls++;
    if (call < array_size(memory->read_addresses)) {
        memory->read_addresses[call] = address;
        memory->read_sizes[call] = size;
    }
    if (memory->fail_read_at >= address &&
            memory->fail_read_at - address < size) {
        *fault = (struct guest_linux_user_fault) {
            .address = memory->fail_read_at,
            .access = GUEST_MEMORY_READ,
            .kind = GUEST_MEMORY_FAULT_PERMISSION,
        };
        return false;
    }
    if (address < ARGUMENT_ADDRESS) {
        *fault = (struct guest_linux_user_fault) {
            .address = address,
            .access = GUEST_MEMORY_READ,
            .kind = GUEST_MEMORY_FAULT_ADDRESS_SIZE,
        };
        return false;
    }
    qword_t offset = address - ARGUMENT_ADDRESS;
    if (offset > ARGUMENT_MEMORY_SIZE ||
            size > ARGUMENT_MEMORY_SIZE - offset) {
        *fault = (struct guest_linux_user_fault) {
            .address = address,
            .access = GUEST_MEMORY_READ,
            .kind = GUEST_MEMORY_FAULT_ADDRESS_SIZE,
        };
        return false;
    }
    memcpy(destination, memory->bytes + (size_t) offset, size);
    return true;
}

static qword_t invoke_clone3(struct clone3_fixture *fixture,
        qword_t address, qword_t size,
        struct guest_linux_user_fault *fault) {
    const struct guest_linux_syscall_context context = {
        .task_opaque = fixture->parent,
        .user = {
            .opaque = &fixture->arguments,
            .read = read_argument_memory,
        },
    };
    const struct guest_linux_syscall syscall = {
        .number = CLONE3_SYSCALL_NUMBER,
        .arguments = {address, size},
    };
    return ish_aarch64_linux_syscall_service.dispatch(
            &context, &syscall, fault);
}

static void install_args(struct clone3_fixture *fixture,
        const struct aarch64_linux_clone_args *args) {
    reset_argument_access(&fixture->arguments);
    memcpy(fixture->arguments.bytes, args, sizeof(*args));
}

static bool parent_has_no_children(struct task *parent) {
    lock(&pids_lock);
    bool empty = list_empty(&parent->children);
    unlock(&pids_lock);
    return empty;
}

static bool test_sizes_and_versioning(struct clone3_fixture *fixture) {
    struct guest_linux_user_fault fault = {0};
    struct aarch64_linux_clone_args args = {
        .flags = AARCH64_LINUX_CLONE_PIDFD,
    };

    install_args(fixture, &args);
    TEST_CHECK(invoke_clone3(fixture, ARGUMENT_ADDRESS, 63, &fault) ==
                    encoded_error(_EINVAL) &&
            fixture->arguments.read_calls == 0,
            "过短 size 在访问用户地址前返回 EINVAL");
    install_args(fixture, &args);
    TEST_CHECK(invoke_clone3(fixture, ARGUMENT_ADDRESS,
                    GUEST_MEMORY_PAGE_SIZE + 1, &fault) ==
                    encoded_error(_E2BIG) &&
            fixture->arguments.read_calls == 0,
            "超过 guest 页的 size 在访问用户地址前返回 E2BIG");

    static const qword_t versions[] = {
        AARCH64_LINUX_CLONE_ARGS_SIZE_VER0,
        AARCH64_LINUX_CLONE_ARGS_SIZE_VER1,
        AARCH64_LINUX_CLONE_ARGS_SIZE_VER2,
    };
    for (size_t index = 0; index < array_size(versions); index++) {
        install_args(fixture, &args);
        TEST_CHECK(invoke_clone3(fixture, ARGUMENT_ADDRESS,
                        versions[index], &fault) ==
                        encoded_error(_EINVAL) &&
                fixture->arguments.read_calls == 1 &&
                fixture->arguments.read_sizes[0] == versions[index],
                "三个已发布版本按给定前缀读取并补零");
    }

    install_args(fixture, &args);
    TEST_CHECK(invoke_clone3(fixture, ARGUMENT_ADDRESS, 96, &fault) ==
                    encoded_error(_EINVAL) &&
            fixture->arguments.read_calls == 2 &&
            fixture->arguments.read_addresses[0] ==
                    ARGUMENT_ADDRESS + sizeof(args) &&
            fixture->arguments.read_sizes[0] == 8 &&
            fixture->arguments.read_addresses[1] == ARGUMENT_ADDRESS &&
            fixture->arguments.read_sizes[1] == sizeof(args),
            "零未知尾部先于已知 88 字节前缀检查并被接受");

    install_args(fixture, &args);
    fixture->arguments.bytes[sizeof(args)] = 1;
    TEST_CHECK(invoke_clone3(fixture, ARGUMENT_ADDRESS, 96, &fault) ==
                    encoded_error(_E2BIG) &&
            fixture->arguments.read_calls == 1,
            "非零未知尾部返回 E2BIG 且不读取已知前缀");

    install_args(fixture, &args);
    fixture->arguments.fail_read_at =
            ARGUMENT_ADDRESS + sizeof(args) + 2;
    TEST_CHECK(invoke_clone3(fixture, ARGUMENT_ADDRESS, 96, &fault) ==
                    encoded_error(_EFAULT) &&
            fixture->arguments.read_calls == 1 &&
            fault.address == fixture->arguments.fail_read_at,
            "未知尾部故障优先返回精确 EFAULT");

    install_args(fixture, &args);
    fixture->arguments.fail_read_at = ARGUMENT_ADDRESS + 4;
    TEST_CHECK(invoke_clone3(fixture, ARGUMENT_ADDRESS, 96, &fault) ==
                    encoded_error(_EFAULT) &&
            fixture->arguments.read_calls == 2 &&
            fault.address == fixture->arguments.fail_read_at,
            "零尾部通过后保留已知前缀的精确读取故障");

    install_args(fixture, &args);
    qword_t crossing =
            AARCH64_LINUX_USER_ADDRESS_MAX - UINT64_C(31);
    TEST_CHECK(invoke_clone3(fixture, crossing, 64, &fault) ==
                    encoded_error(_EFAULT) &&
            fixture->arguments.read_calls == 0 &&
            fault.address == crossing &&
            fault.access == GUEST_MEMORY_READ &&
            fault.kind == GUEST_MEMORY_FAULT_ADDRESS_SIZE,
            "参数结构跨越 48 位 guest 上界时不调用 host 内存回调");
    return true;
}

static bool test_rejected_semantics(struct clone3_fixture *fixture) {
    static const qword_t rejected_flags[] = {
        SIGCHLD_,
        AARCH64_LINUX_CLONE_DETACHED,
        AARCH64_LINUX_CLONE_PIDFD,
        AARCH64_LINUX_CLONE_PTRACE,
        AARCH64_LINUX_CLONE_PARENT,
        AARCH64_LINUX_CLONE_NEWNS,
        AARCH64_LINUX_CLONE_UNTRACED,
        AARCH64_LINUX_CLONE_NEWCGROUP,
        AARCH64_LINUX_CLONE_NEWUTS,
        AARCH64_LINUX_CLONE_NEWIPC,
        AARCH64_LINUX_CLONE_NEWUSER,
        AARCH64_LINUX_CLONE_NEWPID,
        AARCH64_LINUX_CLONE_NEWNET,
        AARCH64_LINUX_CLONE_IO,
        AARCH64_LINUX_CLONE_CLEAR_SIGHAND,
        AARCH64_LINUX_CLONE_INTO_CGROUP,
        AARCH64_LINUX_CLONE_AUTOREAP,
        AARCH64_LINUX_CLONE_NNP,
        AARCH64_LINUX_CLONE_PIDFD_AUTOKILL,
        AARCH64_LINUX_CLONE_EMPTY_MNTNS,
        AARCH64_LINUX_CLONE_NEWTIME,
        UINT64_C(1) << 63,
    };
    struct guest_linux_user_fault fault = {0};
    for (size_t index = 0;
            index < array_size(rejected_flags); index++) {
        struct aarch64_linux_clone_args args = {
            .flags = rejected_flags[index],
        };
        install_args(fixture, &args);
        TEST_CHECK(invoke_clone3(fixture, ARGUMENT_ADDRESS,
                        sizeof(args), &fault) ==
                        encoded_error(_EINVAL),
                "未建模的 clone3 flag 必须无副作用失败");
    }

    struct aarch64_linux_clone_args args = {
        .set_tid_size = 1,
    };
    install_args(fixture, &args);
    TEST_CHECK(invoke_clone3(fixture, ARGUMENT_ADDRESS,
                    sizeof(args), &fault) == encoded_error(_EINVAL),
            "set_tid 数量没有指针时失败");
    args = (struct aarch64_linux_clone_args) {
        .set_tid = UINT64_C(0x0000700012345000),
    };
    install_args(fixture, &args);
    TEST_CHECK(invoke_clone3(fixture, ARGUMENT_ADDRESS,
                    sizeof(args), &fault) == encoded_error(_EINVAL),
            "set_tid 指针没有数量时失败");
    args.set_tid_size = 1;
    install_args(fixture, &args);
    TEST_CHECK(invoke_clone3(fixture, ARGUMENT_ADDRESS,
                    sizeof(args), &fault) == encoded_error(_EINVAL),
            "当前 PID 模型显式拒绝请求指定 TID");
    args.set_tid_size = 33;
    install_args(fixture, &args);
    TEST_CHECK(invoke_clone3(fixture, ARGUMENT_ADDRESS,
                    sizeof(args), &fault) == encoded_error(_EINVAL),
            "set_tid 数量超过命名空间层级上限时失败");

    args = (struct aarch64_linux_clone_args) {
        .flags = AARCH64_LINUX_CLONE_INTO_CGROUP,
    };
    install_args(fixture, &args);
    TEST_CHECK(invoke_clone3(fixture, ARGUMENT_ADDRESS,
                    AARCH64_LINUX_CLONE_ARGS_SIZE_VER0, &fault) ==
                    encoded_error(_EINVAL),
            "INTO_CGROUP 缺少 VER2 cgroup 字段时失败");
    args.cgroup = (qword_t) INT_MAX + 1;
    install_args(fixture, &args);
    TEST_CHECK(invoke_clone3(fixture, ARGUMENT_ADDRESS,
                    sizeof(args), &fault) == encoded_error(_EINVAL),
            "cgroup fd 超过有符号 32 位边界时失败");

    args = (struct aarch64_linux_clone_args) {
        .exit_signal = UINT64_C(0x100),
    };
    install_args(fixture, &args);
    TEST_CHECK(invoke_clone3(fixture, ARGUMENT_ADDRESS,
                    sizeof(args), &fault) == encoded_error(_EINVAL),
            "exit_signal 超过低 8 位时失败");
    args.exit_signal = NUM_SIGS + 1;
    install_args(fixture, &args);
    TEST_CHECK(invoke_clone3(fixture, ARGUMENT_ADDRESS,
                    sizeof(args), &fault) == encoded_error(_EINVAL),
            "不存在的退出信号在创建任务前失败");
    args = (struct aarch64_linux_clone_args) {
        .flags = AARCH64_LINUX_CLONE_THREAD,
        .exit_signal = SIGCHLD_,
    };
    install_args(fixture, &args);
    TEST_CHECK(invoke_clone3(fixture, ARGUMENT_ADDRESS,
                    sizeof(args), &fault) == encoded_error(_EINVAL),
            "线程 clone3 禁止非零退出信号");

    args = (struct aarch64_linux_clone_args) {.stack_size = 1};
    install_args(fixture, &args);
    TEST_CHECK(invoke_clone3(fixture, ARGUMENT_ADDRESS,
                    sizeof(args), &fault) == encoded_error(_EINVAL),
            "缺少栈基址时拒绝孤立 stack_size");
    args = (struct aarch64_linux_clone_args) {.stack = 1};
    install_args(fixture, &args);
    TEST_CHECK(invoke_clone3(fixture, ARGUMENT_ADDRESS,
                    sizeof(args), &fault) == encoded_error(_EINVAL),
            "缺少栈长度时拒绝孤立 stack");
    args = (struct aarch64_linux_clone_args) {
        .stack = AARCH64_LINUX_USER_ADDRESS_MAX,
        .stack_size = 2,
    };
    install_args(fixture, &args);
    TEST_CHECK(invoke_clone3(fixture, ARGUMENT_ADDRESS,
                    sizeof(args), &fault) == encoded_error(_EINVAL),
            "stack 区间跨越 48 位 guest 上界时失败");

    args = (struct aarch64_linux_clone_args) {
        .flags = AARCH64_LINUX_CLONE_SIGHAND,
    };
    install_args(fixture, &args);
    TEST_CHECK(invoke_clone3(fixture, ARGUMENT_ADDRESS,
                    sizeof(args), &fault) == encoded_error(_EINVAL),
            "共享信号动作必须同时共享地址空间");
    args.flags = AARCH64_LINUX_CLONE_THREAD;
    install_args(fixture, &args);
    TEST_CHECK(invoke_clone3(fixture, ARGUMENT_ADDRESS,
                    sizeof(args), &fault) == encoded_error(_EINVAL),
            "线程组成员必须共享信号动作");
    TEST_CHECK(parent_has_no_children(fixture->parent),
            "所有拒绝路径都不得发布子任务");
    return true;
}

static bool write_control(struct aarch64_linux_process *process,
        qword_t stack_pointer, dword_t release,
        qword_t expected_tls) {
    return aarch64_linux_process_write_u32(
                    process, stack_pointer, release, NULL) &&
            aarch64_linux_process_write_u32(
                    process, stack_pointer + 4, 0, NULL) &&
            aarch64_linux_process_write_u32(
                    process, stack_pointer + 8,
                    (dword_t) expected_tls, NULL) &&
            aarch64_linux_process_write_u32(
                    process, stack_pointer + 12,
                    (dword_t) (expected_tls >> 32), NULL);
}

static bool test_process_clone(struct clone3_fixture *fixture) {
    const qword_t expected_tls = UINT64_C(0x1234567887654321);
    TEST_CHECK(write_control(fixture->parent->aarch64_process,
                    PROCESS_STACK_POINTER, 1, expected_tls),
            "准备普通子进程的高位 TLS 与栈控制字");
    struct aarch64_linux_clone_args args = {
        .flags = AARCH64_LINUX_CLONE_SETTLS,
        .exit_signal = SIGCHLD_,
        .stack = PROCESS_STACK_BASE,
        .stack_size = PROCESS_STACK_SIZE,
        .tls = expected_tls,
    };
    install_args(fixture, &args);
    struct guest_linux_user_fault fault = {0};
    qword_t encoded_pid = invoke_clone3(fixture, ARGUMENT_ADDRESS,
            AARCH64_LINUX_CLONE_ARGS_SIZE_VER0, &fault);
    TEST_CHECK((sqword_t) encoded_pid > 0,
            "VER0 clone3 创建独立地址空间子进程");

    struct wait4_result result;
    sdword_t waited = do_wait4((pid_t_) encoded_pid, 0, &result);
    TEST_CHECK(waited == (pid_t_) encoded_pid && result.status == 0,
            "子进程从 stack 基址加长度所得 SP 启动并保留 64 位 TLS");
    TEST_CHECK(parent_has_no_children(fixture->parent),
            "普通 clone3 子进程退出后可被完整回收");
    return true;
}

static bool wait_for_u32(struct aarch64_linux_process *process,
        qword_t address, dword_t expected) {
    for (unsigned attempt = 0; attempt < 100000; attempt++) {
        dword_t value;
        if (!aarch64_linux_process_read_u32(
                process, address, &value, NULL))
            return false;
        if (value == expected)
            return true;
        sched_yield();
    }
    return false;
}

static bool test_thread_clone(struct clone3_fixture *fixture) {
    const qword_t expected_tls = UINT64_C(0xfedcba9876543210);
    const qword_t child_tid = THREAD_STACK_POINTER + 16;
    const qword_t parent_tid = THREAD_STACK_POINTER + 20;
    TEST_CHECK(write_control(fixture->parent->aarch64_process,
                    THREAD_STACK_POINTER, 0, expected_tls) &&
            aarch64_linux_process_write_u32(
                    fixture->parent->aarch64_process,
                    child_tid, 0, NULL) &&
            aarch64_linux_process_write_u32(
                    fixture->parent->aarch64_process,
                    parent_tid, 0, NULL),
            "准备线程 clone3 的共享高地址状态");

    const qword_t thread_flags =
            AARCH64_LINUX_CLONE_VM | AARCH64_LINUX_CLONE_FS |
            AARCH64_LINUX_CLONE_FILES |
            AARCH64_LINUX_CLONE_SIGHAND |
            AARCH64_LINUX_CLONE_THREAD |
            AARCH64_LINUX_CLONE_SYSVSEM |
            AARCH64_LINUX_CLONE_SETTLS |
            AARCH64_LINUX_CLONE_PARENT_SETTID |
            AARCH64_LINUX_CLONE_CHILD_SETTID |
            AARCH64_LINUX_CLONE_CHILD_CLEARTID;
    struct aarch64_linux_clone_args args = {
        .flags = thread_flags,
        .child_tid = child_tid,
        .parent_tid = parent_tid,
        .stack = THREAD_STACK_BASE,
        .stack_size = THREAD_STACK_SIZE,
        .tls = expected_tls,
    };
    install_args(fixture, &args);
    struct guest_linux_user_fault fault = {0};
    qword_t encoded_pid = invoke_clone3(fixture,
            ARGUMENT_ADDRESS, sizeof(args), &fault);
    TEST_CHECK((sqword_t) encoded_pid > 0,
            "完整 88 字节 clone3 创建线程组成员");
    pid_t_ pid = (pid_t_) encoded_pid;
    TEST_CHECK(wait_for_u32(fixture->parent->aarch64_process,
                    THREAD_STACK_POINTER + 4, 1),
            "子线程以计算后的 64 位 SP 和未截断 TLS 到达就绪点");

    dword_t observed_child_tid;
    dword_t observed_parent_tid;
    TEST_CHECK(aarch64_linux_process_read_u32(
                    fixture->parent->aarch64_process,
                    child_tid, &observed_child_tid, NULL) &&
            aarch64_linux_process_read_u32(
                    fixture->parent->aarch64_process,
                    parent_tid, &observed_parent_tid, NULL) &&
            observed_child_tid == (dword_t) pid &&
            observed_parent_tid == (dword_t) pid,
            "4 GiB 以上 parent/child TID 地址未经过 host 指针收窄");

    lock(&pids_lock);
    struct task *child = pid_get_task(pid);
    bool shared = child != NULL &&
            child->group == fixture->parent->group &&
            child->tgid == fixture->parent->tgid &&
            child->mm == fixture->parent->mm &&
            child->files == fixture->parent->files &&
            child->fs == fixture->parent->fs &&
            child->sighand == fixture->parent->sighand &&
            task_has_aarch64_process(child) &&
            aarch64_linux_process_uses_services(
                    child->aarch64_process, pid, child,
                    &ish_aarch64_linux_syscall_service,
                    &ish_aarch64_linux_signal_service);
    unlock(&pids_lock);
    TEST_CHECK(shared,
            "clone3 线程保持现有任务模型的共享对象边界");

    TEST_CHECK(aarch64_linux_process_write_u32(
                    fixture->parent->aarch64_process,
                    THREAD_STACK_POINTER, 1, NULL),
            "释放子线程退出循环");
    bool reaped = false;
    for (unsigned attempt = 0; attempt < 100000 && !reaped; attempt++) {
        lock(&pids_lock);
        reaped = pid_get_task_zombie(pid) == NULL;
        unlock(&pids_lock);
        if (!reaped)
            sched_yield();
    }
    TEST_CHECK(reaped && wait_for_u32(
                    fixture->parent->aarch64_process,
                    child_tid, 0),
            "线程退出后清除 CHILD_CLEARTID 并释放 PID");
    TEST_CHECK(atomic_load_explicit(&fixture->parent->mm->refcount,
                    memory_order_relaxed) == 1 &&
            atomic_load_explicit(&fixture->parent->files->refcount,
                    memory_order_relaxed) == 1 &&
            atomic_load_explicit(&fixture->parent->fs->refcount,
                    memory_order_relaxed) == 1 &&
            atomic_load_explicit(&fixture->parent->sighand->refcount,
                    memory_order_relaxed) == 1,
            "线程退出后所有共享资源引用恢复到父任务基线");
    return true;
}

static bool run_scenario(void) {
    struct clone3_fixture fixture;
    TEST_CHECK(init_fixture(&fixture), "初始化 AArch64 clone3 夹具");
    TEST_CHECK(test_sizes_and_versioning(&fixture),
            "版本化参数复制");
    TEST_CHECK(test_rejected_semantics(&fixture),
            "不支持语义的失败关闭");
    TEST_CHECK(test_process_clone(&fixture),
            "普通进程 clone3 生命周期");
    TEST_CHECK(test_thread_clone(&fixture),
            "线程 clone3 生命周期");
    destroy_fixture(&fixture);
    return true;
}

int main(void) {
    pid_t host_child = fork();
    if (host_child < 0) {
        fprintf(stderr, "无法隔离 AArch64 clone3 测试：%s\n",
                strerror(errno));
        return 1;
    }
    if (host_child == 0) {
        alarm(30);
        bool passed = run_scenario();
        alarm(0);
        _exit(passed ? 0 : 1);
    }

    int status;
    pid_t waited;
    do {
        waited = waitpid(host_child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited != host_child || !WIFEXITED(status) ||
            WEXITSTATUS(status) != 0) {
        fprintf(stderr, "AArch64 clone3 隔离场景失败");
        if (waited == host_child && WIFSIGNALED(status))
            fprintf(stderr, "：host 信号 %d", WTERMSIG(status));
        fputc('\n', stderr);
        return 1;
    }
    return 0;
}
