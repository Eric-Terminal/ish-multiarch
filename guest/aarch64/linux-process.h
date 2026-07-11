#ifndef GUEST_AARCH64_LINUX_PROCESS_H
#define GUEST_AARCH64_LINUX_PROCESS_H

#include "guest/linux/signal-service.h"
#include "guest/linux/syscall-service.h"

#define AARCH64_LINUX_PROCESS_RANDOM_SIZE 16

struct aarch64_linux_process;

enum aarch64_linux_process_error_stage {
    AARCH64_LINUX_PROCESS_ERROR_NONE,
    AARCH64_LINUX_PROCESS_ERROR_ARGUMENT,
    AARCH64_LINUX_PROCESS_ERROR_ALLOCATION,
    AARCH64_LINUX_PROCESS_ERROR_ELF,
    AARCH64_LINUX_PROCESS_ERROR_INTERPRETER,
    AARCH64_LINUX_PROCESS_ERROR_INTERPRETER_ELF,
    AARCH64_LINUX_PROCESS_ERROR_LOAD,
    AARCH64_LINUX_PROCESS_ERROR_INTERPRETER_LOAD,
    AARCH64_LINUX_PROCESS_ERROR_LAYOUT,
    AARCH64_LINUX_PROCESS_ERROR_TRAMPOLINE,
    AARCH64_LINUX_PROCESS_ERROR_STACK,
};

struct aarch64_linux_process_error {
    dword_t stage;
    dword_t detail;
};

enum aarch64_linux_interpreter_config_error {
    AARCH64_LINUX_INTERPRETER_CONFIG_REQUIRED = 1,
    AARCH64_LINUX_INTERPRETER_CONFIG_UNEXPECTED,
    AARCH64_LINUX_INTERPRETER_CONFIG_INVALID,
};

enum aarch64_linux_interpreter_path_status {
    AARCH64_LINUX_INTERPRETER_PATH_NONE,
    AARCH64_LINUX_INTERPRETER_PATH_COPIED,
    AARCH64_LINUX_INTERPRETER_PATH_BUFFER_TOO_SMALL,
    AARCH64_LINUX_INTERPRETER_PATH_BAD_ELF,
};

struct aarch64_linux_interpreter_path_result {
    dword_t status;
    dword_t elf_error;
    qword_t required_size;
};

struct aarch64_linux_process_config {
    const void *elf_data;
    size_t elf_size;
    qword_t load_bias;
    // 以下 guest 布局字段均按 4 KiB 页对齐；brk_limit 是可请求的最大 break。
    qword_t stack_top;
    qword_t stack_size;
    qword_t signal_trampoline_page;
    qword_t brk_limit;
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
    pid_t_ tid;
    void *task_opaque;
    const struct guest_linux_syscall_service *syscalls;
    const struct guest_linux_signal_service *signals;
};

struct aarch64_linux_interpreter_image {
    const void *data;
    size_t size;
    qword_t load_bias;
};

enum aarch64_linux_process_status {
    AARCH64_LINUX_PROCESS_RUNNABLE,
    AARCH64_LINUX_PROCESS_EXIT,
    AARCH64_LINUX_PROCESS_EXIT_GROUP,
    AARCH64_LINUX_PROCESS_STOP,
    AARCH64_LINUX_PROCESS_TERMINATE,
    AARCH64_LINUX_PROCESS_FETCH_FAULT,
    AARCH64_LINUX_PROCESS_DATA_FAULT,
    AARCH64_LINUX_PROCESS_UNDEFINED,
};

struct aarch64_linux_process_result {
    dword_t status;
    sdword_t signal;
    dword_t exit_status;
    dword_t instruction;
    struct guest_linux_user_fault fault;
};

/*
 * create 会复制 ELF、初始栈数据和服务描述符；服务 opaque 指向的后端状态
 * 仍由调用方持有，并须覆盖 process 的完整生命周期。
 */
struct aarch64_linux_process *aarch64_linux_process_create(
        const struct aarch64_linux_process_config *config,
        struct aarch64_linux_process_error *error);
/*
 * 调用方须先用主程序 PT_INTERP 路径从 guest 文件系统读取解释器。
 * 两份 ELF 缓冲区都只在 create 调用期间借用，成功后即可释放。
 */
struct aarch64_linux_process *aarch64_linux_process_create_with_interpreter(
        const struct aarch64_linux_process_config *config,
        const struct aarch64_linux_interpreter_image *interpreter,
        struct aarch64_linux_process_error *error);
// required_size 包含末尾 NUL；destination 为 NULL 可只查询长度，返回
// BUFFER_TOO_SMALL；任何缓冲区不足情形都不写入部分路径。
struct aarch64_linux_interpreter_path_result
        aarch64_linux_copy_interpreter_path(
        const void *elf_data, size_t elf_size,
        char *destination, size_t capacity);
void aarch64_linux_process_destroy(
        struct aarch64_linux_process *process);
// 核对 create 时复制的 tid、服务闭包与 task opaque；不比较描述符地址。
bool aarch64_linux_process_uses_services(
        const struct aarch64_linux_process *process,
        pid_t_ tid, const void *task_opaque,
        const struct guest_linux_syscall_service *syscalls,
        const struct guest_linux_signal_service *signals);
// fault/undefined 不推进 PC；undefined 的 fault.address 保存精确 PC。
// 调用方排入同步信号后应先调用 poll_signals。
struct aarch64_linux_process_result aarch64_linux_process_run_one(
        struct aarch64_linux_process *process);
// 单独建立信号安全点，不执行 guest 指令。
struct aarch64_linux_process_result aarch64_linux_process_poll_signals(
        struct aarch64_linux_process *process);

_Static_assert(sizeof(enum aarch64_linux_process_error_stage) == 4 &&
        sizeof(enum aarch64_linux_process_status) == 4 &&
        sizeof(enum aarch64_linux_interpreter_config_error) == 4 &&
        sizeof(enum aarch64_linux_interpreter_path_status) == 4,
        "AArch64 process 状态枚举必须保持 32 位");
_Static_assert(offsetof(struct aarch64_linux_process_error, stage) == 0 &&
        offsetof(struct aarch64_linux_process_error, detail) == 4 &&
        sizeof(struct aarch64_linux_process_error) == 8,
        "AArch64 process 错误必须保持固定 DTO 布局");
_Static_assert(offsetof(struct aarch64_linux_interpreter_path_result,
                status) == 0 &&
        offsetof(struct aarch64_linux_interpreter_path_result,
                elf_error) == 4 &&
        offsetof(struct aarch64_linux_interpreter_path_result,
                required_size) == 8 &&
        sizeof(struct aarch64_linux_interpreter_path_result) == 16 &&
        _Alignof(struct aarch64_linux_interpreter_path_result) == 8,
        "AArch64 解释器路径结果必须保持固定 DTO 布局");
_Static_assert(sizeof(((struct aarch64_linux_process_config *) 0)->
                load_bias) == 8 &&
        sizeof(((struct aarch64_linux_process_config *) 0)->
                stack_top) == 8 &&
        sizeof(((struct aarch64_linux_process_config *) 0)->
                stack_size) == 8 &&
        sizeof(((struct aarch64_linux_process_config *) 0)->
                signal_trampoline_page) == 8 &&
        sizeof(((struct aarch64_linux_process_config *) 0)->
                brk_limit) == 8,
        "AArch64 process 的 guest 地址与范围必须保持 64 位");
_Static_assert(sizeof(((struct aarch64_linux_interpreter_image *) 0)->
                load_bias) == 8,
        "AArch64 解释器 load bias 必须保持 64 位");
_Static_assert(offsetof(struct aarch64_linux_process_result, status) == 0 &&
        offsetof(struct aarch64_linux_process_result, signal) == 4 &&
        offsetof(struct aarch64_linux_process_result, exit_status) == 8 &&
        offsetof(struct aarch64_linux_process_result, instruction) == 12 &&
        offsetof(struct aarch64_linux_process_result, fault) == 16 &&
        sizeof(struct aarch64_linux_process_result) == 32 &&
        _Alignof(struct aarch64_linux_process_result) == 8,
        "AArch64 process 运行结果必须保持固定 DTO 布局");

#endif
