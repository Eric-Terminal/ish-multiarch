#include "kernel/init.h"
#include "platform/apple-rootfs-seed.h"

static int (*volatile runtime_entry)(void) = become_first_process;
static int (*volatile rootfs_seed_entry)(
        const char *, const char *, const char *,
        enum ish_apple_rootfs_seed_result *) =
        ish_apple_rootfs_seed_install;

int main(void) {
    // 保留真实入口的链接依赖，但验证程序本身不启动 guest。
    return runtime_entry == NULL || rootfs_seed_entry == NULL;
}
