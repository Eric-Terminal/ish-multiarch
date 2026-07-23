#include <fcntl.h>
#include "kernel/calls.h"

#ifdef __APPLE__
#include <CommonCrypto/CommonCrypto.h>
#include <CommonCrypto/CommonRandom.h>
#else
#include <errno.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/random.h>
#endif

int get_random(char *buf, size_t len) {
#ifdef __APPLE__
    return CCRandomGenerateBytes(buf, len) != kCCSuccess;
#else
    size_t completed = 0;
    while (completed != len) {
        ssize_t result = syscall(SYS_getrandom,
                buf + completed, len - completed, 0);
        if (result > 0) {
            completed += (size_t) result;
            continue;
        }
        if (result < 0 && errno == EINTR)
            continue;
        return 1;
    }
    return 0;
#endif
}

dword_t sys_getrandom(addr_t buf_addr, dword_t len, dword_t UNUSED(flags)) {
    if (len > 1 << 20)
        return _EIO;
    char *buf = malloc(len);
    if (get_random(buf, len) != 0) {
        free(buf);
        return _EIO;
    }
    if (user_write(buf_addr, buf, len)) {
        free(buf);
        return _EFAULT;
    }
    free(buf);
    return len;
}
