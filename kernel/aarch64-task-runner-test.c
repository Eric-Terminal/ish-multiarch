#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "debug.h"
#include "fs/fd.h"
#include "guest/aarch64/elf64.h"
#include "guest/aarch64/linux-process.h"
#include "guest/memory/address-space.h"
#include "kernel/aarch64-signal-service.h"
#include "kernel/aarch64-syscall-service.h"
#include "kernel/aarch64-task-runner.h"
#include "kernel/fs.h"
#include "kernel/mm.h"
#include "kernel/signal.h"
#include "kernel/task.h"

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "AArch64 task runner 测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        return 1; \
    } \
} while (0)

#define IMAGE_SIZE 1024
#define IMAGE_FILE_SIZE UINT64_C(0x400)
#define TEXT_BASE UINT64_C(0x0000000000400000)
#define ENTRY_OFFSET UINT64_C(0x200)
#define ENTRY_ADDRESS (TEXT_BASE + ENTRY_OFFSET)
#define STACK_TOP UINT64_C(0x00007fff00000000)
#define SIGNAL_TRAMPOLINE UINT64_C(0x00007ffe00000000)

struct runner_fixture {
    struct task task;
    struct tgroup group;
    struct sighand sighand;
};

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

static void make_image(byte_t file[IMAGE_SIZE],
        const dword_t *program, size_t instruction_count) {
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
    put_u64(file + 24, ENTRY_ADDRESS);
    put_u64(file + 32, AARCH64_ELF64_HEADER_SIZE);
    put_u16(file + 52, AARCH64_ELF64_HEADER_SIZE);
    put_u16(file + 54, AARCH64_ELF64_PROGRAM_HEADER_SIZE);
    put_u16(file + 56, 2);

    byte_t *headers = file + AARCH64_ELF64_HEADER_SIZE;
    qword_t header_bytes = 2 * AARCH64_ELF64_PROGRAM_HEADER_SIZE;
    put_program_header(headers, 6, 4,
            AARCH64_ELF64_HEADER_SIZE,
            TEXT_BASE + AARCH64_ELF64_HEADER_SIZE,
            header_bytes, header_bytes, 8);
    put_program_header(headers + AARCH64_ELF64_PROGRAM_HEADER_SIZE,
            1, 5, 0, TEXT_BASE, IMAGE_FILE_SIZE,
            IMAGE_FILE_SIZE, UINT64_C(0x1000));

    assert(instruction_count <=
            (IMAGE_SIZE - ENTRY_OFFSET) / sizeof(dword_t));
    for (size_t index = 0; index < instruction_count; index++)
        put_u32(file + ENTRY_OFFSET + index * 4, program[index]);
}

static struct aarch64_linux_process *make_process(
        struct runner_fixture *fixture,
        const dword_t *program, size_t instruction_count,
        bool use_kernel_services) {
    byte_t file[IMAGE_SIZE];
    make_image(file, program, instruction_count);
    const char *arguments[] = {"task-runner"};
    byte_t random[AARCH64_LINUX_PROCESS_RANDOM_SIZE];
    for (byte_t index = 0; index < sizeof(random); index++)
        random[index] = index;
    const struct aarch64_linux_process_config config = {
        .elf_data = file,
        .elf_size = sizeof(file),
        .stack_top = STACK_TOP,
        .stack_size = 2 * GUEST_MEMORY_PAGE_SIZE,
        .signal_trampoline_page = SIGNAL_TRAMPOLINE,
        .brk_limit = TEXT_BASE + UINT64_C(0x200000),
        .executable = "/bin/task-runner",
        .arguments = arguments,
        .argument_count = array_size(arguments),
        .random = random,
        .tid = fixture->task.pid,
        .task_opaque = &fixture->task,
        .syscalls = use_kernel_services ?
                &ish_aarch64_linux_syscall_service : NULL,
        .signals = use_kernel_services ?
                &ish_aarch64_linux_signal_service : NULL,
    };
    return aarch64_linux_process_create(&config, NULL);
}

static void init_fixture(struct runner_fixture *fixture) {
    *fixture = (struct runner_fixture) {0};
    list_init(&fixture->group.threads);
    list_init(&fixture->group.session);
    list_init(&fixture->group.pgroup);
    lock_init(&fixture->group.lock);
    cond_init(&fixture->group.child_exit);
    cond_init(&fixture->group.stopped_cond);

    atomic_init(&fixture->sighand.refcount, 1);
    lock_init(&fixture->sighand.lock);

    list_init(&fixture->task.group_links);
    list_init(&fixture->task.children);
    list_init(&fixture->task.siblings);
    list_init(&fixture->task.queue);
    lock_init(&fixture->task.general_lock);
    lock_init(&fixture->task.waiting_cond_lock);
    cond_init(&fixture->task.pause);
    lock_init(&fixture->task.ptrace.lock);
    cond_init(&fixture->task.ptrace.cond);
    fixture->task.pid = fixture->task.tgid = 4321;
    fixture->task.group = &fixture->group;
    fixture->task.sighand = &fixture->sighand;
    fixture->group.leader = &fixture->task;
    list_add_tail(&fixture->group.threads,
            &fixture->task.group_links);
    current = &fixture->task;
}

static void clear_pending(struct task *task) {
    struct sigqueue *queued, *temporary;
    list_for_each_entry_safe(&task->queue,
            queued, temporary, queue) {
        list_remove(&queued->queue);
        free(queued);
    }
    task->pending = 0;
}

static void destroy_fixture(struct runner_fixture *fixture) {
    task_release_aarch64_process(&fixture->task);
    clear_pending(&fixture->task);
    cond_destroy(&fixture->task.ptrace.cond);
    pthread_mutex_destroy(&fixture->task.ptrace.lock.m);
    cond_destroy(&fixture->task.pause);
    pthread_mutex_destroy(&fixture->task.waiting_cond_lock.m);
    pthread_mutex_destroy(&fixture->task.general_lock.m);
    pthread_mutex_destroy(&fixture->sighand.lock.m);
    cond_destroy(&fixture->group.stopped_cond);
    cond_destroy(&fixture->group.child_exit);
    pthread_mutex_destroy(&fixture->group.lock.m);
    current = NULL;
}

static bool attach_program(struct runner_fixture *fixture,
        const dword_t *program, size_t instruction_count,
        bool use_kernel_services) {
    struct aarch64_linux_process *process = make_process(
            fixture, program, instruction_count,
            use_kernel_services);
    if (process == NULL)
        return false;
    if (task_attach_aarch64_process(&fixture->task, process))
        return true;
    aarch64_linux_process_destroy(process);
    return false;
}

static int test_exit_events(void) {
    static const dword_t exit_program[] = {
        UINT32_C(0xd2800540),
        UINT32_C(0xd2800ba8),
        UINT32_C(0xd4000001),
    };
    struct runner_fixture fixture;
    init_fixture(&fixture);
    CHECK(attach_program(&fixture, exit_program,
            array_size(exit_program), true),
            "创建 exit 程序");
    CHECK(aarch64_task_poll_signals(&fixture.task).action ==
                    AARCH64_TASK_EVENT_CONTINUE &&
            aarch64_task_run_one(&fixture.task).action ==
                    AARCH64_TASK_EVENT_CONTINUE &&
            aarch64_task_run_one(&fixture.task).action ==
                    AARCH64_TASK_EVENT_CONTINUE,
            "启动安全点与 exit 参数指令保持可运行");
    struct aarch64_task_event event =
            aarch64_task_run_one(&fixture.task);
    CHECK(event.action == AARCH64_TASK_EVENT_EXIT &&
            event.status == (UINT32_C(42) << 8),
            "exit 转换为 Linux wait 状态");
    destroy_fixture(&fixture);

    static const dword_t exit_group_program[] = {
        UINT32_C(0xd2803fe0),
        UINT32_C(0xd2800bc8),
        UINT32_C(0xd4000001),
    };
    init_fixture(&fixture);
    CHECK(attach_program(&fixture, exit_group_program,
            array_size(exit_group_program), true),
            "创建 exit_group 程序");
    CHECK(aarch64_task_run_one(&fixture.task).action ==
                    AARCH64_TASK_EVENT_CONTINUE &&
            aarch64_task_run_one(&fixture.task).action ==
                    AARCH64_TASK_EVENT_CONTINUE,
            "执行 exit_group 参数指令");
    event = aarch64_task_run_one(&fixture.task);
    CHECK(event.action == AARCH64_TASK_EVENT_EXIT_GROUP &&
            event.status == (UINT32_C(255) << 8),
            "exit_group 保留八位退出码并转换 wait 状态");
    destroy_fixture(&fixture);
    return 0;
}

static int test_fault_mapping(void) {
    static const struct {
        const char *message;
        dword_t kind;
        int signal;
        int code;
        dword_t payload_kind;
        qword_t address;
    } cases[] = {
        {
            .message = "未映射读映射 SIGSEGV MAPERR",
            .kind = GUEST_MEMORY_FAULT_UNMAPPED,
            .signal = SIGSEGV_, .code = SEGV_MAPERR_,
            .payload_kind = SIGNAL_INFO_PAYLOAD_FAULT,
            .address = UINT64_C(0x0000400011110000),
        },
        {
            .message = "只执行页写入映射 SIGSEGV ACCERR",
            .kind = GUEST_MEMORY_FAULT_PERMISSION,
            .signal = SIGSEGV_, .code = SEGV_ACCERR_,
            .payload_kind = SIGNAL_INFO_PAYLOAD_FAULT,
            .address = UINT64_C(0x0000400022220000),
        },
        {
            .message = "独占访问未对齐映射 SIGBUS ADRALN",
            .kind = GUEST_MEMORY_FAULT_ALIGNMENT,
            .signal = SIGBUS_, .code = BUS_ADRALN_,
            .payload_kind = SIGNAL_INFO_PAYLOAD_FAULT,
            .address = UINT64_C(0x0000400033330001),
        },
        {
            .message = "地址尺寸异常精确映射不可捕获 SIGKILL",
            .kind = GUEST_MEMORY_FAULT_ADDRESS_SIZE,
            .signal = SIGKILL_, .code = SI_KERNEL_,
            .payload_kind = SIGNAL_INFO_PAYLOAD_NONE,
        },
    };

    for (size_t index = 0; index < array_size(cases); index++) {
        const struct guest_linux_user_fault fault = {
            .address = cases[index].address,
            .access = GUEST_MEMORY_READ,
            .kind = cases[index].kind,
        };
        struct siginfo_ info;
        int signal = aarch64_task_fault_signal(&fault, &info);
        CHECK(signal == cases[index].signal &&
                info.sig == cases[index].signal &&
                info.code == cases[index].code &&
                info.payload_kind == cases[index].payload_kind &&
                info.fault.addr == cases[index].address,
                cases[index].message);
    }
    return 0;
}

static int test_immediate_fault_poll(void) {
    struct runner_fixture fixture;
    const dword_t undefined_program[] = {UINT32_C(0xffffffff)};
    init_fixture(&fixture);
    CHECK(attach_program(&fixture, undefined_program,
            array_size(undefined_program), true),
            "创建生产信号服务 fault 程序");
    struct aarch64_task_event event =
            aarch64_task_run_one(&fixture.task);
    CHECK(event.action == AARCH64_TASK_EVENT_TERMINATE &&
            event.status == SIGILL_ && fixture.task.pending == 0 &&
            list_empty(&fixture.task.queue),
            "同步 fault 在原 PC 重试前立即 poll 并终止");
    destroy_fixture(&fixture);

    const dword_t data_fault_program[] = {UINT32_C(0xf9400001)};
    init_fixture(&fixture);
    CHECK(attach_program(&fixture, data_fault_program,
            array_size(data_fault_program), true),
            "创建 data fault 程序");
    event = aarch64_task_run_one(&fixture.task);
    CHECK(event.action == AARCH64_TASK_EVENT_TERMINATE &&
            event.status == SIGSEGV_ && fixture.task.pending == 0 &&
            list_empty(&fixture.task.queue),
            "data fault 强制 SIGSEGV 后立即 poll");
    destroy_fixture(&fixture);

    const dword_t fetch_fault_program[] = {UINT32_C(0x14000400)};
    init_fixture(&fixture);
    CHECK(attach_program(&fixture, fetch_fault_program,
            array_size(fetch_fault_program), true),
            "创建 fetch fault 程序");
    CHECK(aarch64_task_run_one(&fixture.task).action ==
                    AARCH64_TASK_EVENT_CONTINUE,
            "分支指令先退休到未映射地址");
    event = aarch64_task_run_one(&fixture.task);
    CHECK(event.action == AARCH64_TASK_EVENT_TERMINATE &&
            event.status == SIGSEGV_ && fixture.task.pending == 0 &&
            list_empty(&fixture.task.queue),
            "fetch fault 强制 SIGSEGV 后立即 poll");
    destroy_fixture(&fixture);
    return 0;
}

static int test_stop_resume_and_kill(void) {
    const dword_t nop_program[] = {UINT32_C(0xd503201f)};
    struct runner_fixture fixture;
    init_fixture(&fixture);
    CHECK(attach_program(&fixture, nop_program,
            array_size(nop_program), true),
            "创建停止恢复程序");

    deliver_signal(&fixture.task, SIGTSTP_,
            (struct siginfo_) {.code = SI_USER_});
    struct aarch64_task_event event =
            aarch64_task_poll_signals(&fixture.task);
    CHECK(event.action == AARCH64_TASK_EVENT_STOP &&
            event.status == SIGTSTP_ && fixture.group.stopped,
            "默认停止信号提交 group 状态并返回 STOP");
    send_signal(&fixture.task, SIGCONT_, SIGINFO_NIL);
    CHECK(!fixture.group.stopped &&
            aarch64_task_poll_signals(&fixture.task).action ==
                    AARCH64_TASK_EVENT_CONTINUE,
            "SIGCONT 唤醒后先建立信号安全点");

    deliver_signal(&fixture.task, SIGTSTP_,
            (struct siginfo_) {.code = SI_USER_});
    CHECK(aarch64_task_poll_signals(&fixture.task).action ==
                    AARCH64_TASK_EVENT_STOP && fixture.group.stopped,
            "再次进入停止状态");
    send_signal(&fixture.task, SIGKILL_, SIGINFO_NIL);
    event = aarch64_task_poll_signals(&fixture.task);
    CHECK(!fixture.group.stopped &&
            event.action == AARCH64_TASK_EVENT_TERMINATE &&
            event.status == SIGKILL_ && fixture.task.pending == 0,
            "SIGKILL 解除停止并在任何 guest 指令前终止");
    destroy_fixture(&fixture);
    return 0;
}

enum runner_integration_case {
    RUNNER_INTEGRATION_EXIT,
    RUNNER_INTEGRATION_EXIT_GROUP,
    RUNNER_INTEGRATION_STOP_CONTINUE_KILL,
};

static struct {
    bool called;
    bool resources_released;
    bool doing_group_exit;
    int status;
} exit_observation;

static void observe_runner_exit(struct task *task, int status) {
    exit_observation = (typeof(exit_observation)) {
        .called = true,
        .resources_released =
                task->aarch64_process == NULL &&
                task->mm == NULL && task->files == NULL &&
                task->fs == NULL && task->sighand == NULL,
        .doing_group_exit = task->group->doing_group_exit,
        .status = status,
    };
}

static void *run_current_thread(void *opaque) {
    current = opaque;
    task_run_current();
    return NULL;
}

static void wait_group_stopped(
        struct tgroup *group, bool expected) {
    while (true) {
        lock(&group->lock);
        bool stopped = group->stopped;
        unlock(&group->lock);
        if (stopped == expected)
            return;
        sched_yield();
    }
}

static int run_task_current_scenario(
        enum runner_integration_case scenario) {
    struct runner_fixture fixture;
    init_fixture(&fixture);

    struct sighand *heap_sighand = sighand_new();
    struct mm *mm = mm_new();
    struct fdtable *files = fdtable_new(1);
    struct fs_info *fs = fs_info_new();
    if (heap_sighand == NULL || mm == NULL ||
            IS_ERR(files) || fs == NULL)
        _exit(21);
    fixture.task.sighand = heap_sighand;
    task_set_mm(&fixture.task, mm);
    fixture.task.files = files;
    fixture.task.fs = fs;

    struct task parent = {0};
    struct tgroup parent_group = {0};
    list_init(&parent_group.threads);
    list_init(&parent_group.session);
    list_init(&parent_group.pgroup);
    lock_init(&parent_group.lock);
    cond_init(&parent_group.child_exit);
    cond_init(&parent_group.stopped_cond);
    parent.group = &parent_group;
    fixture.task.parent = &parent;

    static const dword_t exit_program[] = {
        UINT32_C(0xd2800540),
        UINT32_C(0xd2800ba8),
        UINT32_C(0xd4000001),
    };
    static const dword_t exit_group_program[] = {
        UINT32_C(0xd2803fe0),
        UINT32_C(0xd2800bc8),
        UINT32_C(0xd4000001),
    };
    static const dword_t wait_program[] = {
        UINT32_C(0x14000000),
    };
    const dword_t *program;
    size_t instruction_count;
    if (scenario == RUNNER_INTEGRATION_EXIT) {
        program = exit_program;
        instruction_count = array_size(exit_program);
    } else if (scenario == RUNNER_INTEGRATION_EXIT_GROUP) {
        program = exit_group_program;
        instruction_count = array_size(exit_group_program);
    } else {
        program = wait_program;
        instruction_count = array_size(wait_program);
    }
    if (!attach_program(&fixture, program,
            instruction_count, true))
        return 22;

    extern void (*exit_hook)(struct task *task, int code);
    exit_observation = (typeof(exit_observation)) {0};
    exit_hook = observe_runner_exit;
    current = NULL;
    if (scenario == RUNNER_INTEGRATION_STOP_CONTINUE_KILL)
        signal(SIGUSR1, SIG_IGN);
    pthread_t thread;
    if (pthread_create(&thread, NULL,
            run_current_thread, &fixture.task) != 0)
        return 23;
    task_thread_store(&fixture.task, thread);

    if (scenario == RUNNER_INTEGRATION_STOP_CONTINUE_KILL) {
        send_signal(&fixture.task, SIGTSTP_,
                (struct siginfo_) {.code = SI_USER_});
        wait_group_stopped(&fixture.group, true);
        send_signal(&fixture.task, SIGCONT_, SIGINFO_NIL);
        wait_group_stopped(&fixture.group, false);
        send_signal(&fixture.task, SIGTSTP_,
                (struct siginfo_) {.code = SI_USER_});
        wait_group_stopped(&fixture.group, true);
        send_signal(&fixture.task, SIGKILL_, SIGINFO_NIL);
    }

    if (pthread_join(thread, NULL) != 0)
        return 24;
    exit_hook = NULL;
    int expected_status;
    bool expected_group_exit;
    if (scenario == RUNNER_INTEGRATION_EXIT) {
        expected_status = (int) (UINT32_C(42) << 8);
        expected_group_exit = false;
    } else if (scenario == RUNNER_INTEGRATION_EXIT_GROUP) {
        expected_status = (int) (UINT32_C(255) << 8);
        expected_group_exit = true;
    } else {
        expected_status = SIGKILL_;
        expected_group_exit = true;
    }
    bool passed = exit_observation.called &&
            exit_observation.resources_released &&
            exit_observation.status == expected_status &&
            exit_observation.doing_group_exit == expected_group_exit &&
            fixture.task.exiting && fixture.task.zombie &&
            list_empty(&fixture.group.threads);

    destroy_fixture(&fixture);
    cond_destroy(&parent_group.stopped_cond);
    cond_destroy(&parent_group.child_exit);
    pthread_mutex_destroy(&parent_group.lock.m);
    return passed ? 0 : 25;
}

static bool run_isolated_task_scenario(
        enum runner_integration_case scenario) {
    pid_t child = fork();
    if (child < 0)
        return false;
    if (child == 0) {
        // 将错误分流或条件变量死锁转换成有界失败。
        alarm(15);
        int result = run_task_current_scenario(scenario);
        alarm(0);
        _exit(result);
    }

    int status;
    pid_t waited;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited != child) {
        fprintf(stderr, "等待 AArch64 runner 子进程失败：%s\n",
                strerror(errno));
        return false;
    }
    if (WIFSIGNALED(status)) {
        fprintf(stderr, "AArch64 runner 子进程被 host 信号 %d 终止\n",
                WTERMSIG(status));
        return false;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "AArch64 runner 子进程返回状态 %d\n",
                WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        return false;
    }
    return true;
}

static int test_task_current_exit_integration(void) {
    CHECK(run_isolated_task_scenario(RUNNER_INTEGRATION_EXIT),
            "task_run_current 选择 AArch64 并走完整 exit 尾段");
    CHECK(run_isolated_task_scenario(RUNNER_INTEGRATION_EXIT_GROUP),
            "exit_group 设置组退出状态并走完整退出尾段");
    CHECK(run_isolated_task_scenario(
                    RUNNER_INTEGRATION_STOP_CONTINUE_KILL),
            "实际等待可被 SIGCONT 恢复并被 SIGKILL 终止");
    return 0;
}

int main(void) {
    if (test_exit_events() != 0)
        return 1;
    if (test_fault_mapping() != 0)
        return 1;
    if (test_immediate_fault_poll() != 0)
        return 1;
    if (test_stop_resume_and_kill() != 0)
        return 1;
    if (test_task_current_exit_integration() != 0)
        return 1;
    return 0;
}
