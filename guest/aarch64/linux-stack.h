#ifndef GUEST_AARCH64_LINUX_STACK_H
#define GUEST_AARCH64_LINUX_STACK_H

#include "guest/aarch64/elf64-loader.h"
#include "emu/cpu.h"

#define AARCH64_LINUX_RANDOM_SIZE 16

enum aarch64_linux_stack_error {
    AARCH64_LINUX_STACK_OK,
    AARCH64_LINUX_STACK_INVALID_ARGUMENT,
    AARCH64_LINUX_STACK_OVERFLOW,
    AARCH64_LINUX_STACK_OUT_OF_MEMORY,
    AARCH64_LINUX_STACK_MAPPING_CONFLICT,
};

struct aarch64_linux_stack_config {
    guest_addr_t stack_top;
    size_t stack_size;
    const char *executable;
    const char *const *arguments;
    size_t argument_count;
    const char *const *environment;
    size_t environment_count;
    const byte_t *random;
    dword_t uid;
    dword_t euid;
    dword_t gid;
    dword_t egid;
    dword_t secure;
    guest_addr_t interpreter_base;
};

struct aarch64_linux_stack_result {
    guest_addr_t stack_pointer;
    guest_addr_t argv;
    guest_addr_t environment;
    guest_addr_t auxv_start;
    guest_addr_t auxv_end;
};

enum aarch64_linux_stack_error aarch64_linux_build_initial_stack(
        struct guest_page_table *table,
        const struct aarch64_elf64_load_result *loaded,
        const struct aarch64_linux_stack_config *config,
        struct aarch64_linux_stack_result *result);
void aarch64_linux_prepare_cpu(struct cpu_state *cpu,
        const struct aarch64_elf64_load_result *loaded,
        const struct aarch64_linux_stack_result *stack);
void aarch64_linux_prepare_cpu_at(struct cpu_state *cpu,
        guest_addr_t initial_pc,
        const struct aarch64_linux_stack_result *stack);

#endif
