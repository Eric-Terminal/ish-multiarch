#include "sdk/iSHApple/Headers/iSHApple.h"

#include <stdio.h>
#include <string.h>

#include "kernel/errno.h"

static int failures;

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "Apple 公共 runtime 测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        failures++; \
    } \
} while (0)

int main(void) {
    struct ish_apple_runtime_capabilities_v1 capabilities;
    memset(&capabilities, 0xa5, sizeof(capabilities));
    CHECK(ish_apple_runtime_copy_capabilities(NULL) == _EINVAL,
            "能力快照拒绝空输出指针");
    CHECK(ish_apple_runtime_copy_capabilities(&capabilities) == _EAGAIN &&
            capabilities.version == 0 &&
            capabilities.feature_flags == 0,
            "runtime 就绪前不发布猜测的能力并清空输出");

    struct ish_apple_runtime_spec_v1 spec = {
        .version = ISH_APPLE_ABI_VERSION + 1,
        .structure_size = sizeof(spec),
    };
    CHECK(ish_apple_runtime_start(&spec) == _ENOTSUP,
            "未知 runtime ABI 返回 ENOTSUP");

    spec.version = ISH_APPLE_ABI_VERSION;
    spec.structure_size = sizeof(spec) - 1;
    CHECK(ish_apple_runtime_start(&spec) == _EINVAL,
            "拒绝截断的 runtime 配置");

    spec.structure_size = sizeof(spec);
    spec.reserved[1] = 1;
    CHECK(ish_apple_runtime_start(&spec) == _EINVAL,
            "拒绝非零保留字段");

    spec.reserved[1] = 0;
    spec.root_data = "/missing-root";
    spec.shared_directory = "/missing-shared";
    spec.socket_prefix = "/tmp/ish-public";
    spec.hostname = "Public-Test";
    spec.boot_command = "/bin/true";
    char long_socket_prefix[
            ISH_APPLE_RUNTIME_SOCKET_PREFIX_BYTES_MAX + 2];
    memset(
            long_socket_prefix,
            's',
            sizeof(long_socket_prefix) - 1);
    long_socket_prefix[sizeof(long_socket_prefix) - 1] = '\0';
    spec.socket_prefix = long_socket_prefix;
    CHECK(ish_apple_runtime_start(&spec) == _ENAMETOOLONG,
            "公共 runtime 在系统调用前拒绝超长 socket prefix");

    char long_hostname[
            ISH_APPLE_RUNTIME_HOSTNAME_BYTES_MAX + 2];
    memset(long_hostname, 'h', sizeof(long_hostname) - 1);
    long_hostname[sizeof(long_hostname) - 1] = '\0';
    spec.socket_prefix = "/tmp/ish-public";
    spec.hostname = long_hostname;
    CHECK(ish_apple_runtime_start(&spec) == _E2BIG,
            "公共 runtime 在系统调用前拒绝超长 hostname");

    char long_boot_command[
            ISH_APPLE_RUNTIME_BOOT_COMMAND_BYTES_MAX + 2];
    memset(
            long_boot_command,
            'c',
            sizeof(long_boot_command) - 1);
    long_boot_command[sizeof(long_boot_command) - 1] = '\0';
    spec.hostname = "Public-Test";
    spec.boot_command = long_boot_command;
    CHECK(ish_apple_runtime_start(&spec) == _E2BIG &&
            ish_apple_runtime_current_phase() ==
                    ISH_APPLE_RUNTIME_PHASE_IDLE,
            "超长公共字符串不消耗一次性 runtime 启动机会");

    spec.root_data = "";
    spec.shared_directory = "";
    spec.socket_prefix = "";
    spec.hostname = "";
    spec.boot_command = "";
    CHECK(ish_apple_runtime_start(&spec) == _EINVAL &&
            ish_apple_runtime_current_phase() ==
                    ISH_APPLE_RUNTIME_PHASE_IDLE &&
            ish_apple_runtime_last_error() == 0,
            "底层参数失败不消耗一次性 runtime 启动机会");

    struct ish_apple_runtime_spec_v2 spec_v2 = {
        .version = ISH_APPLE_ABI_VERSION + 1,
        .structure_size = sizeof(spec_v2),
    };
    CHECK(ish_apple_runtime_start_v2(&spec_v2) == _ENOTSUP,
            "runtime v2 拒绝未知 ABI");
    spec_v2.version = ISH_APPLE_ABI_VERSION;
    spec_v2.structure_size--;
    CHECK(ish_apple_runtime_start_v2(&spec_v2) == _EINVAL,
            "runtime v2 拒绝截断配置");
    spec_v2.structure_size = sizeof(spec_v2);
    spec_v2.mount_count = 1;
    CHECK(ish_apple_runtime_start_v2(&spec_v2) == _EINVAL,
            "runtime v2 拒绝缺失的启动 mount 数组");
    spec_v2.mount_count = 0;
    spec_v2.root_data = "";
    spec_v2.shared_directory = "";
    spec_v2.socket_prefix = "";
    spec_v2.hostname = "";
    spec_v2.boot_command = "";
    CHECK(ish_apple_runtime_start_v2(&spec_v2) == _EINVAL &&
            ish_apple_runtime_current_phase() ==
                    ISH_APPLE_RUNTIME_PHASE_IDLE,
            "runtime v2 参数失败不消耗启动机会");

    int32_t disposition = -1;
    CHECK(ish_apple_rootfs_install_seed(
            "/definitely/missing-seed",
            "/definitely/missing-parent",
            "aarch64",
            &disposition) == _ENOENT,
            "公共 RootFS 安装统一映射宿主 ENOENT");

    if (failures == 0)
        puts("Apple 公共 runtime ABI 回归通过");
    return failures == 0 ? 0 : 1;
}
