#ifndef PLATFORM_APPLE_DIAGNOSTICS_PRIVATE_H
#define PLATFORM_APPLE_DIAGNOSTICS_PRIVATE_H

#include <stdint.h>

#include "guest/aarch64/backend.h"

#pragma GCC visibility push(hidden)

struct task;

void ish_apple_diagnostics_record_undefined_instruction(
        struct task *task,
        uint64_t guest_pc,
        uint32_t opcode,
        int32_t signal,
        enum aarch64_backend backend);
void ish_apple_diagnostics_record_unsupported_syscall(
        struct task *task,
        uint64_t guest_pc,
        uint64_t syscall_number,
        int32_t linux_error,
        enum aarch64_backend backend);
void ish_apple_diagnostics_record_filesystem(
        uint32_t scope,
        uint64_t request_id,
        uint32_t kind,
        int32_t linux_error);
void ish_apple_diagnostics_record_runtime(
        uint32_t kind,
        int32_t linux_error);

#pragma GCC visibility pop

#endif
