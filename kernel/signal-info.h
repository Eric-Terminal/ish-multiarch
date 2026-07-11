#ifndef KERNEL_SIGNAL_INFO_H
#define KERNEL_SIGNAL_INFO_H

#include "misc.h"

enum signal_info_payload_kind {
    SIGNAL_INFO_PAYLOAD_NONE,
    SIGNAL_INFO_PAYLOAD_KILL,
    SIGNAL_INFO_PAYLOAD_TIMER,
    SIGNAL_INFO_PAYLOAD_CHILD,
    SIGNAL_INFO_PAYLOAD_FAULT,
    SIGNAL_INFO_PAYLOAD_SIGSYS,
};

// 内部事件不得依赖当前 guest ABI；架构边界负责转换为各自的 wire。
struct siginfo_ {
    sdword_t sig;
    sdword_t sig_errno;
    sdword_t code;
    dword_t payload_kind;
    union {
        struct {
            sdword_t pid;
            dword_t uid;
        } kill;
        struct {
            sdword_t pid;
            dword_t uid;
            sdword_t status;
            sqword_t utime;
            sqword_t stime;
        } child;
        struct {
            qword_t addr;
        } fault;
        struct {
            qword_t addr;
            sdword_t syscall;
            dword_t arch;
        } sigsys;
        struct {
            sdword_t timer;
            sdword_t overrun;
            qword_t value;
            sdword_t _private;
        } timer;
    };
};
_Static_assert(sizeof(struct siginfo_) == 48 &&
        _Alignof(struct siginfo_) == 8 &&
        __builtin_offsetof(struct siginfo_, payload_kind) == 12 &&
        __builtin_offsetof(struct siginfo_, fault.addr) == 16 &&
        __builtin_offsetof(struct siginfo_, child.utime) == 32 &&
        sizeof(((struct siginfo_ *) 0)->fault.addr) == 8 &&
        sizeof(((struct siginfo_ *) 0)->child.utime) == 8,
        "内部信号信息必须保持固定布局和 64 位值域");

struct i386_siginfo {
    sdword_t sig;
    sdword_t sig_errno;
    sdword_t code;
    union {
        dword_t payload_words[29];
        struct {
            sdword_t pid;
            dword_t uid;
        } kill;
        struct {
            sdword_t timer;
            sdword_t overrun;
            dword_t value;
            sdword_t _private;
        } timer;
        struct {
            sdword_t pid;
            dword_t uid;
            sdword_t status;
            sdword_t utime;
            sdword_t stime;
        } child;
        struct {
            dword_t addr;
        } fault;
        struct {
            dword_t addr;
            sdword_t syscall;
            dword_t arch;
        } sigsys;
    };
} __attribute__((packed, aligned(4)));

_Static_assert(sizeof(struct i386_siginfo) == 128 &&
        _Alignof(struct i386_siginfo) == 4 &&
        __builtin_offsetof(struct i386_siginfo, sig) == 0 &&
        __builtin_offsetof(struct i386_siginfo, sig_errno) == 4 &&
        __builtin_offsetof(struct i386_siginfo, code) == 8 &&
        __builtin_offsetof(struct i386_siginfo, payload_words) == 12 &&
        __builtin_offsetof(struct i386_siginfo, kill.pid) == 12 &&
        __builtin_offsetof(struct i386_siginfo, kill.uid) == 16 &&
        __builtin_offsetof(struct i386_siginfo, timer.timer) == 12 &&
        __builtin_offsetof(struct i386_siginfo, timer.overrun) == 16 &&
        __builtin_offsetof(struct i386_siginfo, timer.value) == 20 &&
        __builtin_offsetof(struct i386_siginfo, timer._private) == 24 &&
        __builtin_offsetof(struct i386_siginfo, child.pid) == 12 &&
        __builtin_offsetof(struct i386_siginfo, child.uid) == 16 &&
        __builtin_offsetof(struct i386_siginfo, child.status) == 20 &&
        __builtin_offsetof(struct i386_siginfo, child.utime) == 24 &&
        __builtin_offsetof(struct i386_siginfo, child.stime) == 28 &&
        __builtin_offsetof(struct i386_siginfo, fault.addr) == 12 &&
        __builtin_offsetof(struct i386_siginfo, sigsys.addr) == 12 &&
        __builtin_offsetof(struct i386_siginfo, sigsys.syscall) == 16 &&
        __builtin_offsetof(struct i386_siginfo, sigsys.arch) == 20,
        "i386 siginfo wire 布局必须固定为 128 字节");

struct i386_siginfo pack_i386_siginfo(const struct siginfo_ *info);
int write_i386_siginfo(dword_t address, const struct siginfo_ *info);

#endif
