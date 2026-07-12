#ifndef GUEST_AARCH64_LINUX_SIGNAL_ABI_H
#define GUEST_AARCH64_LINUX_SIGNAL_ABI_H

#include "misc.h"

#define AARCH64_LINUX_SA_NOCLDSTOP UINT64_C(0x00000001)
#define AARCH64_LINUX_SA_NOCLDWAIT UINT64_C(0x00000002)
#define AARCH64_LINUX_SA_SIGINFO UINT64_C(0x00000004)
#define AARCH64_LINUX_SA_UNSUPPORTED UINT64_C(0x00000400)
#define AARCH64_LINUX_SA_EXPOSE_TAGBITS UINT64_C(0x00000800)
#define AARCH64_LINUX_SA_RESTORER UINT64_C(0x04000000)
#define AARCH64_LINUX_SA_ONSTACK UINT64_C(0x08000000)
#define AARCH64_LINUX_SA_RESTART UINT64_C(0x10000000)
#define AARCH64_LINUX_SA_NODEFER UINT64_C(0x40000000)
#define AARCH64_LINUX_SA_RESETHAND UINT64_C(0x80000000)

#define AARCH64_LINUX_MINSIGSTKSZ UINT64_C(5120)
#define AARCH64_LINUX_USER_ADDRESS_MAX UINT64_C(0x0000ffffffffffff)

#define AARCH64_LINUX_SA_SUPPORTED_FLAGS \
    (AARCH64_LINUX_SA_NOCLDSTOP | AARCH64_LINUX_SA_NOCLDWAIT | \
     AARCH64_LINUX_SA_SIGINFO | AARCH64_LINUX_SA_EXPOSE_TAGBITS | \
     AARCH64_LINUX_SA_RESTORER | AARCH64_LINUX_SA_ONSTACK | \
     AARCH64_LINUX_SA_RESTART | AARCH64_LINUX_SA_NODEFER | \
     AARCH64_LINUX_SA_RESETHAND)

struct aarch64_linux_sigaction {
    qword_t handler;
    qword_t flags;
    qword_t restorer;
    qword_t mask;
} __attribute__((packed, aligned(8)));

_Static_assert(sizeof(struct aarch64_linux_sigaction) == 32 &&
        _Alignof(struct aarch64_linux_sigaction) == 8,
        "AArch64 Linux sigaction ABI 必须固定为 32 字节且按 8 字节对齐");
_Static_assert(__builtin_offsetof(struct aarch64_linux_sigaction, handler) == 0 &&
        __builtin_offsetof(struct aarch64_linux_sigaction, flags) == 8 &&
        __builtin_offsetof(struct aarch64_linux_sigaction, restorer) == 16 &&
        __builtin_offsetof(struct aarch64_linux_sigaction, mask) == 24,
        "AArch64 Linux sigaction 字段偏移不正确");
_Static_assert(AARCH64_LINUX_SA_SUPPORTED_FLAGS == UINT64_C(0xdc000807) &&
        !(AARCH64_LINUX_SA_SUPPORTED_FLAGS & AARCH64_LINUX_SA_UNSUPPORTED),
        "AArch64 Linux sigaction 支持标志集合不正确");

#define AARCH64_LINUX_FPSIMD_MAGIC UINT32_C(0x46508001)

// wire 布局不得依赖 host 的 pointer、long 或 size_t，尤其是 arm64_32。
struct aarch64_linux_stack {
    _Alignas(8) qword_t sp;
    sdword_t flags;
    dword_t reserved;
    qword_t size;
};

struct aarch64_linux_siginfo {
    sdword_t signo;
    sdword_t error;
    sdword_t code;
    dword_t reserved;
    union {
        _Alignas(8) byte_t payload[112];
        struct {
            sdword_t pid;
            dword_t uid;
        } kill;
        struct {
            sdword_t pid;
            dword_t uid;
            qword_t value;
        } queue;
        struct {
            sdword_t timer;
            sdword_t overrun;
            qword_t value;
            sdword_t private_value;
        } timer;
        struct {
            sdword_t pid;
            dword_t uid;
            sdword_t status;
            sqword_t utime;
            sqword_t stime;
        } child;
        struct {
            qword_t address;
        } fault;
        struct {
            qword_t address;
            sdword_t syscall;
            dword_t architecture;
        } sigsys;
    };
};

struct aarch64_linux_ctx {
    dword_t magic;
    dword_t size;
};

struct aarch64_linux_fpsimd_context {
    struct aarch64_linux_ctx head;
    dword_t fpsr;
    dword_t fpcr;
    _Alignas(16) byte_t vregs[32][16];
};

struct aarch64_linux_sigcontext {
    qword_t fault_address;
    qword_t regs[31];
    qword_t sp;
    qword_t pc;
    qword_t pstate;
    // 扩展记录从这里开始，并由各记录自己的 magic 与 size 描述。
    _Alignas(16) byte_t reserved[4096];
};

struct aarch64_linux_ucontext {
    qword_t flags;
    qword_t link;
    struct aarch64_linux_stack stack;
    qword_t sigmask;
    // 与 sigmask 合计保留 128 字节，维持 libc 可扩展的掩码区域。
    byte_t unused[120];
    _Alignas(16) struct aarch64_linux_sigcontext mcontext;
};

struct aarch64_linux_rt_sigframe {
    struct aarch64_linux_siginfo info;
    _Alignas(16) struct aarch64_linux_ucontext uc;
};

_Static_assert(sizeof(struct aarch64_linux_stack) == 24 &&
        _Alignof(struct aarch64_linux_stack) == 8 &&
        __builtin_offsetof(struct aarch64_linux_stack, sp) == 0 &&
        __builtin_offsetof(struct aarch64_linux_stack, flags) == 8 &&
        __builtin_offsetof(struct aarch64_linux_stack, size) == 16,
        "AArch64 Linux signal stack ABI 布局不正确");
_Static_assert(sizeof(struct aarch64_linux_siginfo) == 128 &&
        _Alignof(struct aarch64_linux_siginfo) == 8 &&
        __builtin_offsetof(struct aarch64_linux_siginfo, signo) == 0 &&
        __builtin_offsetof(struct aarch64_linux_siginfo, error) == 4 &&
        __builtin_offsetof(struct aarch64_linux_siginfo, code) == 8 &&
        __builtin_offsetof(struct aarch64_linux_siginfo, reserved) == 12 &&
        __builtin_offsetof(struct aarch64_linux_siginfo, payload) == 16 &&
        __builtin_offsetof(struct aarch64_linux_siginfo, kill.pid) == 16 &&
        __builtin_offsetof(struct aarch64_linux_siginfo, kill.uid) == 20 &&
        __builtin_offsetof(struct aarch64_linux_siginfo, queue.pid) == 16 &&
        __builtin_offsetof(struct aarch64_linux_siginfo, queue.uid) == 20 &&
        __builtin_offsetof(struct aarch64_linux_siginfo, queue.value) == 24 &&
        __builtin_offsetof(struct aarch64_linux_siginfo, timer.timer) == 16 &&
        __builtin_offsetof(struct aarch64_linux_siginfo, timer.overrun) == 20 &&
        __builtin_offsetof(struct aarch64_linux_siginfo, timer.value) == 24 &&
        __builtin_offsetof(struct aarch64_linux_siginfo,
                timer.private_value) == 32 &&
        __builtin_offsetof(struct aarch64_linux_siginfo, child.pid) == 16 &&
        __builtin_offsetof(struct aarch64_linux_siginfo, child.uid) == 20 &&
        __builtin_offsetof(struct aarch64_linux_siginfo, child.status) == 24 &&
        __builtin_offsetof(struct aarch64_linux_siginfo, child.utime) == 32 &&
        __builtin_offsetof(struct aarch64_linux_siginfo, child.stime) == 40 &&
        __builtin_offsetof(struct aarch64_linux_siginfo, fault.address) == 16 &&
        __builtin_offsetof(struct aarch64_linux_siginfo, sigsys.address) == 16 &&
        __builtin_offsetof(struct aarch64_linux_siginfo, sigsys.syscall) == 24 &&
        __builtin_offsetof(struct aarch64_linux_siginfo,
                sigsys.architecture) == 28,
        "AArch64 Linux siginfo ABI 布局不正确");
_Static_assert(sizeof(struct aarch64_linux_ctx) == 8 &&
        _Alignof(struct aarch64_linux_ctx) == 4 &&
        __builtin_offsetof(struct aarch64_linux_ctx, magic) == 0 &&
        __builtin_offsetof(struct aarch64_linux_ctx, size) == 4,
        "AArch64 Linux 扩展上下文头布局不正确");
_Static_assert(sizeof(struct aarch64_linux_fpsimd_context) == 528 &&
        _Alignof(struct aarch64_linux_fpsimd_context) == 16 &&
        __builtin_offsetof(struct aarch64_linux_fpsimd_context, head) == 0 &&
        __builtin_offsetof(struct aarch64_linux_fpsimd_context, fpsr) == 8 &&
        __builtin_offsetof(struct aarch64_linux_fpsimd_context, fpcr) == 12 &&
        __builtin_offsetof(struct aarch64_linux_fpsimd_context, vregs) == 16,
        "AArch64 Linux FPSIMD 上下文布局不正确");
_Static_assert(sizeof(struct aarch64_linux_sigcontext) == 4384 &&
        _Alignof(struct aarch64_linux_sigcontext) == 16 &&
        __builtin_offsetof(struct aarch64_linux_sigcontext,
                fault_address) == 0 &&
        __builtin_offsetof(struct aarch64_linux_sigcontext, regs) == 8 &&
        __builtin_offsetof(struct aarch64_linux_sigcontext, sp) == 256 &&
        __builtin_offsetof(struct aarch64_linux_sigcontext, pc) == 264 &&
        __builtin_offsetof(struct aarch64_linux_sigcontext, pstate) == 272 &&
        __builtin_offsetof(struct aarch64_linux_sigcontext, reserved) == 288,
        "AArch64 Linux sigcontext ABI 布局不正确");
_Static_assert(sizeof(struct aarch64_linux_ucontext) == 4560 &&
        _Alignof(struct aarch64_linux_ucontext) == 16 &&
        __builtin_offsetof(struct aarch64_linux_ucontext, flags) == 0 &&
        __builtin_offsetof(struct aarch64_linux_ucontext, link) == 8 &&
        __builtin_offsetof(struct aarch64_linux_ucontext, stack) == 16 &&
        __builtin_offsetof(struct aarch64_linux_ucontext, sigmask) == 40 &&
        __builtin_offsetof(struct aarch64_linux_ucontext, unused) == 48 &&
        __builtin_offsetof(struct aarch64_linux_ucontext, mcontext) == 176,
        "AArch64 Linux ucontext ABI 布局不正确");
_Static_assert(sizeof(struct aarch64_linux_rt_sigframe) == 4688 &&
        _Alignof(struct aarch64_linux_rt_sigframe) == 16 &&
        __builtin_offsetof(struct aarch64_linux_rt_sigframe, info) == 0 &&
        __builtin_offsetof(struct aarch64_linux_rt_sigframe, uc) == 128,
        "AArch64 Linux 实时信号帧 ABI 布局不正确");

_Static_assert(__builtin_offsetof(struct aarch64_linux_rt_sigframe,
                uc.sigmask) == 168 &&
        __builtin_offsetof(struct aarch64_linux_rt_sigframe,
                uc.mcontext) == 304 &&
        __builtin_offsetof(struct aarch64_linux_rt_sigframe,
                uc.mcontext.regs) == 312 &&
        __builtin_offsetof(struct aarch64_linux_rt_sigframe,
                uc.mcontext.sp) == 560 &&
        __builtin_offsetof(struct aarch64_linux_rt_sigframe,
                uc.mcontext.pc) == 568 &&
        __builtin_offsetof(struct aarch64_linux_rt_sigframe,
                uc.mcontext.pstate) == 576 &&
        __builtin_offsetof(struct aarch64_linux_rt_sigframe,
                uc.mcontext.reserved) == 592,
        "AArch64 Linux 实时信号帧嵌套字段偏移不正确");

#endif
