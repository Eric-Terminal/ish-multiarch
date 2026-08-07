#include "sdk/iSHApple/Headers/iSHApple.h"

#include <stdbool.h>
#include <string.h>

#include "guest/aarch64/backend.h"
#include "kernel/errno.h"
#include "platform/apple-rootfs-seed.h"
#include "platform/apple-diagnostics-private.h"
#include "platform/apple-runtime-mount.h"
#include "platform/apple-watch-runtime.h"
#include "platform/apple-watch-runtime-private.h"

_Static_assert(
        ISH_APPLE_RUNTIME_ARCHITECTURE_AARCH64 ==
                ISH_APPLE_DIAGNOSTIC_ARCHITECTURE_AARCH64 &&
        ISH_APPLE_RUNTIME_BACKEND_UNKNOWN ==
                ISH_APPLE_DIAGNOSTIC_BACKEND_UNKNOWN &&
        ISH_APPLE_RUNTIME_BACKEND_C ==
                ISH_APPLE_DIAGNOSTIC_BACKEND_C &&
        ISH_APPLE_RUNTIME_BACKEND_THREADED ==
                ISH_APPLE_DIAGNOSTIC_BACKEND_THREADED,
        "能力快照与诊断事件必须使用同一架构和后端编号");

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
    int32_t start_error = ish_watch_runtime_start(
            spec->root_data,
            spec->shared_directory,
            spec->socket_prefix,
            spec->hostname,
            spec->boot_command);
    if (start_error < 0) {
        ish_apple_diagnostics_record_runtime(
                ISH_APPLE_DIAGNOSTIC_RUNTIME_START_FAILED,
                start_error);
    }
    return start_error;
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
    if (error < 0) {
        ish_apple_diagnostics_record_runtime(
                ISH_APPLE_DIAGNOSTIC_RUNTIME_START_FAILED,
                error);
    }
    return error;
}

int32_t ish_apple_runtime_current_phase(void) {
    return (int32_t) ish_watch_runtime_current_phase();
}

int32_t ish_apple_runtime_last_error(void) {
    return (int32_t) ish_watch_runtime_last_error();
}

int32_t ish_apple_runtime_copy_capabilities(
        struct ish_apple_runtime_capabilities_v1 *capabilities_out) {
    if (capabilities_out == NULL)
        return _EINVAL;
    memset(capabilities_out, 0, sizeof(*capabilities_out));
    int error = ish_watch_runtime_operation_availability();
    if (error < 0)
        return error;

    uint32_t backend = ISH_APPLE_RUNTIME_BACKEND_UNKNOWN;
    switch (aarch64_backend_default()) {
        case AARCH64_BACKEND_C:
            backend = ISH_APPLE_RUNTIME_BACKEND_C;
            break;
        case AARCH64_BACKEND_THREADED:
            backend = ISH_APPLE_RUNTIME_BACKEND_THREADED;
            break;
    }
    *capabilities_out =
            (struct ish_apple_runtime_capabilities_v1) {
        .version = ISH_APPLE_ABI_VERSION,
        .structure_size = sizeof(*capabilities_out),
        .feature_flags = ISH_APPLE_RUNTIME_CAPABILITY_PTY |
                ISH_APPLE_RUNTIME_CAPABILITY_LIVE_MOUNTS |
                ISH_APPLE_RUNTIME_CAPABILITY_DIAGNOSTICS |
                ISH_APPLE_RUNTIME_CAPABILITY_GUEST_FILES,
        .guest_architecture =
                ISH_APPLE_RUNTIME_ARCHITECTURE_AARCH64,
        .backend = backend,
        .public_abi_version = ISH_APPLE_ABI_VERSION,
    };
    return 0;
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
