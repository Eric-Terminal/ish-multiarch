#include <assert.h>
#include <string.h>

#include "guest/aarch64/linux-signal-abi.h"

static dword_t load_dword(const byte_t *wire, size_t offset) {
    dword_t value;
    memcpy(&value, wire + offset, sizeof(value));
    return value;
}

static qword_t load_qword(const byte_t *wire, size_t offset) {
    qword_t value;
    memcpy(&value, wire + offset, sizeof(value));
    return value;
}

int main(void) {
    struct aarch64_linux_rt_sigframe frame = {0};
    struct aarch64_linux_fpsimd_context fpsimd = {
        .head = {
            .magic = AARCH64_LINUX_FPSIMD_MAGIC,
            .size = sizeof(struct aarch64_linux_fpsimd_context),
        },
        .fpsr = UINT32_C(0x12345678),
        .fpcr = UINT32_C(0x9abcdef0),
    };
    fpsimd.vregs[31][15] = UINT8_C(0xd1);
    frame.info.signo = 11;
    frame.info.error = -2;
    frame.info.code = 1;
    frame.info.payload[0] = UINT8_C(0xa5);
    frame.uc.flags = UINT64_C(0x0102030405060708);
    frame.uc.link = UINT64_C(0x1112131415161718);
    frame.uc.stack.sp = UINT64_C(0x2122232425262728);
    frame.uc.stack.flags = UINT32_C(0x31323334);
    frame.uc.stack.size = UINT64_C(0x4142434445464748);
    frame.uc.sigmask = UINT64_C(0x5152535455565758);
    frame.uc.mcontext.fault_address = UINT64_C(0x6162636465666768);
    frame.uc.mcontext.regs[0] = UINT64_C(0x7172737475767778);
    frame.uc.mcontext.regs[30] = UINT64_C(0x8182838485868788);
    frame.uc.mcontext.sp = UINT64_C(0x9192939495969798);
    frame.uc.mcontext.pc = UINT64_C(0xa1a2a3a4a5a6a7a8);
    frame.uc.mcontext.pstate = UINT64_C(0xb1b2b3b4b5b6b7b8);
    memcpy(frame.uc.mcontext.reserved, &fpsimd, sizeof(fpsimd));

    const byte_t *wire = (const byte_t *) &frame;
    assert(load_dword(wire, 0) == 11);
    assert((sdword_t) load_dword(wire, 4) == -2);
    assert(load_dword(wire, 8) == 1);
    assert(wire[16] == UINT8_C(0xa5));
    assert(load_qword(wire, 128) == frame.uc.flags);
    assert(load_qword(wire, 136) == frame.uc.link);
    assert(load_qword(wire, 144) == frame.uc.stack.sp);
    assert((sdword_t) load_dword(wire, 152) == frame.uc.stack.flags);
    assert(load_qword(wire, 160) == frame.uc.stack.size);
    assert(load_qword(wire, 168) == frame.uc.sigmask);
    assert(load_qword(wire, 304) == frame.uc.mcontext.fault_address);
    assert(load_qword(wire, 312) == frame.uc.mcontext.regs[0]);
    assert(load_qword(wire, 552) == frame.uc.mcontext.regs[30]);
    assert(load_qword(wire, 560) == frame.uc.mcontext.sp);
    assert(load_qword(wire, 568) == frame.uc.mcontext.pc);
    assert(load_qword(wire, 576) == frame.uc.mcontext.pstate);
    assert(load_dword(wire, 592) == AARCH64_LINUX_FPSIMD_MAGIC);
    assert(load_dword(wire, 596) == sizeof(fpsimd));
    assert(load_dword(wire, 600) == fpsimd.fpsr);
    assert(load_dword(wire, 604) == fpsimd.fpcr);
    assert(wire[592 + 527] == UINT8_C(0xd1));

    const byte_t *fpsimd_wire = (const byte_t *) &fpsimd;
    assert(load_dword(fpsimd_wire, 0) == AARCH64_LINUX_FPSIMD_MAGIC);
    assert(load_dword(fpsimd_wire, 4) == sizeof(fpsimd));
    assert(load_dword(fpsimd_wire, 8) == fpsimd.fpsr);
    assert(load_dword(fpsimd_wire, 12) == fpsimd.fpcr);
    assert(fpsimd_wire[527] == UINT8_C(0xd1));
    return 0;
}
