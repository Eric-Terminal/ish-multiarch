#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "fs/fd.h"
#include "fs/proc.h"
#include "kernel/calls.h"
#include "kernel/elf.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "kernel/mm.h"
#include "kernel/signal.h"
#include "kernel/task.h"

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "i386 exec /proc 发布测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        return 1; \
    } \
} while (0)

#define OLD_DATA_ADDRESS UINT32_C(0x08048000)
#define NEW_ENTRY_ADDRESS UINT32_C(0x09000000)
#define SHOW_CAPACITY 16384

static const char old_arguments[] = "old\0state\0";
static const char new_arguments[] = "/new\0arg\0";
static const char empty_environment[] = "\0";
static const struct aux_ent old_auxv[] = {
    {UINT32_C(0x7f000001), UINT32_C(0x12345678)},
    {0, 0},
};

struct mmap_gate {
    lock_t lock;
    cond_t changed;
    bool entered;
    bool release;
    bool caller_holds_write_lock;
    struct mem *staged_mem;
    page_t start;
    pages_t pages;
};

struct publication_probe {
    byte_t executable[PAGE_SIZE];
    struct mmap_gate gate;
};

enum probe_file_kind {
    PROBE_ROOT,
    PROBE_OLD_EXECUTABLE,
    PROBE_NEW_EXECUTABLE,
};

struct probe_file {
    struct publication_probe *probe;
    enum probe_file_kind kind;
    const char *path;
    size_t position;
};

struct exec_request {
    struct task *task;
    int result;
};

struct shown_data {
    char *bytes;
    size_t size;
};

static ssize_t probe_read(
        struct fd *fd, void *buffer, size_t buffer_size) {
    struct probe_file *file = fd->data;
    if (file->kind != PROBE_NEW_EXECUTABLE)
        return 0;
    size_t available = sizeof(file->probe->executable) - file->position;
    if (buffer_size > available)
        buffer_size = available;
    memcpy(buffer, file->probe->executable + file->position, buffer_size);
    file->position += buffer_size;
    return (ssize_t) buffer_size;
}

static off_t_ probe_lseek(struct fd *fd, off_t_ offset, int whence) {
    struct probe_file *file = fd->data;
    sqword_t position;
    if (whence == LSEEK_SET)
        position = offset;
    else if (whence == LSEEK_CUR)
        position = (sqword_t) file->position + offset;
    else if (whence == LSEEK_END)
        position = (sqword_t) sizeof(file->probe->executable) + offset;
    else
        return _EINVAL;
    if (position < 0 ||
            (qword_t) position > sizeof(file->probe->executable))
        return _EINVAL;
    file->position = (size_t) position;
    return position;
}

static int probe_mmap(struct fd *fd, struct mem *mem,
        page_t start, pages_t pages, off_t offset,
        int protection, int UNUSED(flags)) {
    struct probe_file *file = fd->data;
    if (file->kind != PROBE_NEW_EXECUTABLE || offset != 0)
        return _EINVAL;

    struct mmap_gate *gate = &file->probe->gate;
    lock(&gate->lock);
    gate->staged_mem = mem;
    gate->start = start;
    gate->pages = pages;
    gate->caller_holds_write_lock =
            atomic_load_explicit(&mem->lock.val, memory_order_relaxed) == -1;
    gate->entered = true;
    notify(&gate->changed);
    while (!gate->release)
        wait_for_ignore_signals(&gate->changed, &gate->lock, NULL);
    unlock(&gate->lock);

    int error = pt_map_nothing(mem, start, pages, protection);
    if (error == 0) {
        struct pt_entry *entry = mem_pt(mem, start);
        memset((char *) entry->data->data + entry->offset,
                'N', pages * PAGE_SIZE);
    }
    return error;
}

static int probe_close(struct fd *fd) {
    free(fd->data);
    return 0;
}

static const struct fd_ops probe_fd_ops = {
    .read = probe_read,
    .lseek = probe_lseek,
    .mmap = probe_mmap,
    .close = probe_close,
};

static struct fd *probe_open(struct mount *mount,
        const char *path, int UNUSED(flags), int UNUSED(mode)) {
    enum probe_file_kind kind;
    const char *stable_path;
    if (strcmp(path, "") == 0 || strcmp(path, "/") == 0) {
        kind = PROBE_ROOT;
        stable_path = "/";
    } else if (strcmp(path, "/old") == 0) {
        kind = PROBE_OLD_EXECUTABLE;
        stable_path = "/old";
    } else if (strcmp(path, "/new") == 0) {
        kind = PROBE_NEW_EXECUTABLE;
        stable_path = "/new";
    } else {
        return ERR_PTR(_ENOENT);
    }

    struct fd *fd = fd_create(&probe_fd_ops);
    if (fd == NULL)
        return ERR_PTR(_ENOMEM);
    struct probe_file *file = malloc(sizeof(*file));
    if (file == NULL) {
        fd_close(fd);
        return ERR_PTR(_ENOMEM);
    }
    *file = (struct probe_file) {
        .probe = mount->data,
        .kind = kind,
        .path = stable_path,
    };
    fd->data = file;
    return fd;
}

static int probe_stat(struct mount *UNUSED(mount),
        const char *path, struct statbuf *stat) {
    bool root = strcmp(path, "") == 0 || strcmp(path, "/") == 0;
    if (!root && strcmp(path, "/old") != 0 && strcmp(path, "/new") != 0)
        return _ENOENT;
    *stat = (struct statbuf) {
        .inode = root ? 1 : (strcmp(path, "/old") == 0 ? 2 : 3),
        .mode = root ? S_IFDIR | 0755 : S_IFREG | 0755,
        .size = root ? 0 : PAGE_SIZE,
    };
    return 0;
}

static int probe_fstat(struct fd *fd, struct statbuf *stat) {
    struct probe_file *file = fd->data;
    bool root = file->kind == PROBE_ROOT;
    *stat = (struct statbuf) {
        .inode = root ? 1 :
                (file->kind == PROBE_OLD_EXECUTABLE ? 2 : 3),
        .mode = root ? S_IFDIR | 0755 : S_IFREG | 0755,
        .size = root ? 0 : PAGE_SIZE,
    };
    return 0;
}

static int probe_getpath(struct fd *fd, char *buffer) {
    struct probe_file *file = fd->data;
    strcpy(buffer, file->path);
    return 0;
}

static const struct fs_ops probe_fs = {
    .open = probe_open,
    .stat = probe_stat,
    .fstat = probe_fstat,
    .getpath = probe_getpath,
};

static void init_group(struct tgroup *group, struct task *leader) {
    *group = (struct tgroup) {0};
    list_init(&group->threads);
    signal_group_pending_init(group);
    list_init(&group->session);
    list_init(&group->pgroup);
    lock_init(&group->lock);
    cond_init(&group->child_exit);
    cond_init(&group->stopped_cond);
    group->leader = leader;
    group->sid = leader->pid;
    group->pgid = leader->pid;
    group->limits[RLIMIT_NOFILE_] = (struct rlimit_) {32, 32};
}

static void make_executable(struct publication_probe *probe) {
    memset(probe->executable, 0, sizeof(probe->executable));
    struct elf_header header = {
        .bitness = ELF_32BIT,
        .endian = ELF_LITTLEENDIAN,
        .elfversion1 = 1,
        .type = ELF_EXECUTABLE,
        .machine = ELF_X86,
        .elfversion2 = 1,
        .entry_point = NEW_ENTRY_ADDRESS,
        .prghead_off = sizeof(struct elf_header),
        .header_size = sizeof(struct elf_header),
        .phent_size = sizeof(struct prg_header),
        .phent_count = 1,
    };
    memcpy(&header.magic, ELF_MAGIC, sizeof(header.magic));
    const struct prg_header segment = {
        .type = PT_LOAD,
        .vaddr = NEW_ENTRY_ADDRESS,
        .filesize = PAGE_SIZE,
        .memsize = PAGE_SIZE,
        .flags = PH_R | PH_X,
        .alignment = PAGE_SIZE,
    };
    memcpy(probe->executable, &header, sizeof(header));
    memcpy(probe->executable + sizeof(header), &segment, sizeof(segment));
}

static struct task *make_task(struct publication_probe *probe,
        struct tgroup *group, struct mm **old_mm_out) {
    lock(&mounts_lock);
    int mount_error = do_mount(&probe_fs, "", "", "", 0);
    unlock(&mounts_lock);
    if (mount_error < 0)
        return NULL;
    char root_path[] = "/";
    struct mount *mount = mount_find(root_path);
    mount->data = probe;
    mount_release(mount);

    struct task *task = task_create_(NULL);
    if (task == NULL)
        return NULL;
    init_group(group, task);
    task->group = group;
    task->tgid = task->pid;
    task->exit_signal = SIGCHLD_;
    task->files = fdtable_new(4);
    task->fs = fs_info_new();
    task->sighand = sighand_new();
    struct mm *old_mm = mm_new();
    if (IS_ERR(task->files) || task->fs == NULL ||
            task->sighand == NULL || old_mm == NULL)
        return NULL;
    task_set_mm(task, old_mm);

    current = task;
    struct fd *root = generic_open("/", O_RDONLY_, 0);
    if (IS_ERR(root))
        return NULL;
    task->fs->root = root;
    task->fs->pwd = fd_retain(root);
    struct fd *old_executable = generic_open("/old", O_RDONLY_, 0);
    if (IS_ERR(old_executable))
        return NULL;
    old_mm->exefile = old_executable;

    write_wrlock(&old_mm->mem.lock);
    int map_error = pt_map_nothing(
            &old_mm->mem, PAGE(OLD_DATA_ADDRESS), 1, P_RWX);
    if (map_error == 0)
        mem_pt(&old_mm->mem, PAGE(OLD_DATA_ADDRESS))->data->name =
                "[旧映像]";
    write_wrunlock(&old_mm->mem.lock);
    if (map_error < 0)
        return NULL;

    old_mm->argv_start = OLD_DATA_ADDRESS + 64;
    old_mm->argv_end = old_mm->argv_start + sizeof(old_arguments);
    old_mm->auxv_start = OLD_DATA_ADDRESS + 256;
    old_mm->auxv_end = old_mm->auxv_start + sizeof(old_auxv);
    const byte_t old_byte = 'O';
    if (user_write_mem(&old_mm->mem,
                OLD_DATA_ADDRESS, &old_byte, sizeof(old_byte)) != 0 ||
            user_write_mem(&old_mm->mem, old_mm->argv_start,
                old_arguments, sizeof(old_arguments)) != 0 ||
            user_write_mem(&old_mm->mem, old_mm->auxv_start,
                old_auxv, sizeof(old_auxv)) != 0)
        return NULL;

    task->cpu.eip = OLD_DATA_ADDRESS;
    task->cpu.esp = OLD_DATA_ADDRESS + PAGE_SIZE - 16;
    task_publish(task);
    *old_mm_out = old_mm;
    return task;
}

static bool find_child(struct proc_entry *directory,
        const char *name, struct proc_entry *child) {
    unsigned long index = 0;
    struct proc_entry candidate = {0};
    while (proc_dir_read(directory, &index, &candidate)) {
        if (candidate.meta->name != NULL &&
                strcmp(candidate.meta->name, name) == 0) {
            *child = candidate;
            return true;
        }
    }
    return false;
}

static bool show_proc_entry(struct proc_entry *process,
        const char *name, struct shown_data *shown) {
    struct proc_entry entry = {0};
    if (!find_child(process, name, &entry) || entry.meta->show == NULL)
        return false;
    char *bytes = malloc(SHOW_CAPACITY + 1);
    if (bytes == NULL)
        return false;
    struct proc_data data = {
        .data = bytes,
        .capacity = SHOW_CAPACITY,
    };
    if (entry.meta->show(&entry, &data) < 0 ||
            data.size > SHOW_CAPACITY) {
        free(data.data);
        return false;
    }
    data.data[data.size] = '\0';
    shown->bytes = data.data;
    shown->size = data.size;
    return true;
}

static bool proc_exe_is(
        struct proc_entry *process, const char *expected) {
    struct proc_entry entry = {0};
    char path[MAX_PATH + 1] = {0};
    return find_child(process, "exe", &entry) &&
            entry.meta->readlink != NULL &&
            entry.meta->readlink(&entry, path) == 0 &&
            strcmp(path, expected) == 0;
}

static bool proc_mem_is(struct proc_entry *process,
        addr_t address, byte_t expected) {
    struct proc_entry entry = {0};
    byte_t actual = 0;
    struct proc_data data = {
        .data = (char *) &actual,
        .size = sizeof(actual),
        .capacity = sizeof(actual),
    };
    return find_child(process, "mem", &entry) &&
            entry.meta->pread != NULL &&
            entry.meta->pread(&entry, &data, address) == sizeof(actual) &&
            actual == expected;
}

static bool proc_maps_contains(
        struct proc_entry *process, const char *expected) {
    struct shown_data shown = {0};
    bool result = show_proc_entry(process, "maps", &shown) &&
            strstr(shown.bytes, expected) != NULL;
    free(shown.bytes);
    return result;
}

static bool proc_blob_is(struct proc_entry *process,
        const char *name, const void *expected, size_t expected_size) {
    struct shown_data shown = {0};
    bool result = show_proc_entry(process, name, &shown) &&
            shown.size == expected_size &&
            memcmp(shown.bytes, expected, expected_size) == 0;
    free(shown.bytes);
    return result;
}

static bool proc_new_auxv_is_complete(struct proc_entry *process) {
    struct shown_data shown = {0};
    if (!show_proc_entry(process, "auxv", &shown))
        return false;
    bool complete = shown.size >= 2 * sizeof(struct aux_ent) &&
            shown.size % sizeof(struct aux_ent) == 0;
    if (complete) {
        const struct aux_ent *entries = (const struct aux_ent *) shown.bytes;
        size_t count = shown.size / sizeof(*entries);
        complete = entries[0].type == AX_SYSINFO &&
                entries[count - 1].type == 0 &&
                entries[count - 1].value == 0;
    }
    free(shown.bytes);
    return complete;
}

static bool old_proc_view_is_complete(struct task *task) {
    struct proc_entry process = {
        .meta = &proc_pid,
        .pid = task->pid,
    };
    return proc_exe_is(&process, "/old") &&
            proc_maps_contains(&process, "[旧映像]") &&
            !proc_maps_contains(&process, "/new") &&
            proc_mem_is(&process, OLD_DATA_ADDRESS, 'O') &&
            proc_blob_is(&process, "cmdline",
                    old_arguments, sizeof(old_arguments)) &&
            proc_blob_is(&process, "auxv", old_auxv, sizeof(old_auxv));
}

static bool new_proc_view_is_complete(struct task *task) {
    struct proc_entry process = {
        .meta = &proc_pid,
        .pid = task->pid,
    };
    return proc_exe_is(&process, "/new") &&
            proc_maps_contains(&process, "/new") &&
            !proc_maps_contains(&process, "[旧映像]") &&
            proc_mem_is(&process, NEW_ENTRY_ADDRESS, 'N') &&
            proc_blob_is(&process, "cmdline",
                    new_arguments, sizeof(new_arguments)) &&
            proc_new_auxv_is_complete(&process);
}

static void *run_exec(void *opaque) {
    struct exec_request *request = opaque;
    current = request->task;
    task_thread_store(current, pthread_self());
    request->result = do_execve("/new", 2,
            new_arguments, empty_environment);
    return NULL;
}

static int run_publication_scenario(void) {
    struct publication_probe probe = {0};
    lock_init(&probe.gate.lock);
    cond_init(&probe.gate.changed);
    make_executable(&probe);

    struct tgroup group;
    struct mm *old_mm = NULL;
    struct task *task = make_task(&probe, &group, &old_mm);
    CHECK(task != NULL && old_mm != NULL, "创建带旧映像的任务夹具");

    // 测试引用让旧实现即使过早退休 mm，也能被确定性检查而不是随机 UAF。
    mm_retain(old_mm);
    struct exec_request request = {
        .task = task,
        .result = _EINTR,
    };
    current = NULL;
    pthread_t worker;
    CHECK(pthread_create(&worker, NULL, run_exec, &request) == 0,
            "启动 exec worker");

    lock(&probe.gate.lock);
    while (!probe.gate.entered)
        wait_for_ignore_signals(
                &probe.gate.changed, &probe.gate.lock, NULL);
    struct mem *staged_mem = probe.gate.staged_mem;
    bool loader_lock_ok = probe.gate.caller_holds_write_lock;
    page_t staged_start = probe.gate.start;
    pages_t staged_pages = probe.gate.pages;
    unlock(&probe.gate.lock);

    lock(&pids_lock);
    lock(&task->general_lock);
    bool task_still_old = task->mm == old_mm &&
            task->mem == &old_mm->mem &&
            task->cpu.mmu == &old_mm->mem.mmu;
    unlock(&task->general_lock);
    unlock(&pids_lock);
    bool private_stage = task_still_old && staged_mem != NULL &&
            staged_mem != &old_mm->mem && loader_lock_ok &&
            staged_start == PAGE(NEW_ENTRY_ADDRESS) && staged_pages == 1 &&
            atomic_load_explicit(
                    &old_mm->refcount, memory_order_acquire) == 2;
    bool old_view_complete = private_stage && old_proc_view_is_complete(task);

    lock(&probe.gate.lock);
    probe.gate.release = true;
    notify(&probe.gate.changed);
    unlock(&probe.gate.lock);
    CHECK(pthread_join(worker, NULL) == 0, "等待 exec worker 完成");

    CHECK(private_stage,
            "loader 在未发布的独立 mm 中构建新映像并保留旧 mm");
    CHECK(old_view_complete,
            "构建期间 /proc 只能读取完整旧 exe、maps、mem、cmdline 与 auxv");
    CHECK(request.result == 0, "释放 mmap 屏障后 exec 成功");
    CHECK(task->mm != old_mm && task->mem == staged_mem &&
            task->cpu.mmu == &task->mm->mem.mmu &&
            task->cpu.eip == NEW_ENTRY_ADDRESS &&
            task->cpu.esp == task->mm->stack_start && task->did_exec,
            "成功路径一次性发布完整新 mm 与 CPU 入口状态");
    CHECK(atomic_load_explicit(
                &old_mm->refcount, memory_order_acquire) == 1,
            "发布后旧 mm 只剩测试持有的稳定引用");
    CHECK(new_proc_view_is_complete(task),
            "发布后 /proc 一次性切换到完整新 exe、maps、mem、cmdline 与 auxv");
    mm_release(old_mm);
    return 0;
}

int main(void) {
    pid_t child = fork();
    if (child < 0) {
        fprintf(stderr, "无法 fork i386 exec /proc 发布测试\n");
        return 1;
    }
    if (child == 0) {
        alarm(20);
        _exit(run_publication_scenario());
    }

    int status;
    pid_t waited;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0);
    if (waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0)
        return 0;
    if (waited == child && WIFSIGNALED(status)) {
        fprintf(stderr, "i386 exec /proc 发布测试被 host 信号 %d 终止\n",
                WTERMSIG(status));
    }
    return 1;
}
