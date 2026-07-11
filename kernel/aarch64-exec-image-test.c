#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "fs/fd.h"
#include "fs/path.h"
#include "guest/aarch64/elf64.h"
#include "kernel/aarch64-exec-image.h"
#include "kernel/errno.h"
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
#define TARGET_ROOT "/target-root"
#define TARGET_PWD TARGET_ROOT "/work"
#define INTERPRETER_PATH "/lib/ld-musl-aarch64.so.1"
#define RESOLVED_INTERPRETER TARGET_ROOT INTERPRETER_PATH

struct fs_probe {
    const byte_t *interpreter_data;
    size_t interpreter_size;
    qword_t interpreter_stat_size;
    mode_t_ interpreter_mode;
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
    if (strcmp(path, RESOLVED_INTERPRETER) != 0)
        return ERR_PTR(_ENOENT);

    struct fd *fd = fd_create(&positioned_ops);
    if (fd == NULL)
        return ERR_PTR(_ENOMEM);
    struct file_state *state = malloc(sizeof(*state));
    if (state == NULL) {
        fd_close(fd);
        return ERR_PTR(_ENOMEM);
    }
    *state = (struct file_state) {
        .probe = probe,
        .path = RESOLVED_INTERPRETER,
        .data = probe->interpreter_data,
        .size = probe->interpreter_size,
        .stat_size = probe->interpreter_stat_size,
        .read_limit = probe->read_limit,
        .read_error = probe->interpreter_read_error,
        .interpreter = true,
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
                probe->interpreter_mode : S_IFDIR | 0711,
        .uid = 1000,
        .gid = 100,
    };
    return 0;
}

static int probe_fstat(struct fd *fd, struct statbuf *stat) {
    struct file_state *state = fd->data;
    *stat = (struct statbuf) {
        .inode = state->interpreter ? 2 : 1,
        .mode = state->directory ? S_IFDIR | 0711 :
                (state->interpreter ? state->probe->interpreter_mode :
                S_IFREG | 0555),
        .uid = 1000,
        .gid = 100,
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
    fixture->task.euid = 1000;
    fixture->task.egid = 100;
    current = &fixture->task;
    return true;
}

static int destroy_fixture(struct task_fixture *fixture) {
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
    main_fd = make_main_fd(&fixture, &probe, &main_state,
            main_file, sizeof(main_file), sizeof(main_file));
    CHECK(main_fd != NULL && ish_aarch64_exec_images_read(
            &fixture.task, main_fd, &images) == _EACCES &&
            images_are_empty(&images),
            "可读但不可执行的解释器返回 EACCES");
    fd_close(main_fd);
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
    CHECK(destroy_fixture(&fixture) == 0 && list_empty(&mounts),
            "测试 guest 文件系统无引用泄漏并成功卸载");
    return 0;
}
