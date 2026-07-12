#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "emu/interrupt.h"
#include "fs/fd.h"
#include "fs/path.h"
#include "guest/aarch64/elf64.h"
#include "guest/aarch64/linux-process.h"
#include "kernel/aarch64-exec.h"
#include "kernel/aarch64-exec-image.h"
#include "kernel/aarch64-task-runner.h"
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/elf.h"
#include "kernel/fs.h"
#include "kernel/task.h"

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "AArch64 执行映像测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        return 1; \
    } \
} while (0)

#define TEST_FILE_SIZE 512
#define TEST_SEGMENT_OFFSET 256
#define TEST_SEGMENT_ADDRESS UINT64_C(0x400100)
#define TEST_INTERPRETER_OFFSET 384
#define DYNAMIC_INTERPRETER_OFFSET 448
#define TARGET_ROOT "/target-root"
#define TARGET_PWD TARGET_ROOT "/work"
#define INTERPRETER_PATH "/lib/ld-musl-aarch64.so.1"
#define RESOLVED_INTERPRETER TARGET_ROOT INTERPRETER_PATH
#define RESOLVED_MAIN TARGET_ROOT "/bin/test"
#define AARCH64_MOV_X0_42 UINT32_C(0xd2800540)
#define AARCH64_MOV_X8_EXIT UINT32_C(0xd2800ba8)
#define AARCH64_SVC_0 UINT32_C(0xd4000001)

struct fs_probe {
    const byte_t *main_data;
    size_t main_size;
    qword_t main_stat_size;
    mode_t_ main_mode;
    uid_t_ main_uid;
    uid_t_ main_gid;
    const byte_t *interpreter_data;
    size_t interpreter_size;
    qword_t interpreter_stat_size;
    mode_t_ interpreter_mode;
    uid_t_ interpreter_uid;
    uid_t_ interpreter_gid;
    int open_error;
    int interpreter_read_error;
    size_t read_limit;
    unsigned opens;
    unsigned closes;
    unsigned main_reads;
    unsigned interpreter_reads;
    char opened_path[MAX_PATH];
};

struct file_state {
    struct fs_probe *probe;
    const char *path;
    const byte_t *data;
    size_t size;
    qword_t stat_size;
    size_t read_limit;
    int read_error;
    bool directory;
    bool interpreter;
    bool owned;
};

struct task_fixture {
    struct task task;
    struct tgroup group;
    struct sighand sighand;
    struct fs_info fs;
    struct mount *mount;
    struct fd *root;
    struct fd *pwd;
    struct file_state root_state;
    struct file_state pwd_state;
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

static void make_runnable_image(byte_t file[TEST_FILE_SIZE]) {
    memset(file, 0, TEST_FILE_SIZE);
    file[0] = 0x7f;
    file[1] = 'E';
    file[2] = 'L';
    file[3] = 'F';
    file[4] = 2;
    file[5] = 1;
    file[6] = 1;
    file[7] = 3;
    put_u16(file + 16, 2);
    put_u16(file + 18, 183);
    put_u32(file + 20, 1);
    put_u64(file + 24, TEST_SEGMENT_ADDRESS);
    put_u64(file + 32, AARCH64_ELF64_HEADER_SIZE);
    put_u16(file + 52, AARCH64_ELF64_HEADER_SIZE);
    put_u16(file + 54, AARCH64_ELF64_PROGRAM_HEADER_SIZE);
    put_u16(file + 56, 2);

    byte_t *headers = file + AARCH64_ELF64_HEADER_SIZE;
    qword_t header_size = 2 * AARCH64_ELF64_PROGRAM_HEADER_SIZE;
    put_program_header(headers, 6, 4,
            AARCH64_ELF64_HEADER_SIZE,
            UINT64_C(0x400000) + AARCH64_ELF64_HEADER_SIZE,
            header_size, header_size, 8);
    put_program_header(headers + AARCH64_ELF64_PROGRAM_HEADER_SIZE,
            1, 5, 0, UINT64_C(0x400000),
            TEST_FILE_SIZE, TEST_FILE_SIZE, UINT64_C(0x1000));
    put_u32(file + TEST_SEGMENT_OFFSET, AARCH64_MOV_X0_42);
    put_u32(file + TEST_SEGMENT_OFFSET + 4, AARCH64_MOV_X8_EXIT);
    put_u32(file + TEST_SEGMENT_OFFSET + 8, AARCH64_SVC_0);
}

static void make_dynamic_main_image(byte_t file[TEST_FILE_SIZE]) {
    make_runnable_image(file);
    put_u16(file + 56, 3);
    byte_t *headers = file + AARCH64_ELF64_HEADER_SIZE;
    put_program_header(headers, 3, 4,
            DYNAMIC_INTERPRETER_OFFSET, 0,
            sizeof(INTERPRETER_PATH), sizeof(INTERPRETER_PATH), 1);
    qword_t header_size = 3 * AARCH64_ELF64_PROGRAM_HEADER_SIZE;
    put_program_header(headers + AARCH64_ELF64_PROGRAM_HEADER_SIZE,
            6, 4, AARCH64_ELF64_HEADER_SIZE,
            UINT64_C(0x400000) + AARCH64_ELF64_HEADER_SIZE,
            header_size, header_size, 8);
    put_program_header(headers + 2 * AARCH64_ELF64_PROGRAM_HEADER_SIZE,
            1, 5, 0, UINT64_C(0x400000),
            TEST_FILE_SIZE, TEST_FILE_SIZE, UINT64_C(0x1000));
    memcpy(file + DYNAMIC_INTERPRETER_OFFSET,
            INTERPRETER_PATH, sizeof(INTERPRETER_PATH));
}

static dword_t movz_x(unsigned reg, word_t immediate) {
    return UINT32_C(0xd2800000) |
            (dword_t) immediate << 5 | (dword_t) reg;
}

static dword_t movk_x_lsl16(unsigned reg, word_t immediate) {
    return UINT32_C(0xf2a00000) |
            (dword_t) immediate << 5 | (dword_t) reg;
}

static void make_exec_caller_image(byte_t file[TEST_FILE_SIZE]) {
    make_runnable_image(file);
    const qword_t base = UINT64_C(0x400000);
    const qword_t filename = base + UINT64_C(0x180);
    const qword_t argument0 = base + UINT64_C(0x190);
    const qword_t argument1 = base + UINT64_C(0x1a0);
    const qword_t environment0 = base + UINT64_C(0x1a8);
    const qword_t argument_table = base + UINT64_C(0x1c0);
    const qword_t environment_table = base + UINT64_C(0x1e0);
    const qword_t addresses[] = {
        filename, argument_table, environment_table,
    };
    size_t cursor = TEST_SEGMENT_OFFSET;
    for (unsigned reg = 0; reg < array_size(addresses); reg++) {
        put_u32(file + cursor,
                movz_x(reg, (word_t) addresses[reg]));
        cursor += 4;
        put_u32(file + cursor, movk_x_lsl16(
                reg, (word_t) (addresses[reg] >> 16)));
        cursor += 4;
    }
    put_u32(file + cursor, movz_x(8, 221));
    cursor += 4;
    put_u32(file + cursor, AARCH64_SVC_0);
    cursor += 4;
    put_u32(file + cursor, movz_x(8, 93));
    cursor += 4;
    put_u32(file + cursor, AARCH64_SVC_0);

    strcpy((char *) file + 0x180, "/bin/test");
    strcpy((char *) file + 0x190, "exec-stage");
    strcpy((char *) file + 0x1a0, "one");
    strcpy((char *) file + 0x1a8, "A=B");
    put_u64(file + 0x1c0, argument0);
    put_u64(file + 0x1c8, argument1);
    put_u64(file + 0x1d0, 0);
    put_u64(file + 0x1e0, environment0);
    put_u64(file + 0x1e8, 0);
}

static void make_main_image(byte_t file[TEST_FILE_SIZE],
        bool has_interpreter) {
    memset(file, 0, TEST_FILE_SIZE);
    file[0] = 0x7f;
    file[1] = 'E';
    file[2] = 'L';
    file[3] = 'F';
    file[4] = 2;
    file[5] = 1;
    file[6] = 1;
    file[7] = 3;
    put_u16(file + 16, 2);
    put_u16(file + 18, 183);
    put_u32(file + 20, 1);
    put_u64(file + 24, TEST_SEGMENT_ADDRESS);
    put_u64(file + 32, AARCH64_ELF64_HEADER_SIZE);
    put_u16(file + 52, AARCH64_ELF64_HEADER_SIZE);
    put_u16(file + 54, AARCH64_ELF64_PROGRAM_HEADER_SIZE);
    put_u16(file + 56, has_interpreter ? 2 : 1);

    byte_t *header = file + AARCH64_ELF64_HEADER_SIZE;
    if (has_interpreter) {
        put_u32(header, 3);
        put_u32(header + 4, 4);
        put_u64(header + 8, TEST_INTERPRETER_OFFSET);
        put_u64(header + 32, sizeof(INTERPRETER_PATH));
        put_u64(header + 40, sizeof(INTERPRETER_PATH));
        put_u64(header + 48, 1);
        memcpy(file + TEST_INTERPRETER_OFFSET,
                INTERPRETER_PATH, sizeof(INTERPRETER_PATH));
        header += AARCH64_ELF64_PROGRAM_HEADER_SIZE;
    }
    put_u32(header, 1);
    put_u32(header + 4, 5);
    put_u64(header + 8, TEST_SEGMENT_OFFSET);
    put_u64(header + 16, TEST_SEGMENT_ADDRESS);
    put_u64(header + 32, 12);
    put_u64(header + 40, 16);
    put_u64(header + 48, 256);
    for (byte_t i = 0; i < 12; i++)
        file[TEST_SEGMENT_OFFSET + i] = (byte_t) (0x80 + i);
}

static ssize_t copy_file_bytes(struct fd *fd, void *buffer,
        size_t size, size_t offset) {
    struct file_state *state = fd->data;
    if (state->interpreter)
        state->probe->interpreter_reads++;
    else
        state->probe->main_reads++;
    if (state->read_error != 0)
        return state->read_error;
    if (offset >= state->size)
        return 0;
    size_t copied = state->size - offset;
    if (copied > size)
        copied = size;
    if (state->read_limit != 0 && copied > state->read_limit)
        copied = state->read_limit;
    memcpy(buffer, state->data + offset, copied);
    return (ssize_t) copied;
}

static ssize_t probe_read(struct fd *fd, void *buffer, size_t size) {
    ssize_t read = copy_file_bytes(fd, buffer, size, fd->offset);
    if (read > 0)
        fd->offset += (unsigned long) read;
    return read;
}

static ssize_t probe_pread(struct fd *fd, void *buffer,
        size_t size, off_t offset) {
    if (offset < 0)
        return _EINVAL;
    return copy_file_bytes(fd, buffer, size, (size_t) offset);
}

static off_t_ probe_lseek(struct fd *fd, off_t_ offset, int whence) {
    off_t_ next;
    if (whence == LSEEK_SET) {
        next = offset;
    } else if (whence == LSEEK_CUR) {
        if (__builtin_add_overflow((off_t_) fd->offset, offset, &next))
            return _EINVAL;
    } else {
        return _EINVAL;
    }
    if (next < 0 || (qword_t) next > ULONG_MAX)
        return _EINVAL;
    fd->offset = (unsigned long) next;
    return next;
}

static int probe_close(struct fd *fd) {
    struct file_state *state = fd->data;
    if (state != NULL && state->owned) {
        state->probe->closes++;
        free(state);
    }
    return 0;
}

static const struct fd_ops stream_ops = {
    .read = probe_read,
    .lseek = probe_lseek,
    .close = probe_close,
};

static const struct fd_ops positioned_ops = {
    .pread = probe_pread,
    .close = probe_close,
};

static const struct fd_ops directory_ops = {
    .close = probe_close,
};

static struct fd *probe_open(struct mount *mount,
        const char *path, int flags, int mode) {
    use(flags, mode);
    struct fs_probe *probe = mount->data;
    probe->opens++;
    strcpy(probe->opened_path, path);
    if (probe->open_error != 0)
        return ERR_PTR(probe->open_error);
    bool interpreter = strcmp(path, RESOLVED_INTERPRETER) == 0;
    bool main = strcmp(path, RESOLVED_MAIN) == 0;
    if (!interpreter && !main)
        return ERR_PTR(_ENOENT);

    struct fd *fd = fd_create(
            interpreter ? &positioned_ops : &stream_ops);
    if (fd == NULL)
        return ERR_PTR(_ENOMEM);
    struct file_state *state = malloc(sizeof(*state));
    if (state == NULL) {
        fd_close(fd);
        return ERR_PTR(_ENOMEM);
    }
    *state = (struct file_state) {
        .probe = probe,
        .path = interpreter ? RESOLVED_INTERPRETER : RESOLVED_MAIN,
        .data = interpreter ?
                probe->interpreter_data : probe->main_data,
        .size = interpreter ?
                probe->interpreter_size : probe->main_size,
        .stat_size = interpreter ?
                probe->interpreter_stat_size : probe->main_stat_size,
        .read_limit = probe->read_limit,
        .read_error = interpreter ? probe->interpreter_read_error : 0,
        .interpreter = interpreter,
        .owned = true,
    };
    fd->data = state;
    return fd;
}

static int probe_stat(struct mount *mount,
        const char *path, struct statbuf *stat) {
    struct fs_probe *probe = mount->data;
    *stat = (struct statbuf) {
        .inode = 1,
        .mode = strcmp(path, RESOLVED_INTERPRETER) == 0 ?
                probe->interpreter_mode :
                (strcmp(path, RESOLVED_MAIN) == 0 ?
                probe->main_mode : S_IFDIR | 0711),
        .uid = strcmp(path, RESOLVED_INTERPRETER) == 0 ?
                probe->interpreter_uid : probe->main_uid,
        .gid = strcmp(path, RESOLVED_INTERPRETER) == 0 ?
                probe->interpreter_gid : probe->main_gid,
    };
    return 0;
}

static int probe_fstat(struct fd *fd, struct statbuf *stat) {
    struct file_state *state = fd->data;
    *stat = (struct statbuf) {
        .inode = state->interpreter ? 2 : 1,
        .mode = state->directory ? S_IFDIR | 0711 :
                (state->interpreter ? state->probe->interpreter_mode :
                (state->owned ? state->probe->main_mode :
                S_IFREG | 0555)),
        .uid = state->interpreter ? state->probe->interpreter_uid :
                (state->owned ? state->probe->main_uid : 1000),
        .gid = state->interpreter ? state->probe->interpreter_gid :
                (state->owned ? state->probe->main_gid : 100),
        .size = state->stat_size,
    };
    return 0;
}

static int probe_getpath(struct fd *fd, char *buffer) {
    struct file_state *state = fd->data;
    strcpy(buffer, state->path);
    return 0;
}

static const struct fs_ops probe_fs = {
    .open = probe_open,
    .stat = probe_stat,
    .fstat = probe_fstat,
    .getpath = probe_getpath,
};

static struct fd *make_fd(struct mount *mount,
        struct file_state *state, const struct fd_ops *ops) {
    struct fd *fd = fd_create(ops);
    if (fd == NULL)
        return NULL;
    fd->data = state;
    fd->type = state->directory ? S_IFDIR : S_IFREG;
    fd->mount = mount;
    mount_retain(mount);
    return fd;
}

static bool init_fixture(struct task_fixture *fixture,
        struct fs_probe *probe) {
    memset(fixture, 0, sizeof(*fixture));
    lock(&mounts_lock);
    int error = do_mount(&probe_fs, "", "", "", 0);
    unlock(&mounts_lock);
    if (error < 0)
        return false;
    char root_path[] = "/";
    fixture->mount = mount_find(root_path);
    fixture->mount->data = probe;

    fixture->root_state = (struct file_state) {
        .probe = probe,
        .path = TARGET_ROOT,
        .directory = true,
    };
    fixture->pwd_state = (struct file_state) {
        .probe = probe,
        .path = TARGET_PWD,
        .directory = true,
    };
    fixture->root = make_fd(fixture->mount,
            &fixture->root_state, &directory_ops);
    fixture->pwd = make_fd(fixture->mount,
            &fixture->pwd_state, &directory_ops);
    if (fixture->root == NULL || fixture->pwd == NULL)
        return false;
    lock_init(&fixture->fs.lock);
    fixture->fs.root = fixture->root;
    fixture->fs.pwd = fixture->pwd;
    fixture->task.fs = &fixture->fs;
    fixture->task.pid = fixture->task.tgid = 4321;
    fixture->task.uid = fixture->task.euid = 1000;
    fixture->task.gid = fixture->task.egid = 100;
    fixture->task.group = &fixture->group;
    fixture->task.sighand = &fixture->sighand;
    list_init(&fixture->task.group_links);
    list_init(&fixture->task.children);
    list_init(&fixture->task.siblings);
    list_init(&fixture->task.queue);
    lock_init(&fixture->task.general_lock);
    lock_init(&fixture->task.ptrace.lock);
    cond_init(&fixture->task.ptrace.cond);
    task_altstack_reset(&fixture->task);
    list_init(&fixture->group.threads);
    list_init(&fixture->group.session);
    list_init(&fixture->group.pgroup);
    lock_init(&fixture->group.lock);
    cond_init(&fixture->group.stopped_cond);
    fixture->group.leader = &fixture->task;
    list_add(&fixture->group.threads, &fixture->task.group_links);
    atomic_init(&fixture->sighand.refcount, 1);
    lock_init(&fixture->sighand.lock);
    struct mm *mm = mm_new();
    if (mm == NULL)
        return false;
    task_set_mm(&fixture->task, mm);
    fixture->task.files = fdtable_new(3);
    if (IS_ERR(fixture->task.files)) {
        mm_release(mm);
        fixture->task.mm = NULL;
        fixture->task.mem = NULL;
        return false;
    }
    current = &fixture->task;
    return true;
}

static int destroy_fixture(struct task_fixture *fixture) {
    task_discard_aarch64_exec(&fixture->task);
    task_release_aarch64_process(&fixture->task);
    mm_release(fixture->task.mm);
    fixture->task.mm = NULL;
    fixture->task.mem = NULL;
    fdtable_release(fixture->task.files);
    fixture->task.files = NULL;
    current = NULL;
    fd_close(fixture->root);
    fd_close(fixture->pwd);
    mount_release(fixture->mount);
    lock(&mounts_lock);
    int error = mount_remove(fixture->mount);
    unlock(&mounts_lock);
    return error;
}

static struct fd *make_main_fd(struct task_fixture *fixture,
        struct fs_probe *probe, struct file_state *state,
        const byte_t *data, size_t size, qword_t stat_size) {
    *state = (struct file_state) {
        .probe = probe,
        .path = TARGET_ROOT "/bin/test",
        .data = data,
        .size = size,
        .stat_size = stat_size,
        .read_limit = 13,
    };
    return make_fd(fixture->mount, state, &stream_ops);
}

static bool images_are_empty(
        const struct ish_aarch64_exec_images *images) {
    return images->main.data == NULL && images->main.size == 0 &&
            images->interpreter.data == NULL &&
            images->interpreter.size == 0;
}

static bool runs_to_exit(
        struct aarch64_linux_process *process, dword_t status) {
    for (unsigned index = 0; index < 2; index++) {
        if (aarch64_linux_process_run_one(process).status !=
                AARCH64_LINUX_PROCESS_RUNNABLE)
            return false;
    }
    struct aarch64_linux_process_result result =
            aarch64_linux_process_run_one(process);
    return result.status == AARCH64_LINUX_PROCESS_EXIT &&
            result.exit_status == status;
}

int main(void) {
    byte_t main_file[TEST_FILE_SIZE];
    byte_t interpreter_file[TEST_FILE_SIZE];
    make_main_image(main_file, true);
    make_main_image(interpreter_file, false);
    byte_t expected_main[TEST_FILE_SIZE];
    byte_t expected_interpreter[TEST_FILE_SIZE];
    memcpy(expected_main, main_file, sizeof(expected_main));
    memcpy(expected_interpreter,
            interpreter_file, sizeof(expected_interpreter));
    struct fs_probe probe = {
        .interpreter_data = interpreter_file,
        .interpreter_size = sizeof(interpreter_file),
        .interpreter_stat_size = sizeof(interpreter_file),
        .interpreter_mode = S_IFREG | 0555,
        .interpreter_uid = 1000,
        .interpreter_gid = 100,
        .read_limit = 11,
    };
    struct task_fixture fixture;
    CHECK(init_fixture(&fixture, &probe), "测试 guest 文件系统初始化成功");
    struct file_state main_state;
    struct fd *main_fd = make_main_fd(&fixture, &probe, &main_state,
            main_file, sizeof(main_file), sizeof(main_file));
    CHECK(main_fd != NULL, "动态主程序 fd 创建成功");
    main_fd->offset = 23;
    struct ish_aarch64_exec_images images;
    CHECK(ish_aarch64_exec_images_read(
            &fixture.task, main_fd, &images) == 0,
            "动态主程序和解释器建立完整快照");
    CHECK(main_fd->offset == 23 && probe.main_reads > 1 &&
            probe.interpreter_reads > 1,
            "顺序短读恢复主 fd offset 且 pread 短读循环到末尾");
    CHECK(strcmp(probe.opened_path, RESOLVED_INTERPRETER) == 0,
            "绝对 PT_INTERP 路径受目标 task root 约束");
    CHECK(probe.opens == 1 && probe.closes == 1,
            "解释器 fd 在成功快照后立即关闭");
    memset(main_file, 0xa5, sizeof(main_file));
    memset(interpreter_file, 0x5a, sizeof(interpreter_file));
    CHECK(images.main.size == sizeof(expected_main) &&
            memcmp(images.main.data, expected_main,
                    sizeof(expected_main)) == 0 &&
            images.interpreter.size == sizeof(expected_interpreter) &&
            memcmp(images.interpreter.data, expected_interpreter,
                    sizeof(expected_interpreter)) == 0,
            "快照不借用主程序或解释器源缓冲区");
    ish_aarch64_exec_images_destroy(&images);
    CHECK(images_are_empty(&images), "销毁后清零两份快照描述符");
    fd_close(main_fd);
    make_main_image(main_file, false);
    unsigned opens_before = probe.opens;
    main_fd = make_main_fd(&fixture, &probe, &main_state,
            main_file, sizeof(main_file), sizeof(main_file));
    CHECK(main_fd != NULL && ish_aarch64_exec_images_read(
            &fixture.task, main_fd, &images) == 0,
            "静态主程序只建立主映像快照");
    CHECK(images.interpreter.data == NULL &&
            images.interpreter.size == 0 && probe.opens == opens_before,
            "无 PT_INTERP 时不打开 guest 文件");
    ish_aarch64_exec_images_destroy(&images);
    fd_close(main_fd);
    make_main_image(main_file, true);
    main_file[0] = 0;
    main_fd = make_main_fd(&fixture, &probe, &main_state,
            main_file, sizeof(main_file), sizeof(main_file));
    CHECK(main_fd != NULL && ish_aarch64_exec_images_read(
            &fixture.task, main_fd, &images) == _ENOEXEC &&
            images_are_empty(&images),
            "损坏主 ELF 返回 ENOEXEC 并回滚主快照");
    fd_close(main_fd);
    make_main_image(main_file, true);
    main_fd = make_main_fd(&fixture, &probe, &main_state,
            main_file, sizeof(main_file), sizeof(main_file) + 1);
    CHECK(main_fd != NULL, "提前 EOF 主程序 fd 创建成功");
    main_fd->offset = 29;
    CHECK(ish_aarch64_exec_images_read(
            &fixture.task, main_fd, &images) == _EIO &&
            images_are_empty(&images) && main_fd->offset == 29,
            "主程序读取失败仍恢复原 offset 并回滚快照");
    fd_close(main_fd);
    probe.open_error = _ENOENT;
    main_fd = make_main_fd(&fixture, &probe, &main_state,
            main_file, sizeof(main_file), sizeof(main_file));
    CHECK(main_fd != NULL && ish_aarch64_exec_images_read(
            &fixture.task, main_fd, &images) == _ENOENT &&
            images_are_empty(&images),
            "缺失解释器保留 guest 文件系统 ENOENT 并回滚");
    fd_close(main_fd);
    probe.open_error = 0;
    probe.interpreter_mode = S_IFREG | 0444;
    fixture.task.euid = 0;
    main_fd = make_main_fd(&fixture, &probe, &main_state,
            main_file, sizeof(main_file), sizeof(main_file));
    CHECK(main_fd != NULL && ish_aarch64_exec_images_read(
            &fixture.task, main_fd, &images) == _EACCES &&
            images_are_empty(&images),
            "root 也不能运行完全没有执行位的解释器");
    fd_close(main_fd);
    fixture.task.euid = 1000;
    probe.interpreter_mode = S_IFREG | 0555;

    probe.interpreter_mode = S_IFREG | 0411;
    main_fd = make_main_fd(&fixture, &probe, &main_state,
            main_file, sizeof(main_file), sizeof(main_file));
    CHECK(main_fd != NULL && ish_aarch64_exec_images_read(
            &fixture.task, main_fd, &images) == _EACCES &&
            images_are_empty(&images),
            "文件其他权限位不可替代所有者执行权限");
    fd_close(main_fd);
    probe.interpreter_mode = S_IFREG | 0555;

    probe.interpreter_stat_size = sizeof(interpreter_file) + 1;
    main_fd = make_main_fd(&fixture, &probe, &main_state,
            main_file, sizeof(main_file), sizeof(main_file));
    CHECK(main_fd != NULL && ish_aarch64_exec_images_read(
            &fixture.task, main_fd, &images) == _EIO &&
            images_are_empty(&images),
            "解释器提前 EOF 返回 EIO 并回滚两份快照");
    fd_close(main_fd);
    probe.interpreter_stat_size = sizeof(interpreter_file);

    probe.interpreter_read_error = _EIO;
    main_fd = make_main_fd(&fixture, &probe, &main_state,
            main_file, sizeof(main_file), sizeof(main_file));
    CHECK(main_fd != NULL && ish_aarch64_exec_images_read(
            &fixture.task, main_fd, &images) == _EIO &&
            images_are_empty(&images),
            "解释器读取错误关闭 fd 并回滚两份快照");
    fd_close(main_fd);
    probe.interpreter_read_error = 0;

    CHECK(probe.closes + 1 == probe.opens,
            "只有未成功打开的 ENOENT 不产生待关闭解释器 fd");
    struct mm *original_mm = fixture.task.mm;
    static const char arguments[] = "exec-stage\0one\0";
    static const char environment[] = "A=B\0";
    const struct ish_aarch64_exec_identity identity = {
        .uid = 1000,
        .euid = 2000,
        .gid = 100,
        .egid = 200,
        .secure = 1,
    };
    byte_t dynamic_main[TEST_FILE_SIZE];
    byte_t dynamic_interpreter[TEST_FILE_SIZE];
    make_dynamic_main_image(dynamic_main);
    put_u32(dynamic_main + TEST_SEGMENT_OFFSET, movz_x(0, 17));
    make_runnable_image(dynamic_interpreter);
    put_u16(dynamic_interpreter + 16, 3);
    probe.interpreter_data = dynamic_interpreter;
    probe.interpreter_size = sizeof(dynamic_interpreter);
    probe.interpreter_stat_size = sizeof(dynamic_interpreter);
    main_fd = make_main_fd(&fixture, &probe, &main_state,
            dynamic_main, sizeof(dynamic_main), sizeof(dynamic_main));
    struct task sibling = {0};
    list_init(&sibling.group_links);
    lock(&pids_lock);
    list_add(&fixture.group.threads, &sibling.group_links);
    unlock(&pids_lock);
    main_state.data = interpreter_file;
    CHECK(ish_aarch64_exec_stage(&fixture.task, main_fd,
            "/bin/not-aarch64", 2, arguments, 1, environment,
            &identity) == _ENOEXEC,
            "多线程中的非 AArch64 格式仍交还后续 shebang 分派");
    main_state.data = dynamic_main;
    CHECK(main_fd != NULL && ish_aarch64_exec_stage(
            &fixture.task, main_fd, "/bin/dynamic-stage",
            2, arguments, 1, environment, &identity) == _EBUSY &&
            !task_has_aarch64_exec_candidate(&fixture.task),
            "多线程任务在 de-thread 实现前拒绝切换执行架构");
    lock(&pids_lock);
    list_remove(&sibling.group_links);
    unlock(&pids_lock);
    fixture.group.leader = &sibling;
    CHECK(ish_aarch64_exec_stage(&fixture.task, main_fd,
            "/bin/dynamic-stage", 2, arguments, 1, environment,
            &identity) == _EBUSY,
            "最后存活的非 leader 仍需完成 de-thread 后才能切换架构");
    fixture.group.leader = &fixture.task;
    CHECK(main_fd != NULL && ish_aarch64_exec_stage(
            &fixture.task, main_fd, "/bin/dynamic-stage",
            2, arguments, 1, environment, &identity) == 0 &&
            task_has_aarch64_exec_candidate(&fixture.task),
            "动态主映像经 guest PT_INTERP 完整建立候选");
    CHECK(runs_to_exit(fixture.task.aarch64_exec_candidate, 42),
            "动态解释器从真实入口运行到唯一退出码");
    task_discard_aarch64_exec(&fixture.task);
    CHECK(fixture.task.mm == original_mm &&
            !task_has_aarch64_exec_candidate(&fixture.task),
            "丢弃动态候选不影响活动 i386 映像");
    static const char boundary_executable[] = "/bin/boundary";
    size_t boundary_size = ISH_AARCH64_EXEC_ARG_MAX -
            3 * sizeof(qword_t) - sizeof(boundary_executable);
    char *boundary = malloc(boundary_size + 2);
    CHECK(boundary != NULL, "ARG_MAX 边界参数缓冲区分配成功");
    memset(boundary, 'x', boundary_size);
    boundary[boundary_size - 1] = boundary[boundary_size] = '\0';
    CHECK(ish_aarch64_exec_stage(&fixture.task, main_fd,
            boundary_executable, 1, boundary, 0, "\0", &identity) == 0,
            "AArch64 argv 总占用恰好等于 ARG_MAX 时成功");
    task_discard_aarch64_exec(&fixture.task);
    boundary[boundary_size - 1] = 'x';
    boundary[boundary_size] = boundary[boundary_size + 1] = '\0';
    CHECK(ish_aarch64_exec_stage(&fixture.task, main_fd,
            boundary_executable, 1, boundary, 0, "\0", &identity) == _E2BIG,
            "AArch64 argv 总占用超过 ARG_MAX 一字节时失败");
    free(boundary);
    fd_close(main_fd);
    make_main_image(main_file, false);
    main_file[0] = 0;
    main_fd = make_main_fd(&fixture, &probe, &main_state,
            main_file, sizeof(main_file), sizeof(main_file));
    CHECK(main_fd != NULL && ish_aarch64_exec_stage(
            &fixture.task, main_fd, "/bin/exec-stage",
            2, arguments, 1, environment, &identity) == _ENOEXEC &&
            fixture.task.mm == original_mm &&
            !task_has_aarch64_process(&fixture.task) &&
            !task_has_aarch64_exec_candidate(&fixture.task),
            "坏 ELF 回滚候选并保留活动 i386 地址空间");
    fd_close(main_fd);
    make_runnable_image(main_file);
    probe.main_data = main_file;
    probe.main_size = sizeof(main_file);
    probe.main_stat_size = sizeof(main_file);
    probe.main_mode = S_IFREG | 0555;
    probe.main_uid = 1000;
    probe.main_gid = 100;
    const addr_t user_page = UINT32_C(0x1000);
    write_wrlock(&fixture.task.mem->lock);
    int map_error = pt_map_nothing(
            fixture.task.mem, PAGE(user_page), 1, P_RWX);
    write_wrunlock(&fixture.task.mem->lock);
    CHECK(map_error == 0, "为 i386 exec 参数映射用户页");
    const addr_t filename_address = user_page;
    const addr_t argument0_address = user_page + 64;
    const addr_t argument1_address = user_page + 80;
    const addr_t environment0_address = user_page + 96;
    const addr_t argument_table_address = user_page + 128;
    const addr_t environment_table_address = user_page + 144;
    const addr_t argument_table[] = {
        argument0_address, argument1_address, 0,
    };
    const addr_t environment_table[] = {
        environment0_address, 0,
    };
    CHECK(user_write_string(filename_address, "/bin/test") == 0 &&
            user_write_string(argument0_address, "exec-stage") == 0 &&
            user_write_string(argument1_address, "one") == 0 &&
            user_write_string(environment0_address, "A=B") == 0 &&
            user_write(argument_table_address, argument_table,
                    sizeof(argument_table)) == 0 &&
            user_write(environment_table_address, environment_table,
                    sizeof(environment_table)) == 0,
            "写入 i386 execve 路径、argv 与 envp");
    fixture.task.cpu.eax = 11;
    fixture.task.cpu.ebx = filename_address;
    fixture.task.cpu.ecx = argument_table_address;
    fixture.task.cpu.edx = environment_table_address;
    struct vfork_info vfork = {0};
    lock_init(&vfork.lock);
    cond_init(&vfork.cond);
    fixture.task.vfork = &vfork;
    handle_interrupt(INT_SYSCALL);
    CHECK(fixture.task.cpu.eax == 11 &&
            fixture.task.mm == original_mm &&
            !task_has_aarch64_process(&fixture.task) &&
            task_has_aarch64_exec_candidate(&fixture.task) &&
            fixture.task.vfork == &vfork && !vfork.done,
            "i386 execve 成功后不向旧 CPU 写回并返回架构安全点");
    struct aarch64_linux_process *first_process =
            fixture.task.aarch64_exec_candidate;
    struct mm *first_mm = fixture.task.aarch64_exec_mm;
    task_commit_aarch64_exec(&fixture.task);
    CHECK(fixture.task.aarch64_process == first_process &&
            !task_has_aarch64_exec_candidate(&fixture.task) &&
            fixture.task.mm == first_mm &&
            fixture.task.mm->exefile != NULL &&
            fixture.task.vfork == NULL && vfork.done,
            "安全点提交 AArch64 process 与元数据 mm 并退休旧映像");
    cond_destroy(&vfork.cond);
    pthread_mutex_destroy(&vfork.lock.m);
    CHECK(runs_to_exit(fixture.task.aarch64_process, 42),
            "提交后的新映像从真实入口运行到唯一退出码");
    byte_t caller_file[TEST_FILE_SIZE];
    make_exec_caller_image(caller_file);
    struct file_state failed_caller_state;
    main_fd = make_main_fd(&fixture, &probe, &failed_caller_state,
            caller_file, sizeof(caller_file), sizeof(caller_file));
    const struct ish_aarch64_exec_identity caller_identity = {
        .uid = fixture.task.uid,
        .euid = fixture.task.euid,
        .gid = fixture.task.gid,
        .egid = fixture.task.egid,
    };
    CHECK(main_fd != NULL && ish_aarch64_exec_stage(
            &fixture.task, main_fd, "/bin/exec-caller",
            2, arguments, 1, environment, &caller_identity) == 0,
            "建立 AArch64 execve 失败路径调用映像");
    fd_close(main_fd);
    struct aarch64_linux_process *failed_caller =
            fixture.task.aarch64_exec_candidate;
    struct mm *failed_caller_mm = fixture.task.aarch64_exec_mm;
    task_commit_aarch64_exec(&fixture.task);
    CHECK(fixture.task.aarch64_process == failed_caller &&
            fixture.task.mm == failed_caller_mm,
            "提交 AArch64 execve 失败路径调用映像");
    byte_t bad_exec[TEST_FILE_SIZE];
    memcpy(bad_exec, main_file, sizeof(bad_exec));
    bad_exec[0] = 0;
    probe.main_data = bad_exec;
    probe.main_size = sizeof(bad_exec);
    probe.main_stat_size = sizeof(bad_exec);
    probe.main_mode = S_IFREG | S_ISUID | 0555;
    probe.main_uid = 2000;
    probe.main_gid = 200;
    strcpy(fixture.task.comm, "before");
    fixture.task.did_exec = false;
    struct aarch64_task_event exec_event;
    for (unsigned index = 0; index < 7; index++) {
        exec_event = aarch64_task_run_one(&fixture.task);
        CHECK(exec_event.action == AARCH64_TASK_EVENT_CONTINUE,
                "AArch64 execve 调用点前的寄存器准备继续运行");
    }
    exec_event = aarch64_task_run_one(&fixture.task);
    CHECK(exec_event.action == AARCH64_TASK_EVENT_CONTINUE &&
            fixture.task.aarch64_process == failed_caller &&
            fixture.task.mm == failed_caller_mm &&
            !task_has_aarch64_exec_candidate(&fixture.task) &&
            fixture.task.euid == 1000 &&
            strcmp(fixture.task.comm, "before") == 0 &&
            !fixture.task.did_exec,
            "AArch64 svc execve 失败写回旧映像并保留任务元数据");
    exec_event = aarch64_task_run_one(&fixture.task);
    CHECK(exec_event.action == AARCH64_TASK_EVENT_CONTINUE,
            "失败后旧映像继续设置 exit 系统调用号");
    exec_event = aarch64_task_run_one(&fixture.task);
    dword_t failed_status =
            ((dword_t) (qword_t) (sqword_t) _ENOEXEC & UINT32_C(0xff)) << 8;
    CHECK(exec_event.action == AARCH64_TASK_EVENT_EXIT &&
            exec_event.status == failed_status,
            "失败 errno 已写回旧 x0 并成为后续唯一退出码");
    byte_t i386_exec[TEST_FILE_SIZE] = {0};
    struct elf_header i386_header = {
        .bitness = ELF_32BIT,
        .endian = ELF_LITTLEENDIAN,
        .elfversion1 = 1,
        .type = ELF_EXECUTABLE,
        .machine = ELF_X86,
        .elfversion2 = 1,
        .header_size = sizeof(struct elf_header),
        .phent_size = sizeof(struct prg_header),
    };
    memcpy(&i386_header.magic, ELF_MAGIC, sizeof(i386_header.magic));
    memcpy(i386_exec, &i386_header, sizeof(i386_header));
    probe.main_data = i386_exec;
    probe.main_size = sizeof(i386_exec);
    probe.main_stat_size = sizeof(i386_exec);
    CHECK(do_execve("/bin/test", 2, arguments, environment) == _ENOEXEC &&
            fixture.task.aarch64_process == failed_caller &&
            fixture.task.mm == failed_caller_mm &&
            !task_has_aarch64_exec_candidate(&fixture.task),
            "尚未支持的 AArch64 到 i386 exec 明确拒绝且不触碰旧映像");
    for (size_t offset = 8; offset < 24; offset += 4)
        put_u32(caller_file + TEST_SEGMENT_OFFSET + offset,
                movz_x(offset < 16 ? 1 : 2, 0));
    struct file_state successful_caller_state;
    main_fd = make_main_fd(&fixture, &probe, &successful_caller_state,
            caller_file, sizeof(caller_file), sizeof(caller_file));
    CHECK(main_fd != NULL && ish_aarch64_exec_stage(
            &fixture.task, main_fd, "/bin/exec-caller",
            2, arguments, 1, environment, &caller_identity) == 0,
            "建立 AArch64 execve 成功路径调用映像");
    fd_close(main_fd);
    struct aarch64_linux_process *successful_caller =
            fixture.task.aarch64_exec_candidate;
    struct mm *successful_caller_mm = fixture.task.aarch64_exec_mm;
    task_commit_aarch64_exec(&fixture.task);
    CHECK(fixture.task.aarch64_process == successful_caller &&
            fixture.task.mm == successful_caller_mm,
            "提交 AArch64 execve 成功路径调用映像");

    probe.main_data = main_file;
    probe.main_size = sizeof(main_file);
    probe.main_stat_size = sizeof(main_file);
    probe.main_mode = S_IFREG | S_ISUID | S_ISGID | 0501;
    probe.main_uid = 2000;
    probe.main_gid = 200;
    aarch64_task_run_current();
    CHECK(task_has_aarch64_process(&fixture.task) &&
            !task_has_aarch64_exec_candidate(&fixture.task) &&
            fixture.task.mm->exefile != NULL &&
            fixture.task.euid == 2000 && fixture.task.suid == 2000 &&
            fixture.task.egid == 100 && fixture.task.sgid == 100 &&
            strcmp(fixture.task.comm, "test") == 0 &&
            fixture.task.did_exec,
            "NULL argv/envp 的真实 svc exec 后自动提交 process、mm 与副作用");

    CHECK(runs_to_exit(fixture.task.aarch64_process, 42),
            "AArch64 到 AArch64 提交后仅运行新映像");

    byte_t script_file[TEST_FILE_SIZE] = {0};
    strcpy((char *) script_file, "#!" INTERPRETER_PATH "\n");
    make_runnable_image(interpreter_file);
    probe.main_data = script_file;
    probe.main_size = sizeof(script_file);
    probe.main_stat_size = sizeof(script_file);
    probe.main_mode = S_IFREG | S_ISUID | 0555;
    probe.main_uid = 2100;
    probe.interpreter_data = interpreter_file;
    probe.interpreter_size = sizeof(interpreter_file);
    probe.interpreter_stat_size = sizeof(interpreter_file);
    probe.interpreter_mode = S_IFREG | S_ISUID | S_ISGID | 0111;
    probe.interpreter_uid = 3000;
    probe.interpreter_gid = 300;
    size_t large_argument_size = ISH_AARCH64_EXEC_ARG_MAX - 70;
    char *large_arguments = malloc(large_argument_size + 2);
    CHECK(large_arguments != NULL, "临界 shebang 参数缓冲区分配成功");
    memset(large_arguments + 1, 'x', large_argument_size - 1);
    large_arguments[0] = large_arguments[large_argument_size] =
            large_arguments[large_argument_size + 1] = '\0';
    CHECK(do_execve("/bin/test", 2, large_arguments, environment) == _E2BIG &&
            !task_has_aarch64_exec_candidate(&fixture.task),
            "shebang 新增字符串与指针仍受最终 AArch64 ARG_MAX 约束");
    free(large_arguments);
    static const char empty_arguments[] = "\0";
    int script_error = do_execve(
            "/bin/test", 0, empty_arguments, environment);
    CHECK(script_error == 0, "空 argv 的 shebang 建立候选");
    CHECK(task_has_aarch64_exec_candidate(&fixture.task) &&
            fixture.task.euid == 3000 && fixture.task.egid == 300 &&
            fixture.task.suid == 3000 && fixture.task.sgid == 300,
            "空 argv 的 shebang 使用解释器而非脚本的 set-id 身份");
    struct aarch64_linux_process *script_process =
            fixture.task.aarch64_exec_candidate;
    task_commit_aarch64_exec(&fixture.task);
    CHECK(fixture.task.aarch64_process == script_process,
            "shebang 候选在安全点提交");
    CHECK(runs_to_exit(script_process, 42),
            "shebang 解释器从真实入口运行到唯一退出码");

    CHECK(destroy_fixture(&fixture) == 0 && list_empty(&mounts),
            "测试 guest 文件系统无引用泄漏并成功卸载");
    return 0;
}
