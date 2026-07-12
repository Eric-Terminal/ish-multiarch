#include <stdio.h>
#include <string.h>

#include "guest/aarch64/elf64.h"
#include "guest/aarch64/linux-process.h"
#include "guest/memory/address-space.h"
#include "kernel/aarch64-signal-service.h"
#include "kernel/aarch64-syscall-service.h"
#include "kernel/task.h"

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "AArch64 task 生命周期测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        return 1; \
    } \
} while (0)

#define IMAGE_SIZE 1024
#define IMAGE_FILE_SIZE UINT64_C(0x300)
#define IMAGE_ALIGNMENT UINT64_C(0x10000)
#define ENTRY_OFFSET UINT64_C(0x200)
#define LOAD_BIAS UINT64_C(0x0000400000000000)
#define STACK_TOP UINT64_C(0x00007fff00000000)
#define SIGNAL_TRAMPOLINE UINT64_C(0x00007ffe00000000)

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

static void make_image(byte_t file[IMAGE_SIZE]) {
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
    put_u16(file + 56, 2);

    byte_t *headers = file + AARCH64_ELF64_HEADER_SIZE;
    qword_t header_bytes = 2 * AARCH64_ELF64_PROGRAM_HEADER_SIZE;
    put_program_header(headers, 6, 4,
            AARCH64_ELF64_HEADER_SIZE, AARCH64_ELF64_HEADER_SIZE,
            header_bytes, header_bytes, 8);
    put_program_header(headers + AARCH64_ELF64_PROGRAM_HEADER_SIZE,
            1, 5, 0, 0, IMAGE_FILE_SIZE,
            IMAGE_FILE_SIZE, IMAGE_ALIGNMENT);
    put_u32(file + ENTRY_OFFSET, UINT32_C(0xd503201f));
}

static struct aarch64_linux_process *make_process(
        struct task *task, pid_t_ tid,
        bool use_kernel_services) {
    byte_t file[IMAGE_SIZE];
    make_image(file);
    const char *arguments[] = {"task-lifecycle"};
    byte_t random[AARCH64_LINUX_PROCESS_RANDOM_SIZE];
    for (byte_t i = 0; i < sizeof(random); i++)
        random[i] = i;
    const struct aarch64_linux_process_config config = {
        .elf_data = file,
        .elf_size = sizeof(file),
        .load_bias = LOAD_BIAS,
        .stack_top = STACK_TOP,
        .stack_size = 2 * GUEST_MEMORY_PAGE_SIZE,
        .signal_trampoline_page = SIGNAL_TRAMPOLINE,
        .brk_limit = LOAD_BIAS + UINT64_C(0x100000),
        .executable = "/bin/task-lifecycle",
        .arguments = arguments,
        .argument_count = array_size(arguments),
        .random = random,
        .tid = tid,
        .task_opaque = task,
        .syscalls = use_kernel_services ?
                &ish_aarch64_linux_syscall_service : NULL,
        .signals = use_kernel_services ?
                &ish_aarch64_linux_signal_service : NULL,
    };
    return aarch64_linux_process_create(&config, NULL);
}

static bool stage_process(struct task *task,
        struct aarch64_linux_process *process) {
    struct mm *mm = mm_new();
    if (mm == NULL)
        return false;
    if (task_stage_aarch64_exec(task, process, mm))
        return true;
    mm_release(mm);
    return false;
}

int main(void) {
    struct task parent = {.pid = 1000};
    lock_init(&parent.general_lock);
    struct aarch64_linux_process *first =
            make_process(&parent, parent.pid, true);
    CHECK(first != NULL, "创建首个 opaque process");
    CHECK(task_attach_aarch64_process(&parent, first),
            "attach 接受匹配的生产服务闭包");
    CHECK(task_has_aarch64_process(&parent) &&
            parent.aarch64_process == first,
            "attach 接管唯一所有权");

    struct aarch64_linux_process *taken =
            task_take_aarch64_process(&parent);
    CHECK(taken == first && !task_has_aarch64_process(&parent),
            "take 原样交还所有权并清空 task");
    CHECK(task_attach_aarch64_process(&parent, taken),
            "take 后可重新交还同一 process");

    struct aarch64_linux_process *replacement =
            make_process(&parent, parent.pid, true);
    CHECK(replacement != NULL &&
            stage_process(&parent, replacement) &&
            task_has_aarch64_exec_candidate(&parent) &&
            parent.aarch64_process == first,
            "exec 候选在安全点前不替换活动 process");
    struct aarch64_linux_process *duplicate_candidate =
            make_process(&parent, parent.pid, true);
    CHECK(duplicate_candidate != NULL &&
            !stage_process(&parent, duplicate_candidate) &&
            parent.aarch64_exec_candidate == replacement,
            "stage 拒绝覆盖尚未提交的 exec 候选");
    aarch64_linux_process_destroy(duplicate_candidate);
    task_commit_aarch64_exec(&parent);
    CHECK(parent.aarch64_process == replacement &&
            !task_has_aarch64_exec_candidate(&parent),
            "安全点提交候选并退休旧 process");

    struct aarch64_linux_process *discarded =
            make_process(&parent, parent.pid, true);
    CHECK(discarded != NULL &&
            stage_process(&parent, discarded),
            "建立可回滚的 exec 候选");
    task_discard_aarch64_exec(&parent);
    CHECK(parent.aarch64_process == replacement &&
            !task_has_aarch64_exec_candidate(&parent),
            "回滚候选不影响活动 process");
    task_release_aarch64_process(&parent);
    CHECK(!task_has_aarch64_process(&parent),
            "release 销毁并清空 opaque process");
    task_release_aarch64_process(&parent);

    struct aarch64_linux_process *inherited =
            make_process(&parent, parent.pid, true);
    CHECK(inherited != NULL, "创建待验证浅拷贝的 process");
    CHECK(task_attach_aarch64_process(&parent, inherited),
            "为浅拷贝验证接管 process");
    struct aarch64_linux_process *inherited_candidate =
            make_process(&parent, parent.pid, true);
    CHECK(inherited_candidate != NULL &&
            stage_process(&parent, inherited_candidate),
            "为浅拷贝验证建立 exec 候选");
    struct task *child = task_create_(&parent);
    CHECK(child != NULL && !task_has_aarch64_process(child) &&
            !task_has_aarch64_exec_candidate(child) &&
            parent.aarch64_process == inherited &&
            parent.aarch64_exec_candidate == inherited_candidate,
            "task_create_ 不复制父任务活动或候选 opaque 所有权");
    task_abort_create(child);
    task_discard_aarch64_exec(&parent);
    task_release_aarch64_process(&parent);

    struct task *aborted = task_create_(&parent);
    CHECK(aborted != NULL, "创建待中止任务");
    struct aarch64_linux_process *aborted_process =
            make_process(aborted, aborted->pid, true);
    CHECK(aborted_process != NULL, "为待中止任务创建 process");
    CHECK(task_attach_aarch64_process(aborted, aborted_process),
            "为待中止任务接管 process");
    struct aarch64_linux_process *aborted_candidate =
            make_process(aborted, aborted->pid, true);
    CHECK(aborted_candidate != NULL &&
            stage_process(aborted, aborted_candidate),
            "为待中止任务建立 exec 候选");
    task_abort_create(aborted);
    CHECK(!task_has_aarch64_process(&parent),
            "中止子任务不影响父任务所有权");

    struct aarch64_linux_process *standalone =
            make_process(&parent, parent.pid, false);
    CHECK(standalone != NULL &&
            !task_attach_aarch64_process(&parent, standalone) &&
            !task_has_aarch64_process(&parent),
            "attach 拒绝缺少内核服务闭包的 standalone process");
    aarch64_linux_process_destroy(standalone);

    struct aarch64_linux_process *wrong_tid =
            make_process(&parent, parent.pid + 1, true);
    CHECK(wrong_tid != NULL &&
            !task_attach_aarch64_process(&parent, wrong_tid) &&
            !task_has_aarch64_process(&parent),
            "attach 拒绝服务正确但 tid 不匹配的 process");
    aarch64_linux_process_destroy(wrong_tid);
    mm_release(parent.mm);
    parent.mm = NULL;
    parent.mem = NULL;
    return 0;
}
