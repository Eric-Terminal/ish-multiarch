#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "kernel/calls.h"
#include "kernel/init.h"
#include "kernel/task.h"
#include "guest/aarch64/threaded-profile.h"
#include "xX_main_Xx.h"

#if ISH_AARCH64_THREADED_PROFILE
static int threaded_profile_fd = -1;

static void write_threaded_profile(void) {
    aarch64_threaded_profile_write_fd(threaded_profile_fd);
    close(threaded_profile_fd);
}
#endif

int main(int argc, char *const argv[]) {
#if ISH_AARCH64_THREADED_PROFILE
    threaded_profile_fd = dup(STDERR_FILENO);
    if (threaded_profile_fd < 0)
        return EXIT_FAILURE;
    if (atexit(write_threaded_profile) != 0) {
        close(threaded_profile_fd);
        return EXIT_FAILURE;
    }
#endif
    char envp[100] = {0};
    if (getenv("TERM"))
        strcpy(envp, getenv("TERM") - strlen("TERM") - 1);
    int err = xX_main_Xx(argc, argv, envp);
    if (err < 0) {
        fprintf(stderr, "xX_main_Xx: %s\n", strerror(-err));
        return err;
    }
    create_some_device_nodes();
    do_mount(&procfs, "proc", "/proc", "", 0);
    do_mount(&devptsfs, "devpts", "/dev/pts", "", 0);
    task_run_current();
}
