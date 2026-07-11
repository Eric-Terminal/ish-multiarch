#include <signal.h>
#include <string.h>
#include <sys/stat.h>
#include "fs/devices.h"
#include "fs/fd.h"
#include "fs/path.h"
#include "fs/real.h"
#include "fs/tty.h"
#include "kernel/calls.h"
#include "kernel/fs.h"
#include "kernel/init.h"
#include "kernel/personality.h"

int mount_root(const struct fs_ops *fs, const char *source) {
    char source_realpath[MAX_PATH + 1];
    if (realpath(source, source_realpath) == NULL)
        return errno_map();
    int err = do_mount(fs, source_realpath, "", "", 0);
    if (err < 0)
        return err;
    return 0;
}

static void establish_signal_handlers(void) {
    extern void sigusr1_handler(int sig);
    struct sigaction sigact;
    sigact.sa_handler = sigusr1_handler;
    sigact.sa_flags = 0;
    sigemptyset(&sigact.sa_mask);
    sigaddset(&sigact.sa_mask, SIGUSR1);
    sigaction(SIGUSR1, &sigact, NULL);
    signal(SIGPIPE, SIG_IGN);
}

// copied from include/asm-generic/resource.h in the kernel
static struct rlimit_ init_rlimits[16] = {
    [RLIMIT_CPU_]        = {RLIM_INFINITY_, RLIM_INFINITY_},
    [RLIMIT_FSIZE_]      = {RLIM_INFINITY_, RLIM_INFINITY_},
    [RLIMIT_DATA_]       = {RLIM_INFINITY_, RLIM_INFINITY_},
    [RLIMIT_STACK_]      = {8*1024*1024, RLIM_INFINITY_},
    [RLIMIT_CORE_]       = {0, RLIM_INFINITY_},
    [RLIMIT_RSS_]        = {RLIM_INFINITY_, RLIM_INFINITY_},
    [RLIMIT_NPROC_]      = {1024, 1024},
    [RLIMIT_NOFILE_]     = {1024, 4096},
    [RLIMIT_MEMLOCK_]    = {64*1024, 64*1024},
    [RLIMIT_AS_]         = {RLIM_INFINITY_, RLIM_INFINITY_},
    [RLIMIT_LOCKS_]      = {RLIM_INFINITY_, RLIM_INFINITY_},
    [RLIMIT_SIGPENDING_] = {1024, 1024},
    [RLIMIT_MSGQUEUE_]   = {819200, 819200},
    [RLIMIT_NICE_]       = {0, 0},
    [RLIMIT_RTPRIO_]     = {0, 0},
    [RLIMIT_RTTIME_]     = {RLIM_INFINITY_, RLIM_INFINITY_},
};

static struct task *construct_task(struct task *parent) {
    struct task *task = task_create_(parent);
    if (task == NULL)
        return ERR_PTR(_ENOMEM);
    task_thread_store(task, pthread_self());
    task->blocked = 0;

    struct tgroup *group = malloc(sizeof(struct tgroup));
    if (group == NULL) {
        task_abort_create(task);
        return ERR_PTR(_ENOMEM);
    }
    *group = (struct tgroup) {};
    list_init(&group->threads);
    list_init(&group->session);
    list_init(&group->pgroup);
    lock_init(&group->lock);
    cond_init(&group->child_exit);
    cond_init(&group->stopped_cond);
    memcpy(group->limits, init_rlimits, sizeof(init_rlimits));
    group->leader = task;
    group->personality = ADDR_NO_RANDOMIZE_;
    group->sid = task->pid;
    group->pgid = task->pid;
    task->group = group;
    task->tgid = task->pid;

    int err = _ENOMEM;
    struct mm *mm = mm_new();
    if (mm == NULL)
        goto fail_group;
    task_set_mm(task, mm);
    task->sighand = sighand_new();
    if (task->sighand == NULL)
        goto fail_mm;
    task->files = fdtable_new(3); // 为 stdin、stdout、stderr 预留槽位。
    if (IS_ERR(task->files)) {
        err = (int) PTR_ERR(task->files);
        goto fail_sighand;
    }

    task->fs = fs_info_new();
    if (task->fs == NULL)
        goto fail_files;
    task->fs->umask = 0022;
    // we'll need to have current set to do the open call
    struct task *old_current = current;
    current = task;
    struct fd *root = generic_open("/", O_RDONLY_, 0);
    current = old_current;
    if (IS_ERR(root)) {
        err = (int) PTR_ERR(root);
        goto fail_fs;
    }
    task->fs->root = root;
    task->fs->pwd = fd_retain(task->fs->root);

    task_publish(task);
    return task;

fail_fs:
    fs_info_release(task->fs);
fail_files:
    fdtable_release(task->files);
fail_sighand:
    sighand_release(task->sighand);
fail_mm:
    mm_release(task->mm);
fail_group:
    cond_destroy(&group->child_exit);
    cond_destroy(&group->stopped_cond);
    free(group);
    task_abort_create(task);
    return ERR_PTR(err);
}

int become_first_process(void) {
    // now seems like a nice time
    establish_signal_handlers();

    struct task *task = construct_task(NULL);
    if (IS_ERR(task))
        return PTR_ERR(task);

    current = task;
    return 0;
}

int become_new_init_child(void) {
    // locking? who needs locking?!
    struct task *init = pid_get_task(1);
    assert(init != NULL);

    struct task *task = construct_task(init);
    if (IS_ERR(task))
        return PTR_ERR(task);

    // TODO: think about whether it would be a good idea to inherit fs_info

    current = task;
    return 0;
}

extern int console_major;
extern int console_minor;
void set_console_device(int major, int minor) {
    console_major = major;
    console_minor = minor;
}

void create_some_device_nodes(void) {
    // create some device nodes
    // this will do nothing if they already exist
    generic_mknodat(AT_PWD, "/dev/tty1", S_IFCHR|0666, dev_make(TTY_CONSOLE_MAJOR, 1));
    generic_mknodat(AT_PWD, "/dev/tty2", S_IFCHR|0666, dev_make(TTY_CONSOLE_MAJOR, 2));
    generic_mknodat(AT_PWD, "/dev/tty3", S_IFCHR|0666, dev_make(TTY_CONSOLE_MAJOR, 3));
    generic_mknodat(AT_PWD, "/dev/tty4", S_IFCHR|0666, dev_make(TTY_CONSOLE_MAJOR, 4));
    generic_mknodat(AT_PWD, "/dev/tty5", S_IFCHR|0666, dev_make(TTY_CONSOLE_MAJOR, 5));
    generic_mknodat(AT_PWD, "/dev/tty6", S_IFCHR|0666, dev_make(TTY_CONSOLE_MAJOR, 6));
    generic_mknodat(AT_PWD, "/dev/tty7", S_IFCHR|0666, dev_make(TTY_CONSOLE_MAJOR, 7));

    generic_mknodat(AT_PWD, "/dev/tty", S_IFCHR|0666, dev_make(TTY_ALTERNATE_MAJOR, DEV_TTY_MINOR));
    generic_mknodat(AT_PWD, "/dev/console", S_IFCHR|0666, dev_make(TTY_ALTERNATE_MAJOR, DEV_CONSOLE_MINOR));
    generic_mknodat(AT_PWD, "/dev/ptmx", S_IFCHR|0666, dev_make(TTY_ALTERNATE_MAJOR, DEV_PTMX_MINOR));

    generic_mknodat(AT_PWD, "/dev/null", S_IFCHR|0666, dev_make(MEM_MAJOR, DEV_NULL_MINOR));
    generic_mknodat(AT_PWD, "/dev/zero", S_IFCHR|0666, dev_make(MEM_MAJOR, DEV_ZERO_MINOR));
    generic_mknodat(AT_PWD, "/dev/full", S_IFCHR|0666, dev_make(MEM_MAJOR, DEV_FULL_MINOR));
    generic_mknodat(AT_PWD, "/dev/random", S_IFCHR|0666, dev_make(MEM_MAJOR, DEV_RANDOM_MINOR));
    generic_mknodat(AT_PWD, "/dev/urandom", S_IFCHR|0666, dev_make(MEM_MAJOR, DEV_URANDOM_MINOR));

    generic_mkdirat(AT_PWD, "/dev/pts", 0755);
}

int create_stdio(const char *file, int major, int minor) {
    struct fd *fd = generic_open(file, O_RDWR_, 0);
    if (IS_ERR(fd)) {
        // fallback to adhoc files for stdio
        fd = adhoc_fd_create(NULL);
        fd->stat.rdev = dev_make(major, minor);
        fd->stat.mode = S_IFCHR | S_IRUSR;
        fd->flags = O_RDWR_;
        int err = dev_open(major, minor, DEV_CHAR, fd);
        if (err < 0)
            return err;
    }

    fd->refcount = 0;
    current->files->files[0] = fd_retain(fd);
    current->files->files[1] = fd_retain(fd);
    current->files->files[2] = fd_retain(fd);
    return 0;
}

static struct fd *open_fd_from_actual_fd(int fd_no) {
    struct fd *fd = adhoc_fd_create(&realfs_fdops);
    if (fd == NULL) {
        return NULL;
    }
    fd->real_fd = fd_no;
    fd->dir = NULL;
    return fd;
}

int create_piped_stdio(void) {
    if (!(current->files->files[0] = open_fd_from_actual_fd(STDIN_FILENO))) {
        return -1;
    }
    if (!(current->files->files[1] = open_fd_from_actual_fd(STDOUT_FILENO))) {
        return -1;
    }
    if (!(current->files->files[2] = open_fd_from_actual_fd(STDERR_FILENO))) {
        return -1;
    }
    return 0;
}
