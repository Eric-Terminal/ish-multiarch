#include "sdk/iSHApple/Headers/iSHApple.h"

#include <stdbool.h>
#include <string.h>

#include "kernel/errno.h"
#include "platform/apple-rootfs-seed.h"
#include "platform/apple-runtime-mount.h"
#include "platform/apple-watch-runtime.h"

static bool apple_public_reserved_zero(const uint64_t values[2]) {
    return values[0] == 0 && values[1] == 0;
}

static bool apple_public_string_fits(
        const char *value, uint32_t maximum) {
    return value != NULL &&
            strnlen(value, (size_t) maximum + 1) <= maximum;
}

static int32_t apple_public_runtime_validate_strings(
        const char *root_data,
        const char *shared_directory,
        const char *socket_prefix,
        const char *hostname,
        const char *boot_command) {
    if (root_data == NULL || shared_directory == NULL)
        return _EINVAL;
    if (!apple_public_string_fits(
                socket_prefix,
                ISH_APPLE_RUNTIME_SOCKET_PREFIX_BYTES_MAX))
        return socket_prefix == NULL ? _EINVAL : _ENAMETOOLONG;
    if (!apple_public_string_fits(
                hostname,
                ISH_APPLE_RUNTIME_HOSTNAME_BYTES_MAX) ||
            !apple_public_string_fits(
                boot_command,
                ISH_APPLE_RUNTIME_BOOT_COMMAND_BYTES_MAX))
        return hostname == NULL || boot_command == NULL ?
                _EINVAL : _E2BIG;
    return 0;
}

int32_t ish_apple_runtime_start(
        const struct ish_apple_runtime_spec_v1 *spec) {
    if (spec == NULL)
        return _EINVAL;
    if (spec->version != ISH_APPLE_ABI_VERSION)
        return _ENOTSUP;
    if (spec->structure_size < sizeof(*spec) ||
            !apple_public_reserved_zero(spec->reserved))
        return _EINVAL;
    int32_t error = apple_public_runtime_validate_strings(
            spec->root_data,
            spec->shared_directory,
            spec->socket_prefix,
            spec->hostname,
            spec->boot_command);
    if (error < 0)
        return error;
    return ish_watch_runtime_start(
            spec->root_data,
            spec->shared_directory,
            spec->socket_prefix,
            spec->hostname,
            spec->boot_command);
}

int32_t ish_apple_runtime_start_v2(
        const struct ish_apple_runtime_spec_v2 *spec) {
    if (spec == NULL)
        return _EINVAL;
    if (spec->version != ISH_APPLE_ABI_VERSION)
        return _ENOTSUP;
    if (spec->structure_size < sizeof(*spec) ||
            !apple_public_reserved_zero(spec->reserved) ||
            spec->reserved_0 != 0 ||
            (spec->mount_count != 0 && spec->mounts == NULL))
        return _EINVAL;
    int32_t error = apple_public_runtime_validate_strings(
            spec->root_data,
            spec->shared_directory,
            spec->socket_prefix,
            spec->hostname,
            spec->boot_command);
    if (error < 0)
        return error;

    error = ish_apple_mount_prepare_startup(
            spec->mounts, spec->mount_count);
    if (error < 0)
        return error;
    error = ish_watch_runtime_start(
            spec->root_data,
            spec->shared_directory,
            spec->socket_prefix,
            spec->hostname,
            spec->boot_command);
    ish_apple_mount_finish_startup(error == 0);
    return error;
}

int32_t ish_apple_runtime_current_phase(void) {
    return (int32_t) ish_watch_runtime_current_phase();
}

int32_t ish_apple_runtime_last_error(void) {
    return (int32_t) ish_watch_runtime_last_error();
}

int32_t ish_apple_rootfs_install_seed(
        const char *seed_root,
        const char *persistent_parent,
        const char *root_name,
        int32_t *disposition_out) {
    if (disposition_out == NULL)
        return _EINVAL;
    enum ish_apple_rootfs_seed_result result;
    int error = ish_apple_rootfs_seed_install(
            seed_root,
            persistent_parent,
            root_name,
            &result);
    if (error != 0)
        return err_map(error);
    *disposition_out =
            result == ISH_APPLE_ROOTFS_SEED_INSTALLED ?
            ISH_APPLE_ROOTFS_INSTALL_RESULT_INSTALLED :
            ISH_APPLE_ROOTFS_INSTALL_RESULT_ALREADY_PRESENT;
    return 0;
}
