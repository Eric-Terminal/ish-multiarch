#include <stdio.h>
#include <string.h>

#include "guest/aarch64/elf64.h"
#include "guest/aarch64/linux-futex-abi.h"
#include "guest/aarch64/linux-process.h"
#include "guest/memory/address-space.h"
#include "kernel/aarch64-signal-service.h"
#include "kernel/aarch64-syscall-service.h"
#include "kernel/calls.h"
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
#define ROBUST_HEAD (STACK_TOP - 2 * GUEST_MEMORY_PAGE_SIZE + UINT64_C(0x100))
#define ROBUST_ENTRY (ROBUST_HEAD + UINT64_C(0x40))
#define ROBUST_FUTEX (ROBUST_ENTRY + UINT64_C(0x8))
#define CLEAR_CHILD_TID (ROBUST_HEAD + UINT64_C(0x100))

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

static struct aarch64_linux_process *make_process_with_clear_tid(
        struct task *task, pid_t_ tid, qword_t clear_child_tid) {
    struct aarch64_linux_process *base =
            make_process(task, tid, true);
    if (base == NULL)
        return NULL;
    const struct aarch64_linux_process_thread_config config = {
        .tid = tid,
        .task_opaque = task,
        .clear_child_tid = clear_child_tid,
    };
    struct aarch64_linux_process *process =
            aarch64_linux_process_clone_thread(base, &config, NULL);
    aarch64_linux_process_destroy(base);
    return process;
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

static bool write_guest_u64(struct aarch64_linux_process *process,
        qword_t address, qword_t value) {
    return aarch64_linux_process_write_u32(
                    process, address, (dword_t) value, NULL) &&
            aarch64_linux_process_write_u32(
                    process, address + 4,
                    (dword_t) (value >> 32), NULL);
}

static bool write_robust_state(
        struct aarch64_linux_process *process, pid_t_ owner) {
    return write_guest_u64(process, ROBUST_HEAD, ROBUST_ENTRY) &&
            write_guest_u64(process, ROBUST_HEAD + 8, 8) &&
            write_guest_u64(process, ROBUST_HEAD + 16, 0) &&
            write_guest_u64(process, ROBUST_ENTRY, ROBUST_HEAD) &&
            aarch64_linux_process_write_u32(
                    process, ROBUST_FUTEX, (dword_t) owner, NULL);
}

int main(void) {
    struct task parent = {.pid = 1000};
    lock_init(&parent.general_lock);
    struct mm *initial_mm = mm_new();
    CHECK(initial_mm != NULL, "创建首个 metadata mm");
    task_set_mm(&parent, initial_mm);
    current = &parent;
    struct aarch64_linux_process *first =
            make_process_with_clear_tid(
                    &parent, parent.pid, CLEAR_CHILD_TID);
    CHECK(first != NULL && aarch64_linux_process_write_u32(
                    first, CLEAR_CHILD_TID,
                    (dword_t) parent.pid, NULL),
            "创建带 clear-child-tid 注册的首个 opaque process");
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

    const struct aarch64_linux_process_thread_config observer_config = {
        .tid = parent.pid + 1,
        .task_opaque = &parent,
    };
    struct aarch64_linux_process *old_image_observer =
            aarch64_linux_process_clone_thread(
                    first, &observer_config, NULL);
    // guest 观察者保留旧字节；额外 mm 引用模拟仍存活的 CLONE_VM peer。
    struct mm *old_metadata_observer = parent.mm;
    mm_retain(old_metadata_observer);
    CHECK(old_image_observer != NULL &&
            write_robust_state(first, parent.pid) &&
            sys_set_robust_list_aarch64(ROBUST_HEAD,
                    sizeof(struct aarch64_linux_robust_list_head)) == 0,
            "在旧 exec 映像登记可观察的 robust 节点");

    struct aarch64_linux_process *replacement =
            make_process_with_clear_tid(
                    &parent, parent.pid, CLEAR_CHILD_TID);
    CHECK(replacement != NULL && aarch64_linux_process_write_u32(
                    replacement, CLEAR_CHILD_TID,
                    (dword_t) parent.pid, NULL) &&
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
    dword_t old_word;
    dword_t old_clear_tid;
    qword_t registration = UINT64_MAX;
    CHECK(parent.aarch64_process == replacement &&
            !task_has_aarch64_exec_candidate(&parent) &&
            aarch64_linux_process_read_u32(
                    old_image_observer, ROBUST_FUTEX,
                    &old_word, NULL) &&
            old_word == AARCH64_LINUX_FUTEX_OWNER_DIED &&
            aarch64_linux_process_read_u32(
                    old_image_observer, CLEAR_CHILD_TID,
                    &old_clear_tid, NULL) &&
            old_clear_tid == 0 &&
            sys_get_robust_list_aarch64(0, &registration) == 0 &&
            registration == 0,
            "安全点在销毁旧映像前完成 robust 与 clear-child-tid 清理");
    aarch64_linux_process_destroy(old_image_observer);
    mm_release(old_metadata_observer);

    struct aarch64_linux_process *discarded =
            make_process_with_clear_tid(
                    &parent, parent.pid, CLEAR_CHILD_TID);
    CHECK(discarded != NULL && aarch64_linux_process_write_u32(
                    discarded, CLEAR_CHILD_TID,
                    (dword_t) parent.pid, NULL) &&
            write_robust_state(
                    replacement, parent.pid) &&
            sys_set_robust_list_aarch64(ROBUST_HEAD,
                    sizeof(struct aarch64_linux_robust_list_head)) == 0 &&
            stage_process(&parent, discarded),
            "建立可回滚的 exec 候选");
    const struct aarch64_linux_process_thread_config
            discarded_observer_config = {
        .tid = parent.pid + 2,
        .task_opaque = &parent,
    };
    struct aarch64_linux_process *discarded_observer =
            aarch64_linux_process_clone_thread(
                    discarded, &discarded_observer_config, NULL);
    CHECK(discarded_observer != NULL,
            "保留可回滚候选的地址空间观察者");
    task_discard_aarch64_exec(&parent);
    dword_t preserved_word;
    dword_t preserved_clear_tid;
    dword_t discarded_clear_tid;
    CHECK(parent.aarch64_process == replacement &&
            !task_has_aarch64_exec_candidate(&parent) &&
            sys_get_robust_list_aarch64(0, &registration) == 0 &&
            registration == ROBUST_HEAD &&
            aarch64_linux_process_read_u32(
                    replacement, ROBUST_FUTEX,
                    &preserved_word, NULL) &&
            preserved_word == (dword_t) parent.pid &&
            aarch64_linux_process_take_clear_child_tid(replacement) ==
                    CLEAR_CHILD_TID &&
            aarch64_linux_process_read_u32(
                    replacement, CLEAR_CHILD_TID,
                    &preserved_clear_tid, NULL) &&
            preserved_clear_tid == (dword_t) parent.pid &&
            aarch64_linux_process_read_u32(
                    discarded_observer, CLEAR_CHILD_TID,
                    &discarded_clear_tid, NULL) &&
            discarded_clear_tid == (dword_t) parent.pid,
            "回滚候选不触发活动映像或候选映像的退出副作用");
    aarch64_linux_process_destroy(discarded_observer);
    CHECK(sys_set_robust_list_aarch64(0,
            sizeof(struct aarch64_linux_robust_list_head)) == 0,
            "释放活动映像前清除测试注册");
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
    const qword_t inherited_registration =
            UINT64_C(0x00007fff12345678);
    CHECK(sys_set_robust_list_aarch64(inherited_registration,
            sizeof(struct aarch64_linux_robust_list_head)) == 0,
            "登记待验证的父任务 robust 指针");
    struct task *child = task_create_(&parent);
    CHECK(child != NULL && !task_has_aarch64_process(child) &&
            !task_has_aarch64_exec_candidate(child) &&
            child->aarch64_robust_list == 0 &&
            parent.aarch64_robust_list == inherited_registration &&
            parent.aarch64_process == inherited &&
            parent.aarch64_exec_candidate == inherited_candidate,
            "task_create_ 不复制父任务 opaque 所有权或 robust 注册");
    task_abort_create(child);
    CHECK(sys_set_robust_list_aarch64(0,
            sizeof(struct aarch64_linux_robust_list_head)) == 0,
            "浅拷贝验证后清除父任务注册");
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
    current = NULL;
    return 0;
}
