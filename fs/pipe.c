#include <sys/stat.h>
#include <unistd.h>
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/task.h"
#include "fs/fd.h"
#include "fs/pipe.h"
#include "fs/real.h"
#include "debug.h"

static struct fd *pipe_f_create(struct task *task, int pipe_fd) {
    struct fd *fd = adhoc_fd_create(&realfs_fdops);
    if (fd == NULL)
        return NULL;
    fd->real_fd = pipe_fd;
    fd->type = S_IFIFO;
    fd->stat.mode = S_IFIFO | 0660;
    fd->stat.uid = task->uid;
    fd->stat.gid = task->gid;
    return fd;
}

int file_pipe2_task(
        struct task *task, int flags, struct file_pipe_result *result) {
    *result = (struct file_pipe_result) {};
    if (flags & ~(O_CLOEXEC_|O_NONBLOCK_)) {
        return _EINVAL;
    }

    int p[2];
    if (pipe(p) < 0)
        return errno_map();

    struct fd *read_end = pipe_f_create(task, p[0]);
    if (read_end == NULL) {
        close(p[0]);
        close(p[1]);
        return _ENOMEM;
    }
    struct fd *write_end = pipe_f_create(task, p[1]);
    if (write_end == NULL) {
        fd_close(read_end);
        close(p[1]);
        return _ENOMEM;
    }

    if (flags & O_NONBLOCK_) {
        int error = fd_setflags(read_end, O_NONBLOCK_);
        if (error < 0) {
            fd_close(write_end);
            fd_close(read_end);
            return error;
        }
        error = fd_setflags(write_end, O_NONBLOCK_);
        if (error < 0) {
            fd_close(write_end);
            fd_close(read_end);
            return error;
        }
    }

    fd_retain(read_end);
    fd_t read_fd = f_install_task_tracked(task, read_end,
            flags & O_CLOEXEC_, &result->generations[0]);
    if (read_fd < 0) {
        fd_close(read_end);
        fd_close(write_end);
        return read_fd;
    }
    fd_retain(write_end);
    fd_t write_fd = f_install_task_tracked(task, write_end,
            flags & O_CLOEXEC_, &result->generations[1]);
    if (write_fd < 0) {
        fd_close(write_end);
        f_close_task_if_matches(task, read_fd, read_end,
                result->generations[0]);
        fd_close(read_end);
        return write_fd;
    }

    result->fds[0] = read_fd;
    result->fds[1] = write_fd;
    result->ends[0] = read_end;
    result->ends[1] = write_end;
    return 0;
}

void file_pipe_result_release(struct file_pipe_result *result) {
    fd_close(result->ends[1]);
    fd_close(result->ends[0]);
    result->ends[0] = NULL;
    result->ends[1] = NULL;
}

void file_pipe_result_rollback(
        struct task *task, struct file_pipe_result *result) {
    f_close_task_if_matches(task, result->fds[1], result->ends[1],
            result->generations[1]);
    f_close_task_if_matches(task, result->fds[0], result->ends[0],
            result->generations[0]);
    file_pipe_result_release(result);
}

int_t sys_pipe2(addr_t pipe_addr, int_t flags) {
    STRACE("pipe2(%#x, %#x)", pipe_addr, flags);
    struct file_pipe_result pipe_result;
    int error = file_pipe2_task(current, flags, &pipe_result);
    if (error < 0)
        return error;
    if (user_put(pipe_addr, pipe_result.fds)) {
        file_pipe_result_rollback(current, &pipe_result);
        return _EFAULT;
    }
    STRACE(" [%d %d]", pipe_result.fds[0], pipe_result.fds[1]);
    file_pipe_result_release(&pipe_result);
    return 0;
}

int_t sys_pipe(addr_t pipe_addr) {
    return sys_pipe2(pipe_addr, 0);
}
