#include "platform/apple-command-session-private.h"

#include <stdlib.h>
#include <string.h>

#include "kernel/errno.h"

#define COMMAND_ARGUMENT_BYTES_MAX ISH_APPLE_COMMAND_ARGUMENT_BYTES_MAX
#define COMMAND_ARGUMENT_COUNT_MAX ISH_APPLE_COMMAND_ARGUMENT_COUNT_MAX
#define COMMAND_PATH_BYTES_MAX ISH_APPLE_COMMAND_PATH_BYTES_MAX

void command_arguments_destroy(struct command_arguments *arguments) {
    free(arguments->working_directory);
    free(arguments->environment_vector);
    free(arguments->environment_bytes);
    free(arguments->argument_vector);
    free(arguments->argument_bytes);
    free(arguments->executable);
    *arguments = (struct command_arguments) {};
}

static int32_t command_string_length(
        const char *string, uint32_t maximum, size_t *length_out) {
    if (string == NULL)
        return _EINVAL;
    size_t length = strnlen(string, (size_t) maximum + 1);
    if (length > maximum)
        return _E2BIG;
    *length_out = length;
    return 0;
}

static int32_t command_pack_string_array(
        const char *const *source,
        uint32_t count,
        bool reject_empty,
        char **bytes_out,
        char ***vector_out,
        size_t *budget) {
    char **vector = calloc((size_t) count + 1, sizeof(*vector));
    if (vector == NULL)
        return _ENOMEM;

    size_t total = 1;
    for (uint32_t index = 0; index < count; index++) {
        size_t length;
        int32_t error = command_string_length(
                source[index], COMMAND_ARGUMENT_BYTES_MAX, &length);
        if (error < 0 || (reject_empty && length == 0)) {
            free(vector);
            return error < 0 ? error : _EINVAL;
        }
        if (length + 1 > *budget) {
            free(vector);
            return _E2BIG;
        }
        *budget -= length + 1;
        total += length + 1;
    }

    char *bytes = calloc(total, 1);
    if (bytes == NULL) {
        free(vector);
        return _ENOMEM;
    }
    char *cursor = bytes;
    for (uint32_t index = 0; index < count; index++) {
        size_t length = strlen(source[index]);
        vector[index] = cursor;
        memcpy(cursor, source[index], length + 1);
        cursor += length + 1;
    }
    *bytes_out = bytes;
    *vector_out = vector;
    return 0;
}

static bool command_u64_reserved_zero(const uint64_t values[2]) {
    return values[0] == 0 && values[1] == 0;
}

int32_t command_arguments_create_for_spec(
        const struct ish_apple_command_spec_v1 *spec,
        struct command_arguments *arguments) {
    if (spec == NULL)
        return _EINVAL;
    if (spec->version != ISH_APPLE_ABI_VERSION)
        return _ENOTSUP;
    if (spec->structure_size < sizeof(*spec) ||
            spec->reserved_0 != 0 ||
            !command_u64_reserved_zero(spec->reserved) ||
            spec->request_id == 0 ||
            (spec->timeout_milliseconds >
                    ISH_APPLE_COMMAND_TIMEOUT_MS_MAX &&
                    spec->timeout_milliseconds !=
                    ISH_APPLE_COMMAND_TIMEOUT_MS_DISABLED) ||
            (spec->output_byte_limit >
                    ISH_APPLE_COMMAND_OUTPUT_BYTES_MAX &&
                    spec->output_byte_limit !=
                    ISH_APPLE_COMMAND_OUTPUT_BYTES_DISABLED) ||
            spec->argument_count == 0 ||
            spec->argument_count > COMMAND_ARGUMENT_COUNT_MAX ||
            spec->arguments == NULL ||
            spec->environment_count > COMMAND_ARGUMENT_COUNT_MAX ||
            (spec->environment_count != 0 &&
                    spec->environment == NULL))
        return _EINVAL;

    size_t executable_length;
    int32_t error = command_string_length(
            spec->executable, COMMAND_PATH_BYTES_MAX,
            &executable_length);
    if (error < 0)
        return error;
    if (executable_length == 0)
        return _EINVAL;

    arguments->executable = strdup(spec->executable);
    if (arguments->executable == NULL)
        return _ENOMEM;

    size_t budget = COMMAND_ARGUMENT_BYTES_MAX;
    error = command_pack_string_array(
            spec->arguments, spec->argument_count, false,
            &arguments->argument_bytes,
            &arguments->argument_vector, &budget);
    if (error < 0)
        goto fail;
    arguments->argument_count = spec->argument_count;

    if (spec->environment_count == 0) {
        arguments->environment_bytes = calloc(1, 1);
        arguments->environment_vector = calloc(
                1, sizeof(*arguments->environment_vector));
        if (arguments->environment_bytes == NULL ||
                arguments->environment_vector == NULL) {
            error = _ENOMEM;
            goto fail;
        }
    } else {
        error = command_pack_string_array(
                spec->environment, spec->environment_count, true,
                &arguments->environment_bytes,
                &arguments->environment_vector, &budget);
        if (error < 0)
            goto fail;
    }
    arguments->environment_count = spec->environment_count;

    if (spec->working_directory != NULL) {
        size_t directory_length;
        error = command_string_length(
                spec->working_directory, COMMAND_PATH_BYTES_MAX,
                &directory_length);
        if (error < 0)
            goto fail;
        if (directory_length == 0) {
            error = _EINVAL;
            goto fail;
        }
        arguments->working_directory =
                strdup(spec->working_directory);
        if (arguments->working_directory == NULL) {
            error = _ENOMEM;
            goto fail;
        }
    }
    return 0;

fail:
    command_arguments_destroy(arguments);
    return error;
}

int32_t command_arguments_create(
        const struct ish_apple_command_spec_v1 *spec,
        const struct ish_apple_command_callbacks_v1 *callbacks,
        struct command_arguments *arguments) {
    if (callbacks == NULL)
        return _EINVAL;
    if (callbacks->version != ISH_APPLE_ABI_VERSION)
        return _ENOTSUP;
    if (callbacks->structure_size < sizeof(*callbacks) ||
            callbacks->stream == NULL ||
            callbacks->completed == NULL ||
            !command_u64_reserved_zero(callbacks->reserved))
        return _EINVAL;
    return command_arguments_create_for_spec(spec, arguments);
}
