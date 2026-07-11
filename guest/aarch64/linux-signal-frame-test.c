#include <assert.h>
#include <string.h>

#include "guest/aarch64/linux-signal-frame.h"
#include "guest/linux/user-memory.h"
#include "guest/memory/page-table.h"

#define STACK_BASE UINT64_C(0x0000700000000000)
#define STACK_TOP (STACK_BASE + 3 * GUEST_MEMORY_PAGE_SIZE)
#define FAULT_STACK_BASE UINT64_C(0x0000710000000000)

struct frame_record {
    qword_t fp;
    qword_t lr;
};

static void map_page(struct guest_page_table *table,
        guest_addr_t address, unsigned permissions) {
    byte_t *host_page;
    assert(guest_page_table_map(table, address,
            permissions, &host_page) == GUEST_PAGE_TABLE_OK);
}

static struct cpu_state make_interrupted_cpu(void) {
    struct cpu_state cpu = {
        .cycle = 99,
        .sp = STACK_TOP + 7,
        .pc = UINT64_C(0x0000400012345678),
        .nzcv = UINT32_C(0xa0000000),
        .fpcr = UINT32_C(0x01000000),
        .fpsr = UINT32_C(0x08000000),
        .tpidr_el0 = UINT64_C(0x1122334455667788),
        .exclusive = {
            .address = UINT64_C(0x0000500000000000),
            .size = 8,
            .valid = true,
        },
        .segfault_addr = UINT64_C(0x0000600000000123),
    };
    for (byte_t i = 0; i < 31; i++)
        cpu.x[i] = UINT64_C(0x1000000000000000) + i;
    for (byte_t reg = 0; reg < 32; reg++) {
        for (byte_t index = 0; index < 16; index++)
            cpu.v[reg].b[index] = (byte_t) (reg * 16 + index);
    }
    return cpu;
}

static struct aarch64_linux_signal_delivery make_delivery(void) {
    struct aarch64_linux_signal_delivery delivery = {
        .signal = 11,
        .handler = UINT64_C(0x00004000abcdef00),
        .restorer = UINT64_C(0x00004000fedcba00),
        .action_flags = AARCH64_LINUX_SA_SIGINFO,
        .blocked_mask = UINT64_C(0x1020304050607080),
        .altstack = {
            .sp = STACK_BASE,
            .flags = 1,
            .reserved = UINT32_C(0xa5a5a5a5),
            .size = 3 * GUEST_MEMORY_PAGE_SIZE,
        },
        .stack_top = STACK_TOP + 7,
        .fault_address = UINT64_C(0x0000600000000123),
    };
    delivery.info.signo = 9;
    delivery.info.error = -2;
    delivery.info.code = 1;
    delivery.info.payload[0] = UINT8_C(0xa5);
    return delivery;
}

static struct aarch64_linux_rt_sigframe read_frame(
        struct guest_tlb *tlb, guest_addr_t address) {
    struct aarch64_linux_rt_sigframe frame;
    struct guest_memory_fault fault;
    assert(guest_linux_copy_from_user(
            tlb, address, &frame, sizeof(frame), &fault));
    return frame;
}

static void write_frame(struct guest_tlb *tlb, guest_addr_t address,
        const struct aarch64_linux_rt_sigframe *frame) {
    struct guest_memory_fault fault;
    assert(guest_linux_copy_to_user(
            tlb, address, frame, sizeof(*frame), &fault));
}

static void assert_bad_decode(const struct cpu_state *handler_cpu,
        struct guest_tlb *tlb,
        enum aarch64_linux_signal_frame_status expected) {
    struct aarch64_linux_signal_resume resume;
    memset(&resume, 0xcc, sizeof(resume));
    struct aarch64_linux_signal_resume sentinel = resume;
    struct guest_memory_fault fault;
    assert(aarch64_linux_decode_rt_sigreturn(
            handler_cpu, tlb, &resume, &fault) == expected);
    assert(memcmp(&resume, &sentinel, sizeof(resume)) == 0);
}

int main(void) {
    struct guest_page_table table;
    assert(guest_page_table_init(&table, 48));
    for (guest_addr_t page = STACK_BASE; page < STACK_TOP;
            page += GUEST_MEMORY_PAGE_SIZE) {
        map_page(&table, page, GUEST_MEMORY_READ | GUEST_MEMORY_WRITE);
    }
    struct guest_tlb tlb;
    guest_tlb_init(&tlb, &table.address_space);

    struct cpu_state interrupted = make_interrupted_cpu();
    struct cpu_state interrupted_sentinel = interrupted;
    struct aarch64_linux_signal_delivery delivery = make_delivery();
    guest_addr_t expected_record = STACK_TOP - 16;
    guest_addr_t expected_frame = expected_record -
            sizeof(struct aarch64_linux_rt_sigframe);
    delivery.stack_bottom = expected_frame;
    struct cpu_state handler_cpu;
    memset(&handler_cpu, 0xcc, sizeof(handler_cpu));
    guest_addr_t frame_address = UINT64_MAX;
    struct guest_memory_fault fault;
    assert(aarch64_linux_build_rt_sigframe(&interrupted, &tlb,
            &delivery, &handler_cpu, &frame_address, &fault) ==
            AARCH64_LINUX_SIGNAL_FRAME_OK);
    assert(memcmp(&interrupted, &interrupted_sentinel,
            sizeof(interrupted)) == 0);

    assert(frame_address == expected_frame);
    assert(fault.kind == GUEST_MEMORY_FAULT_NONE);
    assert(handler_cpu.x[0] == (qword_t) delivery.signal);
    assert(handler_cpu.x[1] == frame_address);
    assert(handler_cpu.x[2] == frame_address + 128);
    assert(handler_cpu.x[3] == interrupted.x[3]);
    assert(handler_cpu.x[29] == expected_record);
    assert(handler_cpu.x[30] == delivery.restorer);
    assert(handler_cpu.sp == frame_address);
    assert(handler_cpu.pc == delivery.handler);
    assert(handler_cpu.nzcv == interrupted.nzcv);
    assert(!handler_cpu.exclusive.valid);

    byte_t image_before[sizeof(struct aarch64_linux_rt_sigframe) +
            sizeof(struct frame_record)];
    byte_t image_after[sizeof(image_before)];
    assert(guest_linux_copy_from_user(&tlb, expected_frame,
            image_before, sizeof(image_before), &fault));
    struct aarch64_linux_signal_delivery below_bottom = delivery;
    below_bottom.signal++;
    below_bottom.blocked_mask ^= UINT64_MAX;
    // 帧首地址低于非零栈下界一字节时，必须在任何用户写入前拒绝。
    below_bottom.stack_bottom = expected_frame + 1;
    struct cpu_state rejected_cpu;
    memset(&rejected_cpu, 0x5a, sizeof(rejected_cpu));
    struct cpu_state rejected_cpu_before = rejected_cpu;
    guest_addr_t rejected_address = UINT64_C(0xdeadbeefdeadbeef);
    assert(aarch64_linux_build_rt_sigframe(&interrupted, &tlb,
            &below_bottom, &rejected_cpu, &rejected_address, &fault) ==
            AARCH64_LINUX_SIGNAL_FRAME_BAD_FRAME);
    assert(memcmp(&rejected_cpu, &rejected_cpu_before,
            sizeof(rejected_cpu)) == 0);
    assert(rejected_address == UINT64_C(0xdeadbeefdeadbeef));
    assert(guest_linux_copy_from_user(&tlb, expected_frame,
            image_after, sizeof(image_after), &fault));
    assert(memcmp(image_after, image_before, sizeof(image_before)) == 0);
    delivery.stack_bottom = 0;

    struct aarch64_linux_rt_sigframe frame =
            read_frame(&tlb, frame_address);
    assert(frame.info.signo == delivery.signal);
    assert(frame.info.error == delivery.info.error);
    assert(frame.info.code == delivery.info.code);
    assert(frame.info.payload[0] == delivery.info.payload[0]);
    assert(frame.uc.flags == 0 && frame.uc.link == 0);
    assert(frame.uc.stack.sp == delivery.altstack.sp);
    assert(frame.uc.stack.flags == delivery.altstack.flags);
    assert(frame.uc.stack.reserved == 0);
    assert(frame.uc.stack.size == delivery.altstack.size);
    assert(frame.uc.sigmask == delivery.blocked_mask);
    assert(frame.uc.mcontext.fault_address == delivery.fault_address);
    assert(memcmp(frame.uc.mcontext.regs,
            interrupted.x, sizeof(interrupted.x)) == 0);
    assert(frame.uc.mcontext.sp == interrupted.sp);
    assert(frame.uc.mcontext.pc == interrupted.pc);
    assert(frame.uc.mcontext.pstate == interrupted.nzcv);

    struct aarch64_linux_fpsimd_context fpsimd;
    memcpy(&fpsimd, frame.uc.mcontext.reserved, sizeof(fpsimd));
    assert(fpsimd.head.magic == AARCH64_LINUX_FPSIMD_MAGIC);
    assert(fpsimd.head.size == sizeof(fpsimd));
    assert(fpsimd.fpsr == interrupted.fpsr);
    assert(fpsimd.fpcr == interrupted.fpcr);
    assert(memcmp(fpsimd.vregs,
            interrupted.v, sizeof(interrupted.v)) == 0);
    struct aarch64_linux_ctx terminator;
    memcpy(&terminator, frame.uc.mcontext.reserved + sizeof(fpsimd),
            sizeof(terminator));
    assert(terminator.magic == 0 && terminator.size == 0);

    struct frame_record record;
    assert(guest_linux_copy_from_user(&tlb, expected_record,
            &record, sizeof(record), &fault));
    assert(record.fp == interrupted.x[29]);
    assert(record.lr == interrupted.x[30]);

    struct cpu_state returning_cpu = handler_cpu;
    returning_cpu.cycle = 777;
    returning_cpu.tpidr_el0 = UINT64_C(0x8877665544332211);
    returning_cpu.exclusive.valid = true;
    struct aarch64_linux_signal_resume resume;
    assert(aarch64_linux_decode_rt_sigreturn(
            &returning_cpu, &tlb, &resume, &fault) ==
            AARCH64_LINUX_SIGNAL_FRAME_OK);
    assert(memcmp(resume.cpu.x, interrupted.x,
            sizeof(interrupted.x)) == 0);
    assert(resume.cpu.sp == interrupted.sp);
    assert(resume.cpu.pc == interrupted.pc);
    assert(resume.cpu.nzcv == interrupted.nzcv);
    assert(memcmp(resume.cpu.v, interrupted.v,
            sizeof(interrupted.v)) == 0);
    assert(resume.cpu.fpsr == interrupted.fpsr);
    assert(resume.cpu.fpcr == interrupted.fpcr);
    assert(resume.cpu.cycle == returning_cpu.cycle);
    assert(resume.cpu.tpidr_el0 == returning_cpu.tpidr_el0);
    assert(!resume.cpu.exclusive.valid);
    assert(resume.blocked_mask == delivery.blocked_mask);
    assert(resume.altstack.sp == delivery.altstack.sp);
    assert(resume.altstack.flags == delivery.altstack.flags);
    assert(resume.altstack.reserved == 0);
    assert(resume.altstack.size == delivery.altstack.size);

    struct cpu_state misaligned_cpu = handler_cpu;
    misaligned_cpu.sp += 8;
    assert_bad_decode(&misaligned_cpu, &tlb,
            AARCH64_LINUX_SIGNAL_FRAME_BAD_FRAME);

    struct aarch64_linux_rt_sigframe bad_frame = frame;
    bad_frame.uc.mcontext.pstate |= 1;
    write_frame(&tlb, frame_address, &bad_frame);
    assert_bad_decode(&handler_cpu, &tlb,
            AARCH64_LINUX_SIGNAL_FRAME_BAD_FRAME);

    bad_frame = frame;
    memset(bad_frame.uc.mcontext.reserved, 0, 16);
    write_frame(&tlb, frame_address, &bad_frame);
    assert_bad_decode(&handler_cpu, &tlb,
            AARCH64_LINUX_SIGNAL_FRAME_BAD_FRAME);

    bad_frame = frame;
    struct aarch64_linux_ctx *head =
            (struct aarch64_linux_ctx *) bad_frame.uc.mcontext.reserved;
    head->size = 512;
    write_frame(&tlb, frame_address, &bad_frame);
    assert_bad_decode(&handler_cpu, &tlb,
            AARCH64_LINUX_SIGNAL_FRAME_BAD_FRAME);

    bad_frame = frame;
    head = (struct aarch64_linux_ctx *)
            bad_frame.uc.mcontext.reserved;
    head->magic = UINT32_C(0x12345678);
    write_frame(&tlb, frame_address, &bad_frame);
    assert_bad_decode(&handler_cpu, &tlb,
            AARCH64_LINUX_SIGNAL_FRAME_BAD_FRAME);

    bad_frame = frame;
    head = (struct aarch64_linux_ctx *)
            bad_frame.uc.mcontext.reserved;
    head->size = 8;
    write_frame(&tlb, frame_address, &bad_frame);
    assert_bad_decode(&handler_cpu, &tlb,
            AARCH64_LINUX_SIGNAL_FRAME_BAD_FRAME);

    bad_frame = frame;
    head = (struct aarch64_linux_ctx *)
            bad_frame.uc.mcontext.reserved;
    head->size = 4112;
    write_frame(&tlb, frame_address, &bad_frame);
    assert_bad_decode(&handler_cpu, &tlb,
            AARCH64_LINUX_SIGNAL_FRAME_BAD_FRAME);

    bad_frame = frame;
    memcpy(bad_frame.uc.mcontext.reserved + sizeof(fpsimd),
            &fpsimd, sizeof(fpsimd));
    write_frame(&tlb, frame_address, &bad_frame);
    assert_bad_decode(&handler_cpu, &tlb,
            AARCH64_LINUX_SIGNAL_FRAME_BAD_FRAME);

    bad_frame = frame;
    head = (struct aarch64_linux_ctx *)
            (bad_frame.uc.mcontext.reserved + sizeof(fpsimd));
    head->magic = UINT32_C(0x87654321);
    head->size = 16;
    write_frame(&tlb, frame_address, &bad_frame);
    assert_bad_decode(&handler_cpu, &tlb,
            AARCH64_LINUX_SIGNAL_FRAME_BAD_FRAME);

    bad_frame = frame;
    memset(bad_frame.uc.mcontext.reserved + sizeof(fpsimd), 0xff,
            sizeof(bad_frame.uc.mcontext.reserved) - sizeof(fpsimd));
    write_frame(&tlb, frame_address, &bad_frame);
    assert_bad_decode(&handler_cpu, &tlb,
            AARCH64_LINUX_SIGNAL_FRAME_BAD_FRAME);

    bad_frame = frame;
    head = (struct aarch64_linux_ctx *)
            (bad_frame.uc.mcontext.reserved + sizeof(fpsimd));
    head->size = 16;
    write_frame(&tlb, frame_address, &bad_frame);
    assert_bad_decode(&handler_cpu, &tlb,
            AARCH64_LINUX_SIGNAL_FRAME_BAD_FRAME);
    write_frame(&tlb, frame_address, &frame);

    struct cpu_state unmapped_return = handler_cpu;
    unmapped_return.sp = UINT64_C(0x0000720000000000);
    assert_bad_decode(&unmapped_return, &tlb,
            AARCH64_LINUX_SIGNAL_FRAME_MEMORY_FAULT);

    struct cpu_state no_siginfo_cpu = interrupted;
    no_siginfo_cpu.sp = STACK_TOP - GUEST_MEMORY_PAGE_SIZE;
    no_siginfo_cpu.x[1] = UINT64_C(0x123456789abcdef0);
    no_siginfo_cpu.x[2] = UINT64_C(0x0fedcba987654321);
    delivery.action_flags = 0;
    delivery.stack_top = no_siginfo_cpu.sp;
    assert(aarch64_linux_build_rt_sigframe(&no_siginfo_cpu, &tlb,
            &delivery, &handler_cpu, &frame_address, &fault) ==
            AARCH64_LINUX_SIGNAL_FRAME_OK);
    assert(handler_cpu.x[1] == no_siginfo_cpu.x[1]);
    assert(handler_cpu.x[2] == no_siginfo_cpu.x[2]);
    frame = read_frame(&tlb, frame_address);
    const struct aarch64_linux_siginfo zero_info = {0};
    assert(memcmp(&frame.info, &zero_info, sizeof(zero_info)) == 0);

    struct cpu_state output_sentinel;
    memset(&output_sentinel, 0x5a, sizeof(output_sentinel));
    struct cpu_state output_before = output_sentinel;
    guest_addr_t address_sentinel = UINT64_C(0xdeadbeefdeadbeef);
    delivery.stack_top = 8;
    assert(aarch64_linux_build_rt_sigframe(&interrupted, &tlb,
            &delivery, &output_sentinel, &address_sentinel, &fault) ==
            AARCH64_LINUX_SIGNAL_FRAME_BAD_FRAME);
    assert(memcmp(&output_sentinel, &output_before,
            sizeof(output_sentinel)) == 0);
    assert(address_sentinel == UINT64_C(0xdeadbeefdeadbeef));

    struct guest_page_table fault_table;
    assert(guest_page_table_init(&fault_table, 48));
    map_page(&fault_table, FAULT_STACK_BASE,
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE);
    map_page(&fault_table, FAULT_STACK_BASE + GUEST_MEMORY_PAGE_SIZE,
            GUEST_MEMORY_READ);
    struct guest_tlb fault_tlb;
    guest_tlb_init(&fault_tlb, &fault_table.address_space);
    delivery.stack_top = FAULT_STACK_BASE + 2 * GUEST_MEMORY_PAGE_SIZE;
    output_sentinel = output_before;
    address_sentinel = UINT64_C(0xdeadbeefdeadbeef);
    assert(aarch64_linux_build_rt_sigframe(&interrupted, &fault_tlb,
            &delivery, &output_sentinel, &address_sentinel, &fault) ==
            AARCH64_LINUX_SIGNAL_FRAME_MEMORY_FAULT);
    assert(fault.kind == GUEST_MEMORY_FAULT_PERMISSION);
    assert(memcmp(&output_sentinel, &output_before,
            sizeof(output_sentinel)) == 0);
    assert(address_sentinel == UINT64_C(0xdeadbeefdeadbeef));

    struct cpu_state partial_return = handler_cpu;
    partial_return.sp = FAULT_STACK_BASE + GUEST_MEMORY_PAGE_SIZE - 64;
    memset(&resume, 0xcc, sizeof(resume));
    struct aarch64_linux_signal_resume resume_before = resume;
    assert(aarch64_linux_decode_rt_sigreturn(
            &partial_return, &fault_tlb, &resume, &fault) ==
            AARCH64_LINUX_SIGNAL_FRAME_MEMORY_FAULT);
    assert(memcmp(&resume, &resume_before, sizeof(resume)) == 0);
    assert(fault.address == FAULT_STACK_BASE +
            2 * GUEST_MEMORY_PAGE_SIZE);
    assert(fault.access == GUEST_MEMORY_READ);
    assert(fault.kind == GUEST_MEMORY_FAULT_UNMAPPED);

    guest_page_table_destroy(&fault_table);
    guest_page_table_destroy(&table);
    return 0;
}
