#include <stdlib.h>
#include <string.h>

#include "fs/fd.h"
#include "guest/aarch64/elf64-loader.h"
#include "guest/aarch64/linux-process.h"
#include "guest/aarch64/linux-stack.h"
#include "kernel/aarch64-exec-image.h"
#include "kernel/aarch64-exec.h"
#include "kernel/aarch64-signal-service.h"
#include "kernel/aarch64-syscall-service.h"
#include "kernel/errno.h"
#include "kernel/mm.h"
#include "kernel/random.h"
#include "kernel/task.h"

#define AARCH64_EXEC_PIE_BIAS UINT64_C(0x0000555500000000)
#define AARCH64_EXEC_BRK_LIMIT UINT64_C(0x0000600000000000)
#define AARCH64_EXEC_INTERPRETER_BIAS UINT64_C(0x00007ff000000000)
#define AARCH64_EXEC_SIGNAL_TRAMPOLINE UINT64_C(0x00007ffe00000000)
#define AARCH64_EXEC_STACK_TOP UINT64_C(0x00007fff00000000)
#define AARCH64_EXEC_STACK_SIZE (UINT64_C(8) * 1024 * 1024)

static bool consume_packed_strings(const char *packed,
        size_t count, size_t *budget) {
    for (size_t index = 0; index < count; index++) {
        size_t size = strlen(packed) + 1;
        if (size > *budget)
            return false;
        *budget -= size;
        packed += size;
    }
    return true;
}

static bool exec_arguments_fit(const char *executable,
        size_t argument_count, const char *arguments,
        size_t environment_count, const char *environment) {
    size_t budget = ISH_AARCH64_EXEC_ARG_MAX;
    size_t pointer_count;
    if (__builtin_add_overflow(argument_count, environment_count,
            &pointer_count) ||
            __builtin_add_overflow(pointer_count, (size_t) 2,
                    &pointer_count) ||
            pointer_count > budget / sizeof(qword_t))
        return false;
    budget -= pointer_count * sizeof(qword_t);
    size_t executable_size = strlen(executable) + 1;
    if (executable_size > budget)
        return false;
    budget -= executable_size;
    return consume_packed_strings(arguments, argument_count, &budget) &&
            consume_packed_strings(
                    environment, environment_count, &budget);
}

static const char **unpack_strings(const char *packed, size_t count) {
    if (count == 0)
        return NULL;
    const char **strings = malloc(count * sizeof(*strings));
    if (strings == NULL)
        return NULL;
    const char *cursor = packed;
    for (size_t index = 0; index < count; index++) {
        strings[index] = cursor;
        cursor += strlen(cursor) + 1;
    }
    return strings;
}

static int process_error_number(
        const struct aarch64_linux_process_error *error) {
    switch ((enum aarch64_linux_process_error_stage) error->stage) {
        case AARCH64_LINUX_PROCESS_ERROR_NONE:
            return 0;
        case AARCH64_LINUX_PROCESS_ERROR_ARGUMENT:
            return _EINVAL;
        case AARCH64_LINUX_PROCESS_ERROR_ALLOCATION:
            return _ENOMEM;
        case AARCH64_LINUX_PROCESS_ERROR_ELF:
            return _ENOEXEC;
        case AARCH64_LINUX_PROCESS_ERROR_INTERPRETER:
        case AARCH64_LINUX_PROCESS_ERROR_INTERPRETER_ELF:
            return _ELIBBAD;
        case AARCH64_LINUX_PROCESS_ERROR_LOAD:
            return error->detail == AARCH64_ELF64_LOAD_OUT_OF_MEMORY ?
                    _ENOMEM : _ENOEXEC;
        case AARCH64_LINUX_PROCESS_ERROR_INTERPRETER_LOAD:
            return error->detail == AARCH64_ELF64_LOAD_OUT_OF_MEMORY ?
                    _ENOMEM : _ELIBBAD;
        case AARCH64_LINUX_PROCESS_ERROR_LAYOUT:
        case AARCH64_LINUX_PROCESS_ERROR_TRAMPOLINE:
            return _ENOMEM;
        case AARCH64_LINUX_PROCESS_ERROR_STACK:
            return error->detail == AARCH64_LINUX_STACK_OVERFLOW ?
                    _E2BIG : _ENOMEM;
    }
    return _ENOEXEC;
}

int ish_aarch64_exec_stage(struct task *task, struct fd *main_fd,
        const char *executable,
        size_t argument_count, const char *arguments,
        size_t environment_count, const char *environment,
        const struct ish_aarch64_exec_identity *identity) {
    if (task == NULL || task != current || main_fd == NULL ||
            executable == NULL || arguments == NULL || environment == NULL ||
            identity == NULL || task->group == NULL)
        return _EINVAL;
    struct ish_aarch64_exec_images images;
    int error = ish_aarch64_exec_images_read(task, main_fd, &images);
    if (error < 0)
        return error;

    struct aarch64_linux_executable_info main_image =
            aarch64_linux_inspect_executable(
                    images.main.data, images.main.size);
    if (main_image.status != AARCH64_LINUX_EXECUTABLE_VALID) {
        error = _ENOEXEC;
        goto out_images;
    }
    if (!exec_arguments_fit(executable,
            argument_count, arguments,
            environment_count, environment)) {
        error = _E2BIG;
        goto out_images;
    }
    const char **argument_vector =
            unpack_strings(arguments, argument_count);
    const char **environment_vector =
            unpack_strings(environment, environment_count);
    if ((argument_count != 0 && argument_vector == NULL) ||
            (environment_count != 0 && environment_vector == NULL)) {
        error = _ENOMEM;
        goto out_vectors;
    }

    byte_t random[AARCH64_LINUX_PROCESS_RANDOM_SIZE];
    if (get_random((char *) random, sizeof(random)) != 0) {
        error = _EIO;
        goto out_vectors;
    }
    const struct aarch64_linux_process_config config = {
        .elf_data = images.main.data,
        .elf_size = images.main.size,
        .elf_file_source = images.main.file_source,
        .load_bias = main_image.position_independent != 0 ?
                AARCH64_EXEC_PIE_BIAS : 0,
        .stack_top = AARCH64_EXEC_STACK_TOP,
        .stack_size = AARCH64_EXEC_STACK_SIZE,
        .signal_trampoline_page = AARCH64_EXEC_SIGNAL_TRAMPOLINE,
        .brk_limit = AARCH64_EXEC_BRK_LIMIT,
        .executable = executable,
        .arguments = argument_vector,
        .argument_count = argument_count,
        .environment = environment_vector,
        .environment_count = environment_count,
        .random = random,
        .uid = identity->uid,
        .euid = identity->euid,
        .gid = identity->gid,
        .egid = identity->egid,
        .secure = identity->secure,
        // 非 leader 在 PONR 后接管 TGID，候选必须提前使用最终 TID。
        .tid = task->tgid,
        .task_opaque = task,
        .syscalls = &ish_aarch64_linux_syscall_service,
        .signals = &ish_aarch64_linux_signal_service,
    };
    const struct aarch64_linux_interpreter_image interpreter = {
        .data = images.interpreter.data,
        .size = images.interpreter.size,
        .file_source = images.interpreter.file_source,
        .load_bias = AARCH64_EXEC_INTERPRETER_BIAS,
    };
    struct aarch64_linux_process_error process_error;
    struct aarch64_linux_process *process =
            aarch64_linux_process_create_with_interpreter(
                    &config,
                    images.interpreter.data != NULL ? &interpreter : NULL,
                    &process_error);
    if (process == NULL) {
        error = process_error_number(&process_error);
        goto out_vectors;
    }

    struct mm *metadata = mm_new();
    if (metadata == NULL) {
        aarch64_linux_process_destroy(process);
        error = _ENOMEM;
        goto out_vectors;
    }
    metadata->exefile = fd_retain(main_fd);
    if (!task_stage_aarch64_exec(task, process, metadata,
                identity->euid, identity->egid, executable)) {
        mm_release(metadata);
        aarch64_linux_process_destroy(process);
        error = _EBUSY;
        goto out_vectors;
    }
    error = 0;

out_vectors:
    free(environment_vector);
    free(argument_vector);
out_images:
    ish_aarch64_exec_images_destroy(&images);
    return error;
}
