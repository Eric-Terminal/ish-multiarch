#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "guest/aarch64/runner.h"
#include "guest/aarch64/threaded-profile.h"
#include "aarch64-backend-config.h"

#define CODE_PAGE UINT64_C(0x0000456789abc000)
#define DATA_PAGE (CODE_PAGE + GUEST_MEMORY_PAGE_SIZE)

#define INSTRUCTION_NOP UINT32_C(0xd503201f)
#define INSTRUCTION_ADD_X0 UINT32_C(0x91000400)
#define INSTRUCTION_STR_X0 UINT32_C(0xf9000020)
#define INSTRUCTION_LDR_X2 UINT32_C(0xf9400022)
#define INSTRUCTION_ADD_X3 UINT32_C(0x8b000043)
#define INSTRUCTION_ADDS_X3 UINT32_C(0xab000043)
#define INSTRUCTION_SUB_X5_X6_X1 UINT32_C(0xcb0100c5)
#define INSTRUCTION_SUB_W0_W4_W3 UINT32_C(0x4b030080)
#define INSTRUCTION_CCMP_W0_W1_4_NE UINT32_C(0x7a411004)
#define INSTRUCTION_CCMP_W0_W19_4_NE UINT32_C(0x7a531004)
#define INSTRUCTION_CCMN_X0_1_0_EQ UINT32_C(0xba410800)
#define INSTRUCTION_SUBS_XZR_X2_X5 UINT32_C(0xeb05005f)
#define INSTRUCTION_SUBS_WZR_W0_W4 UINT32_C(0x6b04001f)
#define INSTRUCTION_ADRP_X1_PLUS_86_PAGES UINT32_C(0xd00002a1)
#define INSTRUCTION_ADRP_X2_PLUS_122_PAGES UINT32_C(0xd00003c2)
#define INSTRUCTION_ADR_X1_PLUS_12 UINT32_C(0x10000061)
#define INSTRUCTION_AND_SP_X0_FFFFFFFFFFFFFFF0 UINT32_C(0x927cec1f)
#define INSTRUCTION_AND_W0_W3_7FFFFFFF UINT32_C(0x12007860)
#define INSTRUCTION_ORR_W10_W10_1 UINT32_C(0x3200014a)
#define INSTRUCTION_ORR_W5_WZR_7FFFFFFE UINT32_C(0x321f77e5)
#define INSTRUCTION_EOR_W0_W4_1 UINT32_C(0x52000080)
#define INSTRUCTION_EOR_X10_X11_0000FFFF0000FFFF UINT32_C(0xd2003d6a)
#define INSTRUCTION_ANDS_XZR_X12_8000000000000001 UINT32_C(0xf241059f)
#define INSTRUCTION_AND_X2_X2_X6 UINT32_C(0x8a060042)
#define INSTRUCTION_AND_W1_W24_W1 UINT32_C(0x0a010301)
#define INSTRUCTION_ANDS_XZR_X0_X5 UINT32_C(0xea05001f)
#define INSTRUCTION_ANDS_WZR_W0_W2 UINT32_C(0x6a02001f)
#define INSTRUCTION_UBFM_X1_X1_1_63 UINT32_C(0xd341fc21)
#define INSTRUCTION_UBFM_W2_W4_28_27 UINT32_C(0x531c6c82)
#define INSTRUCTION_SXTW_X2_W2 UINT32_C(0x93407c42)
#define INSTRUCTION_SXTW_X0_W19 UINT32_C(0x93407e60)
#define INSTRUCTION_SXTB_W3_W3 UINT32_C(0x13001c63)
#define INSTRUCTION_SXTB_X7_WZR UINT32_C(0x93401fe7)
#define INSTRUCTION_SXTH_WZR_W8 UINT32_C(0x13003d1f)
#define INSTRUCTION_SXTH_X9_W10 UINT32_C(0x93403d49)
#define INSTRUCTION_ASR_W2_W0_1 UINT32_C(0x13017c02)
#define INSTRUCTION_ASR_X1_X1_1 UINT32_C(0x9341fc21)
#define INSTRUCTION_SBFIZ_W15_W16_8_8 UINT32_C(0x13181e0f)
#define INSTRUCTION_SBFIZ_X27_X23_3_32 UINT32_C(0x937d7efb)
#define INSTRUCTION_SBFX_W19_W20_8_8 UINT32_C(0x13083e93)
#define INSTRUCTION_SBFX_X1_X1_8_16 UINT32_C(0x93485c21)
#define INSTRUCTION_BFM_X0_X1_52_51 UINT32_C(0xb374cc20)
#define INSTRUCTION_BFM_W2_W0_0_4 UINT32_C(0x33001002)
#define INSTRUCTION_LSRV_W4_W10_W4 UINT32_C(0x1ac42544)
#define INSTRUCTION_LSRV_W0_W0_W2 UINT32_C(0x1ac22400)
#define INSTRUCTION_LSRV_X21_X22_X23 UINT32_C(0x9ad726d5)
#define INSTRUCTION_UDIV_W0_W1_W2 UINT32_C(0x1ac20820)
#define INSTRUCTION_SDIV_X9_X10_X11 UINT32_C(0x9acb0d49)
#define INSTRUCTION_LSLV_W12_W13_W14 UINT32_C(0x1ace21ac)
#define INSTRUCTION_ASRV_W2_W23_W2 UINT32_C(0x1ac22ae2)
#define INSTRUCTION_RORV_X0_X1_X2 UINT32_C(0x9ac22c20)
#define INSTRUCTION_LSLV_X5_X7_X1 UINT32_C(0x9ac120e5)
#define INSTRUCTION_LSLV_W0_W0_W1 UINT32_C(0x1ac12000)
#define INSTRUCTION_MADD_W0_W1_W2_W3 UINT32_C(0x1b020c20)
#define INSTRUCTION_MADD_X4_X5_X6_X7 UINT32_C(0x9b061ca4)
#define INSTRUCTION_MUL_X6_X6_X0 UINT32_C(0x9b007cc6)
#define INSTRUCTION_MUL_W2_W0_W2 UINT32_C(0x1b027c02)
#define INSTRUCTION_MSUB_W6_W6_W5_W10 UINT32_C(0x1b05a8c6)
#define INSTRUCTION_SMADDL_X0_W0_W1_XZR UINT32_C(0x9b217c00)
#define INSTRUCTION_SMADDL_X6_W7_W8_X9 UINT32_C(0x9b2824e6)
#define INSTRUCTION_SMSUBL_X10_W11_W12_X13 UINT32_C(0x9b2cb56a)
#define INSTRUCTION_UMADDL_X1_W0_W11_XZR UINT32_C(0x9bab7c01)
#define INSTRUCTION_UMADDL_X14_W15_W16_X17 UINT32_C(0x9bb045ee)
#define INSTRUCTION_UMSUBL_X18_W19_W20_X21 UINT32_C(0x9bb4d672)
#define INSTRUCTION_SMULH_X8_X9_X10 UINT32_C(0x9b4a7d28)
#define INSTRUCTION_UMULH_X11_X12_X13 UINT32_C(0x9bcd7d8b)
#define INSTRUCTION_EXTR_W3_W1_W1_19 UINT32_C(0x13814c23)
#define INSTRUCTION_EXTR_W2_W1_W1_29 UINT32_C(0x13817422)
#define INSTRUCTION_LDR_X4_X3_X2_LSL_3 UINT32_C(0xf8627864)
#define INSTRUCTION_LDR_X3_X0_X1 UINT32_C(0xf8616803)
#define INSTRUCTION_STR_X5_X3 UINT32_C(0xf9000065)
#define INSTRUCTION_STR_W0_X1_32 UINT32_C(0xb9002020)
#define INSTRUCTION_LDXR_X2_X1 UINT32_C(0xc85f7c22)
#define INSTRUCTION_STXR_W4_X3_X1 UINT32_C(0xc8047c23)
#define INSTRUCTION_STXR_W6_X3_X1 UINT32_C(0xc8067c23)
#define INSTRUCTION_LDP_X6_X1_SP_400 UINT32_C(0xa95907e6)
#define INSTRUCTION_LDP_X29_X30_SP_POST_16 UINT32_C(0xa8c17bfd)
#define INSTRUCTION_LDP_X4_X5_X1 UINT32_C(0xa9401424)
#define INSTRUCTION_STP_X29_X30_SP_PRE_352 UINT32_C(0xa9aa7bfd)
#define INSTRUCTION_STP_X29_X30_SP_PRE_16 UINT32_C(0xa9bf7bfd)
#define INSTRUCTION_STP_X5_X2_SP_16 UINT32_C(0xa9010be5)
#define INSTRUCTION_LDP_Q30_Q31_X1 UINT32_C(0xad407c3e)
#define INSTRUCTION_LDP_Q30_Q29_SP_80 UINT32_C(0xad42f7fe)
#define INSTRUCTION_MOVI_V27_4S_0 UINT32_C(0x4f00041b)
#define INSTRUCTION_MOVI_V31_4S_0 UINT32_C(0x4f00041f)
#define INSTRUCTION_MVNI_V9_8H UINT32_C(0x6f04a749)
#define INSTRUCTION_ORR_V12_8H UINT32_C(0x4f00b64c)
#define INSTRUCTION_BIC_V14_4H UINT32_C(0x2f0296ce)
#define INSTRUCTION_LDXP_X4_X5_X1 UINT32_C(0xc87f1424)
#define INSTRUCTION_STR_X6_X7_X3_LSL_3 UINT32_C(0xf82378e6)
#define INSTRUCTION_STR_XZR_X1_X3_LSL_3 UINT32_C(0xf823783f)
#define INSTRUCTION_STR_XZR_X2_POST_8 UINT32_C(0xf800845f)
#define INSTRUCTION_STURH_WZR_X7_NEG_2 UINT32_C(0x781fe0ff)
#define INSTRUCTION_LDTR_X4_X3 UINT32_C(0xf8400864)
#define INSTRUCTION_STTR_W5_X3_4 UINT32_C(0xb8004865)
#define INSTRUCTION_LDUR_H2_SP_255 UINT32_C(0x7c4ff3e2)
#define INSTRUCTION_STUR_B0_X1_NEG_256 UINT32_C(0x3c100020)
#define INSTRUCTION_STUR_Q31_X0_56 UINT32_C(0x3c83801f)
#define INSTRUCTION_STUR_Q0_X4_NEG_16 UINT32_C(0x3c9f0080)
#define INSTRUCTION_CSEL_X1_X0_X1_EQ UINT32_C(0x9a810001)
#define INSTRUCTION_CSEL_W19_W19_W0_NE UINT32_C(0x1a801273)
#define INSTRUCTION_ANDS_W22_W2_7FFFFFFF UINT32_C(0x72007856)
#define INSTRUCTION_TST_W1_FF UINT32_C(0x72001c3f)
#define INSTRUCTION_STP_Q0_Q0_X3_32 UINT32_C(0xad010060)
#define INSTRUCTION_STP_Q31_Q31_X0 UINT32_C(0xad007c1f)
#define INSTRUCTION_STP_Q31_Q30_SP_PRE_32 UINT32_C(0xadbf7bff)
#define INSTRUCTION_STP_Q0_Q1_SP_POST_32 UINT32_C(0xac8107e0)
#define INSTRUCTION_CSINC_X3_XZR_XZR_NE UINT32_C(0x9a9f17e3)
#define INSTRUCTION_CSINC_W20_WZR_WZR_NE UINT32_C(0x1a9f17f4)
#define INSTRUCTION_REV_W2_W2 UINT32_C(0x5ac00842)
#define INSTRUCTION_REV_W0_W0 UINT32_C(0x5ac00800)
#define INSTRUCTION_REV32_X4_X5 UINT32_C(0xdac008a4)
#define INSTRUCTION_CLZ_W2_W2 UINT32_C(0x5ac01042)
#define INSTRUCTION_CLZ_W0_W0 UINT32_C(0x5ac01000)
#define INSTRUCTION_ADD_X0_X4_W0_UXTW_3 UINT32_C(0x8b204c80)
#define INSTRUCTION_ADD_X0_X21_W20_SXTW_3 UINT32_C(0x8b34cea0)
#define INSTRUCTION_ADDS_X22_SP_W23_SXTW_2 UINT32_C(0xab37cbf6)
#define INSTRUCTION_SUB_SP_SP_X0 UINT32_C(0xcb2063ff)
#define INSTRUCTION_SUB_X1_X1_W2_SXTW UINT32_C(0xcb22c021)
#define INSTRUCTION_SUBS_X0_X1_W2_UXTB_4 UINT32_C(0xeb221020)
#define INSTRUCTION_SUBS_WZR_W2_W1_UXTB UINT32_C(0x6b21005f)
#define INSTRUCTION_LDR_X2_X3_POST_8 UINT32_C(0xf8408462)
#define INSTRUCTION_LDURB_W1_X0_NEG_4 UINT32_C(0x385fc001)
#define INSTRUCTION_ORR_X3_XZR_X0 UINT32_C(0xaa0003e3)
#define INSTRUCTION_EOR_X5_X3_X6 UINT32_C(0xca060065)
#define INSTRUCTION_SVC UINT32_C(0xd4000001)
#define INSTRUCTION_USHLL_V31 UINT32_C(0x2f20a7ff)
#define INSTRUCTION_LDAR_X2_X1 UINT32_C(0xc8dffc22)
#define INSTRUCTION_STLR_X0_X1 UINT32_C(0xc89ffc20)
#define INSTRUCTION_PRFM_X0 UINT32_C(0xf9800000)
#define INSTRUCTION_STR_Q30_X21_X0 UINT32_C(0x3ca06abe)
#define INSTRUCTION_LDR_Q11_X12_X13_SXTX_4 UINT32_C(0x3cedf98b)
#define INSTRUCTION_STR_Q0_X0 UINT32_C(0x3d800000)
#define INSTRUCTION_STR_Q31_X0_32 UINT32_C(0x3d80081f)
#define INSTRUCTION_LDR_Q31_X0_D60 UINT32_C(0x3dc3581f)
#define INSTRUCTION_LD4_V28_X0 UINT32_C(0x4c40081c)
#define INSTRUCTION_USHR_D28_D31_39 UINT32_C(0x7f5907fc)
#define INSTRUCTION_XTN_V28_4H_V28_4S UINT32_C(0x0e612b9c)
#define INSTRUCTION_FCVT_D0_S0 UINT32_C(0x1e22c000)
#define INSTRUCTION_MOV_S15_V31_S3 UINT32_C(0x5e1c07ef)
#define INSTRUCTION_FDIV_S30_S0_S30 UINT32_C(0x1e3e181e)
#define INSTRUCTION_UCVTF_D31_D31 UINT32_C(0x7e61dbff)
#define INSTRUCTION_FCVTZU_X1_D31 UINT32_C(0x9e7903e1)
#define INSTRUCTION_SSHR_V30_2S_V31_2S_8 UINT32_C(0x0f3807fe)
#define INSTRUCTION_LD1_V31_B4_X1 UINT32_C(0x0d40103f)
#define INSTRUCTION_FCSEL_D0_D0_D31_MI UINT32_C(0x1e7f4c00)
#define INSTRUCTION_FRINTM_D0_D0 UINT32_C(0x1e654000)
#define INSTRUCTION_FNEG_D0_D0 UINT32_C(0x1e614000)
#define INSTRUCTION_EXT_V0_V27_V30_8 UINT32_C(0x6e1e4360)
#define INSTRUCTION_MVN_V31_V30 UINT32_C(0x6e205bdf)
#define INSTRUCTION_USHR_V30_2D_V30_2D_6 UINT32_C(0x6f7a07de)
#define INSTRUCTION_ADDP_D31_V29_2D UINT32_C(0x5ef1bbbf)
/* GCC cc1 实际触发过的融合乘加指令。 */
#define INSTRUCTION_FMADD_D31_D0_D15_D31 UINT32_C(0x1f4f7c1f)
/* GCC cc1 实际触发过的无符号配对加长指令。 */
#define INSTRUCTION_UADDLP_V31_8H_V31_16B UINT32_C(0x6e202bff)
/* GCC cc1 实际触发过的无符号逐 lane 可变移位指令。 */
#define INSTRUCTION_USHL_V31_8H_V31_8H_V24_8H UINT32_C(0x6e7847ff)
/* GCC cc1 实际触发过的向量横向求和指令。 */
#define INSTRUCTION_ADDV_H31_V31_8H UINT32_C(0x4e71bbff)
/* GCC cc1 实际触发过的逐 lane 二补数取负指令。 */
#define INSTRUCTION_NEG_V29_2S_V31_2S UINT32_C(0x2ea0bbfd)
/* GCC cc1 实际触发过的逐字节位计数指令。 */
#define INSTRUCTION_CNT_V31_8B_V31_8B UINT32_C(0x0e205bff)
/* GCC cc1 实际触发过的标量 64 位整数加法指令。 */
#define INSTRUCTION_ADD_D31_D31_D30 UINT32_C(0x5efe87ff)
/* GCC cc1 实际触发过的两 lane 64 位整数减法指令。 */
#define INSTRUCTION_SUB_V30_2D_V31_2D_V30_2D UINT32_C(0x6efe87fe)
/* GCC cc1 实际触发过的标量 64 位有符号非负比较指令。 */
#define INSTRUCTION_CMGE_D31_D31_ZERO UINT32_C(0x7ee08bff)
/* GCC cc1 实际触发过的两 lane 64 位有符号正数比较指令。 */
#define INSTRUCTION_CMGT_V30_2D_V31_2D_ZERO UINT32_C(0x4ee08bfe)
/* GCC cc1 实际触发过的单 lane 64 位存储指令。 */
#define INSTRUCTION_ST1_V31_D1_X0 UINT32_C(0x4d00841f)
/* GCC cc1 实际触发过的两 lane 64 位左移立即数指令。 */
#define INSTRUCTION_SHL_V31_2D_V31_2D_3 UINT32_C(0x4f4357ff)
/* GCC cc1 实际触发过的 64 位块内 32 位 lane 反序指令。 */
#define INSTRUCTION_REV64_V30_2S_V30_2S UINT32_C(0x0ea00bde)
/* GNU assembler 实际触发过的两结构四 lane 读取指令。 */
#define INSTRUCTION_LD2_V30_V31_4S_X1 UINT32_C(0x4c40883e)
/* GNU assembler 实际触发过的单字节广播读取指令。 */
#define INSTRUCTION_LD1R_V31_16B_X27 UINT32_C(0x4d40c37f)
/* GNU 链接器实际触发过的 32 位块内字节反序指令。 */
#define INSTRUCTION_REV32_V31_16B_V31_16B UINT32_C(0x6e200bff)
/* GCC cc1 的 evrp pass 实际触发过的标量 64 位整数取负指令。 */
#define INSTRUCTION_NEG_D31_D31 UINT32_C(0x7ee0bbff)
/* GCC cc1 的 RTL expand pass 实际触发过的标量算术右移指令。 */
#define INSTRUCTION_SSHR_D29_D31_3 UINT32_C(0x5f7d07fd)
#define INSTRUCTION_UNDEFINED UINT32_C(0)

struct test_memory {
    byte_t primary_code[GUEST_MEMORY_PAGE_SIZE];
    byte_t replacement_code[GUEST_MEMORY_PAGE_SIZE];
    byte_t data[GUEST_MEMORY_PAGE_SIZE];
    byte_t *code_mapping;
    unsigned code_permissions;
    bool code_mapped;
};

struct test_fixture {
    struct test_memory memory;
    struct guest_address_space space;
    struct guest_tlb tlb;
};

static enum guest_memory_fault_kind resolve_test_page(void *opaque,
        guest_addr_t page_base, enum guest_memory_access access,
        struct guest_page_view *view) {
    struct test_memory *memory = opaque;
    use(access);
    if (page_base == CODE_PAGE && memory->code_mapped) {
        *view = (struct guest_page_view) {
            .host_page = memory->code_mapping,
            .permissions = memory->code_permissions,
        };
        return GUEST_MEMORY_FAULT_NONE;
    }
    if (page_base == DATA_PAGE) {
        *view = (struct guest_page_view) {
            .host_page = memory->data,
            .permissions = GUEST_MEMORY_READ | GUEST_MEMORY_WRITE,
        };
        return GUEST_MEMORY_FAULT_NONE;
    }
    return GUEST_MEMORY_FAULT_UNMAPPED;
}

static const struct guest_address_space_ops test_ops = {
    .resolve_page = resolve_test_page,
};

static void init_fixture(struct test_fixture *fixture) {
    *fixture = (struct test_fixture) {0};
    fixture->memory.code_mapping = fixture->memory.primary_code;
    fixture->memory.code_permissions = GUEST_MEMORY_READ |
            GUEST_MEMORY_WRITE | GUEST_MEMORY_EXECUTE;
    fixture->memory.code_mapped = true;
    guest_address_space_init(
            &fixture->space, &test_ops, &fixture->memory, 48);
    guest_tlb_init(&fixture->tlb, &fixture->space);
}

#if defined(__aarch64__)
static void encode_instruction(byte_t bytes[4], dword_t instruction) {
    bytes[0] = (byte_t) instruction;
    bytes[1] = (byte_t) (instruction >> 8);
    bytes[2] = (byte_t) (instruction >> 16);
    bytes[3] = (byte_t) (instruction >> 24);
}

static void write_instruction(struct guest_tlb *tlb,
        guest_addr_t address, dword_t instruction) {
    byte_t bytes[4];
    encode_instruction(bytes, instruction);
    struct guest_memory_fault fault;
    assert(guest_tlb_write(tlb, address, bytes, sizeof(bytes), &fault));
}

static void put_instruction(byte_t *destination, dword_t instruction) {
    encode_instruction(destination, instruction);
}

static void put_value(byte_t *destination, byte_t size, qword_t value) {
    for (byte_t index = 0; index < size; index++)
        destination[index] = (byte_t) (value >> (index * 8));
}

static void assert_step_equal(const struct aarch64_step_result *left,
        const struct aarch64_step_result *right) {
    assert(left->stop == right->stop);
    assert(left->instruction == right->instruction);
    assert(left->fault.address == right->fault.address);
    assert(left->fault.access == right->fault.access);
    assert(left->fault.kind == right->fault.kind);
}

static void assert_cpu_equal(const struct cpu_state *left,
        const struct cpu_state *right) {
    assert(left->mmu == NULL);
    assert(right->mmu == NULL);
    assert(left->cycle == right->cycle);
    for (unsigned index = 0; index < array_size(left->x); index++)
        assert(left->x[index] == right->x[index]);
    assert(left->sp == right->sp);
    assert(left->pc == right->pc);
    assert(left->nzcv == right->nzcv);
    for (unsigned index = 0; index < array_size(left->v); index++) {
        assert(left->v[index].d[0] == right->v[index].d[0]);
        assert(left->v[index].d[1] == right->v[index].d[1]);
    }
    assert(left->fpcr == right->fpcr);
    assert(left->fpsr == right->fpsr);
    assert(left->tpidr_el0 == right->tpidr_el0);
    assert(left->exclusive.address == right->exclusive.address);
    assert(left->exclusive.value_low == right->exclusive.value_low);
    assert(left->exclusive.value_high == right->exclusive.value_high);
    assert((left->exclusive.address_space == NULL) ==
            (right->exclusive.address_space == NULL));
    assert(left->exclusive.mapping_epoch ==
            right->exclusive.mapping_epoch);
    assert(left->exclusive.write_epoch == right->exclusive.write_epoch);
    assert(left->exclusive.sync_identity ==
            right->exclusive.sync_identity);
    assert(left->exclusive.size == right->exclusive.size);
    assert(left->exclusive.pair == right->exclusive.pair);
    assert(left->exclusive.valid == right->exclusive.valid);
    assert(left->segfault_addr == right->segfault_addr);
    assert(left->segfault_was_write == right->segfault_was_write);
    assert(left->trapno == right->trapno);
    assert(left->single_step == right->single_step);
    assert(left->poked_ptr == NULL);
    assert(right->poked_ptr == NULL);
    assert(left->_poked == right->_poked);
}

static void assert_memory_equal(const struct test_memory *left,
        const struct test_memory *right) {
    assert(memcmp(left->primary_code, right->primary_code,
            sizeof(left->primary_code)) == 0);
    assert(memcmp(left->replacement_code, right->replacement_code,
            sizeof(left->replacement_code)) == 0);
    assert(memcmp(left->data, right->data, sizeof(left->data)) == 0);
    assert(left->code_permissions == right->code_permissions);
    assert(left->code_mapped == right->code_mapped);
}
#endif

static void assert_stats(const struct aarch64_runner *runner,
        qword_t hits, qword_t misses, qword_t fast_dispatches,
        qword_t fallbacks) {
    const struct aarch64_threaded_stats *stats =
            aarch64_runner_threaded_stats(runner);
    assert(stats->cache_hits == hits);
    assert(stats->cache_misses == misses);
    assert(stats->fast_dispatches == fast_dispatches);
    assert(stats->c_fallbacks == fallbacks);
}

#if defined(__aarch64__)
static struct aarch64_step_result run_at(struct aarch64_runner *runner,
        struct cpu_state *cpu, guest_addr_t pc) {
    cpu->pc = pc;
    return aarch64_run_one(runner, cpu);
}

static void init_differential_cpu(struct cpu_state *cpu) {
    *cpu = (struct cpu_state) {
        .cycle = 9,
        .sp = DATA_PAGE + 0x300,
        .pc = CODE_PAGE,
        .nzcv = UINT32_C(0xa0000000),
        .fpcr = AARCH64_FPCR_FZ,
        .fpsr = AARCH64_FPSR_IXC,
        .tpidr_el0 = UINT64_C(0x1122334455667788),
        .segfault_addr = UINT64_C(0x778899aabbccddee),
        .segfault_was_write = true,
        .trapno = UINT32_C(0x12345678),
        .single_step = true,
        ._poked = true,
    };
    cpu->x[0] = 7;
    cpu->x[1] = DATA_PAGE + 0x80;
    cpu->x[10] = UINT64_C(0x1020304050607080);
    cpu->x[30] = UINT64_C(0x8877665544332211);
    cpu->v[0].d[0] = UINT64_C(0x0123456789abcdef);
    cpu->v[0].d[1] = UINT64_C(0xfedcba9876543210);
    cpu->v[31].d[0] = UINT64_C(0x13579bdf2468ace0);
    cpu->v[31].d[1] = UINT64_C(0x02468ace13579bdf);
}

static void write_differential_program(struct test_fixture *fixture) {
    const dword_t instructions[] = {
        INSTRUCTION_NOP,
        INSTRUCTION_ADD_X0,
        INSTRUCTION_STR_X0,
        INSTRUCTION_LDR_X2,
        INSTRUCTION_ADD_X3,
        INSTRUCTION_USHLL_V31,
        INSTRUCTION_SVC,
        INSTRUCTION_UNDEFINED,
    };
    for (unsigned index = 0; index < array_size(instructions); index++)
        write_instruction(&fixture->tlb, CODE_PAGE + index * 4,
                instructions[index]);
}

static dword_t encode_add_sub_immediate(bool is_64, bool subtract,
        bool set_flags, qword_t immediate, byte_t rn, byte_t rd) {
    bool shift_12 = immediate > UINT16_C(0xfff);
    if (shift_12) {
        assert((immediate & UINT16_C(0xfff)) == 0);
        immediate >>= 12;
    }
    assert(immediate <= UINT16_C(0xfff));
    assert(rn < 32);
    assert(rd < 32);
    return UINT32_C(0x11000000) |
            ((dword_t) is_64 << 31) |
            ((dword_t) subtract << 30) |
            ((dword_t) set_flags << 29) |
            ((dword_t) shift_12 << 22) |
            ((dword_t) immediate << 10) |
            ((dword_t) rn << 5) | rd;
}

static dword_t encode_add_sub_shifted(bool is_64,
        bool subtract, bool set_flags,
        enum aarch64_shift_type shift_type, byte_t shift,
        byte_t rm, byte_t rn, byte_t rd) {
    byte_t width = is_64 ? 64 : 32;
    assert(shift_type <= AARCH64_SHIFT_ASR);
    assert(shift < width);
    assert(rm < 32);
    assert(rn < 32);
    assert(rd < 32);
    return UINT32_C(0x0b000000) |
            ((dword_t) is_64 << 31) |
            ((dword_t) subtract << 30) |
            ((dword_t) set_flags << 29) |
            ((dword_t) shift_type << 22) |
            ((dword_t) rm << 16) |
            ((dword_t) shift << 10) |
            ((dword_t) rn << 5) | rd;
}

static dword_t encode_add_shifted(bool is_64,
        enum aarch64_shift_type shift_type, byte_t shift,
        byte_t rm, byte_t rn, byte_t rd) {
    return encode_add_sub_shifted(is_64, false, false,
            shift_type, shift, rm, rn, rd);
}

static dword_t encode_add_sub_extended(bool is_64,
        bool subtract, bool set_flags,
        enum aarch64_extend_type extend_type, byte_t shift,
        byte_t rm, byte_t rn, byte_t rd) {
    assert(extend_type <= AARCH64_EXTEND_SXTX);
    assert(shift <= 4);
    assert(rm < 32);
    assert(rn < 32);
    assert(rd < 32);
    return UINT32_C(0x0b200000) |
            ((dword_t) is_64 << 31) |
            ((dword_t) subtract << 30) |
            ((dword_t) set_flags << 29) |
            ((dword_t) rm << 16) |
            ((dword_t) extend_type << 13) |
            ((dword_t) shift << 10) |
            ((dword_t) rn << 5) | rd;
}

static dword_t encode_sub_shifted(bool is_64,
        enum aarch64_shift_type shift_type, byte_t shift,
        byte_t rm, byte_t rn, byte_t rd) {
    return encode_add_sub_shifted(is_64, true, false,
            shift_type, shift, rm, rn, rd);
}

static dword_t encode_subs_shifted(bool is_64,
        enum aarch64_shift_type shift_type, byte_t shift,
        byte_t rm, byte_t rn, byte_t rd) {
    return encode_add_sub_shifted(is_64, true, true,
            shift_type, shift, rm, rn, rd);
}

static dword_t encode_conditional_compare(bool is_64, bool compare,
        bool immediate, byte_t operand, byte_t condition,
        byte_t rn, byte_t nzcv) {
    assert(operand < 32);
    assert(condition < 16);
    assert(rn < 32);
    assert(nzcv < 16);
    return UINT32_C(0x3a400000) |
            ((dword_t) is_64 << 31) |
            ((dword_t) compare << 30) |
            ((dword_t) operand << 16) |
            ((dword_t) condition << 12) |
            ((dword_t) immediate << 11) |
            ((dword_t) rn << 5) | nzcv;
}

static dword_t encode_conditional_select(enum aarch64_opcode opcode,
        bool is_64, byte_t condition,
        byte_t rm, byte_t rn, byte_t rd) {
    byte_t operation;
    if (opcode == AARCH64_OP_CSEL)
        operation = 0;
    else if (opcode == AARCH64_OP_CSINC)
        operation = 1;
    else if (opcode == AARCH64_OP_CSINV)
        operation = 2;
    else {
        assert(opcode == AARCH64_OP_CSNEG);
        operation = 3;
    }
    assert(condition < 16);
    assert(rm < 32);
    assert(rn < 32);
    assert(rd < 32);
    return UINT32_C(0x1a800000) |
            ((dword_t) is_64 << 31) |
            ((dword_t) (operation >> 1) << 30) |
            ((dword_t) rm << 16) |
            ((dword_t) condition << 12) |
            ((dword_t) (operation & 1) << 10) |
            ((dword_t) rn << 5) | rd;
}

static dword_t encode_data_processing_1source(
        bool is_64, byte_t operation, byte_t rn, byte_t rd) {
    assert(operation < 64);
    assert(rn < 32);
    assert(rd < 32);
    return UINT32_C(0x5ac00000) |
            ((dword_t) is_64 << 31) |
            ((dword_t) operation << 10) |
            ((dword_t) rn << 5) | rd;
}

static dword_t encode_lsrv(
        bool is_64, byte_t rm, byte_t rn, byte_t rd) {
    assert(rm < 32);
    assert(rn < 32);
    assert(rd < 32);
    return UINT32_C(0x1ac02400) |
            ((dword_t) is_64 << 31) |
            ((dword_t) rm << 16) |
            ((dword_t) rn << 5) | rd;
}

static dword_t encode_asrv(
        bool is_64, byte_t rm, byte_t rn, byte_t rd) {
    assert(rm < 32);
    assert(rn < 32);
    assert(rd < 32);
    return UINT32_C(0x1ac02800) |
            ((dword_t) is_64 << 31) |
            ((dword_t) rm << 16) |
            ((dword_t) rn << 5) | rd;
}

static dword_t encode_lslv(
        bool is_64, byte_t rm, byte_t rn, byte_t rd) {
    assert(rm < 32);
    assert(rn < 32);
    assert(rd < 32);
    return UINT32_C(0x1ac02000) |
            ((dword_t) is_64 << 31) |
            ((dword_t) rm << 16) |
            ((dword_t) rn << 5) | rd;
}

static dword_t encode_madd(bool is_64,
        byte_t rn, byte_t rm, byte_t ra, byte_t rd) {
    assert(rn < 32);
    assert(rm < 32);
    assert(ra < 32);
    assert(rd < 32);
    return UINT32_C(0x1b000000) |
            ((dword_t) is_64 << 31) |
            ((dword_t) rm << 16) |
            ((dword_t) ra << 10) |
            ((dword_t) rn << 5) | rd;
}

static dword_t encode_msub(bool is_64,
        byte_t rn, byte_t rm, byte_t ra, byte_t rd) {
    assert(rn < 32);
    assert(rm < 32);
    assert(ra < 32);
    assert(rd < 32);
    return UINT32_C(0x1b008000) |
            ((dword_t) is_64 << 31) |
            ((dword_t) rm << 16) |
            ((dword_t) ra << 10) |
            ((dword_t) rn << 5) | rd;
}

static dword_t encode_umaddl(
        byte_t rn, byte_t rm, byte_t ra, byte_t rd) {
    assert(rn < 32);
    assert(rm < 32);
    assert(ra < 32);
    assert(rd < 32);
    return UINT32_C(0x9ba00000) |
            ((dword_t) rm << 16) |
            ((dword_t) ra << 10) |
            ((dword_t) rn << 5) | rd;
}

static dword_t encode_smaddl(
        byte_t rn, byte_t rm, byte_t ra, byte_t rd) {
    assert(rn < 32);
    assert(rm < 32);
    assert(ra < 32);
    assert(rd < 32);
    return UINT32_C(0x9b200000) |
            ((dword_t) rm << 16) |
            ((dword_t) ra << 10) |
            ((dword_t) rn << 5) | rd;
}

static qword_t reference_rev32(qword_t value, bool is_64) {
    qword_t result = (qword_t) __builtin_bswap32((dword_t) value);
    if (is_64) {
        result |= (qword_t) __builtin_bswap32(
                (dword_t) (value >> 32)) << 32;
    }
    return result;
}

static qword_t reference_clz(qword_t value, byte_t width) {
    assert(width == 32 || width == 64);
    value &= width == 32 ? UINT32_MAX : UINT64_MAX;
    qword_t count = 0;
    while (count < width) {
        qword_t bit = UINT64_C(1) << (width - 1 - count);
        if ((value & bit) != 0)
            break;
        count++;
    }
    return count;
}

static dword_t encode_adr(int64_t displacement, byte_t rd) {
    assert(displacement >= -INT64_C(0x100000));
    assert(displacement <= INT64_C(0xfffff));
    assert(rd < 32);
    qword_t immediate =
            (qword_t) displacement & UINT64_C(0x1fffff);
    return UINT32_C(0x10000000) |
            (dword_t) (immediate & 3) << 29 |
            (dword_t) (immediate >> 2) << 5 | rd;
}

static dword_t encode_adrp(int64_t page_displacement, byte_t rd) {
    assert(page_displacement >= -INT64_C(0x100000));
    assert(page_displacement <= INT64_C(0xfffff));
    assert(rd < 32);
    qword_t immediate =
            (qword_t) page_displacement & UINT64_C(0x1fffff);
    return UINT32_C(0x90000000) |
            (dword_t) (immediate & 3) << 29 |
            (dword_t) (immediate >> 2) << 5 | rd;
}

// 测试侧从规范化元素独立生成编码与掩码，不复用生产 decoder。
static qword_t reference_low_ones(byte_t count) {
    assert(count > 0 && count <= 64);
    return count == 64 ? UINT64_MAX :
            (UINT64_C(1) << count) - 1;
}

static qword_t reference_logical_immediate(bool is_64,
        byte_t element_size, byte_t ones, byte_t rotation) {
    byte_t width = is_64 ? 64 : 32;
    assert(element_size >= 2 && element_size <= width);
    assert((element_size & (element_size - 1)) == 0);
    assert(ones > 0 && ones < element_size);
    assert(rotation < element_size);
    qword_t element_mask = reference_low_ones(element_size);
    qword_t element = reference_low_ones(ones);
    if (rotation != 0) {
        element = ((element >> rotation) |
                (element << (element_size - rotation))) &
                element_mask;
    }
    qword_t immediate = 0;
    for (byte_t offset = 0; offset < width; offset += element_size)
        immediate |= element << offset;
    return immediate;
}

static dword_t encode_logical_immediate(enum aarch64_opcode opcode,
        bool is_64, byte_t element_size, byte_t ones, byte_t rotation,
        byte_t rn, byte_t rd) {
    byte_t operation;
    if (opcode == AARCH64_OP_AND_IMMEDIATE) {
        operation = 0;
    } else if (opcode == AARCH64_OP_ORR_IMMEDIATE) {
        operation = 1;
    } else if (opcode == AARCH64_OP_EOR_IMMEDIATE) {
        operation = 2;
    } else {
        assert(opcode == AARCH64_OP_ANDS_IMMEDIATE);
        operation = 3;
    }
    byte_t width = is_64 ? 64 : 32;
    assert(element_size >= 2 && element_size <= width);
    assert((element_size & (element_size - 1)) == 0);
    assert(ones > 0 && ones < element_size);
    assert(rotation < element_size);
    assert(rn < 32);
    assert(rd < 32);
    bool n = element_size == 64;
    byte_t imms = (byte_t) (
            (~((unsigned) element_size * 2 - 1) & 0x3f) |
            (ones - 1));
    return UINT32_C(0x12000000) |
            ((dword_t) is_64 << 31) |
            ((dword_t) operation << 29) |
            ((dword_t) n << 22) |
            ((dword_t) rotation << 16) |
            ((dword_t) imms << 10) |
            ((dword_t) rn << 5) | rd;
}

static dword_t encode_logical_shifted(enum aarch64_opcode opcode,
        bool is_64, bool invert,
        enum aarch64_shift_type shift_type, byte_t shift,
        byte_t rm, byte_t rn, byte_t rd) {
    byte_t operation;
    if (opcode == AARCH64_OP_AND_SHIFTED_REGISTER) {
        operation = 0;
    } else if (opcode == AARCH64_OP_ORR_SHIFTED_REGISTER) {
        operation = 1;
    } else if (opcode == AARCH64_OP_EOR_SHIFTED_REGISTER) {
        operation = 2;
    } else {
        assert(opcode == AARCH64_OP_ANDS_SHIFTED_REGISTER);
        operation = 3;
    }
    byte_t width = is_64 ? 64 : 32;
    assert(shift_type <= AARCH64_SHIFT_ROR);
    assert(shift < width);
    assert(rm < 32);
    assert(rn < 32);
    assert(rd < 32);
    return UINT32_C(0x0a000000) |
            ((dword_t) is_64 << 31) |
            ((dword_t) operation << 29) |
            ((dword_t) shift_type << 22) |
            ((dword_t) invert << 21) |
            ((dword_t) rm << 16) |
            ((dword_t) shift << 10) |
            ((dword_t) rn << 5) | rd;
}

static dword_t encode_and_shifted(bool is_64, bool invert,
        enum aarch64_shift_type shift_type, byte_t shift,
        byte_t rm, byte_t rn, byte_t rd) {
    return encode_logical_shifted(AARCH64_OP_AND_SHIFTED_REGISTER,
            is_64, invert, shift_type, shift, rm, rn, rd);
}

static dword_t encode_orr_shifted(bool is_64, bool invert,
        enum aarch64_shift_type shift_type, byte_t shift,
        byte_t rm, byte_t rn, byte_t rd) {
    return encode_logical_shifted(AARCH64_OP_ORR_SHIFTED_REGISTER,
            is_64, invert, shift_type, shift, rm, rn, rd);
}

static dword_t encode_eor_shifted(bool is_64, bool invert,
        enum aarch64_shift_type shift_type, byte_t shift,
        byte_t rm, byte_t rn, byte_t rd) {
    return encode_logical_shifted(AARCH64_OP_EOR_SHIFTED_REGISTER,
            is_64, invert, shift_type, shift, rm, rn, rd);
}

static dword_t encode_ands_shifted(bool is_64, bool invert,
        enum aarch64_shift_type shift_type, byte_t shift,
        byte_t rm, byte_t rn, byte_t rd) {
    return encode_logical_shifted(AARCH64_OP_ANDS_SHIFTED_REGISTER,
            is_64, invert, shift_type, shift, rm, rn, rd);
}

static dword_t encode_bitfield_move(enum aarch64_opcode opcode,
        bool is_64, byte_t immr, byte_t imms, byte_t rn, byte_t rd) {
    byte_t operation;
    if (opcode == AARCH64_OP_SBFM)
        operation = 0;
    else if (opcode == AARCH64_OP_BFM)
        operation = 1;
    else {
        assert(opcode == AARCH64_OP_UBFM);
        operation = 2;
    }
    byte_t width = is_64 ? 64 : 32;
    assert(immr < width);
    assert(imms < width);
    assert(rn < 32);
    assert(rd < 32);
    return UINT32_C(0x13000000) |
            ((dword_t) is_64 << 31) |
            ((dword_t) operation << 29) |
            ((dword_t) is_64 << 22) |
            ((dword_t) immr << 16) |
            ((dword_t) imms << 10) |
            ((dword_t) rn << 5) | rd;
}

static dword_t encode_extract(bool is_64, byte_t lsb,
        byte_t rm, byte_t rn, byte_t rd) {
    byte_t width = is_64 ? 64 : 32;
    assert(lsb < width);
    assert(rm < 32);
    assert(rn < 32);
    assert(rd < 32);
    return UINT32_C(0x13800000) |
            ((dword_t) is_64 << 31) |
            ((dword_t) is_64 << 22) |
            ((dword_t) rm << 16) |
            ((dword_t) lsb << 10) |
            ((dword_t) rn << 5) | rd;
}

static dword_t encode_scalar_register_offset(byte_t size_shift,
        byte_t operation, enum aarch64_extend_type extend_type,
        bool scaled, byte_t rm, byte_t rn, byte_t rt) {
    assert(size_shift < 4);
    assert(operation < 2 ||
            (operation == 2 && size_shift < 3) ||
            (operation == 3 && size_shift < 2));
    unsigned extend = (unsigned) extend_type;
    assert(extend < 8 && (extend & 2) != 0);
    assert(rm < 32);
    assert(rn < 32);
    assert(rt < 32);
    return UINT32_C(0x38200800) |
            ((dword_t) size_shift << 30) |
            ((dword_t) operation << 22) |
            ((dword_t) rm << 16) |
            ((dword_t) extend << 13) |
            ((dword_t) scaled << 12) |
            ((dword_t) rn << 5) | rt;
}

static dword_t encode_scalar_imm9(byte_t size_shift,
        byte_t operation, enum aarch64_address_mode address_mode,
        int64_t offset, byte_t rn, byte_t rt) {
    assert(size_shift < 4);
    assert(operation < 2 ||
            (operation == 2 && size_shift < 3) ||
            (operation == 3 && size_shift < 2));
    assert(offset >= -256 && offset <= 255);
    assert(rn < 32);
    assert(rt < 32);
    byte_t mode;
    if (address_mode == AARCH64_ADDRESS_OFFSET) {
        mode = 0;
    } else if (address_mode == AARCH64_ADDRESS_POST_INDEX) {
        mode = 1;
    } else {
        assert(address_mode == AARCH64_ADDRESS_PRE_INDEX);
        mode = 3;
    }
    assert(mode == 0 || rn == 31 || rn != rt);
    return UINT32_C(0x38000000) |
            ((dword_t) size_shift << 30) |
            ((dword_t) operation << 22) |
            (((dword_t) offset & UINT32_C(0x1ff)) << 12) |
            ((dword_t) mode << 10) |
            ((dword_t) rn << 5) | rt;
}

static dword_t encode_scalar_load_imm9(byte_t size_shift,
        byte_t operation, enum aarch64_address_mode address_mode,
        int64_t offset, byte_t rn, byte_t rt) {
    assert(operation != 0);
    return encode_scalar_imm9(size_shift, operation,
            address_mode, offset, rn, rt);
}

static dword_t encode_scalar_store_imm9(byte_t size_shift,
        enum aarch64_address_mode address_mode,
        int64_t offset, byte_t rn, byte_t rt) {
    return encode_scalar_imm9(size_shift, 0,
            address_mode, offset, rn, rt);
}

static dword_t encode_store_simd_imm9(byte_t size_shift,
        enum aarch64_address_mode address_mode,
        int64_t offset, byte_t rn, byte_t rt) {
    assert(size_shift <= 4);
    assert(address_mode == AARCH64_ADDRESS_OFFSET ||
            address_mode == AARCH64_ADDRESS_POST_INDEX ||
            address_mode == AARCH64_ADDRESS_PRE_INDEX);
    assert(offset >= -256 && offset <= 255);
    assert(rn < 32);
    assert(rt < 32);

    dword_t encoding = UINT32_C(0x3c000000);
    if (size_shift == 4)
        encoding |= UINT32_C(2) << 22;
    else
        encoding |= (dword_t) size_shift << 30;
    dword_t mode = address_mode == AARCH64_ADDRESS_OFFSET ? 0 :
            address_mode == AARCH64_ADDRESS_POST_INDEX ? 1 : 3;
    return encoding |
            (((dword_t) offset & UINT32_C(0x1ff)) << 12) |
            (mode << 10) | ((dword_t) rn << 5) | rt;
}

static dword_t encode_store_imm12(byte_t size_shift,
        word_t immediate, byte_t rn, byte_t rt) {
    assert(size_shift < 4);
    assert(immediate <= UINT16_C(0xfff));
    assert(rn < 32);
    assert(rt < 32);
    return UINT32_C(0x39000000) |
            ((dword_t) size_shift << 30) |
            ((dword_t) immediate << 10) |
            ((dword_t) rn << 5) | rt;
}

static dword_t encode_store_simd_imm12(byte_t size_shift,
        word_t immediate, byte_t rn, byte_t rt) {
    assert(size_shift <= 4);
    assert(immediate <= UINT16_C(0xfff));
    assert(rn < 32);
    assert(rt < 32);
    dword_t encoding = UINT32_C(0x3d000000);
    if (size_shift == 4)
        encoding |= UINT32_C(2) << 22;
    else
        encoding |= (dword_t) size_shift << 30;
    return encoding | (dword_t) immediate << 10 |
            (dword_t) rn << 5 | rt;
}

static dword_t encode_load_simd_imm12(byte_t size_shift,
        word_t immediate, byte_t rn, byte_t rt) {
    return encode_store_simd_imm12(
            size_shift, immediate, rn, rt) | UINT32_C(1) << 22;
}

static dword_t encode_integer_pair(byte_t operation, bool load,
        enum aarch64_address_mode address_mode, int64_t offset,
        byte_t rn, byte_t rt, byte_t rt2) {
    assert(operation <= 2);
    assert(load || operation != 1);
    assert(rn < 32);
    assert(rt < 32);
    assert(rt2 < 32);
    assert(!load || rt != rt2);
    byte_t size = operation == 2 ? 8 : 4;
    assert(offset % size == 0);
    int64_t scaled_offset = offset / size;
    assert(scaled_offset >= -64 && scaled_offset <= 63);
    byte_t mode;
    if (address_mode == AARCH64_ADDRESS_POST_INDEX)
        mode = 1;
    else if (address_mode == AARCH64_ADDRESS_OFFSET)
        mode = 2;
    else {
        assert(address_mode == AARCH64_ADDRESS_PRE_INDEX);
        mode = 3;
    }
    assert(mode == 2 || rn == 31 || (rn != rt && rn != rt2));
    return UINT32_C(0x28000000) |
            ((dword_t) operation << 30) |
            ((dword_t) mode << 23) |
            ((dword_t) load << 22) |
            (((dword_t) scaled_offset & UINT32_C(0x7f)) << 15) |
            ((dword_t) rt2 << 10) |
            ((dword_t) rn << 5) | rt;
}

static dword_t encode_load_pair(byte_t operation,
        enum aarch64_address_mode address_mode, int64_t offset,
        byte_t rn, byte_t rt, byte_t rt2) {
    return encode_integer_pair(operation, true, address_mode,
            offset, rn, rt, rt2);
}

static dword_t encode_store_pair(bool is_64,
        enum aarch64_address_mode address_mode, int64_t offset,
        byte_t rn, byte_t rt, byte_t rt2) {
    return encode_integer_pair(is_64 ? 2 : 0, false, address_mode,
            offset, rn, rt, rt2);
}

static dword_t encode_simd_pair(byte_t operation, byte_t mode, bool load,
        int64_t offset, byte_t rn, byte_t rt, byte_t rt2) {
    assert(operation < 3);
    assert(mode < 4);
    assert(rn < 32);
    assert(rt < 32);
    assert(rt2 < 32);
    assert(!load || rt != rt2);
    byte_t size = (byte_t) (UINT8_C(1) << (operation + 2));
    assert(offset % size == 0);
    int64_t scaled_offset = offset / size;
    assert(scaled_offset >= -64 && scaled_offset <= 63);
    return UINT32_C(0x2c000000) |
            ((dword_t) operation << 30) |
            ((dword_t) mode << 23) |
            ((dword_t) load << 22) |
            (((dword_t) scaled_offset & UINT32_C(0x7f)) << 15) |
            ((dword_t) rt2 << 10) |
            ((dword_t) rn << 5) | rt;
}

static dword_t encode_load_simd_pair(byte_t operation, byte_t mode,
        int64_t offset, byte_t rn, byte_t rt, byte_t rt2) {
    return encode_simd_pair(
            operation, mode, true, offset, rn, rt, rt2);
}

static dword_t encode_store_simd_pair(byte_t operation, byte_t mode,
        int64_t offset, byte_t rn, byte_t rt, byte_t rt2) {
    return encode_simd_pair(
            operation, mode, false, offset, rn, rt, rt2);
}

static dword_t encode_advsimd_immediate(bool q, bool op,
        byte_t cmode, byte_t immediate, byte_t rd) {
    assert(cmode < 15);
    assert(rd < 32);
    return UINT32_C(0x0f000400) |
            (dword_t) q << 30 |
            (dword_t) op << 29 |
            (dword_t) (immediate & UINT8_C(0xe0)) << 11 |
            (dword_t) cmode << 12 |
            (dword_t) (immediate & UINT8_C(0x1f)) << 5 |
            rd;
}

static dword_t encode_move_wide(bool is_64, enum aarch64_opcode opcode,
        byte_t halfword, word_t immediate, byte_t rd) {
    byte_t operation;
    if (opcode == AARCH64_OP_MOVN)
        operation = 0;
    else if (opcode == AARCH64_OP_MOVZ)
        operation = 2;
    else {
        assert(opcode == AARCH64_OP_MOVK);
        operation = 3;
    }
    assert(halfword < (is_64 ? 4 : 2));
    assert(rd < 32);
    return UINT32_C(0x12800000) |
            ((dword_t) is_64 << 31) |
            ((dword_t) operation << 29) |
            ((dword_t) halfword << 21) |
            ((dword_t) immediate << 5) | rd;
}

static dword_t encode_branch_immediate(bool link, int64_t displacement) {
    assert((displacement & 3) == 0);
    return UINT32_C(0x14000000) |
            ((dword_t) link << 31) |
            ((dword_t) (displacement / 4) & UINT32_C(0x03ffffff));
}

static dword_t encode_branch_register(enum aarch64_opcode opcode,
        byte_t rn) {
    assert(rn < 31);
    dword_t base;
    if (opcode == AARCH64_OP_BR)
        base = UINT32_C(0xd61f0000);
    else if (opcode == AARCH64_OP_BLR)
        base = UINT32_C(0xd63f0000);
    else {
        assert(opcode == AARCH64_OP_RET);
        base = UINT32_C(0xd65f0000);
    }
    return base | ((dword_t) rn << 5);
}

static dword_t encode_conditional_branch(int64_t displacement,
        byte_t condition) {
    assert((displacement & 3) == 0);
    assert(condition < 16);
    return UINT32_C(0x54000000) |
            (((dword_t) (displacement / 4) & UINT32_C(0x7ffff)) << 5) |
            condition;
}

static dword_t encode_compare_branch(bool is_64, bool nonzero,
        int64_t displacement, byte_t rt) {
    assert((displacement & 3) == 0);
    assert(rt < 32);
    return UINT32_C(0x34000000) |
            ((dword_t) is_64 << 31) |
            ((dword_t) nonzero << 24) |
            (((dword_t) (displacement / 4) & UINT32_C(0x7ffff)) << 5) |
            rt;
}

static dword_t encode_test_branch(bool nonzero, byte_t bit,
        int64_t displacement, byte_t rt) {
    assert(bit < 64);
    assert((displacement & 3) == 0);
    assert(rt < 32);
    return UINT32_C(0x36000000) |
            ((dword_t) (bit >> 5) << 31) |
            ((dword_t) nonzero << 24) |
            ((dword_t) (bit & 0x1f) << 19) |
            (((dword_t) (displacement / 4) & UINT32_C(0x3fff)) << 5) |
            rt;
}
#endif

static void test_backend_selection(void) {
#if ISH_AARCH64_BACKEND_THREADED_DEFAULT
    const enum aarch64_backend expected_default = AARCH64_BACKEND_THREADED;
#else
    const enum aarch64_backend expected_default = AARCH64_BACKEND_C;
#endif
    assert(aarch64_backend_default() == expected_default);
    assert(aarch64_backend_available(AARCH64_BACKEND_C));

    struct test_fixture fixture;
    init_fixture(&fixture);
    struct aarch64_runner runner;
    aarch64_runner_init(&runner, &fixture.tlb);
    assert(aarch64_runner_backend(&runner) == expected_default);
    assert_stats(&runner, 0, 0, 0, 0);

#if defined(__aarch64__)
    assert(aarch64_backend_available(AARCH64_BACKEND_THREADED));
    assert(aarch64_runner_init_backend(&runner, &fixture.tlb,
            AARCH64_BACKEND_THREADED));
    assert(aarch64_runner_backend(&runner) == AARCH64_BACKEND_THREADED);
#else
    assert(!aarch64_backend_available(AARCH64_BACKEND_THREADED));
    assert(!aarch64_runner_init_backend(&runner, &fixture.tlb,
            AARCH64_BACKEND_THREADED));
#endif
}

#if defined(__aarch64__)
static struct cpu_state run_fast_differential_fixtures(
        dword_t instruction, struct cpu_state initial,
        enum aarch64_step_stop expected_stop,
        struct test_fixture *c_fixture,
        struct test_fixture *threaded_fixture,
        struct aarch64_step_result *step_out) {
    struct aarch64_runner c_runner;
    struct aarch64_runner threaded_runner;
    assert(aarch64_runner_init_backend(
            &c_runner, &c_fixture->tlb, AARCH64_BACKEND_C));
    assert(aarch64_runner_init_backend(&threaded_runner,
            &threaded_fixture->tlb, AARCH64_BACKEND_THREADED));
    struct cpu_state c_cpu = initial;
    struct cpu_state threaded_cpu = initial;
    c_cpu.pc = CODE_PAGE;
    threaded_cpu.pc = CODE_PAGE;

    struct aarch64_step_result c_result =
            aarch64_run_one(&c_runner, &c_cpu);
    struct aarch64_step_result threaded_result =
            aarch64_run_one(&threaded_runner, &threaded_cpu);
    assert(c_result.stop == expected_stop);
    assert(c_result.instruction == instruction);
    assert_step_equal(&c_result, &threaded_result);
    assert_cpu_equal(&c_cpu, &threaded_cpu);
    assert_memory_equal(&c_fixture->memory, &threaded_fixture->memory);
    assert_stats(&threaded_runner, 0, 1, 1, 0);
    if (step_out != NULL)
        *step_out = threaded_result;
    return threaded_cpu;
}

static struct cpu_state run_fast_differential(dword_t instruction,
        struct cpu_state initial, enum aarch64_step_stop expected_stop) {
    struct test_fixture c_fixture;
    struct test_fixture threaded_fixture;
    init_fixture(&c_fixture);
    init_fixture(&threaded_fixture);
    write_instruction(&c_fixture.tlb, CODE_PAGE, instruction);
    write_instruction(&threaded_fixture.tlb, CODE_PAGE, instruction);
    return run_fast_differential_fixtures(instruction, initial,
            expected_stop, &c_fixture, &threaded_fixture, NULL);
}

static struct cpu_state run_pc_relative_direct_differential(
        dword_t instruction, enum aarch64_opcode expected_opcode,
        guest_addr_t pc,
        struct cpu_state initial) {
    struct test_fixture c_fixture;
    struct test_fixture threaded_fixture;
    init_fixture(&c_fixture);
    init_fixture(&threaded_fixture);
    struct aarch64_decoded decoded;
    assert(aarch64_decode(instruction, &decoded));
    assert(expected_opcode == AARCH64_OP_ADR ||
            expected_opcode == AARCH64_OP_ADRP);
    assert(decoded.opcode == expected_opcode);
    assert(decoded.width == 64);

    struct cpu_state c_cpu = initial;
    struct cpu_state threaded_cpu = initial;
    c_cpu.pc = pc;
    threaded_cpu.pc = pc;
    struct aarch64_execute_result c_result =
            aarch64_execute(&c_cpu, &c_fixture.tlb, &decoded);
    struct aarch64_threaded_cache cache = {0};
    struct aarch64_execute_result threaded_result;
    assert(aarch64_threaded_execute(&cache,
            &threaded_cpu, &threaded_fixture.tlb,
            pc, instruction, &threaded_result));

    assert(c_result.stop == threaded_result.stop);
    assert(c_result.fault.address == threaded_result.fault.address);
    assert(c_result.fault.access == threaded_result.fault.access);
    assert(c_result.fault.kind == threaded_result.fault.kind);
    assert_cpu_equal(&c_cpu, &threaded_cpu);
    assert_memory_equal(&c_fixture.memory, &threaded_fixture.memory);
    assert(cache.stats.cache_hits == 0);
    assert(cache.stats.cache_misses == 1);
    assert(cache.stats.fast_dispatches == 1);
    assert(cache.stats.c_fallbacks == 0);
    return threaded_cpu;
}

static void test_fast_data_processing_differential(void) {
    struct cpu_state initial;
    struct cpu_state result;

    init_differential_cpu(&initial);
    aarch64_set_exclusive(&initial, DATA_PAGE + 0x40, 8, false,
            UINT64_C(0x1122), 0, NULL, 3, 5, 7);
    result = run_fast_differential(
            INSTRUCTION_NOP, initial, AARCH64_STEP_RETIRED);
    assert(result.pc == CODE_PAGE + 4);
    assert(result.cycle == initial.cycle + 1);
    assert(result.exclusive.valid);
    assert(result.exclusive.write_epoch == 5);
    assert(result.exclusive.sync_identity == 7);

    init_differential_cpu(&initial);
    result = run_fast_differential(encode_move_wide(true,
            AARCH64_OP_MOVN, 1, UINT16_C(0x1234), 2),
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[2] == ~UINT64_C(0x0000000012340000));

    init_differential_cpu(&initial);
    initial.x[2] = UINT64_MAX;
    result = run_fast_differential(encode_move_wide(false,
            AARCH64_OP_MOVN, 0, 0, 2),
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[2] == UINT64_C(0x00000000ffffffff));

    init_differential_cpu(&initial);
    initial.x[2] = UINT64_MAX;
    result = run_fast_differential(encode_move_wide(true,
            AARCH64_OP_MOVZ, 1, UINT16_C(0x1234), 2),
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[2] == UINT64_C(0x0000000012340000));

    init_differential_cpu(&initial);
    initial.x[2] = UINT64_MAX;
    result = run_fast_differential(encode_move_wide(false,
            AARCH64_OP_MOVZ, 0, UINT16_C(0xabcd), 2),
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[2] == UINT64_C(0x000000000000abcd));

    init_differential_cpu(&initial);
    initial.x[2] = UINT64_C(0x1122334455667788);
    result = run_fast_differential(encode_move_wide(true,
            AARCH64_OP_MOVK, 2, UINT16_C(0xabcd), 2),
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[2] == UINT64_C(0x1122abcd55667788));

    init_differential_cpu(&initial);
    initial.x[2] = UINT64_C(0x1122334455667788);
    result = run_fast_differential(encode_move_wide(false,
            AARCH64_OP_MOVK, 1, UINT16_C(0xbeef), 2),
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[2] == UINT64_C(0x00000000beef7788));

    init_differential_cpu(&initial);
    qword_t old_sp = initial.sp;
    result = run_fast_differential(encode_move_wide(true,
            AARCH64_OP_MOVZ, 0, UINT16_C(0xffff), 31),
            initial, AARCH64_STEP_RETIRED);
    assert(result.sp == old_sp);

    init_differential_cpu(&initial);
    initial.sp = UINT64_C(0x1000);
    result = run_fast_differential(encode_add_sub_immediate(
            true, false, false, UINT16_C(0x20), 31, 31),
            initial, AARCH64_STEP_RETIRED);
    assert(result.sp == UINT64_C(0x1020));
    assert(result.nzcv == initial.nzcv);

    init_differential_cpu(&initial);
    initial.sp = UINT64_C(0x1000);
    result = run_fast_differential(encode_add_sub_immediate(
            true, true, false, UINT16_C(0x20), 31, 31),
            initial, AARCH64_STEP_RETIRED);
    assert(result.sp == UINT64_C(0x0fe0));
    assert(result.nzcv == initial.nzcv);

    init_differential_cpu(&initial);
    initial.x[3] = UINT64_C(0xaaaaaaaa80000000);
    result = run_fast_differential(encode_add_sub_immediate(
            false, false, false, 1, 3, 4),
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[4] == UINT64_C(0x0000000080000001));

    init_differential_cpu(&initial);
    initial.x[3] = UINT64_C(0x100000000);
    result = run_fast_differential(encode_add_sub_immediate(
            true, false, false, UINT64_C(0x123000), 3, 4),
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[4] == UINT64_C(0x100123000));

    init_differential_cpu(&initial);
    initial.x[3] = UINT64_C(0xaaaaaaaaffffffff);
    result = run_fast_differential(encode_add_sub_immediate(
            false, true, false, 1, 3, 4),
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[4] == UINT64_C(0x00000000fffffffe));

    init_differential_cpu(&initial);
    initial.sp = 0;
    result = run_fast_differential(encode_add_sub_immediate(
            true, false, true, 0, 31, 31),
            initial, AARCH64_STEP_RETIRED);
    assert(result.sp == 0);
    assert(result.nzcv == UINT32_C(0x40000000));

    init_differential_cpu(&initial);
    initial.sp = 0;
    result = run_fast_differential(encode_add_sub_immediate(
            true, true, true, 1, 31, 31),
            initial, AARCH64_STEP_RETIRED);
    assert(result.sp == 0);
    assert(result.nzcv == UINT32_C(0x80000000));

    init_differential_cpu(&initial);
    initial.x[3] = UINT32_C(0x7fffffff);
    result = run_fast_differential(encode_add_sub_immediate(
            false, false, true, 1, 3, 4),
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[4] == UINT64_C(0x0000000080000000));
    assert(result.nzcv == UINT32_C(0x90000000));

    init_differential_cpu(&initial);
    initial.x[3] = UINT64_MAX;
    result = run_fast_differential(encode_add_sub_immediate(
            true, false, true, 1, 3, 4),
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[4] == 0);
    assert(result.nzcv == UINT32_C(0x60000000));

    init_differential_cpu(&initial);
    initial.x[3] = 0;
    result = run_fast_differential(encode_add_sub_immediate(
            false, true, true, 1, 3, 4),
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[4] == UINT64_C(0x00000000ffffffff));
    assert(result.nzcv == UINT32_C(0x80000000));

    init_differential_cpu(&initial);
    initial.x[3] = UINT64_C(0x8000000000000000);
    result = run_fast_differential(encode_add_sub_immediate(
            true, true, true, 1, 3, 4),
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[4] == UINT64_C(0x7fffffffffffffff));
    assert(result.nzcv == UINT32_C(0x30000000));
}

static void test_fast_add_shifted_differential(void) {
    const enum aarch64_shift_type shift_types[] = {
        AARCH64_SHIFT_LSL,
        AARCH64_SHIFT_LSR,
        AARCH64_SHIFT_ASR,
    };
    for (unsigned width_index = 0; width_index < 2; width_index++) {
        bool is_64 = width_index != 0;
        byte_t width = is_64 ? 64 : 32;
        const byte_t shifts[] = {0, 1, (byte_t) (width - 1)};
        for (unsigned type_index = 0;
                type_index < array_size(shift_types); type_index++) {
            for (unsigned shift_index = 0;
                    shift_index < array_size(shifts); shift_index++) {
                struct cpu_state initial;
                init_differential_cpu(&initial);
                initial.x[4] = UINT64_C(0x8123456789abcdef);
                initial.x[5] = UINT64_C(0xfedcba9886543211);
                struct cpu_state result = run_fast_differential(
                        encode_add_shifted(is_64,
                                shift_types[type_index],
                                shifts[shift_index], 5, 4, 6),
                        initial, AARCH64_STEP_RETIRED);
                assert(result.pc == CODE_PAGE + 4);
                assert(result.cycle == initial.cycle + 1);
                assert(result.nzcv == initial.nzcv);
                assert(result.fpcr == initial.fpcr);
                assert(result.fpsr == initial.fpsr);
                assert(result.x[4] == initial.x[4]);
                assert(result.x[5] == initial.x[5]);
            }
        }
    }

    struct cpu_state initial;
    init_differential_cpu(&initial);
    initial.x[2] = 7;
    initial.x[3] = 5;
    aarch64_set_exclusive(&initial, DATA_PAGE + 0x40, 8, false,
            UINT64_C(0x1122), 0, NULL, 3, 5, 7);
    assert(encode_add_shifted(true, AARCH64_SHIFT_LSL,
            3, 2, 3, 3) == UINT32_C(0x8b020c63));
    struct cpu_state result = run_fast_differential(
            UINT32_C(0x8b020c63), initial, AARCH64_STEP_RETIRED);
    assert(result.x[3] == 61);
    assert(result.sp == initial.sp);
    assert(result.nzcv == initial.nzcv);
    assert(result.fpcr == initial.fpcr);
    assert(result.fpsr == initial.fpsr);
    assert(result.exclusive.valid);
    assert(result.exclusive.address == DATA_PAGE + 0x40);
    assert(result.exclusive.write_epoch == 5);
    assert(result.exclusive.sync_identity == 7);

    init_differential_cpu(&initial);
    initial.x[0] = 19;
    initial.x[1] = 23;
    initial.x[2] = UINT64_MAX;
    assert(encode_add_shifted(true, AARCH64_SHIFT_LSL,
            0, 1, 0, 2) == UINT32_C(0x8b010002));
    result = run_fast_differential(
            UINT32_C(0x8b010002), initial, AARCH64_STEP_RETIRED);
    assert(result.x[2] == 42);

    init_differential_cpu(&initial);
    initial.x[4] = UINT64_MAX;
    initial.x[5] = 1;
    result = run_fast_differential(encode_add_shifted(
            true, AARCH64_SHIFT_LSL, 0, 5, 4, 6),
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[6] == 0);
    assert(result.nzcv == initial.nzcv);

    init_differential_cpu(&initial);
    initial.x[4] = UINT64_C(0x80);
    initial.x[5] = 3;
    result = run_fast_differential(encode_add_shifted(
            true, AARCH64_SHIFT_LSR, 1, 5, 4, 5),
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[5] == UINT64_C(0x81));

    init_differential_cpu(&initial);
    initial.x[4] = 1;
    initial.x[5] = UINT64_C(0xdeadbeef80000000);
    initial.x[6] = UINT64_MAX;
    result = run_fast_differential(encode_add_shifted(
            false, AARCH64_SHIFT_ASR, 31, 5, 4, 6),
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[6] == 0);
    assert(result.x[5] == initial.x[5]);

    init_differential_cpu(&initial);
    initial.x[4] = 3;
    result = run_fast_differential(encode_add_shifted(
            true, AARCH64_SHIFT_LSL, 1, 4, 31, 6),
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[6] == 6);
    assert(result.sp == initial.sp);

    init_differential_cpu(&initial);
    initial.x[4] = UINT64_C(0x123456789abcdef0);
    result = run_fast_differential(encode_add_shifted(
            true, AARCH64_SHIFT_ASR, 63, 31, 4, 6),
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[6] == initial.x[4]);
    assert(result.sp == initial.sp);

    init_differential_cpu(&initial);
    initial.x[4] = UINT64_C(0x123456789abcdef0);
    initial.x[5] = UINT64_C(0x0fedcba987654321);
    result = run_fast_differential(encode_add_shifted(
            true, AARCH64_SHIFT_LSL, 4, 5, 4, 31),
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[4] == initial.x[4]);
    assert(result.x[5] == initial.x[5]);
    assert(result.sp == initial.sp);
}

static void test_fast_add_extended_differential(void) {
    static const qword_t extended_values[2][8] = {
        {
            UINT64_C(0x00000080),
            UINT64_C(0x00008080),
            UINT64_C(0x80008080),
            UINT64_C(0x80008080),
            UINT64_C(0xffffff80),
            UINT64_C(0xffff8080),
            UINT64_C(0x80008080),
            UINT64_C(0x80008080),
        },
        {
            UINT64_C(0x0000000000000080),
            UINT64_C(0x0000000000008080),
            UINT64_C(0x0000000080008080),
            UINT64_C(0x8000000080008080),
            UINT64_C(0xffffffffffffff80),
            UINT64_C(0xffffffffffff8080),
            UINT64_C(0xffffffff80008080),
            UINT64_C(0x8000000080008080),
        },
    };
    unsigned case_count = 0;
    for (unsigned width_index = 0; width_index < 2; width_index++) {
        bool is_64 = width_index != 0;
        qword_t mask = is_64 ? UINT64_MAX : UINT32_MAX;
        for (unsigned extend_index = 0;
                extend_index < array_size(extended_values[0]);
                extend_index++) {
            for (byte_t shift = 0; shift <= 4; shift++) {
                struct cpu_state initial;
                init_differential_cpu(&initial);
                initial.x[4] = UINT64_C(0x0123456789abcdef);
                initial.x[5] = UINT64_C(0x8000000080008080);
                initial.x[6] = UINT64_MAX;
                aarch64_set_exclusive(&initial,
                        DATA_PAGE + 0x40, 8, false,
                        UINT64_C(0x1122), 0, NULL, 3, 5, 7);
                qword_t right =
                        (extended_values[width_index][extend_index] <<
                        shift) & mask;
                qword_t expected = ((initial.x[4] & mask) + right) & mask;
                struct cpu_state result = run_fast_differential(
                        encode_add_sub_extended(is_64,
                                false, false,
                                (enum aarch64_extend_type) extend_index,
                                shift, 5, 4, 6),
                        initial, AARCH64_STEP_RETIRED);
                assert(result.x[4] == initial.x[4]);
                assert(result.x[5] == initial.x[5]);
                assert(result.x[6] == expected);
                assert(result.sp == initial.sp);
                assert(result.pc == CODE_PAGE + 4);
                assert(result.cycle == initial.cycle + 1);
                assert(result.nzcv == initial.nzcv);
                assert(result.fpcr == initial.fpcr);
                assert(result.fpsr == initial.fpsr);
                assert(result.tpidr_el0 == initial.tpidr_el0);
                assert(memcmp(result.v, initial.v,
                        sizeof(result.v)) == 0);
                assert(result.exclusive.valid);
                assert(result.exclusive.address == DATA_PAGE + 0x40);
                assert(result.exclusive.write_epoch == 5);
                assert(result.exclusive.sync_identity == 7);
                case_count++;
            }
        }
    }

    static const struct {
        byte_t rn;
        byte_t rm;
        byte_t rd;
        qword_t expected[2];
    } register_cases[] = {
        {31, 5, 6, {
            UINT64_C(0x33374845), UINT64_C(0x1111221e33374845)}},
        {4, 5, 31, {
            UINT64_C(0x89afd1ef), UINT64_C(0x0123456389afd1ef)}},
        {31, 5, 31, {
            UINT64_C(0x33374845), UINT64_C(0x1111221e33374845)}},
        {4, 31, 6, {
            UINT64_C(0x89abcdef), UINT64_C(0x0123456789abcdef)}},
        {4, 5, 4, {
            UINT64_C(0x89afd1ef), UINT64_C(0x0123456389afd1ef)}},
        {4, 5, 5, {
            UINT64_C(0x89afd1ef), UINT64_C(0x0123456389afd1ef)}},
        {4, 4, 4, {
            UINT64_C(0xd70a3d67), UINT64_C(0x01234563d70a3d67)}},
    };
    for (unsigned width_index = 0; width_index < 2; width_index++) {
        bool is_64 = width_index != 0;
        for (unsigned index = 0;
                index < array_size(register_cases); index++) {
            struct cpu_state initial;
            init_differential_cpu(&initial);
            initial.sp = UINT64_C(0x1111222233334445);
            initial.x[4] = UINT64_C(0x0123456789abcdef);
            initial.x[5] = UINT64_C(0x8000000080008080);
            initial.x[6] = UINT64_MAX;
            struct cpu_state result = run_fast_differential(
                    encode_add_sub_extended(is_64,
                            false, false, AARCH64_EXTEND_SXTW, 3,
                            register_cases[index].rm,
                            register_cases[index].rn,
                            register_cases[index].rd),
                    initial, AARCH64_STEP_RETIRED);
            if (register_cases[index].rd == 31) {
                assert(result.sp ==
                        register_cases[index].expected[width_index]);
            } else {
                assert(result.x[register_cases[index].rd] ==
                        register_cases[index].expected[width_index]);
                assert(result.sp == initial.sp);
            }
            assert(result.nzcv == initial.nzcv);
            case_count++;
        }
    }

    struct cpu_state initial;
    init_differential_cpu(&initial);
    initial.x[0] = UINT64_C(0xffffffff00000003);
    initial.x[4] = UINT64_C(0x1000);
    assert(encode_add_sub_extended(true, false, false,
            AARCH64_EXTEND_UXTW, 3, 0, 4, 0) ==
            INSTRUCTION_ADD_X0_X4_W0_UXTW_3);
    struct cpu_state result = run_fast_differential(
            INSTRUCTION_ADD_X0_X4_W0_UXTW_3,
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[0] == UINT64_C(0x1018));
    assert(result.x[4] == UINT64_C(0x1000));
    case_count++;

    init_differential_cpu(&initial);
    initial.x[20] = UINT64_C(0x01234567fffffffd);
    initial.x[21] = UINT64_C(0x2000);
    assert(encode_add_sub_extended(true, false, false,
            AARCH64_EXTEND_SXTW, 3, 20, 21, 0) ==
            INSTRUCTION_ADD_X0_X21_W20_SXTW_3);
    result = run_fast_differential(
            INSTRUCTION_ADD_X0_X21_W20_SXTW_3,
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[0] == UINT64_C(0x1fe8));
    assert(result.x[20] == UINT64_C(0x01234567fffffffd));
    assert(result.x[21] == UINT64_C(0x2000));
    case_count++;

    assert(case_count == 96);
}

static void test_fast_sub_extended_differential(void) {
    static const qword_t extended_values[2][8] = {
        {
            UINT64_C(0x00000080),
            UINT64_C(0x00008080),
            UINT64_C(0x80008080),
            UINT64_C(0x80008080),
            UINT64_C(0xffffff80),
            UINT64_C(0xffff8080),
            UINT64_C(0x80008080),
            UINT64_C(0x80008080),
        },
        {
            UINT64_C(0x0000000000000080),
            UINT64_C(0x0000000000008080),
            UINT64_C(0x0000000080008080),
            UINT64_C(0x8000000080008080),
            UINT64_C(0xffffffffffffff80),
            UINT64_C(0xffffffffffff8080),
            UINT64_C(0xffffffff80008080),
            UINT64_C(0x8000000080008080),
        },
    };
    unsigned case_count = 0;
    for (unsigned width_index = 0; width_index < 2; width_index++) {
        bool is_64 = width_index != 0;
        qword_t mask = is_64 ? UINT64_MAX : UINT32_MAX;
        for (unsigned extend_index = 0;
                extend_index < array_size(extended_values[0]);
                extend_index++) {
            for (byte_t shift = 0; shift <= 4; shift++) {
                struct cpu_state initial;
                init_differential_cpu(&initial);
                initial.x[4] = UINT64_C(0x0123456789abcdef);
                initial.x[5] = UINT64_C(0x8000000080008080);
                initial.x[6] = UINT64_MAX;
                aarch64_set_exclusive(&initial,
                        DATA_PAGE + 0x40, 8, false,
                        UINT64_C(0x1122), 0, NULL, 3, 5, 7);
                qword_t right =
                        (extended_values[width_index][extend_index] <<
                        shift) & mask;
                qword_t expected = ((initial.x[4] & mask) - right) & mask;
                struct cpu_state result = run_fast_differential(
                        encode_add_sub_extended(is_64,
                                true, false,
                                (enum aarch64_extend_type) extend_index,
                                shift, 5, 4, 6),
                        initial, AARCH64_STEP_RETIRED);
                assert(result.x[4] == initial.x[4]);
                assert(result.x[5] == initial.x[5]);
                assert(result.x[6] == expected);
                assert(result.sp == initial.sp);
                assert(result.pc == CODE_PAGE + 4);
                assert(result.cycle == initial.cycle + 1);
                assert(result.nzcv == initial.nzcv);
                assert(result.fpcr == initial.fpcr);
                assert(result.fpsr == initial.fpsr);
                assert(result.tpidr_el0 == initial.tpidr_el0);
                assert(memcmp(result.v, initial.v,
                        sizeof(result.v)) == 0);
                assert(result.exclusive.valid);
                assert(result.exclusive.address == DATA_PAGE + 0x40);
                assert(result.exclusive.write_epoch == 5);
                assert(result.exclusive.sync_identity == 7);
                case_count++;
            }
        }
    }

    static const struct {
        byte_t rn;
        byte_t rm;
        byte_t rd;
        qword_t expected[2];
    } register_cases[] = {
        {31, 5, 6, {
            UINT64_C(0x332f4045), UINT64_C(0x11112226332f4045)}},
        {4, 5, 31, {
            UINT64_C(0x89a7c9ef), UINT64_C(0x0123456b89a7c9ef)}},
        {31, 5, 31, {
            UINT64_C(0x332f4045), UINT64_C(0x11112226332f4045)}},
        {4, 31, 6, {
            UINT64_C(0x89abcdef), UINT64_C(0x0123456789abcdef)}},
        {4, 5, 4, {
            UINT64_C(0x89a7c9ef), UINT64_C(0x0123456b89a7c9ef)}},
        {4, 5, 5, {
            UINT64_C(0x89a7c9ef), UINT64_C(0x0123456b89a7c9ef)}},
        {4, 4, 4, {
            UINT64_C(0x3c4d5e77), UINT64_C(0x0123456b3c4d5e77)}},
    };
    for (unsigned width_index = 0; width_index < 2; width_index++) {
        bool is_64 = width_index != 0;
        for (unsigned index = 0;
                index < array_size(register_cases); index++) {
            struct cpu_state initial;
            init_differential_cpu(&initial);
            initial.sp = UINT64_C(0x1111222233334445);
            initial.x[4] = UINT64_C(0x0123456789abcdef);
            initial.x[5] = UINT64_C(0x8000000080008080);
            initial.x[6] = UINT64_MAX;
            struct cpu_state result = run_fast_differential(
                    encode_add_sub_extended(is_64,
                            true, false, AARCH64_EXTEND_SXTW, 3,
                            register_cases[index].rm,
                            register_cases[index].rn,
                            register_cases[index].rd),
                    initial, AARCH64_STEP_RETIRED);
            if (register_cases[index].rd == 31) {
                assert(result.sp ==
                        register_cases[index].expected[width_index]);
            } else {
                assert(result.x[register_cases[index].rd] ==
                        register_cases[index].expected[width_index]);
                assert(result.sp == initial.sp);
            }
            assert(result.nzcv == initial.nzcv);
            case_count++;
        }
    }

    struct cpu_state initial;
    init_differential_cpu(&initial);
    initial.sp = UINT64_C(0x1000);
    initial.x[0] = 8;
    initial.nzcv = UINT32_C(0x30000000);
    assert(encode_add_sub_extended(true, true, false,
            AARCH64_EXTEND_UXTX, 0, 0, 31, 31) ==
            INSTRUCTION_SUB_SP_SP_X0);
    struct cpu_state result = run_fast_differential(
            INSTRUCTION_SUB_SP_SP_X0,
            initial, AARCH64_STEP_RETIRED);
    assert(result.sp == UINT64_C(0x0ff8));
    assert(result.x[0] == 8);
    assert(result.nzcv == UINT32_C(0x30000000));
    case_count++;

    init_differential_cpu(&initial);
    initial.x[1] = UINT64_C(0x100);
    initial.x[2] = 1;
    initial.nzcv = UINT32_C(0x90000000);
    assert(encode_add_sub_extended(true, true, false,
            AARCH64_EXTEND_SXTW, 0, 2, 1, 1) ==
            INSTRUCTION_SUB_X1_X1_W2_SXTW);
    result = run_fast_differential(
            INSTRUCTION_SUB_X1_X1_W2_SXTW,
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[1] == UINT64_C(0xff));
    assert(result.x[2] == 1);
    assert(result.sp == initial.sp);
    assert(result.nzcv == UINT32_C(0x90000000));
    case_count++;

    assert(case_count == 96);
}

static void test_fast_subs_extended_differential(void) {
    static const qword_t extended_values[2][8] = {
        {
            UINT64_C(0x00000080),
            UINT64_C(0x00008080),
            UINT64_C(0x80008080),
            UINT64_C(0x80008080),
            UINT64_C(0xffffff80),
            UINT64_C(0xffff8080),
            UINT64_C(0x80008080),
            UINT64_C(0x80008080),
        },
        {
            UINT64_C(0x0000000000000080),
            UINT64_C(0x0000000000008080),
            UINT64_C(0x0000000080008080),
            UINT64_C(0x8000000080008080),
            UINT64_C(0xffffffffffffff80),
            UINT64_C(0xffffffffffff8080),
            UINT64_C(0xffffffff80008080),
            UINT64_C(0x8000000080008080),
        },
    };
    unsigned case_count = 0;
    for (unsigned width_index = 0; width_index < 2; width_index++) {
        bool is_64 = width_index != 0;
        byte_t width = is_64 ? 64 : 32;
        qword_t mask = is_64 ? UINT64_MAX : UINT32_MAX;
        qword_t sign = UINT64_C(1) << (width - 1);
        for (unsigned extend_index = 0;
                extend_index < array_size(extended_values[0]);
                extend_index++) {
            for (byte_t shift = 0; shift <= 4; shift++) {
                struct cpu_state initial;
                init_differential_cpu(&initial);
                initial.x[4] = UINT64_C(0x8123456789abcdef);
                initial.x[5] = UINT64_C(0x8000000080008080);
                initial.x[6] = UINT64_MAX;
                aarch64_set_exclusive(&initial,
                        DATA_PAGE + 0x40, 8, false,
                        UINT64_C(0x1122), 0, NULL, 3, 5, 7);

                qword_t left = initial.x[4] & mask;
                qword_t right =
                        (extended_values[width_index][extend_index] <<
                        shift) & mask;
                qword_t expected = (left - right) & mask;
                dword_t expected_nzcv =
                        (expected & sign ?
                                UINT32_C(1) << 31 : 0) |
                        (expected == 0 ?
                                UINT32_C(1) << 30 : 0) |
                        (left >= right ?
                                UINT32_C(1) << 29 : 0) |
                        (((left ^ right) &
                                (left ^ expected) & sign) != 0 ?
                                UINT32_C(1) << 28 : 0);
                struct cpu_state result = run_fast_differential(
                        encode_add_sub_extended(is_64,
                                true, true,
                                (enum aarch64_extend_type) extend_index,
                                shift, 5, 4, 6),
                        initial, AARCH64_STEP_RETIRED);
                assert(result.x[4] == initial.x[4]);
                assert(result.x[5] == initial.x[5]);
                assert(result.x[6] == expected);
                assert(result.sp == initial.sp);
                assert(result.pc == CODE_PAGE + 4);
                assert(result.cycle == initial.cycle + 1);
                assert(result.nzcv == expected_nzcv);
                assert(result.fpcr == initial.fpcr);
                assert(result.fpsr == initial.fpsr);
                assert(result.tpidr_el0 == initial.tpidr_el0);
                assert(memcmp(result.v, initial.v,
                        sizeof(result.v)) == 0);
                assert(result.exclusive.valid);
                assert(result.exclusive.address == DATA_PAGE + 0x40);
                assert(result.exclusive.write_epoch == 5);
                assert(result.exclusive.sync_identity == 7);
                case_count++;
            }
        }
    }

    static const struct {
        bool is_64;
        qword_t left;
        qword_t right;
        qword_t expected;
        dword_t nzcv;
    } flag_cases[] = {
        {true, 7, 7, 0, UINT32_C(0x60000000)},
        {true, 0, 1, UINT64_MAX, UINT32_C(0x80000000)},
        {
            true,
            UINT64_C(0x8000000000000000),
            1,
            UINT64_C(0x7fffffffffffffff),
            UINT32_C(0x30000000),
        },
        {
            true,
            UINT64_C(0x7fffffffffffffff),
            UINT64_MAX,
            UINT64_C(0x8000000000000000),
            UINT32_C(0x90000000),
        },
        {
            false,
            UINT64_C(0xdeadbeef00000005),
            UINT64_C(0xaaaaaaaa00000003),
            2,
            UINT32_C(0x20000000),
        },
    };
    for (unsigned index = 0; index < array_size(flag_cases); index++) {
        struct cpu_state initial;
        init_differential_cpu(&initial);
        initial.x[4] = flag_cases[index].left;
        initial.x[5] = flag_cases[index].right;
        initial.x[6] = UINT64_MAX;
        enum aarch64_extend_type extend_type =
                flag_cases[index].is_64 ?
                AARCH64_EXTEND_UXTX : AARCH64_EXTEND_UXTW;
        struct cpu_state result = run_fast_differential(
                encode_add_sub_extended(flag_cases[index].is_64,
                        true, true, extend_type, 0, 5, 4, 6),
                initial, AARCH64_STEP_RETIRED);
        assert(result.x[4] == initial.x[4]);
        assert(result.x[5] == initial.x[5]);
        assert(result.x[6] == flag_cases[index].expected);
        assert(result.sp == initial.sp);
        assert(result.nzcv == flag_cases[index].nzcv);
        case_count++;
    }

    static const struct {
        byte_t rn;
        byte_t rm;
        byte_t rd;
    } register_cases[] = {
        {31, 5, 6},
        {4, 5, 31},
        {31, 5, 31},
        {4, 31, 6},
        {4, 5, 4},
        {4, 5, 5},
        {4, 4, 4},
    };
    for (unsigned width_index = 0; width_index < 2; width_index++) {
        bool is_64 = width_index != 0;
        byte_t width = is_64 ? 64 : 32;
        qword_t mask = is_64 ? UINT64_MAX : UINT32_MAX;
        qword_t sign = UINT64_C(1) << (width - 1);
        for (unsigned index = 0;
                index < array_size(register_cases); index++) {
            struct cpu_state initial;
            init_differential_cpu(&initial);
            initial.sp = UINT64_C(0x1111222233334445);
            initial.x[4] = UINT64_C(0x0123456789abcdef);
            initial.x[5] = UINT64_C(0x8000000080008080);
            initial.x[6] = UINT64_MAX;
            aarch64_set_exclusive(&initial,
                    DATA_PAGE + 0x40, 8, false,
                    UINT64_C(0x1122), 0, NULL, 3, 5, 7);
            qword_t registers[array_size(initial.x)];
            memcpy(registers, initial.x, sizeof(registers));

            qword_t left = (register_cases[index].rn == 31 ?
                    initial.sp :
                    initial.x[register_cases[index].rn]) & mask;
            qword_t right = register_cases[index].rm == 31 ?
                    0 : initial.x[register_cases[index].rm] & UINT32_MAX;
            if ((right & (UINT64_C(1) << 31)) != 0)
                right |= mask & ~UINT64_C(0xffffffff);
            right = (right << 3) & mask;
            qword_t expected = (left - right) & mask;
            dword_t expected_nzcv =
                    (expected & sign ? UINT32_C(1) << 31 : 0) |
                    (expected == 0 ? UINT32_C(1) << 30 : 0) |
                    (left >= right ? UINT32_C(1) << 29 : 0) |
                    (((left ^ right) &
                            (left ^ expected) & sign) != 0 ?
                            UINT32_C(1) << 28 : 0);
            struct cpu_state result = run_fast_differential(
                    encode_add_sub_extended(is_64, true, true,
                            AARCH64_EXTEND_SXTW, 3,
                            register_cases[index].rm,
                            register_cases[index].rn,
                            register_cases[index].rd),
                    initial, AARCH64_STEP_RETIRED);
            for (unsigned reg = 0; reg < array_size(result.x); reg++) {
                if (register_cases[index].rd == 31 ||
                        reg != register_cases[index].rd)
                    assert(result.x[reg] == registers[reg]);
            }
            if (register_cases[index].rd != 31)
                assert(result.x[register_cases[index].rd] == expected);
            assert(result.sp == initial.sp);
            assert(result.nzcv == expected_nzcv);
            assert(result.exclusive.valid);
            assert(result.exclusive.write_epoch == 5);
            assert(result.exclusive.sync_identity == 7);
            case_count++;
        }
    }

    assert(encode_add_sub_extended(false, true, true,
            AARCH64_EXTEND_UXTB, 0, 1, 2, 31) ==
            INSTRUCTION_SUBS_WZR_W2_W1_UXTB);
    struct cpu_state initial;
    init_differential_cpu(&initial);
    initial.x[1] = UINT64_C(0xaaaaaaaa00000080);
    initial.x[2] = UINT64_C(0xbbbbbbbb00000080);
    qword_t registers[array_size(initial.x)];
    memcpy(registers, initial.x, sizeof(registers));
    struct cpu_state result = run_fast_differential(
            INSTRUCTION_SUBS_WZR_W2_W1_UXTB,
            initial, AARCH64_STEP_RETIRED);
    assert(memcmp(result.x, registers, sizeof(registers)) == 0);
    assert(result.sp == initial.sp);
    assert(result.nzcv == UINT32_C(0x60000000));
    case_count++;

    assert(encode_add_sub_extended(true, true, true,
            AARCH64_EXTEND_UXTB, 4, 2, 1, 0) ==
            INSTRUCTION_SUBS_X0_X1_W2_UXTB_4);
    init_differential_cpu(&initial);
    initial.x[0] = UINT64_MAX;
    initial.x[1] = 0;
    initial.x[2] = UINT64_C(0xaaaaaaaa00000001);
    result = run_fast_differential(
            INSTRUCTION_SUBS_X0_X1_W2_UXTB_4,
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[0] == UINT64_C(0xfffffffffffffff0));
    assert(result.x[1] == initial.x[1]);
    assert(result.x[2] == initial.x[2]);
    assert(result.sp == initial.sp);
    assert(result.nzcv == UINT32_C(0x80000000));
    case_count++;

    assert(case_count == 101);
}

static void test_sub_extended_invalid_shifts(void) {
    const dword_t instructions[] = {
        UINT32_C(0x4b21145f),
        UINT32_C(0x4b21185f),
        UINT32_C(0x4b211c5f),
    };
    struct test_fixture c_fixture;
    struct test_fixture threaded_fixture;
    init_fixture(&c_fixture);
    init_fixture(&threaded_fixture);
    for (unsigned index = 0; index < array_size(instructions); index++) {
        struct aarch64_decoded decoded;
        assert(!aarch64_decode(instructions[index], &decoded));
        write_instruction(&c_fixture.tlb, CODE_PAGE + index * 4,
                instructions[index]);
        write_instruction(&threaded_fixture.tlb, CODE_PAGE + index * 4,
                instructions[index]);
    }
    struct test_memory expected_memory = c_fixture.memory;

    struct aarch64_runner c_runner;
    struct aarch64_runner threaded_runner;
    assert(aarch64_runner_init_backend(
            &c_runner, &c_fixture.tlb, AARCH64_BACKEND_C));
    assert(aarch64_runner_init_backend(&threaded_runner,
            &threaded_fixture.tlb, AARCH64_BACKEND_THREADED));
    struct cpu_state c_cpu;
    struct cpu_state threaded_cpu;
    init_differential_cpu(&c_cpu);
    init_differential_cpu(&threaded_cpu);
    for (unsigned index = 0; index < array_size(instructions); index++) {
        guest_addr_t pc = CODE_PAGE + index * 4;
        c_cpu.pc = pc;
        threaded_cpu.pc = pc;
        struct cpu_state expected = c_cpu;
        struct aarch64_step_result c_result =
                aarch64_run_one(&c_runner, &c_cpu);
        struct aarch64_step_result threaded_result =
                aarch64_run_one(&threaded_runner, &threaded_cpu);
        assert(c_result.stop == AARCH64_STEP_UNDEFINED);
        assert(c_result.instruction == instructions[index]);
        assert_step_equal(&c_result, &threaded_result);
        assert_cpu_equal(&c_cpu, &threaded_cpu);
        assert_cpu_equal(&c_cpu, &expected);
        assert_memory_equal(&c_fixture.memory,
                &threaded_fixture.memory);
        assert_memory_equal(&c_fixture.memory, &expected_memory);
        assert_stats(&threaded_runner, 0, index + 1, 0, 0);
    }
}

static void test_extended_sibling_fallback(void) {
    assert(encode_add_sub_extended(true, false, true,
            AARCH64_EXTEND_SXTW, 2, 23, 31, 22) ==
            INSTRUCTION_ADDS_X22_SP_W23_SXTW_2);

    struct test_fixture fixture;
    init_fixture(&fixture);
    write_instruction(&fixture.tlb,
            CODE_PAGE, INSTRUCTION_ADDS_X22_SP_W23_SXTW_2);

    struct aarch64_runner runner;
    assert(aarch64_runner_init_backend(
            &runner, &fixture.tlb, AARCH64_BACKEND_THREADED));
    struct cpu_state cpu;
    init_differential_cpu(&cpu);
    cpu.sp = UINT64_C(0x1000);
    cpu.x[23] = 1;
    aarch64_set_exclusive(&cpu, DATA_PAGE + 0x40,
            8, false, UINT64_C(0x1122), 0, NULL, 3, 5, 7);

    assert(aarch64_run_one(&runner, &cpu).stop ==
            AARCH64_STEP_RETIRED);
    assert(cpu.x[22] == UINT64_C(0x1004));
    assert(cpu.sp == UINT64_C(0x1000));
    assert(cpu.nzcv == 0);
    assert(cpu.pc == CODE_PAGE + 4);
    assert(cpu.cycle == 10);
    assert(cpu.exclusive.valid);
    assert(cpu.exclusive.write_epoch == 5);
    assert(cpu.exclusive.sync_identity == 7);
    assert_stats(&runner, 0, 1, 0, 1);
}

static void test_fast_sub_shifted_differential(void) {
    static const enum aarch64_shift_type shift_types[] = {
        AARCH64_SHIFT_LSL,
        AARCH64_SHIFT_LSR,
        AARCH64_SHIFT_ASR,
    };
    unsigned case_count = 0;
    for (unsigned width_index = 0; width_index < 2; width_index++) {
        bool is_64 = width_index != 0;
        byte_t width = is_64 ? 64 : 32;
        qword_t mask = is_64 ? UINT64_MAX : UINT32_MAX;
        qword_t sign = UINT64_C(1) << (width - 1);
        const byte_t shifts[] = {0, 1, (byte_t) (width - 1)};
        for (unsigned type_index = 0;
                type_index < array_size(shift_types); type_index++) {
            for (unsigned shift_index = 0;
                    shift_index < array_size(shifts); shift_index++) {
                byte_t shift = shifts[shift_index];
                struct cpu_state initial;
                init_differential_cpu(&initial);
                initial.x[4] = UINT64_C(0x8123456789abcdef);
                initial.x[5] = UINT64_C(0xfedcba9886543211);
                initial.x[6] = UINT64_MAX;
                aarch64_set_exclusive(&initial,
                        DATA_PAGE + 0x40, 8, false,
                        UINT64_C(0x1122), 0, NULL, 3, 5, 7);

                qword_t left = initial.x[4] & mask;
                qword_t source = initial.x[5] & mask;
                qword_t right = source;
                if (shift != 0) {
                    if (shift_types[type_index] == AARCH64_SHIFT_LSL) {
                        right = (source << shift) & mask;
                    } else if (shift_types[type_index] ==
                            AARCH64_SHIFT_LSR) {
                        right = source >> shift;
                    } else {
                        right = source >> shift;
                        if ((source & sign) != 0)
                            right |= mask << (width - shift);
                        right &= mask;
                    }
                }
                qword_t expected = (left - right) & mask;
                struct cpu_state result = run_fast_differential(
                        encode_sub_shifted(is_64,
                                shift_types[type_index], shift,
                                5, 4, 6),
                        initial, AARCH64_STEP_RETIRED);
                assert(result.x[4] == initial.x[4]);
                assert(result.x[5] == initial.x[5]);
                assert(result.x[6] == expected);
                assert(result.pc == CODE_PAGE + 4);
                assert(result.cycle == initial.cycle + 1);
                assert(result.sp == initial.sp);
                assert(result.nzcv == initial.nzcv);
                assert(result.fpcr == initial.fpcr);
                assert(result.fpsr == initial.fpsr);
                assert(result.tpidr_el0 == initial.tpidr_el0);
                assert(memcmp(result.v, initial.v,
                        sizeof(result.v)) == 0);
                assert(result.exclusive.valid);
                assert(result.exclusive.address == DATA_PAGE + 0x40);
                assert(result.exclusive.write_epoch == 5);
                assert(result.exclusive.sync_identity == 7);
                case_count++;
            }
        }
    }
    assert(case_count == 18);

    struct cpu_state initial;
    init_differential_cpu(&initial);
    initial.x[1] = 7;
    initial.x[5] = UINT64_MAX;
    initial.x[6] = 100;
    assert(encode_sub_shifted(true, AARCH64_SHIFT_LSL,
            0, 1, 6, 5) == INSTRUCTION_SUB_X5_X6_X1);
    struct cpu_state result = run_fast_differential(
            INSTRUCTION_SUB_X5_X6_X1,
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[1] == 7);
    assert(result.x[5] == 93);
    assert(result.x[6] == 100);
    assert(result.nzcv == initial.nzcv);

    init_differential_cpu(&initial);
    initial.x[0] = UINT64_MAX;
    initial.x[3] = UINT64_C(0xaaaaaaaa00000003);
    initial.x[4] = UINT64_C(0xffffffff0000000a);
    assert(encode_sub_shifted(false, AARCH64_SHIFT_LSL,
            0, 3, 4, 0) == INSTRUCTION_SUB_W0_W4_W3);
    result = run_fast_differential(
            INSTRUCTION_SUB_W0_W4_W3,
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[0] == 7);
    assert(result.x[3] == initial.x[3]);
    assert(result.x[4] == initial.x[4]);
    assert(result.nzcv == initial.nzcv);

    init_differential_cpu(&initial);
    initial.x[5] = 3;
    result = run_fast_differential(encode_sub_shifted(
            true, AARCH64_SHIFT_LSL, 1, 5, 31, 6),
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[6] == UINT64_MAX - 5);
    assert(result.sp == initial.sp);
    assert(result.nzcv == initial.nzcv);

    init_differential_cpu(&initial);
    initial.x[4] = UINT64_C(0xabcdef1289abcdef);
    result = run_fast_differential(encode_sub_shifted(
            false, AARCH64_SHIFT_ASR, 31, 31, 4, 6),
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[6] == UINT64_C(0x0000000089abcdef));
    assert(result.sp == initial.sp);
    assert(result.nzcv == initial.nzcv);

    init_differential_cpu(&initial);
    initial.x[4] = UINT64_C(0x123456789abcdef0);
    initial.x[5] = UINT64_C(0x0fedcba987654321);
    qword_t registers[array_size(initial.x)];
    memcpy(registers, initial.x, sizeof(registers));
    result = run_fast_differential(encode_sub_shifted(
            true, AARCH64_SHIFT_LSL, 0, 5, 4, 31),
            initial, AARCH64_STEP_RETIRED);
    assert(memcmp(result.x, registers, sizeof(registers)) == 0);
    assert(result.sp == initial.sp);
    assert(result.nzcv == initial.nzcv);

    init_differential_cpu(&initial);
    initial.x[4] = 11;
    initial.x[5] = 3;
    result = run_fast_differential(encode_sub_shifted(
            true, AARCH64_SHIFT_LSL, 0, 5, 4, 4),
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[4] == 8);
    assert(result.x[5] == 3);
    assert(result.nzcv == initial.nzcv);

    init_differential_cpu(&initial);
    initial.x[4] = 11;
    initial.x[5] = 3;
    result = run_fast_differential(encode_sub_shifted(
            true, AARCH64_SHIFT_LSL, 0, 5, 4, 5),
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[4] == 11);
    assert(result.x[5] == 8);
    assert(result.nzcv == initial.nzcv);
}

static void assert_fast_ccmp(dword_t instruction,
        struct cpu_state initial, dword_t expected_nzcv) {
    struct cpu_state expected = initial;
    expected.pc = CODE_PAGE + 4;
    expected.cycle++;
    expected.nzcv = expected_nzcv;
    struct cpu_state result = run_fast_differential(
            instruction, initial, AARCH64_STEP_RETIRED);
    assert_cpu_equal(&result, &expected);
}

static void test_fast_ccmp_differential(void) {
    static const byte_t true_nzcv[] = {
        4, 0, 2, 0, 8, 0, 1, 0,
        2, 0, 0, 8, 0, 4, 0, 15,
    };
    static const byte_t false_nzcv[] = {
        0, 4, 0, 2, 0, 8, 0,
        1, 0, 2, 8, 0, 4, 0,
    };
    unsigned case_count = 0;

    for (byte_t condition = 0; condition < 16; condition++) {
        struct cpu_state initial;
        init_differential_cpu(&initial);
        initial.x[4] = 7;
        initial.x[5] = 7;
        initial.nzcv = (dword_t) true_nzcv[condition] << 28;
        aarch64_set_exclusive(&initial, DATA_PAGE + 0x40,
                8, false, UINT64_C(0x1122), 0, NULL, 3, 5, 7);
        assert_fast_ccmp(encode_conditional_compare(
                true, true, false, 5, condition, 4, 5),
                initial, UINT32_C(0x60000000));
        case_count++;

        if (condition < array_size(false_nzcv)) {
            init_differential_cpu(&initial);
            initial.x[4] = 7;
            initial.x[5] = 7;
            initial.nzcv = (dword_t) false_nzcv[condition] << 28;
            assert_fast_ccmp(encode_conditional_compare(
                    true, true, false, 5, condition, 4, 5),
                    initial, UINT32_C(0x50000000));
            case_count++;
        }
    }

    for (byte_t index = 0; index < 16; index++) {
        struct cpu_state initial;
        init_differential_cpu(&initial);
        byte_t fallback = index ^ 15;
        byte_t condition = (index & 4) != 0 ? 1 : 0;
        initial.nzcv = (dword_t) index << 28;
        initial.x[4] = UINT64_C(0x1234567887654321);
        initial.x[5] = UINT64_C(0xfedcba9876543210);
        assert_fast_ccmp(encode_conditional_compare(
                (index & 1) != 0, true, (index & 2) != 0,
                5, condition, 4, fallback),
                initial, (dword_t) fallback << 28);
        case_count++;
    }

    struct {
        bool is_64;
        bool immediate;
        qword_t left;
        qword_t right;
        dword_t expected_nzcv;
    } const flag_cases[] = {
        {true, false, 7, 7, UINT32_C(0x60000000)},
        {true, true, 0, 1, UINT32_C(0x80000000)},
        {
            false,
            false,
            UINT64_C(0x1234567880000000),
            UINT64_C(0xabcdef0100000001),
            UINT32_C(0x30000000),
        },
        {
            true,
            false,
            INT64_MAX,
            UINT64_MAX,
            UINT32_C(0x90000000),
        },
        {
            false,
            false,
            UINT64_C(0xdeadbeef00000005),
            UINT64_C(0xaaaaaaaa00000003),
            UINT32_C(0x20000000),
        },
        {false, true, 0, 0, UINT32_C(0x60000000)},
        {false, true, 0, 31, UINT32_C(0x80000000)},
    };
    for (unsigned index = 0; index < array_size(flag_cases); index++) {
        struct cpu_state initial;
        init_differential_cpu(&initial);
        initial.x[4] = flag_cases[index].left;
        initial.x[5] = flag_cases[index].right;
        initial.nzcv = UINT32_C(0xf0000000);
        assert_fast_ccmp(encode_conditional_compare(
                flag_cases[index].is_64, true,
                flag_cases[index].immediate,
                flag_cases[index].immediate ?
                        (byte_t) flag_cases[index].right : 5,
                14, 4, 3),
                initial, flag_cases[index].expected_nzcv);
        case_count++;
    }

    assert(encode_conditional_compare(false, true, false,
            1, 1, 0, 4) == INSTRUCTION_CCMP_W0_W1_4_NE);
    struct cpu_state initial;
    init_differential_cpu(&initial);
    initial.x[0] = UINT32_C(0x80000000);
    initial.x[1] = 1;
    initial.nzcv = 0;
    assert_fast_ccmp(INSTRUCTION_CCMP_W0_W1_4_NE,
            initial, UINT32_C(0x30000000));
    case_count++;

    assert(encode_conditional_compare(false, true, false,
            19, 1, 0, 4) == INSTRUCTION_CCMP_W0_W19_4_NE);
    init_differential_cpu(&initial);
    initial.x[0] = 10;
    initial.x[19] = 3;
    initial.nzcv = UINT32_C(0xf0000000);
    assert_fast_ccmp(INSTRUCTION_CCMP_W0_W19_4_NE,
            initial, UINT32_C(0x40000000));
    case_count++;

    init_differential_cpu(&initial);
    initial.x[4] = 3;
    assert_fast_ccmp(encode_conditional_compare(
            true, true, false, 4, 14, 31, 0),
            initial, UINT32_C(0x80000000));
    case_count++;

    init_differential_cpu(&initial);
    initial.x[4] = 3;
    assert_fast_ccmp(encode_conditional_compare(
            true, true, false, 31, 14, 4, 0),
            initial, UINT32_C(0x20000000));
    case_count++;

    assert(case_count == 57);
}

static void test_ccmn_sibling_fallback(void) {
    assert(encode_conditional_compare(true, false, true,
            1, 0, 0, 0) == INSTRUCTION_CCMN_X0_1_0_EQ);
    struct aarch64_decoded decoded;
    assert(aarch64_decode(INSTRUCTION_CCMN_X0_1_0_EQ, &decoded));
    assert(decoded.opcode == AARCH64_OP_CCMN);

    struct test_fixture fixture;
    init_fixture(&fixture);
    write_instruction(&fixture.tlb,
            CODE_PAGE, INSTRUCTION_CCMN_X0_1_0_EQ);
    struct aarch64_runner runner;
    assert(aarch64_runner_init_backend(
            &runner, &fixture.tlb, AARCH64_BACKEND_THREADED));
    struct cpu_state cpu;
    init_differential_cpu(&cpu);
    cpu.x[0] = INT64_MAX;
    cpu.nzcv = UINT32_C(0x40000000);
    aarch64_set_exclusive(&cpu, DATA_PAGE + 0x40,
            8, false, UINT64_C(0x1122), 0, NULL, 3, 5, 7);
    qword_t registers[array_size(cpu.x)];
    memcpy(registers, cpu.x, sizeof(registers));
    qword_t sp = cpu.sp;

    assert(run_at(&runner, &cpu, CODE_PAGE).stop ==
            AARCH64_STEP_RETIRED);
    assert(memcmp(cpu.x, registers, sizeof(registers)) == 0);
    assert(cpu.sp == sp);
    assert(cpu.nzcv == UINT32_C(0x90000000));
    assert(cpu.pc == CODE_PAGE + 4);
    assert(cpu.cycle == 10);
    assert(cpu.exclusive.valid);
    assert(cpu.exclusive.write_epoch == 5);
    assert(cpu.exclusive.sync_identity == 7);
    assert_stats(&runner, 0, 1, 0, 1);
}

static void assert_fast_csel(dword_t instruction,
        struct cpu_state initial, byte_t rd, qword_t expected_value) {
    struct cpu_state expected = initial;
    expected.pc = CODE_PAGE + 4;
    expected.cycle++;
    if (rd != 31)
        expected.x[rd] = expected_value;
    struct cpu_state result = run_fast_differential(
            instruction, initial, AARCH64_STEP_RETIRED);
    assert_cpu_equal(&result, &expected);
}

static void test_fast_csel_differential(void) {
    static const byte_t true_nzcv[] = {
        4, 0, 2, 0, 8, 0, 1, 0,
        2, 0, 0, 8, 0, 4, 0, 15,
    };
    static const byte_t false_nzcv[] = {
        0, 4, 0, 2, 0, 8, 0,
        1, 0, 2, 8, 0, 4, 0,
    };
    unsigned case_count = 0;

    for (unsigned width_index = 0; width_index < 2; width_index++) {
        bool is_64 = width_index != 0;
        qword_t mask = is_64 ? UINT64_MAX : UINT32_MAX;
        qword_t rn_value = UINT64_C(0x8877665544332211);
        qword_t rm_value = UINT64_C(0x11223344aabbccdd);
        for (byte_t condition = 0; condition < 16; condition++) {
            struct cpu_state initial;
            init_differential_cpu(&initial);
            initial.x[3] = UINT64_C(0xdeadbeefdeadbeef);
            initial.x[4] = rn_value;
            initial.x[5] = rm_value;
            initial.nzcv = (dword_t) true_nzcv[condition] << 28;
            aarch64_set_exclusive(&initial, DATA_PAGE + 0x40,
                    8, false, UINT64_C(0x1122), 0,
                    NULL, 3, 5, 7);
            assert_fast_csel(encode_conditional_select(
                    AARCH64_OP_CSEL, is_64,
                    condition, 5, 4, 3),
                    initial, 3, rn_value & mask);
            case_count++;

            if (condition < array_size(false_nzcv)) {
                init_differential_cpu(&initial);
                initial.x[3] = UINT64_C(0xdeadbeefdeadbeef);
                initial.x[4] = rn_value;
                initial.x[5] = rm_value;
                initial.nzcv =
                        (dword_t) false_nzcv[condition] << 28;
                aarch64_set_exclusive(&initial, DATA_PAGE + 0x40,
                        8, false, UINT64_C(0x1122), 0,
                        NULL, 3, 5, 7);
                assert_fast_csel(encode_conditional_select(
                        AARCH64_OP_CSEL, is_64,
                        condition, 5, 4, 3),
                        initial, 3, rm_value & mask);
                case_count++;
            }
        }
    }
    assert(case_count == 60);

    assert(encode_conditional_select(AARCH64_OP_CSEL,
            true, 0, 1, 0, 1) ==
            INSTRUCTION_CSEL_X1_X0_X1_EQ);
    assert(encode_conditional_select(AARCH64_OP_CSEL,
            false, 1, 0, 19, 19) ==
            INSTRUCTION_CSEL_W19_W19_W0_NE);

    struct cpu_state initial;
    init_differential_cpu(&initial);
    initial.x[0] = UINT64_C(0x0123456789abcdef);
    initial.x[1] = UINT64_C(0xfedcba9876543210);
    initial.nzcv = UINT32_C(0x40000000);
    assert_fast_csel(INSTRUCTION_CSEL_X1_X0_X1_EQ,
            initial, 1, initial.x[0]);
    case_count++;

    init_differential_cpu(&initial);
    initial.x[0] = UINT64_C(0x0123456789abcdef);
    initial.x[1] = UINT64_C(0xfedcba9876543210);
    initial.nzcv = 0;
    assert_fast_csel(INSTRUCTION_CSEL_X1_X0_X1_EQ,
            initial, 1, initial.x[1]);
    case_count++;

    init_differential_cpu(&initial);
    initial.x[0] = UINT64_C(0xaaaaaaaa12345678);
    initial.x[19] = UINT64_C(0xffffffff89abcdef);
    initial.nzcv = 0;
    assert_fast_csel(INSTRUCTION_CSEL_W19_W19_W0_NE,
            initial, 19, UINT32_C(0x89abcdef));
    case_count++;

    init_differential_cpu(&initial);
    initial.x[0] = UINT64_C(0xaaaaaaaa12345678);
    initial.x[19] = UINT64_C(0xffffffff89abcdef);
    initial.nzcv = UINT32_C(0x40000000);
    assert_fast_csel(INSTRUCTION_CSEL_W19_W19_W0_NE,
            initial, 19, UINT32_C(0x12345678));
    case_count++;

    init_differential_cpu(&initial);
    initial.x[5] = UINT64_C(0x8877665544332211);
    initial.nzcv = UINT32_C(0x40000000);
    assert_fast_csel(encode_conditional_select(
            AARCH64_OP_CSEL, true, 0, 5, 31, 3),
            initial, 3, 0);
    case_count++;

    init_differential_cpu(&initial);
    initial.x[4] = UINT64_C(0x8877665544332211);
    initial.nzcv = 0;
    assert_fast_csel(encode_conditional_select(
            AARCH64_OP_CSEL, true, 0, 31, 4, 3),
            initial, 3, 0);
    case_count++;

    init_differential_cpu(&initial);
    initial.x[4] = UINT64_C(0x8877665544332211);
    initial.x[5] = UINT64_C(0x11223344aabbccdd);
    initial.nzcv = UINT32_C(0x40000000);
    assert_fast_csel(encode_conditional_select(
            AARCH64_OP_CSEL, true, 0, 5, 4, 31),
            initial, 31, 0);
    case_count++;

    assert(case_count == 67);
}

static void assert_fast_csinc(dword_t instruction,
        struct cpu_state initial, byte_t rd, qword_t expected_value) {
    struct cpu_state expected = initial;
    expected.pc = CODE_PAGE + 4;
    expected.cycle++;
    if (rd != 31)
        expected.x[rd] = expected_value;
    struct cpu_state result = run_fast_differential(
            instruction, initial, AARCH64_STEP_RETIRED);
    assert_cpu_equal(&result, &expected);
}

static void test_fast_csinc_differential(void) {
    static const byte_t true_nzcv[] = {
        4, 0, 2, 0, 8, 0, 1, 0,
        2, 0, 0, 8, 0, 4, 0, 15,
    };
    static const byte_t false_nzcv[] = {
        0, 4, 0, 2, 0, 8, 0,
        1, 0, 2, 8, 0, 4, 0,
    };
    unsigned case_count = 0;

    for (unsigned width_index = 0; width_index < 2; width_index++) {
        bool is_64 = width_index != 0;
        qword_t mask = is_64 ? UINT64_MAX : UINT32_MAX;
        qword_t source = is_64 ?
                UINT64_C(0x8877665544332211) :
                UINT64_C(0xffffffff44332211);
        for (byte_t condition = 0; condition < 16; condition++) {
            struct cpu_state initial;
            init_differential_cpu(&initial);
            initial.x[3] = UINT64_C(0xdeadbeefdeadbeef);
            initial.x[4] = source;
            initial.x[5] = mask;
            initial.nzcv = (dword_t) true_nzcv[condition] << 28;
            aarch64_set_exclusive(&initial, DATA_PAGE + 0x40,
                    8, false, UINT64_C(0x1122), 0,
                    NULL, 3, 5, 7);
            assert_fast_csinc(encode_conditional_select(
                    AARCH64_OP_CSINC, is_64,
                    condition, 5, 4, 3),
                    initial, 3, source & mask);
            case_count++;

            if (condition < array_size(false_nzcv)) {
                init_differential_cpu(&initial);
                initial.x[3] = UINT64_C(0xdeadbeefdeadbeef);
                initial.x[4] = source;
                initial.x[5] = mask;
                initial.nzcv =
                        (dword_t) false_nzcv[condition] << 28;
                aarch64_set_exclusive(&initial, DATA_PAGE + 0x40,
                        8, false, UINT64_C(0x1122), 0,
                        NULL, 3, 5, 7);
                assert_fast_csinc(encode_conditional_select(
                        AARCH64_OP_CSINC, is_64,
                        condition, 5, 4, 3),
                        initial, 3, 0);
                case_count++;
            }
        }
    }
    assert(case_count == 60);

    assert(encode_conditional_select(AARCH64_OP_CSINC,
            true, 1, 31, 31, 3) ==
            INSTRUCTION_CSINC_X3_XZR_XZR_NE);
    assert(encode_conditional_select(AARCH64_OP_CSINC,
            false, 1, 31, 31, 20) ==
            INSTRUCTION_CSINC_W20_WZR_WZR_NE);
    const struct {
        dword_t instruction;
        byte_t rd;
    } profile_cases[] = {
        {INSTRUCTION_CSINC_X3_XZR_XZR_NE, 3},
        {INSTRUCTION_CSINC_W20_WZR_WZR_NE, 20},
    };
    for (unsigned index = 0;
            index < array_size(profile_cases); index++) {
        struct cpu_state initial;
        init_differential_cpu(&initial);
        initial.x[profile_cases[index].rd] = UINT64_MAX;
        initial.nzcv = 0;
        assert_fast_csinc(profile_cases[index].instruction,
                initial, profile_cases[index].rd, 0);
        case_count++;

        init_differential_cpu(&initial);
        initial.x[profile_cases[index].rd] = UINT64_MAX;
        initial.nzcv = UINT32_C(0x40000000);
        assert_fast_csinc(profile_cases[index].instruction,
                initial, profile_cases[index].rd, 1);
        case_count++;
    }

    struct cpu_state initial;
    init_differential_cpu(&initial);
    initial.x[5] = UINT64_C(0x8877665544332211);
    initial.nzcv = 0;
    assert_fast_csinc(encode_conditional_select(
            AARCH64_OP_CSINC, true, 14, 5, 31, 3),
            initial, 3, 0);
    case_count++;

    init_differential_cpu(&initial);
    initial.x[4] = UINT64_C(0x8877665544332211);
    initial.nzcv = 0;
    assert_fast_csinc(encode_conditional_select(
            AARCH64_OP_CSINC, true, 0, 31, 4, 3),
            initial, 3, 1);
    case_count++;

    init_differential_cpu(&initial);
    initial.x[4] = UINT64_C(0x8877665544332211);
    initial.x[5] = UINT64_MAX;
    initial.nzcv = 0;
    assert_fast_csinc(encode_conditional_select(
            AARCH64_OP_CSINC, true, 0, 5, 4, 31),
            initial, 31, 0);
    case_count++;

    init_differential_cpu(&initial);
    initial.x[5] = UINT64_MAX;
    initial.nzcv = 0;
    assert_fast_csinc(encode_conditional_select(
            AARCH64_OP_CSINC, true, 0, 5, 5, 5),
            initial, 5, 0);
    case_count++;

    init_differential_cpu(&initial);
    initial.x[4] = UINT64_C(0x1122334455667788);
    initial.x[5] = 20;
    initial.nzcv = 0;
    assert_fast_csinc(encode_conditional_select(
            AARCH64_OP_CSINC, true, 0, 5, 4, 4),
            initial, 4, 21);
    case_count++;

    init_differential_cpu(&initial);
    initial.x[4] = UINT64_C(0x1122334455667788);
    initial.x[5] = 20;
    initial.nzcv = UINT32_C(0x40000000);
    assert_fast_csinc(encode_conditional_select(
            AARCH64_OP_CSINC, true, 0, 5, 4, 5),
            initial, 5, UINT64_C(0x1122334455667788));
    case_count++;

    assert(case_count == 70);
}

static void test_conditional_select_sibling_fallback(void) {
    const dword_t instructions[] = {
        encode_conditional_select(
                AARCH64_OP_CSINV, true, 0, 8, 7, 6),
        encode_conditional_select(
                AARCH64_OP_CSNEG, false, 0, 11, 10, 9),
    };
    const enum aarch64_opcode opcodes[] = {
        AARCH64_OP_CSINV,
        AARCH64_OP_CSNEG,
    };
    struct test_fixture fixture;
    init_fixture(&fixture);
    for (unsigned index = 0; index < array_size(instructions); index++) {
        struct aarch64_decoded decoded;
        assert(aarch64_decode(instructions[index], &decoded));
        assert(decoded.opcode == opcodes[index]);
        write_instruction(&fixture.tlb,
                CODE_PAGE + index * 4, instructions[index]);
    }

    struct aarch64_runner runner;
    assert(aarch64_runner_init_backend(
            &runner, &fixture.tlb, AARCH64_BACKEND_THREADED));
    struct cpu_state cpu;
    init_differential_cpu(&cpu);
    cpu.x[7] = UINT64_C(0x8877665544332211);
    cpu.x[8] = UINT64_C(0x00ff00ff00ff00ff);
    cpu.x[10] = UINT64_C(0x1122334455667788);
    cpu.x[11] = UINT64_C(0xffffffff00000003);
    cpu.nzcv = 0;
    aarch64_set_exclusive(&cpu, DATA_PAGE + 0x40,
            8, false, UINT64_C(0x1122), 0, NULL, 3, 5, 7);
    qword_t sp = cpu.sp;

    for (unsigned index = 0; index < array_size(instructions); index++) {
        assert(aarch64_run_one(&runner, &cpu).stop ==
                AARCH64_STEP_RETIRED);
    }
    assert(cpu.x[6] == UINT64_C(0xff00ff00ff00ff00));
    assert(cpu.x[9] == UINT32_C(0xfffffffd));
    assert(cpu.sp == sp);
    assert(cpu.nzcv == 0);
    assert(cpu.pc == CODE_PAGE + array_size(instructions) * 4);
    assert(cpu.cycle == 11);
    assert(cpu.exclusive.valid);
    assert(cpu.exclusive.write_epoch == 5);
    assert(cpu.exclusive.sync_identity == 7);
    assert_stats(&runner, 0, 2, 0, 2);
}

static void assert_fast_rev32(dword_t instruction,
        struct cpu_state initial, byte_t rd, qword_t expected_value) {
    struct cpu_state expected = initial;
    expected.pc = CODE_PAGE + 4;
    expected.cycle++;
    if (rd != 31)
        expected.x[rd] = expected_value;
    struct cpu_state result = run_fast_differential(
            instruction, initial, AARCH64_STEP_RETIRED);
    assert_cpu_equal(&result, &expected);
}

static void test_fast_rev32_differential(void) {
    unsigned case_count = 0;
    for (unsigned width_index = 0; width_index < 2; width_index++) {
        bool is_64 = width_index != 0;
        for (byte_t rn = 0; rn < 32; rn++) {
            for (byte_t rd = 0; rd < 32; rd++) {
                struct cpu_state initial;
                init_differential_cpu(&initial);
                for (unsigned reg = 0;
                        reg < array_size(initial.x); reg++) {
                    initial.x[reg] =
                            UINT64_C(0x0123456789abcdef) ^
                            (qword_t) (reg + 1) *
                            UINT64_C(0x0101010101010101);
                }
                aarch64_set_exclusive(&initial, DATA_PAGE + 0x40,
                        8, false, UINT64_C(0x1122), 0,
                        NULL, 3, 5, 7);
                qword_t source = rn == 31 ? 0 : initial.x[rn];
                qword_t expected = reference_rev32(source, is_64);
                assert_fast_rev32(encode_data_processing_1source(
                        is_64, 2, rn, rd),
                        initial, rd, expected);
                case_count++;
            }
        }
    }
    assert(case_count == 2048);

    assert(encode_data_processing_1source(
            false, 2, 2, 2) == INSTRUCTION_REV_W2_W2);
    assert(encode_data_processing_1source(
            false, 2, 0, 0) == INSTRUCTION_REV_W0_W0);
    assert(encode_data_processing_1source(
            true, 2, 5, 4) == INSTRUCTION_REV32_X4_X5);

    struct cpu_state initial;
    init_differential_cpu(&initial);
    initial.x[2] = UINT64_C(0xffffffff12345678);
    assert_fast_rev32(INSTRUCTION_REV_W2_W2,
            initial, 2, UINT32_C(0x78563412));
    case_count++;

    init_differential_cpu(&initial);
    initial.x[0] = UINT64_C(0xaaaaaaaa89abcdef);
    assert_fast_rev32(INSTRUCTION_REV_W0_W0,
            initial, 0, UINT32_C(0xefcdab89));
    case_count++;

    init_differential_cpu(&initial);
    initial.x[5] = UINT64_C(0x0123456789abcdef);
    assert_fast_rev32(INSTRUCTION_REV32_X4_X5,
            initial, 4, UINT64_C(0x67452301efcdab89));
    case_count++;

    assert(case_count == 2051);
}

static void assert_fast_clz(dword_t instruction,
        struct cpu_state initial, byte_t rd, qword_t expected_value) {
    struct cpu_state expected = initial;
    expected.pc = CODE_PAGE + 4;
    expected.cycle++;
    if (rd != 31)
        expected.x[rd] = expected_value;
    struct cpu_state result = run_fast_differential(
            instruction, initial, AARCH64_STEP_RETIRED);
    assert_cpu_equal(&result, &expected);
}

static void test_fast_clz_differential(void) {
    unsigned case_count = 0;
    for (unsigned width_index = 0; width_index < 2; width_index++) {
        bool is_64 = width_index != 0;
        byte_t width = is_64 ? 64 : 32;
        for (qword_t count = 0; count <= width; count++) {
            qword_t source = count == width ? 0 :
                    UINT64_C(1) << (width - 1 - count);
            if (!is_64)
                source |= UINT64_C(0xa5a5a5a500000000);
            struct cpu_state initial;
            init_differential_cpu(&initial);
            initial.x[4] = source;
            initial.x[6] = UINT64_MAX;
            aarch64_set_exclusive(&initial, DATA_PAGE + 0x40,
                    8, false, UINT64_C(0x1122), 0,
                    NULL, 3, 5, 7);
            assert_fast_clz(encode_data_processing_1source(
                    is_64, 4, 4, 6), initial, 6,
                    reference_clz(source, width));
            case_count++;
        }
    }
    assert(case_count == 98);

    assert(encode_data_processing_1source(
            false, 4, 2, 2) == INSTRUCTION_CLZ_W2_W2);
    assert(encode_data_processing_1source(
            false, 4, 0, 0) == INSTRUCTION_CLZ_W0_W0);

    struct cpu_state initial;
    init_differential_cpu(&initial);
    initial.x[2] = UINT64_C(0xffffffff00000001);
    assert_fast_clz(INSTRUCTION_CLZ_W2_W2, initial, 2, 31);
    case_count++;

    init_differential_cpu(&initial);
    initial.x[0] = UINT64_C(0xaaaaaaaa00800001);
    assert_fast_clz(INSTRUCTION_CLZ_W0_W0, initial, 0, 8);
    case_count++;

    init_differential_cpu(&initial);
    initial.x[7] = UINT64_C(0x0000000100000001);
    assert_fast_clz(encode_data_processing_1source(
            true, 4, 7, 7), initial, 7, 31);
    case_count++;

    init_differential_cpu(&initial);
    initial.sp = UINT64_C(0xfedcba9876543210);
    initial.x[3] = UINT64_MAX;
    assert_fast_clz(encode_data_processing_1source(
            false, 4, 31, 3), initial, 3, 32);
    case_count++;

    init_differential_cpu(&initial);
    initial.x[4] = UINT64_C(0x0000000100000000);
    assert_fast_clz(encode_data_processing_1source(
            true, 4, 4, 31), initial, 31, 31);
    case_count++;

    assert(case_count == 103);
}

static void test_data_processing_1source_sibling_fallback(void) {
    const dword_t instructions[] = {
        encode_data_processing_1source(false, 0, 4, 3),
        encode_data_processing_1source(true, 1, 6, 5),
        encode_data_processing_1source(true, 3, 8, 7),
        encode_data_processing_1source(true, 5, 12, 11),
    };
    const enum aarch64_opcode opcodes[] = {
        AARCH64_OP_RBIT,
        AARCH64_OP_REV16,
        AARCH64_OP_REV64,
        AARCH64_OP_CLS,
    };
    struct test_fixture fixture;
    init_fixture(&fixture);
    for (unsigned index = 0; index < array_size(instructions); index++) {
        struct aarch64_decoded decoded;
        assert(aarch64_decode(instructions[index], &decoded));
        assert(decoded.opcode == opcodes[index]);
        write_instruction(&fixture.tlb,
                CODE_PAGE + index * 4, instructions[index]);
    }

    struct aarch64_runner runner;
    assert(aarch64_runner_init_backend(
            &runner, &fixture.tlb, AARCH64_BACKEND_THREADED));
    struct cpu_state cpu;
    init_differential_cpu(&cpu);
    cpu.x[4] = 1;
    cpu.x[6] = UINT64_C(0x0123456789abcdef);
    cpu.x[8] = UINT64_C(0x0123456789abcdef);
    cpu.x[12] = 0;
    aarch64_set_exclusive(&cpu, DATA_PAGE + 0x40,
            8, false, UINT64_C(0x1122), 0, NULL, 3, 5, 7);
    qword_t sp = cpu.sp;
    dword_t nzcv = cpu.nzcv;

    for (unsigned index = 0; index < array_size(instructions); index++) {
        assert(aarch64_run_one(&runner, &cpu).stop ==
                AARCH64_STEP_RETIRED);
    }
    assert(cpu.x[3] == UINT32_C(0x80000000));
    assert(cpu.x[5] == UINT64_C(0x23016745ab89efcd));
    assert(cpu.x[7] == UINT64_C(0xefcdab8967452301));
    assert(cpu.x[11] == 63);
    assert(cpu.sp == sp);
    assert(cpu.nzcv == nzcv);
    assert(cpu.pc == CODE_PAGE + array_size(instructions) * 4);
    assert(cpu.cycle == 9 + array_size(instructions));
    assert(cpu.exclusive.valid);
    assert(cpu.exclusive.write_epoch == 5);
    assert(cpu.exclusive.sync_identity == 7);
    assert_stats(&runner, 0, 4, 0, 4);
}

static void test_fast_subs_shifted_differential(void) {
    static const enum aarch64_shift_type shift_types[] = {
        AARCH64_SHIFT_LSL,
        AARCH64_SHIFT_LSR,
        AARCH64_SHIFT_ASR,
    };
    for (unsigned width_index = 0; width_index < 2; width_index++) {
        bool is_64 = width_index != 0;
        byte_t width = is_64 ? 64 : 32;
        qword_t mask = is_64 ? UINT64_MAX : UINT32_MAX;
        qword_t sign = UINT64_C(1) << (width - 1);
        for (unsigned type_index = 0;
                type_index < array_size(shift_types); type_index++) {
            for (byte_t shift = 0; shift < width; shift++) {
                struct cpu_state initial;
                init_differential_cpu(&initial);
                initial.x[4] = UINT64_C(0x8123456789abcdef);
                initial.x[5] = UINT64_C(0xfedcba9886543211);
                initial.x[6] = UINT64_MAX;
                aarch64_set_exclusive(&initial, DATA_PAGE + 0x40,
                        8, false, UINT64_C(0x1122),
                        0, NULL, 3, 5, 7);

                qword_t left = initial.x[4] & mask;
                qword_t right = initial.x[5] & mask;
                if (shift != 0) {
                    if (shift_types[type_index] == AARCH64_SHIFT_LSL) {
                        right = (right << shift) & mask;
                    } else if (shift_types[type_index] ==
                            AARCH64_SHIFT_LSR) {
                        right >>= shift;
                    } else {
                        qword_t source = right;
                        right >>= shift;
                        if ((source & sign) != 0)
                            right |= mask << (width - shift);
                        right &= mask;
                    }
                }
                qword_t expected = (left - right) & mask;
                dword_t expected_nzcv =
                        (expected & sign ?
                                UINT32_C(1) << 31 : 0) |
                        (expected == 0 ?
                                UINT32_C(1) << 30 : 0) |
                        (left >= right ?
                                UINT32_C(1) << 29 : 0) |
                        (((left ^ right) &
                                (left ^ expected) & sign) != 0 ?
                                UINT32_C(1) << 28 : 0);
                struct cpu_state result = run_fast_differential(
                        encode_subs_shifted(is_64,
                                shift_types[type_index], shift,
                                5, 4, 6),
                        initial, AARCH64_STEP_RETIRED);
                assert(result.x[6] == expected);
                assert(result.x[4] == initial.x[4]);
                assert(result.x[5] == initial.x[5]);
                assert(result.nzcv == expected_nzcv);
                assert(result.pc == CODE_PAGE + 4);
                assert(result.cycle == initial.cycle + 1);
                assert(result.sp == initial.sp);
                assert(result.fpcr == initial.fpcr);
                assert(result.fpsr == initial.fpsr);
                assert(memcmp(result.v, initial.v,
                        sizeof(result.v)) == 0);
                assert(result.exclusive.valid);
                assert(result.exclusive.address == DATA_PAGE + 0x40);
                assert(result.exclusive.write_epoch == 5);
                assert(result.exclusive.sync_identity == 7);
            }
        }
    }

    struct {
        bool is_64;
        qword_t left;
        qword_t right;
        qword_t expected;
        dword_t nzcv;
    } const flag_cases[] = {
        {true, 7, 7, 0, UINT32_C(0x60000000)},
        {true, 0, 1, UINT64_MAX, UINT32_C(0x80000000)},
        {
            true,
            UINT64_C(0x8000000000000000),
            1,
            UINT64_C(0x7fffffffffffffff),
            UINT32_C(0x30000000),
        },
        {
            true,
            UINT64_C(0x7fffffffffffffff),
            UINT64_MAX,
            UINT64_C(0x8000000000000000),
            UINT32_C(0x90000000),
        },
        {
            false,
            UINT64_C(0xdeadbeef00000005),
            UINT64_C(0xaaaaaaaa00000003),
            2,
            UINT32_C(0x20000000),
        },
    };
    for (unsigned index = 0; index < array_size(flag_cases); index++) {
        struct cpu_state initial;
        init_differential_cpu(&initial);
        initial.x[4] = flag_cases[index].left;
        initial.x[5] = flag_cases[index].right;
        initial.x[6] = UINT64_MAX;
        struct cpu_state result = run_fast_differential(
                encode_subs_shifted(flag_cases[index].is_64,
                        AARCH64_SHIFT_LSL, 0, 5, 4, 6),
                initial, AARCH64_STEP_RETIRED);
        assert(result.x[6] == flag_cases[index].expected);
        assert(result.nzcv == flag_cases[index].nzcv);
    }

    struct cpu_state initial;
    init_differential_cpu(&initial);
    initial.x[2] = 5;
    initial.x[5] = 7;
    aarch64_set_exclusive(&initial, DATA_PAGE + 0x40,
            8, false, UINT64_C(0x1122), 0, NULL, 3, 5, 7);
    assert(encode_subs_shifted(true, AARCH64_SHIFT_LSL,
            0, 5, 2, 31) == INSTRUCTION_SUBS_XZR_X2_X5);
    struct cpu_state result = run_fast_differential(
            INSTRUCTION_SUBS_XZR_X2_X5,
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[2] == initial.x[2]);
    assert(result.x[5] == initial.x[5]);
    assert(result.nzcv == UINT32_C(0x80000000));
    assert(result.sp == initial.sp);
    assert(result.exclusive.valid);
    assert(result.exclusive.write_epoch == 5);
    assert(result.exclusive.sync_identity == 7);

    init_differential_cpu(&initial);
    initial.x[0] = UINT64_C(0xdeadbeef80000000);
    initial.x[4] = UINT64_C(0xaaaaaaaa00000001);
    assert(encode_subs_shifted(false, AARCH64_SHIFT_LSL,
            0, 4, 0, 31) == INSTRUCTION_SUBS_WZR_W0_W4);
    result = run_fast_differential(INSTRUCTION_SUBS_WZR_W0_W4,
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[0] == initial.x[0]);
    assert(result.x[4] == initial.x[4]);
    assert(result.nzcv == UINT32_C(0x30000000));
    assert(result.sp == initial.sp);

    init_differential_cpu(&initial);
    initial.x[4] = 11;
    initial.x[5] = 3;
    result = run_fast_differential(encode_subs_shifted(
            true, AARCH64_SHIFT_LSL, 0, 5, 4, 4),
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[4] == 8);
    assert(result.x[5] == 3);
    assert(result.nzcv == UINT32_C(0x20000000));

    init_differential_cpu(&initial);
    initial.x[4] = 11;
    initial.x[5] = 3;
    result = run_fast_differential(encode_subs_shifted(
            true, AARCH64_SHIFT_LSL, 0, 5, 4, 5),
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[4] == 11);
    assert(result.x[5] == 8);
    assert(result.nzcv == UINT32_C(0x20000000));

    init_differential_cpu(&initial);
    initial.x[5] = 3;
    result = run_fast_differential(encode_subs_shifted(
            true, AARCH64_SHIFT_LSL, 0, 5, 31, 6),
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[6] == UINT64_MAX - 2);
    assert(result.nzcv == UINT32_C(0x80000000));
    assert(result.sp == initial.sp);

    init_differential_cpu(&initial);
    initial.x[4] = 3;
    result = run_fast_differential(encode_subs_shifted(
            true, AARCH64_SHIFT_ASR, 63, 31, 4, 6),
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[6] == 3);
    assert(result.nzcv == UINT32_C(0x20000000));
    assert(result.sp == initial.sp);
}

static void test_fast_adrp_differential(void) {
    static const int64_t page_displacements[] = {
        -INT64_C(0x100000),
        -1,
        0,
        1,
        INT64_C(0xfffff),
    };
    static const qword_t pc_offsets[] = {
        0,
        UINT64_C(0x7fc),
        UINT64_C(0xffc),
    };
    assert(encode_adrp(-INT64_C(0x100000), 6) ==
            UINT32_C(0x90800006));
    assert(encode_adrp(-1, 6) == UINT32_C(0xf0ffffe6));
    assert(encode_adrp(0, 6) == UINT32_C(0x90000006));
    assert(encode_adrp(1, 6) == UINT32_C(0xb0000006));
    assert(encode_adrp(INT64_C(0xfffff), 6) ==
            UINT32_C(0xf07fffe6));

    for (unsigned pc_index = 0;
            pc_index < array_size(pc_offsets); pc_index++) {
        for (unsigned displacement_index = 0;
                displacement_index < array_size(page_displacements);
                displacement_index++) {
            struct cpu_state initial;
            init_differential_cpu(&initial);
            initial.x[6] = UINT64_C(0xdeadbeefdeadbeef);
            aarch64_set_exclusive(&initial, DATA_PAGE + 0x40,
                    8, false, UINT64_C(0x1122),
                    0, NULL, 3, 5, 7);
            int64_t displacement =
                    page_displacements[displacement_index] *
                    INT64_C(4096);
            dword_t instruction = encode_adrp(
                    page_displacements[displacement_index], 6);
            struct aarch64_decoded decoded;
            assert(aarch64_decode(instruction, &decoded));
            assert(decoded.opcode == AARCH64_OP_ADRP);
            assert(decoded.width == 64);
            assert(decoded.operands.pc_relative.rd == 6);
            assert(decoded.operands.pc_relative.displacement ==
                    displacement);
            guest_addr_t pc = CODE_PAGE + pc_offsets[pc_index];
            struct cpu_state result =
                    run_pc_relative_direct_differential(
                            instruction, AARCH64_OP_ADRP,
                            pc, initial);
            qword_t expected = CODE_PAGE +
                    (qword_t) displacement;
            for (unsigned register_index = 0;
                    register_index < array_size(result.x);
                    register_index++) {
                assert(result.x[register_index] ==
                        (register_index == 6 ?
                                expected :
                                initial.x[register_index]));
            }
            assert(result.pc == pc + 4);
            assert(result.cycle == initial.cycle);
            assert(result.nzcv == initial.nzcv);
            assert(result.sp == initial.sp);
            assert(result.fpcr == initial.fpcr);
            assert(result.fpsr == initial.fpsr);
            assert(memcmp(result.v, initial.v,
                    sizeof(result.v)) == 0);
            assert(result.exclusive.valid);
            assert(result.exclusive.address == DATA_PAGE + 0x40);
            assert(result.exclusive.write_epoch == 5);
            assert(result.exclusive.sync_identity == 7);
        }
    }

    struct cpu_state initial;
    init_differential_cpu(&initial);
    initial.x[1] = UINT64_MAX;
    assert(encode_adrp(86, 1) ==
            INSTRUCTION_ADRP_X1_PLUS_86_PAGES);
    struct cpu_state result = run_fast_differential(
            INSTRUCTION_ADRP_X1_PLUS_86_PAGES,
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[1] == CODE_PAGE +
            UINT64_C(86) * GUEST_MEMORY_PAGE_SIZE);
    assert(result.pc == CODE_PAGE + 4);
    assert(result.cycle == initial.cycle + 1);

    init_differential_cpu(&initial);
    initial.x[2] = UINT64_MAX;
    assert(encode_adrp(122, 2) ==
            INSTRUCTION_ADRP_X2_PLUS_122_PAGES);
    result = run_fast_differential(
            INSTRUCTION_ADRP_X2_PLUS_122_PAGES,
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[2] == CODE_PAGE +
            UINT64_C(122) * GUEST_MEMORY_PAGE_SIZE);

    init_differential_cpu(&initial);
    qword_t registers[array_size(initial.x)];
    memcpy(registers, initial.x, sizeof(registers));
    assert(encode_adrp(0, 31) == UINT32_C(0x9000001f));
    result = run_fast_differential(encode_adrp(0, 31),
            initial, AARCH64_STEP_RETIRED);
    assert(memcmp(result.x, registers, sizeof(registers)) == 0);
    assert(result.sp == initial.sp);
    assert(result.nzcv == initial.nzcv);

    init_differential_cpu(&initial);
    initial.x[6] = UINT64_MAX;
    result = run_pc_relative_direct_differential(
            encode_adrp(1, 6), AARCH64_OP_ADRP,
            UINT64_MAX - 3, initial);
    assert(result.x[6] == 0);
    assert(result.pc == 0);

    init_differential_cpu(&initial);
    initial.x[6] = 0;
    result = run_pc_relative_direct_differential(
            encode_adrp(-1, 6), AARCH64_OP_ADRP,
            0, initial);
    assert(result.x[6] == UINT64_C(0xfffffffffffff000));
    assert(result.pc == 4);
}

static void test_fast_adr_differential(void) {
    static const int64_t displacements[] = {
        -INT64_C(0x100000),
        -INT64_C(0xfffff),
        -INT64_C(4097),
        -INT64_C(4096),
        -INT64_C(13),
        -1,
        0,
        1,
        12,
        INT64_C(4095),
        INT64_C(4096),
        INT64_C(0xffffe),
        INT64_C(0xfffff),
    };
    static const dword_t expected_words[] = {
        UINT32_C(0x10800006),
        UINT32_C(0x30800006),
        UINT32_C(0x70ff7fe6),
        UINT32_C(0x10ff8006),
        UINT32_C(0x70ffff86),
        UINT32_C(0x70ffffe6),
        UINT32_C(0x10000006),
        UINT32_C(0x30000006),
        UINT32_C(0x10000066),
        UINT32_C(0x70007fe6),
        UINT32_C(0x10008006),
        UINT32_C(0x507fffe6),
        UINT32_C(0x707fffe6),
    };
    static const qword_t pc_offsets[] = {
        0,
        4,
        UINT64_C(0x7fc),
        UINT64_C(0xffc),
    };
    _Static_assert(array_size(displacements) ==
            array_size(expected_words),
            "ADR 位移与编码锚点必须一一对应");
    unsigned case_count = 0;

    for (unsigned displacement_index = 0;
            displacement_index < array_size(displacements);
            displacement_index++) {
        dword_t instruction =
                encode_adr(displacements[displacement_index], 6);
        assert(instruction == expected_words[displacement_index]);
        struct aarch64_decoded decoded;
        assert(aarch64_decode(instruction, &decoded));
        assert(decoded.opcode == AARCH64_OP_ADR);
        assert(decoded.width == 64);
        assert(decoded.operands.pc_relative.rd == 6);
        assert(decoded.operands.pc_relative.displacement ==
                displacements[displacement_index]);

        for (unsigned pc_index = 0;
                pc_index < array_size(pc_offsets); pc_index++) {
            struct cpu_state initial;
            init_differential_cpu(&initial);
            initial.x[6] = UINT64_C(0xdeadbeefdeadbeef);
            aarch64_set_exclusive(&initial, DATA_PAGE + 0x40,
                    8, false, UINT64_C(0x1122),
                    0, NULL, 3, 5, 7);
            guest_addr_t pc = CODE_PAGE + pc_offsets[pc_index];
            struct cpu_state result =
                    run_pc_relative_direct_differential(
                            instruction, AARCH64_OP_ADR,
                            pc, initial);
            qword_t expected =
                    pc + (qword_t) displacements[displacement_index];
            for (unsigned register_index = 0;
                    register_index < array_size(result.x);
                    register_index++) {
                assert(result.x[register_index] ==
                        (register_index == 6 ?
                                expected :
                                initial.x[register_index]));
            }
            assert(result.pc == pc + 4);
            assert(result.cycle == initial.cycle);
            assert(result.nzcv == initial.nzcv);
            assert(result.sp == initial.sp);
            assert(result.fpcr == initial.fpcr);
            assert(result.fpsr == initial.fpsr);
            assert(memcmp(result.v, initial.v,
                    sizeof(result.v)) == 0);
            assert(result.exclusive.valid);
            assert(result.exclusive.address == DATA_PAGE + 0x40);
            assert(result.exclusive.write_epoch == 5);
            assert(result.exclusive.sync_identity == 7);
            case_count++;
        }
    }

    assert(encode_adr(-13, 0) == UINT32_C(0x70ffff80));
    assert(encode_adr(-13, 31) == UINT32_C(0x70ffff9f));
    for (byte_t rd = 0; rd < 32; rd++) {
        struct cpu_state initial;
        init_differential_cpu(&initial);
        aarch64_set_exclusive(&initial, DATA_PAGE + 0x40,
                8, false, UINT64_C(0x1122),
                0, NULL, 3, 5, 7);
        qword_t registers[array_size(initial.x)];
        memcpy(registers, initial.x, sizeof(registers));
        dword_t instruction = encode_adr(-13, rd);
        struct aarch64_decoded decoded;
        assert(aarch64_decode(instruction, &decoded));
        assert(decoded.opcode == AARCH64_OP_ADR);
        assert(decoded.width == 64);
        assert(decoded.operands.pc_relative.rd == rd);
        assert(decoded.operands.pc_relative.displacement == -13);
        guest_addr_t pc = CODE_PAGE + 4;
        struct cpu_state result =
                run_pc_relative_direct_differential(
                        instruction, AARCH64_OP_ADR, pc, initial);
        qword_t expected = pc - UINT64_C(13);
        for (unsigned register_index = 0;
                register_index < array_size(result.x);
                register_index++) {
            assert(result.x[register_index] ==
                    (rd < array_size(result.x) &&
                            register_index == rd ?
                            expected : registers[register_index]));
        }
        assert(result.sp == initial.sp);
        assert(result.pc == pc + 4);
        assert(result.cycle == initial.cycle);
        assert(result.nzcv == initial.nzcv);
        assert(result.exclusive.valid);
        assert(result.exclusive.address == DATA_PAGE + 0x40);
        assert(result.exclusive.write_epoch == 5);
        assert(result.exclusive.sync_identity == 7);
        case_count++;
    }

    assert(encode_adr(12, 1) == INSTRUCTION_ADR_X1_PLUS_12);
    struct cpu_state initial;
    init_differential_cpu(&initial);
    initial.x[1] = UINT64_MAX;
    struct cpu_state result = run_fast_differential(
            INSTRUCTION_ADR_X1_PLUS_12,
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[1] == CODE_PAGE + 12);
    assert(result.pc == CODE_PAGE + 4);
    assert(result.cycle == initial.cycle + 1);
    case_count++;

    init_differential_cpu(&initial);
    initial.x[1] = UINT64_MAX;
    result = run_pc_relative_direct_differential(
            INSTRUCTION_ADR_X1_PLUS_12, AARCH64_OP_ADR,
            UINT64_MAX - 3, initial);
    assert(result.x[1] == 8);
    assert(result.pc == 0);
    assert(result.cycle == initial.cycle);
    case_count++;

    init_differential_cpu(&initial);
    initial.x[6] = 0;
    result = run_pc_relative_direct_differential(
            encode_adr(-1, 6), AARCH64_OP_ADR,
            0, initial);
    assert(result.x[6] == UINT64_MAX);
    assert(result.pc == 4);
    assert(result.cycle == initial.cycle);
    case_count++;

    assert(case_count == 87);
}

static void test_fast_and_immediate_differential(void) {
    unsigned case_count = 0;
    for (unsigned width_index = 0; width_index < 2; width_index++) {
        bool is_64 = width_index != 0;
        byte_t width = is_64 ? 64 : 32;
        for (byte_t element_size = 2;
                element_size <= width; element_size *= 2) {
            const byte_t ones[] = {
                1,
                (byte_t) (element_size - 1),
            };
            const byte_t rotations[] = {
                0,
                (byte_t) (element_size - 1),
            };
            for (unsigned ones_index = 0;
                    ones_index < array_size(ones); ones_index++) {
                for (unsigned rotation_index = 0;
                        rotation_index < array_size(rotations);
                        rotation_index++) {
                    qword_t immediate = reference_logical_immediate(
                            is_64, element_size, ones[ones_index],
                            rotations[rotation_index]);
                    dword_t instruction = encode_logical_immediate(
                            AARCH64_OP_AND_IMMEDIATE,
                            is_64, element_size, ones[ones_index],
                            rotations[rotation_index], 4, 6);
                    struct aarch64_decoded decoded;
                    assert(aarch64_decode(instruction, &decoded));
                    assert(decoded.opcode ==
                            AARCH64_OP_AND_IMMEDIATE);
                    assert(decoded.width == width);
                    assert(decoded.operands.logical_immediate.rn == 4);
                    assert(decoded.operands.logical_immediate.rd == 6);
                    assert(decoded.operands.logical_immediate.immediate ==
                            immediate);

                    struct cpu_state initial;
                    init_differential_cpu(&initial);
                    initial.x[4] = UINT64_C(0x8123456789abcdef);
                    initial.x[6] = UINT64_MAX;
                    aarch64_set_exclusive(&initial,
                            DATA_PAGE + 0x40, 8, false,
                            UINT64_C(0x1122), 0, NULL, 3, 5, 7);
                    struct cpu_state result = run_fast_differential(
                            instruction, initial,
                            AARCH64_STEP_RETIRED);
                    qword_t expected = initial.x[4] & immediate;
                    for (unsigned register_index = 0;
                            register_index < array_size(result.x);
                            register_index++) {
                        assert(result.x[register_index] ==
                                (register_index == 6 ?
                                        expected :
                                        initial.x[register_index]));
                    }
                    assert(result.pc == CODE_PAGE + 4);
                    assert(result.cycle == initial.cycle + 1);
                    assert(result.sp == initial.sp);
                    assert(result.nzcv == initial.nzcv);
                    assert(result.fpcr == initial.fpcr);
                    assert(result.fpsr == initial.fpsr);
                    assert(result.tpidr_el0 == initial.tpidr_el0);
                    assert(memcmp(result.v, initial.v,
                            sizeof(result.v)) == 0);
                    assert(result.exclusive.valid);
                    assert(result.exclusive.address ==
                            DATA_PAGE + 0x40);
                    assert(result.exclusive.write_epoch == 5);
                    assert(result.exclusive.sync_identity == 7);
                    case_count++;
                }
            }
        }
    }
    assert(case_count == 44);

    struct cpu_state initial;
    init_differential_cpu(&initial);
    initial.x[0] = UINT64_C(0x123456789abcdefb);
    assert(encode_logical_immediate(AARCH64_OP_AND_IMMEDIATE,
            true, 64, 60, 60, 0, 31) ==
            INSTRUCTION_AND_SP_X0_FFFFFFFFFFFFFFF0);
    struct cpu_state result = run_fast_differential(
            INSTRUCTION_AND_SP_X0_FFFFFFFFFFFFFFF0,
            initial, AARCH64_STEP_RETIRED);
    assert(result.sp == UINT64_C(0x123456789abcdef0));
    assert(result.x[0] == initial.x[0]);
    assert(result.nzcv == initial.nzcv);

    init_differential_cpu(&initial);
    initial.x[3] = UINT64_C(0xffffffff89abcdef);
    assert(encode_logical_immediate(AARCH64_OP_AND_IMMEDIATE,
            false, 32, 31, 0, 3, 0) ==
            INSTRUCTION_AND_W0_W3_7FFFFFFF);
    result = run_fast_differential(
            INSTRUCTION_AND_W0_W3_7FFFFFFF,
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[0] == UINT64_C(0x0000000009abcdef));
    assert(result.x[3] == initial.x[3]);
    assert(result.sp == initial.sp);

    init_differential_cpu(&initial);
    initial.x[16] = UINT64_C(0xabcdef12ffffffff);
    assert(encode_logical_immediate(AARCH64_OP_AND_IMMEDIATE,
            false, 32, 28, 28, 16, 31) ==
            UINT32_C(0x121c6e1f));
    result = run_fast_differential(
            UINT32_C(0x121c6e1f),
            initial, AARCH64_STEP_RETIRED);
    assert(result.sp == UINT64_C(0x00000000fffffff0));
    assert(result.x[16] == initial.x[16]);
    assert(result.nzcv == initial.nzcv);

    init_differential_cpu(&initial);
    initial.x[6] = UINT64_MAX;
    result = run_fast_differential(encode_logical_immediate(
            AARCH64_OP_AND_IMMEDIATE,
            true, 16, 8, 0, 31, 6),
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[6] == 0);
    assert(result.sp == initial.sp);
    assert(result.nzcv == initial.nzcv);

    init_differential_cpu(&initial);
    initial.x[4] = UINT64_C(0x123456789abcdef0);
    qword_t overlap_mask = reference_logical_immediate(
            true, 16, 8, 8);
    result = run_fast_differential(encode_logical_immediate(
            AARCH64_OP_AND_IMMEDIATE,
            true, 16, 8, 8, 4, 4),
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[4] == (initial.x[4] & overlap_mask));
    assert(result.sp == initial.sp);
    assert(result.nzcv == initial.nzcv);
}

static void assert_fast_ands_immediate(dword_t instruction,
        struct cpu_state initial, byte_t width,
        byte_t rd, qword_t expected_value) {
    assert(width == 32 || width == 64);
    qword_t mask = width == 32 ? UINT32_MAX : UINT64_MAX;
    expected_value &= mask;
    aarch64_set_exclusive(&initial, DATA_PAGE + 0x40,
            8, false, UINT64_C(0x1122), 0,
            NULL, 3, 5, 7);

    struct cpu_state expected = initial;
    expected.pc = CODE_PAGE + 4;
    expected.cycle++;
    expected.nzcv =
            (expected_value & (UINT64_C(1) << (width - 1)) ?
                    UINT32_C(1) << 31 : 0) |
            (expected_value == 0 ? UINT32_C(1) << 30 : 0);
    if (rd != 31)
        expected.x[rd] = expected_value;

    struct cpu_state result = run_fast_differential(
            instruction, initial, AARCH64_STEP_RETIRED);
    assert_cpu_equal(&result, &expected);
}

static void test_fast_ands_immediate_differential(void) {
    unsigned case_count = 0;
    for (unsigned width_index = 0; width_index < 2; width_index++) {
        bool is_64 = width_index != 0;
        byte_t width = is_64 ? 64 : 32;
        for (byte_t element_size = 2;
                element_size <= width; element_size *= 2) {
            const byte_t ones[] = {
                1,
                (byte_t) (element_size - 1),
            };
            const byte_t rotations[] = {
                0,
                (byte_t) (element_size - 1),
            };
            for (unsigned ones_index = 0;
                    ones_index < array_size(ones); ones_index++) {
                for (unsigned rotation_index = 0;
                        rotation_index < array_size(rotations);
                        rotation_index++) {
                    qword_t immediate = reference_logical_immediate(
                            is_64, element_size, ones[ones_index],
                            rotations[rotation_index]);
                    dword_t instruction = encode_logical_immediate(
                            AARCH64_OP_ANDS_IMMEDIATE,
                            is_64, element_size, ones[ones_index],
                            rotations[rotation_index], 4, 6);
                    struct cpu_state initial;
                    init_differential_cpu(&initial);
                    initial.x[4] = UINT64_C(0x8123456789abcdef);
                    initial.x[6] = UINT64_MAX;
                    initial.nzcv = UINT32_C(0xf0000000);
                    assert_fast_ands_immediate(instruction,
                            initial, width, 6,
                            initial.x[4] & immediate);
                    case_count++;
                }
            }
        }
    }
    assert(case_count == 44);

    struct cpu_state initial;
    init_differential_cpu(&initial);
    initial.x[2] = UINT64_C(0xffffffff89abcdef);
    initial.x[22] = UINT64_MAX;
    initial.nzcv = UINT32_C(0xf0000000);
    assert(encode_logical_immediate(AARCH64_OP_ANDS_IMMEDIATE,
            false, 32, 31, 0, 2, 22) ==
            INSTRUCTION_ANDS_W22_W2_7FFFFFFF);
    assert_fast_ands_immediate(INSTRUCTION_ANDS_W22_W2_7FFFFFFF,
            initial, 32, 22, UINT32_C(0x09abcdef));
    case_count++;

    init_differential_cpu(&initial);
    initial.x[1] = UINT64_C(0xaaaaaaaa00000100);
    initial.nzcv = UINT32_C(0xf0000000);
    assert(encode_logical_immediate(AARCH64_OP_ANDS_IMMEDIATE,
            false, 32, 8, 0, 1, 31) ==
            INSTRUCTION_TST_W1_FF);
    assert_fast_ands_immediate(
            INSTRUCTION_TST_W1_FF, initial, 32, 31, 0);
    case_count++;

    init_differential_cpu(&initial);
    initial.x[12] = UINT64_C(0x8000000000000000);
    initial.nzcv = UINT32_C(0xf0000000);
    assert(encode_logical_immediate(AARCH64_OP_ANDS_IMMEDIATE,
            true, 64, 2, 1, 12, 31) ==
            INSTRUCTION_ANDS_XZR_X12_8000000000000001);
    assert_fast_ands_immediate(
            INSTRUCTION_ANDS_XZR_X12_8000000000000001,
            initial, 64, 31, UINT64_C(0x8000000000000000));
    case_count++;

    init_differential_cpu(&initial);
    initial.x[6] = UINT64_MAX;
    initial.sp = UINT64_C(0x123456789abcdeff);
    initial.nzcv = UINT32_C(0xf0000000);
    assert_fast_ands_immediate(encode_logical_immediate(
            AARCH64_OP_ANDS_IMMEDIATE,
            true, 64, 8, 0, 31, 6),
            initial, 64, 6, 0);
    case_count++;

    init_differential_cpu(&initial);
    initial.x[4] = UINT64_C(0xffffffff80000001);
    initial.nzcv = UINT32_C(0xf0000000);
    assert_fast_ands_immediate(encode_logical_immediate(
            AARCH64_OP_ANDS_IMMEDIATE,
            false, 32, 2, 1, 4, 4),
            initial, 32, 4, UINT32_C(0x80000001));
    case_count++;

    assert(case_count == 49);
}

static void assert_fast_orr_immediate(dword_t instruction,
        struct cpu_state initial, byte_t width,
        byte_t rn, byte_t rd, qword_t immediate) {
    assert(width == 32 || width == 64);
    struct aarch64_decoded decoded;
    assert(aarch64_decode(instruction, &decoded));
    assert(decoded.opcode == AARCH64_OP_ORR_IMMEDIATE);
    assert(decoded.width == width);
    assert(decoded.operands.logical_immediate.rn == rn);
    assert(decoded.operands.logical_immediate.rd == rd);
    assert(decoded.operands.logical_immediate.immediate == immediate);

    aarch64_set_exclusive(&initial, DATA_PAGE + 0x40,
            8, false, UINT64_C(0x1122), 0,
            NULL, 3, 5, 7);
    qword_t width_mask = width == 32 ? UINT32_MAX : UINT64_MAX;
    qword_t left = rn == 31 ? 0 : initial.x[rn] & width_mask;
    qword_t value = (left | immediate) & width_mask;
    struct cpu_state expected = initial;
    expected.pc = CODE_PAGE + 4;
    expected.cycle++;
    if (rd == 31)
        expected.sp = value;
    else
        expected.x[rd] = value;

    struct cpu_state result = run_fast_differential(
            instruction, initial, AARCH64_STEP_RETIRED);
    assert_cpu_equal(&result, &expected);
}

static void test_fast_orr_immediate_differential(void) {
    unsigned case_count = 0;
    for (unsigned width_index = 0; width_index < 2; width_index++) {
        bool is_64 = width_index != 0;
        byte_t width = is_64 ? 64 : 32;
        const byte_t element_sizes[] = {2, 8, width};
        for (unsigned element_index = 0;
                element_index < array_size(element_sizes);
                element_index++) {
            byte_t element_size = element_sizes[element_index];
            for (unsigned shape = 0; shape < 2; shape++) {
                byte_t ones = shape == 0 ? 1 : element_size - 1;
                byte_t rotation = shape == 0 ? 0 : element_size - 1;
                qword_t immediate = reference_logical_immediate(
                        is_64, element_size, ones, rotation);
                dword_t instruction = encode_logical_immediate(
                        AARCH64_OP_ORR_IMMEDIATE,
                        is_64, element_size, ones, rotation, 4, 6);
                struct cpu_state initial;
                init_differential_cpu(&initial);
                initial.x[4] = UINT64_C(0x8123456789abcdef);
                initial.x[6] = UINT64_MAX;
                assert_fast_orr_immediate(instruction,
                        initial, width, 4, 6, immediate);
                case_count++;
            }
        }
    }
    assert(case_count == 12);

    struct cpu_state initial;
    init_differential_cpu(&initial);
    initial.x[10] = UINT64_C(0xaaaaaaaa80000040);
    assert(encode_logical_immediate(AARCH64_OP_ORR_IMMEDIATE,
            false, 32, 1, 0, 10, 10) ==
            INSTRUCTION_ORR_W10_W10_1);
    assert_fast_orr_immediate(INSTRUCTION_ORR_W10_W10_1,
            initial, 32, 10, 10, UINT32_C(1));
    case_count++;

    init_differential_cpu(&initial);
    initial.x[5] = UINT64_MAX;
    initial.sp = UINT64_C(0x123456789abcdef0);
    assert(encode_logical_immediate(AARCH64_OP_ORR_IMMEDIATE,
            false, 32, 30, 31, 31, 5) ==
            INSTRUCTION_ORR_W5_WZR_7FFFFFFE);
    assert_fast_orr_immediate(INSTRUCTION_ORR_W5_WZR_7FFFFFFE,
            initial, 32, 31, 5, UINT32_C(0x7ffffffe));
    case_count++;

    init_differential_cpu(&initial);
    initial.sp = UINT64_C(0xaaaaaaaa55555555);
    qword_t wsp_immediate = reference_logical_immediate(
            false, 16, 8, 0);
    assert_fast_orr_immediate(encode_logical_immediate(
            AARCH64_OP_ORR_IMMEDIATE,
            false, 16, 8, 0, 31, 31),
            initial, 32, 31, 31, wsp_immediate);
    case_count++;

    init_differential_cpu(&initial);
    initial.x[12] = UINT64_C(0x1234000012340000);
    initial.sp = UINT64_C(0xaaaaaaaa55555555);
    qword_t sp_immediate = reference_logical_immediate(
            true, 16, 8, 8);
    assert_fast_orr_immediate(encode_logical_immediate(
            AARCH64_OP_ORR_IMMEDIATE,
            true, 16, 8, 8, 12, 31),
            initial, 64, 12, 31, sp_immediate);
    case_count++;

    init_differential_cpu(&initial);
    initial.x[8] = UINT64_C(0x0102030405060708);
    initial.x[9] = UINT64_MAX;
    qword_t synonym_immediate = reference_logical_immediate(
            true, 8, 3, 1);
    dword_t canonical = encode_logical_immediate(
            AARCH64_OP_ORR_IMMEDIATE,
            true, 8, 3, 1, 8, 9);
    // 元素宽度以上的 immr 位不改变逻辑立即数的循环右旋结果。
    dword_t synonym = canonical | (UINT32_C(8) << 16);
    assert(synonym != canonical);
    assert_fast_orr_immediate(synonym,
            initial, 64, 8, 9, synonym_immediate);
    case_count++;

    assert(case_count == 17);
}

static void test_fast_eor_immediate_differential(void) {
    static const struct {
        dword_t instruction;
        bool is_64;
        byte_t element_size;
        byte_t ones;
        byte_t rotation;
        byte_t rn;
        byte_t rd;
        qword_t source;
        qword_t expected;
    } cases[] = {
        {
            INSTRUCTION_EOR_W0_W4_1,
            false, 32, 1, 0, 4, 0,
            UINT64_C(0xffffffff89abcdef),
            UINT64_C(0x0000000089abcdee),
        },
        {
            INSTRUCTION_EOR_X10_X11_0000FFFF0000FFFF,
            true, 32, 16, 0, 11, 10,
            UINT64_C(0xffff0000ffff0000),
            UINT64_MAX,
        },
        {
            UINT32_C(0xd201f3e6),
            true, 2, 1, 1, 31, 6,
            0,
            UINT64_C(0xaaaaaaaaaaaaaaaa),
        },
        {
            UINT32_C(0x521f78bf),
            false, 32, 31, 31, 5, 31,
            UINT64_C(0xaaaaaaaa12345678),
            UINT64_C(0x00000000edcba986),
        },
        {
            UINT32_C(0xd27ff884),
            true, 64, 63, 63, 4, 4,
            UINT64_C(0x0123456789abcdef),
            UINT64_C(0xfedcba9876543211),
        },
    };
    _Static_assert(array_size(cases) == 5,
            "EOR immediate 画像、宽度与寄存器角色专例数量必须保持稳定");

    for (unsigned index = 0; index < array_size(cases); index++) {
        qword_t immediate = reference_logical_immediate(
                cases[index].is_64, cases[index].element_size,
                cases[index].ones, cases[index].rotation);
        assert(encode_logical_immediate(AARCH64_OP_EOR_IMMEDIATE,
                cases[index].is_64, cases[index].element_size,
                cases[index].ones, cases[index].rotation,
                cases[index].rn, cases[index].rd) ==
                cases[index].instruction);

        struct cpu_state initial;
        init_differential_cpu(&initial);
        initial.sp = UINT64_C(0x1111222233334444);
        if (cases[index].rn != 31)
            initial.x[cases[index].rn] = cases[index].source;
        if (cases[index].rd != 31 &&
                cases[index].rd != cases[index].rn) {
            initial.x[cases[index].rd] = UINT64_MAX;
        }
        aarch64_set_exclusive(&initial, DATA_PAGE + 0x40, 8, false,
                UINT64_C(0x1122), 0, NULL, 3, 5, 7);

        byte_t width = cases[index].is_64 ? 64 : 32;
        qword_t width_mask =
                cases[index].is_64 ? UINT64_MAX : UINT32_MAX;
        qword_t source = cases[index].rn == 31 ?
                0 : cases[index].source & width_mask;
        qword_t value = (source ^ immediate) & width_mask;
        assert(value == cases[index].expected);
        struct cpu_state expected = initial;
        expected.pc = CODE_PAGE + 4;
        expected.cycle++;
        if (cases[index].rd == 31)
            expected.sp = value;
        else
            expected.x[cases[index].rd] = value;

        struct cpu_state result = run_fast_differential(
                cases[index].instruction,
                initial, AARCH64_STEP_RETIRED);
        assert_cpu_equal(&result, &expected);
        if (width == 32 && cases[index].rd != 31)
            assert((result.x[cases[index].rd] >> 32) == 0);
    }
}

static void test_add_sub_shifted_sibling_fallback(void) {
    const dword_t instructions[] = {
        UINT32_C(0xab020020),
    };
    const enum aarch64_opcode opcodes[] = {
        AARCH64_OP_ADDS_SHIFTED_REGISTER,
    };
    struct test_fixture fixture;
    init_fixture(&fixture);
    for (unsigned index = 0; index < array_size(instructions); index++) {
        struct aarch64_decoded decoded;
        assert(aarch64_decode(instructions[index], &decoded));
        assert(decoded.opcode == opcodes[index]);
        write_instruction(&fixture.tlb, CODE_PAGE + index * 4,
                instructions[index]);
    }

    struct aarch64_runner runner;
    assert(aarch64_runner_init_backend(
            &runner, &fixture.tlb, AARCH64_BACKEND_THREADED));
    struct cpu_state cpu;
    init_differential_cpu(&cpu);
    cpu.x[1] = UINT64_MAX;
    cpu.x[2] = 1;
    for (unsigned index = 0; index < array_size(instructions); index++) {
        assert(run_at(&runner, &cpu, CODE_PAGE + index * 4).stop ==
                AARCH64_STEP_RETIRED);
    }
    assert(cpu.x[0] == 0);
    assert(cpu.nzcv == UINT32_C(0x60000000));
    assert_stats(&runner, 0, array_size(instructions),
            0, array_size(instructions));
}

static void test_fast_and_shifted_differential(void) {
    const enum aarch64_shift_type shift_types[] = {
        AARCH64_SHIFT_LSL,
        AARCH64_SHIFT_LSR,
        AARCH64_SHIFT_ASR,
        AARCH64_SHIFT_ROR,
    };
    for (unsigned width_index = 0; width_index < 2; width_index++) {
        bool is_64 = width_index != 0;
        byte_t width = is_64 ? 64 : 32;
        const byte_t shifts[] = {0, 1, (byte_t) (width - 1)};
        for (unsigned type_index = 0;
                type_index < array_size(shift_types); type_index++) {
            for (unsigned shift_index = 0;
                    shift_index < array_size(shifts); shift_index++) {
                for (unsigned invert = 0; invert < 2; invert++) {
                    struct cpu_state initial;
                    init_differential_cpu(&initial);
                    initial.x[4] = UINT64_C(0x8123456789abcdef);
                    initial.x[5] = UINT64_C(0xfedcba9886543211);
                    struct cpu_state result = run_fast_differential(
                            encode_and_shifted(is_64, invert != 0,
                                    shift_types[type_index],
                                    shifts[shift_index], 5, 4, 6),
                            initial, AARCH64_STEP_RETIRED);
                    assert(result.pc == CODE_PAGE + 4);
                    assert(result.cycle == initial.cycle + 1);
                    assert(result.nzcv == initial.nzcv);
                    assert(result.fpcr == initial.fpcr);
                    assert(result.fpsr == initial.fpsr);
                    assert(result.x[4] == initial.x[4]);
                    assert(result.x[5] == initial.x[5]);
                }
            }
        }
    }

    struct cpu_state initial;
    init_differential_cpu(&initial);
    initial.x[2] = UINT64_C(0xf0f0f0f0f0f0f0f0);
    initial.x[6] = UINT64_C(0xff00ff00ff00ff00);
    aarch64_set_exclusive(&initial, DATA_PAGE + 0x40, 8, false,
            UINT64_C(0x1122), 0, NULL, 3, 5, 7);
    assert(encode_and_shifted(true, false, AARCH64_SHIFT_LSL,
            0, 6, 2, 2) == INSTRUCTION_AND_X2_X2_X6);
    struct cpu_state result = run_fast_differential(
            INSTRUCTION_AND_X2_X2_X6,
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[2] == UINT64_C(0xf000f000f000f000));
    assert(result.x[6] == initial.x[6]);
    assert(result.sp == initial.sp);
    assert(result.nzcv == initial.nzcv);
    assert(result.fpcr == initial.fpcr);
    assert(result.fpsr == initial.fpsr);
    assert(result.exclusive.valid);
    assert(result.exclusive.address == DATA_PAGE + 0x40);
    assert(result.exclusive.write_epoch == 5);
    assert(result.exclusive.sync_identity == 7);

    init_differential_cpu(&initial);
    initial.x[3] = UINT64_C(0xffff0000ffff0000);
    initial.x[4] = UINT64_C(0x00ff00ff00ff00ff);
    result = run_fast_differential(encode_and_shifted(
            true, true, AARCH64_SHIFT_LSL, 0, 4, 3, 5),
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[5] == UINT64_C(0xff000000ff000000));
    assert(result.x[3] == initial.x[3]);
    assert(result.x[4] == initial.x[4]);

    init_differential_cpu(&initial);
    initial.x[24] = UINT64_C(0xffffffffdeadbeef);
    initial.x[1] = UINT64_C(0xffffffff0ff00ff0);
    assert(encode_and_shifted(false, false, AARCH64_SHIFT_LSL,
            0, 1, 24, 1) == INSTRUCTION_AND_W1_W24_W1);
    result = run_fast_differential(
            INSTRUCTION_AND_W1_W24_W1,
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[1] == UINT64_C(0x000000000ea00ee0));
    assert(result.x[24] == initial.x[24]);

    init_differential_cpu(&initial);
    initial.x[4] = UINT64_C(0x123456789abcdef0);
    result = run_fast_differential(encode_and_shifted(
            true, false, AARCH64_SHIFT_LSL, 0, 4, 31, 6),
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[6] == 0);
    assert(result.x[4] == initial.x[4]);
    assert(result.sp == initial.sp);

    init_differential_cpu(&initial);
    initial.x[4] = UINT64_C(0x123456789abcdef0);
    result = run_fast_differential(encode_and_shifted(
            true, true, AARCH64_SHIFT_LSL, 0, 31, 4, 6),
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[6] == initial.x[4]);
    assert(result.x[4] == initial.x[4]);
    assert(result.sp == initial.sp);

    init_differential_cpu(&initial);
    initial.x[4] = UINT64_C(0x123456789abcdef0);
    initial.x[5] = UINT64_C(0x0fedcba987654321);
    result = run_fast_differential(encode_and_shifted(
            true, false, AARCH64_SHIFT_ROR, 17, 5, 4, 31),
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[4] == initial.x[4]);
    assert(result.x[5] == initial.x[5]);
    assert(result.sp == initial.sp);
}

static void test_fast_ubfm_differential(void) {
    for (unsigned width_index = 0; width_index < 2; width_index++) {
        bool is_64 = width_index != 0;
        byte_t width = is_64 ? 64 : 32;
        for (byte_t immr = 0; immr < width; immr++) {
            for (byte_t imms = 0; imms < width; imms++) {
                struct cpu_state initial;
                init_differential_cpu(&initial);
                initial.x[4] = UINT64_C(0x8123456789abcdef);
                initial.x[6] = UINT64_MAX;
                struct cpu_state result = run_fast_differential(
                        encode_bitfield_move(AARCH64_OP_UBFM,
                                is_64, immr, imms, 4, 6),
                        initial, AARCH64_STEP_RETIRED);
                assert(result.pc == CODE_PAGE + 4);
                assert(result.cycle == initial.cycle + 1);
                assert(result.x[4] == initial.x[4]);
                assert(result.sp == initial.sp);
                assert(result.nzcv == initial.nzcv);
                assert(result.fpcr == initial.fpcr);
                assert(result.fpsr == initial.fpsr);
                if (!is_64)
                    assert((result.x[6] >> 32) == 0);
            }
        }
    }

    struct cpu_state initial;
    init_differential_cpu(&initial);
    initial.x[1] = UINT64_C(0x8877665544332211);
    aarch64_set_exclusive(&initial, DATA_PAGE + 0x60, 8, false,
            UINT64_C(0x3344), 0, NULL, 4, 6, 8);
    assert(encode_bitfield_move(AARCH64_OP_UBFM,
            true, 1, 63, 1, 1) == INSTRUCTION_UBFM_X1_X1_1_63);
    struct cpu_state result = run_fast_differential(
            INSTRUCTION_UBFM_X1_X1_1_63,
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[1] == UINT64_C(0x443bb32aa2199108));
    assert(result.exclusive.valid);
    assert(result.exclusive.address == DATA_PAGE + 0x60);
    assert(result.exclusive.write_epoch == 6);
    assert(result.exclusive.sync_identity == 8);

    init_differential_cpu(&initial);
    initial.x[2] = UINT64_MAX;
    initial.x[4] = UINT64_C(0xffffffff89abcdef);
    assert(encode_bitfield_move(AARCH64_OP_UBFM,
            false, 28, 27, 4, 2) == INSTRUCTION_UBFM_W2_W4_28_27);
    result = run_fast_differential(
            INSTRUCTION_UBFM_W2_W4_28_27,
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[2] == UINT64_C(0x000000009abcdef0));
    assert(result.x[4] == initial.x[4]);

    init_differential_cpu(&initial);
    initial.x[6] = UINT64_MAX;
    result = run_fast_differential(encode_bitfield_move(
            AARCH64_OP_UBFM, true, 17, 31, 31, 6),
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[6] == 0);
    assert(result.sp == initial.sp);

    init_differential_cpu(&initial);
    initial.x[4] = UINT64_C(0x0123456789abcdef);
    result = run_fast_differential(encode_bitfield_move(
            AARCH64_OP_UBFM, true, 8, 15, 4, 31),
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[4] == initial.x[4]);
    assert(result.sp == initial.sp);
}

static void test_fast_sbfm_differential(void) {
    unsigned immediate_case_count = 0;
    for (unsigned width_index = 0; width_index < 2; width_index++) {
        bool is_64 = width_index != 0;
        byte_t width = is_64 ? 64 : 32;
        for (byte_t immr = 0; immr < width; immr++) {
            for (byte_t imms = 0; imms < width; imms++) {
                struct cpu_state initial;
                init_differential_cpu(&initial);
                initial.x[4] = UINT64_C(0x8123456789abcdef);
                initial.x[6] = UINT64_MAX;
                aarch64_set_exclusive(&initial,
                        DATA_PAGE + 0x60, 8, false,
                        UINT64_C(0x3344), 0, NULL, 4, 6, 8);
                struct cpu_state result = run_fast_differential(
                        encode_bitfield_move(AARCH64_OP_SBFM,
                                is_64, immr, imms, 4, 6),
                        initial, AARCH64_STEP_RETIRED);
                assert(result.pc == CODE_PAGE + 4);
                assert(result.cycle == initial.cycle + 1);
                assert(result.x[4] == initial.x[4]);
                assert(result.sp == initial.sp);
                assert(result.nzcv == initial.nzcv);
                assert(result.fpcr == initial.fpcr);
                assert(result.fpsr == initial.fpsr);
                assert(result.exclusive.valid);
                assert(result.exclusive.address ==
                        initial.exclusive.address);
                assert(result.exclusive.write_epoch ==
                        initial.exclusive.write_epoch);
                assert(result.exclusive.sync_identity ==
                        initial.exclusive.sync_identity);
                if (!is_64)
                    assert((result.x[6] >> 32) == 0);
                immediate_case_count++;
            }
        }
    }
    assert(immediate_case_count == 5120);

    assert(encode_bitfield_move(AARCH64_OP_SBFM,
            false, 0, 0, 4, 6) == UINT32_C(0x13000086));
    assert(encode_bitfield_move(AARCH64_OP_SBFM,
            false, 0, 31, 4, 6) == UINT32_C(0x13007c86));
    assert(encode_bitfield_move(AARCH64_OP_SBFM,
            false, 31, 0, 4, 6) == UINT32_C(0x131f0086));
    assert(encode_bitfield_move(AARCH64_OP_SBFM,
            false, 31, 31, 4, 6) == UINT32_C(0x131f7c86));
    assert(encode_bitfield_move(AARCH64_OP_SBFM,
            true, 0, 0, 4, 6) == UINT32_C(0x93400086));
    assert(encode_bitfield_move(AARCH64_OP_SBFM,
            true, 0, 63, 4, 6) == UINT32_C(0x9340fc86));
    assert(encode_bitfield_move(AARCH64_OP_SBFM,
            true, 63, 0, 4, 6) == UINT32_C(0x937f0086));
    assert(encode_bitfield_move(AARCH64_OP_SBFM,
            true, 63, 63, 4, 6) == UINT32_C(0x937ffc86));

    static const struct {
        dword_t instruction;
        bool is_64;
        byte_t immr;
        byte_t imms;
        byte_t rn;
        byte_t rd;
        qword_t source;
        qword_t expected;
    } aliases[] = {
        {
            INSTRUCTION_SXTW_X2_W2, true, 0, 31, 2, 2,
            UINT64_C(0xaaaaaaaa80000001),
            UINT64_C(0xffffffff80000001),
        },
        {
            INSTRUCTION_SXTW_X0_W19, true, 0, 31, 19, 0,
            UINT64_C(0xaaaaaaaa7fffffff),
            UINT64_C(0x000000007fffffff),
        },
        {
            INSTRUCTION_SXTB_W3_W3, false, 0, 7, 3, 3,
            UINT64_C(0xaaaaaaaa00000080),
            UINT64_C(0x00000000ffffff80),
        },
        {
            INSTRUCTION_SXTB_X7_WZR, true, 0, 7, 31, 7,
            0, 0,
        },
        {
            INSTRUCTION_SXTH_WZR_W8, false, 0, 15, 8, 31,
            UINT64_C(0xaaaaaaaa00008001), 0,
        },
        {
            INSTRUCTION_SXTH_X9_W10, true, 0, 15, 10, 9,
            UINT64_C(0xaaaaaaaa00008001),
            UINT64_C(0xffffffffffff8001),
        },
        {
            INSTRUCTION_ASR_W2_W0_1, false, 1, 31, 0, 2,
            UINT64_C(0xaaaaaaaa80000001),
            UINT64_C(0x00000000c0000000),
        },
        {
            INSTRUCTION_ASR_X1_X1_1, true, 1, 63, 1, 1,
            UINT64_C(0x8000000000000001),
            UINT64_C(0xc000000000000000),
        },
        {
            INSTRUCTION_SBFIZ_W15_W16_8_8,
            false, 24, 7, 16, 15,
            UINT64_C(0xaaaaaaaa00000080),
            UINT64_C(0x00000000ffff8000),
        },
        {
            INSTRUCTION_SBFIZ_X27_X23_3_32,
            true, 61, 31, 23, 27,
            UINT64_C(0xaaaaaaaa80000001),
            UINT64_C(0xfffffffc00000008),
        },
        {
            INSTRUCTION_SBFX_W19_W20_8_8,
            false, 8, 15, 20, 19,
            UINT64_C(0xaaaaaaaa00008000),
            UINT64_C(0x00000000ffffff80),
        },
        {
            INSTRUCTION_SBFX_X1_X1_8_16,
            true, 8, 23, 1, 1,
            UINT64_C(0xaaaaaaaa00800100),
            UINT64_C(0xffffffffffff8001),
        },
    };
    _Static_assert(array_size(aliases) == 12,
            "SBFM 别名与寄存器角色专例数量必须保持稳定");
    for (unsigned index = 0; index < array_size(aliases); index++) {
        struct cpu_state initial;
        init_differential_cpu(&initial);
        initial.sp = DATA_PAGE + 0x381;
        if (aliases[index].rd != 31)
            initial.x[aliases[index].rd] = UINT64_MAX;
        if (aliases[index].rn != 31)
            initial.x[aliases[index].rn] = aliases[index].source;
        aarch64_set_exclusive(&initial, DATA_PAGE + 0x60, 8, false,
                UINT64_C(0x3344), 0, NULL, 4, 6, 8);
        assert(encode_bitfield_move(AARCH64_OP_SBFM,
                aliases[index].is_64,
                aliases[index].immr, aliases[index].imms,
                aliases[index].rn, aliases[index].rd) ==
                aliases[index].instruction);

        struct test_fixture c_fixture;
        struct test_fixture threaded_fixture;
        init_fixture(&c_fixture);
        init_fixture(&threaded_fixture);
        write_instruction(&c_fixture.tlb,
                CODE_PAGE, aliases[index].instruction);
        write_instruction(&threaded_fixture.tlb,
                CODE_PAGE, aliases[index].instruction);
        memset(c_fixture.memory.data + 0x100, 0xa5, 32);
        memset(threaded_fixture.memory.data + 0x100, 0xa5, 32);
        byte_t expected_data[GUEST_MEMORY_PAGE_SIZE];
        memcpy(expected_data,
                c_fixture.memory.data, sizeof(expected_data));
        struct cpu_state result = run_fast_differential_fixtures(
                aliases[index].instruction,
                initial, AARCH64_STEP_RETIRED,
                &c_fixture, &threaded_fixture, NULL);
        assert(memcmp(c_fixture.memory.data,
                expected_data, sizeof(expected_data)) == 0);
        assert(memcmp(threaded_fixture.memory.data,
                expected_data, sizeof(expected_data)) == 0);
        assert(result.pc == CODE_PAGE + 4);
        assert(result.cycle == initial.cycle + 1);
        if (aliases[index].rd == 31) {
            assert(memcmp(result.x, initial.x, sizeof(result.x)) == 0);
        } else {
            assert(result.x[aliases[index].rd] ==
                    aliases[index].expected);
        }
        if (aliases[index].rn != 31 &&
                aliases[index].rn != aliases[index].rd) {
            assert(result.x[aliases[index].rn] ==
                    aliases[index].source);
        }
        assert(result.sp == initial.sp);
        assert(result.nzcv == initial.nzcv);
        assert(result.fpcr == initial.fpcr);
        assert(result.fpsr == initial.fpsr);
        assert(result.exclusive.valid);
        assert(result.exclusive.address == initial.exclusive.address);
        assert(result.exclusive.write_epoch ==
                initial.exclusive.write_epoch);
        assert(result.exclusive.sync_identity ==
                initial.exclusive.sync_identity);
    }
}

static void test_fast_bfm_differential(void) {
    static const struct {
        dword_t instruction;
        bool is_64;
        byte_t immr;
        byte_t imms;
        byte_t rn;
        byte_t rd;
        qword_t source;
        qword_t destination;
        qword_t expected;
    } cases[] = {
        {
            INSTRUCTION_BFM_X0_X1_52_51,
            true, 52, 51, 1, 0,
            UINT64_C(0x0123456789abcdef),
            UINT64_C(0xffff000000000abc),
            UINT64_C(0x3456789abcdefabc),
        },
        {
            INSTRUCTION_BFM_W2_W0_0_4,
            false, 0, 4, 0, 2,
            UINT64_C(0xaaaaaaaa00000013),
            UINT64_C(0xbbbbbbbb89abcde0),
            UINT64_C(0x0000000089abcdf3),
        },
        {
            UINT32_C(0x33181d8c),
            false, 24, 7, 12, 12,
            UINT64_C(0xaaaaaaaa89abcdef),
            UINT64_C(0xaaaaaaaa89abcdef),
            UINT64_C(0x0000000089abefef),
        },
        {
            UINT32_C(0xb3473fe6),
            true, 7, 15, 31, 6,
            0,
            UINT64_C(0x123456789abcdef0),
            UINT64_C(0x123456789abcde00),
        },
        {
            UINT32_C(0xb3473c9f),
            true, 7, 15, 4, 31,
            UINT64_C(0x0123456789abcdef),
            0,
            0,
        },
    };
    _Static_assert(array_size(cases) == 5,
            "BFM 画像、掩码路径与寄存器角色专例数量必须保持稳定");

    for (unsigned index = 0; index < array_size(cases); index++) {
        struct cpu_state initial;
        init_differential_cpu(&initial);
        if (cases[index].rn != 31)
            initial.x[cases[index].rn] = cases[index].source;
        if (cases[index].rd != 31 &&
                cases[index].rd != cases[index].rn) {
            initial.x[cases[index].rd] = cases[index].destination;
        }
        aarch64_set_exclusive(&initial, DATA_PAGE + 0x60, 8, false,
                UINT64_C(0x3344), 0, NULL, 4, 6, 8);
        assert(encode_bitfield_move(AARCH64_OP_BFM,
                cases[index].is_64,
                cases[index].immr, cases[index].imms,
                cases[index].rn, cases[index].rd) ==
                cases[index].instruction);

        struct test_fixture c_fixture;
        struct test_fixture threaded_fixture;
        init_fixture(&c_fixture);
        init_fixture(&threaded_fixture);
        write_instruction(&c_fixture.tlb,
                CODE_PAGE, cases[index].instruction);
        write_instruction(&threaded_fixture.tlb,
                CODE_PAGE, cases[index].instruction);
        memset(c_fixture.memory.data + 0x100, 0xa5, 32);
        memset(threaded_fixture.memory.data + 0x100, 0xa5, 32);
        byte_t expected_data[GUEST_MEMORY_PAGE_SIZE];
        memcpy(expected_data,
                c_fixture.memory.data, sizeof(expected_data));

        struct cpu_state result = run_fast_differential_fixtures(
                cases[index].instruction,
                initial, AARCH64_STEP_RETIRED,
                &c_fixture, &threaded_fixture, NULL);
        qword_t expected_x[31];
        memcpy(expected_x, initial.x, sizeof(expected_x));
        if (cases[index].rd != 31)
            expected_x[cases[index].rd] = cases[index].expected;
        assert(memcmp(result.x, expected_x, sizeof(expected_x)) == 0);
        assert(memcmp(c_fixture.memory.data,
                expected_data, sizeof(expected_data)) == 0);
        assert(memcmp(threaded_fixture.memory.data,
                expected_data, sizeof(expected_data)) == 0);
        assert(memcmp(result.v, initial.v, sizeof(result.v)) == 0);
        assert(result.pc == CODE_PAGE + 4);
        assert(result.cycle == initial.cycle + 1);
        assert(result.sp == initial.sp);
        assert(result.nzcv == initial.nzcv);
        assert(result.fpcr == initial.fpcr);
        assert(result.fpsr == initial.fpsr);
        assert(result.tpidr_el0 == initial.tpidr_el0);
        assert(result.exclusive.valid);
        assert(result.exclusive.address == initial.exclusive.address);
        assert(result.exclusive.mapping_epoch ==
                initial.exclusive.mapping_epoch);
        assert(result.exclusive.write_epoch ==
                initial.exclusive.write_epoch);
        assert(result.exclusive.sync_identity ==
                initial.exclusive.sync_identity);
        assert(result.segfault_addr == initial.segfault_addr);
        assert(result.segfault_was_write ==
                initial.segfault_was_write);
        assert(result.trapno == initial.trapno);
        assert(result.single_step == initial.single_step);
        assert(result._poked == initial._poked);
        if (!cases[index].is_64 && cases[index].rd != 31)
            assert((result.x[cases[index].rd] >> 32) == 0);
    }
}

static void test_fast_lsrv_differential(void) {
    const qword_t source = UINT64_C(0x8123456789abcdef);
    unsigned shift_case_count = 0;
    for (unsigned width_index = 0; width_index < 2; width_index++) {
        bool is_64 = width_index != 0;
        byte_t width = is_64 ? 64 : 32;
        for (unsigned case_index = 0;
                case_index < (unsigned) width + 3; case_index++) {
            qword_t raw_amount;
            if (case_index < width)
                raw_amount = case_index;
            else if (case_index == width)
                raw_amount = width;
            else if (case_index == (unsigned) width + 1)
                raw_amount = (qword_t) width + 1;
            else
                raw_amount = UINT64_MAX;

            struct cpu_state initial;
            init_differential_cpu(&initial);
            initial.x[4] = source;
            initial.x[5] = raw_amount;
            initial.x[6] = UINT64_MAX;
            aarch64_set_exclusive(&initial,
                    DATA_PAGE + 0x60, 8, false,
                    UINT64_C(0x3344), 0, NULL, 4, 6, 8);

            dword_t instruction =
                    encode_lsrv(is_64, 5, 4, 6);
            struct test_fixture c_fixture;
            struct test_fixture threaded_fixture;
            init_fixture(&c_fixture);
            init_fixture(&threaded_fixture);
            write_instruction(&c_fixture.tlb,
                    CODE_PAGE, instruction);
            write_instruction(&threaded_fixture.tlb,
                    CODE_PAGE, instruction);
            memset(c_fixture.memory.data, 0xa5,
                    sizeof(c_fixture.memory.data));
            memset(threaded_fixture.memory.data, 0xa5,
                    sizeof(threaded_fixture.memory.data));
            byte_t expected_data[GUEST_MEMORY_PAGE_SIZE];
            memcpy(expected_data, c_fixture.memory.data,
                    sizeof(expected_data));

            struct cpu_state result =
                    run_fast_differential_fixtures(
                            instruction, initial,
                            AARCH64_STEP_RETIRED,
                            &c_fixture, &threaded_fixture, NULL);
            qword_t operand = is_64 ?
                    source : (dword_t) source;
            qword_t expected_value = operand >>
                    (raw_amount & (width - 1));
            struct cpu_state expected_cpu = initial;
            expected_cpu.x[6] = expected_value;
            expected_cpu.pc = CODE_PAGE + 4;
            expected_cpu.cycle++;
            assert_cpu_equal(&result, &expected_cpu);
            assert(memcmp(c_fixture.memory.data,
                    expected_data, sizeof(expected_data)) == 0);
            assert(memcmp(threaded_fixture.memory.data,
                    expected_data, sizeof(expected_data)) == 0);

            if (!is_64 && case_index == 0)
                assert(result.x[6] == UINT32_C(0x89abcdef));
            if (!is_64 && case_index == 1)
                assert(result.x[6] == UINT32_C(0x44d5e6f7));
            if (!is_64 && case_index == 31)
                assert(result.x[6] == 1);
            if (!is_64 && case_index == 32)
                assert(result.x[6] == UINT32_C(0x89abcdef));
            if (!is_64 && case_index == 33)
                assert(result.x[6] == UINT32_C(0x44d5e6f7));
            if (!is_64 && case_index == 34)
                assert(result.x[6] == 1);
            if (is_64 && case_index == 0)
                assert(result.x[6] ==
                        UINT64_C(0x8123456789abcdef));
            if (is_64 && case_index == 1)
                assert(result.x[6] ==
                        UINT64_C(0x4091a2b3c4d5e6f7));
            if (is_64 && case_index == 63)
                assert(result.x[6] == 1);
            if (is_64 && case_index == 64)
                assert(result.x[6] ==
                        UINT64_C(0x8123456789abcdef));
            if (is_64 && case_index == 65)
                assert(result.x[6] ==
                        UINT64_C(0x4091a2b3c4d5e6f7));
            if (is_64 && case_index == 66)
                assert(result.x[6] == 1);
            shift_case_count++;
        }
    }
    assert(shift_case_count == 102);

    assert(encode_lsrv(false, 0, 0, 0) ==
            UINT32_C(0x1ac02400));
    assert(encode_lsrv(false, 31, 31, 31) ==
            UINT32_C(0x1adf27ff));
    assert(encode_lsrv(true, 0, 0, 0) ==
            UINT32_C(0x9ac02400));
    assert(encode_lsrv(true, 31, 31, 31) ==
            UINT32_C(0x9adf27ff));

    static const struct {
        dword_t instruction;
        bool is_64;
        byte_t rn;
        byte_t rm;
        byte_t rd;
        qword_t rn_value;
        qword_t rm_value;
        qword_t expected;
    } roles[] = {
        {
            INSTRUCTION_LSRV_W4_W10_W4,
            false, 10, 4, 4,
            UINT64_C(0xaaaaaaaa80000040), 4,
            UINT32_C(0x08000004),
        },
        {
            INSTRUCTION_LSRV_W0_W0_W2,
            false, 0, 2, 0,
            UINT64_C(0xaaaaaaaa80000000), 4,
            UINT32_C(0x08000000),
        },
        {
            INSTRUCTION_LSRV_X21_X22_X23,
            true, 22, 23, 21,
            UINT64_C(0x8000000000000001), 65,
            UINT64_C(0x4000000000000000),
        },
        {
            UINT32_C(0x1ac527e6),
            false, 31, 5, 6,
            0, 1, 0,
        },
        {
            UINT32_C(0x9adf2486),
            true, 4, 31, 6,
            UINT64_C(0x8123456789abcdef), 0,
            UINT64_C(0x8123456789abcdef),
        },
        {
            UINT32_C(0x1ac5249f),
            false, 4, 5, 31,
            UINT64_C(0xaaaaaaaa89abcdef), 8, 0,
        },
        {
            UINT32_C(0x9ac42486),
            true, 4, 4, 6,
            UINT64_C(0x8000000000000001),
            UINT64_C(0x8000000000000001),
            UINT64_C(0x4000000000000000),
        },
        {
            UINT32_C(0x1ac724e7),
            false, 7, 7, 7,
            UINT64_C(0xaaaaaaaa80000004),
            UINT64_C(0xaaaaaaaa80000004),
            UINT32_C(0x08000000),
        },
    };
    _Static_assert(array_size(roles) == 8,
            "LSRV 寄存器角色专例数量必须保持稳定");
    for (unsigned index = 0; index < array_size(roles); index++) {
        assert(encode_lsrv(roles[index].is_64,
                roles[index].rm, roles[index].rn,
                roles[index].rd) == roles[index].instruction);
        if (roles[index].rn == roles[index].rm)
            assert(roles[index].rn_value ==
                    roles[index].rm_value);

        struct cpu_state initial;
        init_differential_cpu(&initial);
        initial.sp = DATA_PAGE + 0x381;
        if (roles[index].rd != 31)
            initial.x[roles[index].rd] = UINT64_MAX;
        if (roles[index].rn != 31)
            initial.x[roles[index].rn] =
                    roles[index].rn_value;
        if (roles[index].rm != 31)
            initial.x[roles[index].rm] =
                    roles[index].rm_value;
        aarch64_set_exclusive(&initial,
                DATA_PAGE + 0x60, 8, false,
                UINT64_C(0x3344), 0, NULL, 4, 6, 8);

        struct test_fixture c_fixture;
        struct test_fixture threaded_fixture;
        init_fixture(&c_fixture);
        init_fixture(&threaded_fixture);
        write_instruction(&c_fixture.tlb,
                CODE_PAGE, roles[index].instruction);
        write_instruction(&threaded_fixture.tlb,
                CODE_PAGE, roles[index].instruction);
        memset(c_fixture.memory.data, 0xa5,
                sizeof(c_fixture.memory.data));
        memset(threaded_fixture.memory.data, 0xa5,
                sizeof(threaded_fixture.memory.data));
        byte_t expected_data[GUEST_MEMORY_PAGE_SIZE];
        memcpy(expected_data, c_fixture.memory.data,
                sizeof(expected_data));

        struct cpu_state result =
                run_fast_differential_fixtures(
                        roles[index].instruction, initial,
                        AARCH64_STEP_RETIRED,
                        &c_fixture, &threaded_fixture, NULL);
        struct cpu_state expected_cpu = initial;
        expected_cpu.pc = CODE_PAGE + 4;
        expected_cpu.cycle++;
        if (roles[index].rd != 31)
            expected_cpu.x[roles[index].rd] =
                    roles[index].expected;
        assert_cpu_equal(&result, &expected_cpu);
        assert(memcmp(c_fixture.memory.data,
                expected_data, sizeof(expected_data)) == 0);
        assert(memcmp(threaded_fixture.memory.data,
                expected_data, sizeof(expected_data)) == 0);
    }
    assert(shift_case_count + array_size(roles) == 110);
}

static void test_fast_asrv_differential(void) {
    static const struct {
        bool is_64;
        qword_t source;
        qword_t raw_amount;
        qword_t expected;
    } semantic_cases[] = {
        {
            false,
            UINT64_C(0xaaaaaaaa81234567), 0,
            UINT32_C(0x81234567),
        },
        {
            false,
            UINT64_C(0xaaaaaaaa81234567), 1,
            UINT32_C(0xc091a2b3),
        },
        {
            false,
            UINT64_C(0xaaaaaaaa7fffffff), 31,
            0,
        },
        {
            false,
            UINT64_C(0xbbbbbbbb80000000), 31,
            UINT32_MAX,
        },
        {
            false,
            UINT64_C(0xaaaaaaaa81234567), 32,
            UINT32_C(0x81234567),
        },
        {
            false,
            UINT64_C(0xaaaaaaaa81234567),
            UINT64_C(0xaaaaaaaa00000021),
            UINT32_C(0xc091a2b3),
        },
        {
            true,
            UINT64_C(0x8123456789abcdef), 0,
            UINT64_C(0x8123456789abcdef),
        },
        {
            true,
            UINT64_C(0x8123456789abcdef), 1,
            UINT64_C(0xc091a2b3c4d5e6f7),
        },
        {
            true,
            UINT64_C(0x7fffffffffffffff), 63,
            0,
        },
        {
            true,
            UINT64_C(0x8000000000000000), 63,
            UINT64_MAX,
        },
        {
            true,
            UINT64_C(0x8123456789abcdef), 64,
            UINT64_C(0x8123456789abcdef),
        },
        {
            true,
            UINT64_C(0x8123456789abcdef), 65,
            UINT64_C(0xc091a2b3c4d5e6f7),
        },
    };
    _Static_assert(array_size(semantic_cases) == 12,
            "ASRV 语义边界用例数量必须保持稳定");
    for (unsigned index = 0;
            index < array_size(semantic_cases); index++) {
        dword_t instruction = encode_asrv(
                semantic_cases[index].is_64, 5, 4, 6);
        struct aarch64_decoded decoded;
        assert(aarch64_decode(instruction, &decoded));
        assert(decoded.opcode == AARCH64_OP_ASRV);
        assert(decoded.width ==
                (semantic_cases[index].is_64 ? 64 : 32));
        assert(decoded.operands.data_processing_2source.rd == 6);
        assert(decoded.operands.data_processing_2source.rn == 4);
        assert(decoded.operands.data_processing_2source.rm == 5);

        struct cpu_state initial;
        init_differential_cpu(&initial);
        initial.sp = DATA_PAGE + 0x381;
        initial.x[4] = semantic_cases[index].source;
        initial.x[5] = semantic_cases[index].raw_amount;
        initial.x[6] = UINT64_MAX;
        aarch64_set_exclusive(&initial,
                DATA_PAGE + 0x60, 8, false,
                UINT64_C(0x3344), 0, NULL, 4, 6, 8);

        struct test_fixture c_fixture;
        struct test_fixture threaded_fixture;
        init_fixture(&c_fixture);
        init_fixture(&threaded_fixture);
        write_instruction(&c_fixture.tlb, CODE_PAGE, instruction);
        write_instruction(&threaded_fixture.tlb,
                CODE_PAGE, instruction);
        memset(c_fixture.memory.data, 0xa5,
                sizeof(c_fixture.memory.data));
        memset(threaded_fixture.memory.data, 0xa5,
                sizeof(threaded_fixture.memory.data));
        byte_t expected_data[GUEST_MEMORY_PAGE_SIZE];
        memcpy(expected_data, c_fixture.memory.data,
                sizeof(expected_data));

        struct cpu_state result =
                run_fast_differential_fixtures(
                        instruction, initial,
                        AARCH64_STEP_RETIRED,
                        &c_fixture, &threaded_fixture, NULL);
        struct cpu_state expected_cpu = initial;
        expected_cpu.x[6] = semantic_cases[index].expected;
        expected_cpu.pc = CODE_PAGE + 4;
        expected_cpu.cycle++;
        assert_cpu_equal(&result, &expected_cpu);
        assert(memcmp(c_fixture.memory.data,
                expected_data, sizeof(expected_data)) == 0);
        assert(memcmp(threaded_fixture.memory.data,
                expected_data, sizeof(expected_data)) == 0);
    }

    assert(encode_asrv(false, 0, 0, 0) ==
            UINT32_C(0x1ac02800));
    assert(encode_asrv(false, 31, 31, 31) ==
            UINT32_C(0x1adf2bff));
    assert(encode_asrv(true, 0, 0, 0) ==
            UINT32_C(0x9ac02800));
    assert(encode_asrv(true, 31, 31, 31) ==
            UINT32_C(0x9adf2bff));

    static const struct {
        dword_t instruction;
        bool is_64;
        byte_t rn;
        byte_t rm;
        byte_t rd;
        qword_t rn_value;
        qword_t rm_value;
        qword_t expected;
    } roles[] = {
        {
            INSTRUCTION_ASRV_W2_W23_W2,
            false, 23, 2, 2,
            UINT64_C(0xaaaaaaaa81234567), 3,
            UINT32_C(0xf02468ac),
        },
        {
            UINT32_C(0x9add2b9b),
            true, 28, 29, 27,
            UINT64_C(0x8000000000000000), 127,
            UINT64_MAX,
        },
        {
            UINT32_C(0x1ac52be6),
            false, 31, 5, 6,
            0, 1, 0,
        },
        {
            UINT32_C(0x9adf2886),
            true, 4, 31, 6,
            UINT64_C(0x8123456789abcdef), 0,
            UINT64_C(0x8123456789abcdef),
        },
        {
            UINT32_C(0x1ac5289f),
            false, 4, 5, 31,
            UINT64_C(0xaaaaaaaa89abcdef), 8, 0,
        },
        {
            UINT32_C(0x9ac52884),
            true, 4, 5, 4,
            UINT64_C(0x8123456789abcdef), 4,
            UINT64_C(0xf8123456789abcde),
        },
        {
            UINT32_C(0x1ac42886),
            false, 4, 4, 6,
            UINT64_C(0xaaaaaaaa80000004),
            UINT64_C(0xaaaaaaaa80000004),
            UINT32_C(0xf8000000),
        },
        {
            UINT32_C(0x1ac728e7),
            false, 7, 7, 7,
            UINT64_C(0xaaaaaaaa80000004),
            UINT64_C(0xaaaaaaaa80000004),
            UINT32_C(0xf8000000),
        },
    };
    _Static_assert(array_size(roles) == 8,
            "ASRV 寄存器角色专例数量必须保持稳定");
    _Static_assert(array_size(semantic_cases) + array_size(roles) == 20,
            "ASRV 快路径差分用例总数必须保持稳定");
    for (unsigned index = 0; index < array_size(roles); index++) {
        assert(encode_asrv(roles[index].is_64,
                roles[index].rm, roles[index].rn,
                roles[index].rd) == roles[index].instruction);
        if (roles[index].rn == roles[index].rm)
            assert(roles[index].rn_value ==
                    roles[index].rm_value);

        struct aarch64_decoded decoded;
        assert(aarch64_decode(roles[index].instruction, &decoded));
        assert(decoded.opcode == AARCH64_OP_ASRV);
        assert(decoded.width == (roles[index].is_64 ? 64 : 32));
        assert(decoded.operands.data_processing_2source.rd ==
                roles[index].rd);
        assert(decoded.operands.data_processing_2source.rn ==
                roles[index].rn);
        assert(decoded.operands.data_processing_2source.rm ==
                roles[index].rm);

        struct cpu_state initial;
        init_differential_cpu(&initial);
        initial.sp = DATA_PAGE + 0x381;
        if (roles[index].rd != 31)
            initial.x[roles[index].rd] = UINT64_MAX;
        if (roles[index].rn != 31)
            initial.x[roles[index].rn] =
                    roles[index].rn_value;
        if (roles[index].rm != 31)
            initial.x[roles[index].rm] =
                    roles[index].rm_value;
        aarch64_set_exclusive(&initial,
                DATA_PAGE + 0x60, 8, false,
                UINT64_C(0x3344), 0, NULL, 4, 6, 8);

        struct test_fixture c_fixture;
        struct test_fixture threaded_fixture;
        init_fixture(&c_fixture);
        init_fixture(&threaded_fixture);
        write_instruction(&c_fixture.tlb,
                CODE_PAGE, roles[index].instruction);
        write_instruction(&threaded_fixture.tlb,
                CODE_PAGE, roles[index].instruction);
        memset(c_fixture.memory.data, 0xa5,
                sizeof(c_fixture.memory.data));
        memset(threaded_fixture.memory.data, 0xa5,
                sizeof(threaded_fixture.memory.data));
        byte_t expected_data[GUEST_MEMORY_PAGE_SIZE];
        memcpy(expected_data, c_fixture.memory.data,
                sizeof(expected_data));

        struct cpu_state result =
                run_fast_differential_fixtures(
                        roles[index].instruction, initial,
                        AARCH64_STEP_RETIRED,
                        &c_fixture, &threaded_fixture, NULL);
        struct cpu_state expected_cpu = initial;
        expected_cpu.pc = CODE_PAGE + 4;
        expected_cpu.cycle++;
        if (roles[index].rd != 31)
            expected_cpu.x[roles[index].rd] =
                    roles[index].expected;
        assert_cpu_equal(&result, &expected_cpu);
        assert(memcmp(c_fixture.memory.data,
                expected_data, sizeof(expected_data)) == 0);
        assert(memcmp(threaded_fixture.memory.data,
                expected_data, sizeof(expected_data)) == 0);
    }
}

static void test_fast_lslv_differential(void) {
    const qword_t source = UINT64_C(0x8123456789abcdef);
    unsigned shift_case_count = 0;
    for (unsigned width_index = 0; width_index < 2; width_index++) {
        bool is_64 = width_index != 0;
        byte_t width = is_64 ? 64 : 32;
        for (unsigned case_index = 0;
                case_index < (unsigned) width + 3; case_index++) {
            qword_t raw_amount;
            if (case_index < width)
                raw_amount = case_index;
            else if (case_index == (unsigned) width)
                raw_amount = width;
            else if (case_index == (unsigned) width + 1)
                raw_amount = (qword_t) width + 1;
            else
                raw_amount = UINT64_MAX;

            struct cpu_state initial;
            init_differential_cpu(&initial);
            initial.x[4] = source;
            initial.x[5] = raw_amount;
            initial.x[6] = UINT64_MAX;
            aarch64_set_exclusive(&initial,
                    DATA_PAGE + 0x60, 8, false,
                    UINT64_C(0x3344), 0, NULL, 4, 6, 8);

            dword_t instruction =
                    encode_lslv(is_64, 5, 4, 6);
            struct test_fixture c_fixture;
            struct test_fixture threaded_fixture;
            init_fixture(&c_fixture);
            init_fixture(&threaded_fixture);
            write_instruction(&c_fixture.tlb,
                    CODE_PAGE, instruction);
            write_instruction(&threaded_fixture.tlb,
                    CODE_PAGE, instruction);
            memset(c_fixture.memory.data, 0xa5,
                    sizeof(c_fixture.memory.data));
            memset(threaded_fixture.memory.data, 0xa5,
                    sizeof(threaded_fixture.memory.data));
            byte_t expected_data[GUEST_MEMORY_PAGE_SIZE];
            memcpy(expected_data, c_fixture.memory.data,
                    sizeof(expected_data));

            struct cpu_state result =
                    run_fast_differential_fixtures(
                            instruction, initial,
                            AARCH64_STEP_RETIRED,
                            &c_fixture, &threaded_fixture, NULL);
            qword_t mask = is_64 ? UINT64_MAX : UINT32_MAX;
            qword_t operand = source & mask;
            qword_t amount = raw_amount & (width - 1);
            qword_t expected_value =
                    (operand << amount) & mask;
            struct cpu_state expected_cpu = initial;
            expected_cpu.x[6] = expected_value;
            expected_cpu.pc = CODE_PAGE + 4;
            expected_cpu.cycle++;
            assert_cpu_equal(&result, &expected_cpu);
            assert(memcmp(c_fixture.memory.data,
                    expected_data, sizeof(expected_data)) == 0);
            assert(memcmp(threaded_fixture.memory.data,
                    expected_data, sizeof(expected_data)) == 0);

            if (!is_64 && case_index == 0)
                assert(result.x[6] == UINT32_C(0x89abcdef));
            if (!is_64 && case_index == 1)
                assert(result.x[6] == UINT32_C(0x13579bde));
            if (!is_64 && case_index == 31)
                assert(result.x[6] == UINT32_C(0x80000000));
            if (!is_64 && case_index == 32)
                assert(result.x[6] == UINT32_C(0x89abcdef));
            if (!is_64 && case_index == 33)
                assert(result.x[6] == UINT32_C(0x13579bde));
            if (!is_64 && case_index == 34)
                assert(result.x[6] == UINT32_C(0x80000000));
            if (is_64 && case_index == 0)
                assert(result.x[6] ==
                        UINT64_C(0x8123456789abcdef));
            if (is_64 && case_index == 1)
                assert(result.x[6] ==
                        UINT64_C(0x02468acf13579bde));
            if (is_64 && case_index == 63)
                assert(result.x[6] ==
                        UINT64_C(0x8000000000000000));
            if (is_64 && case_index == 64)
                assert(result.x[6] ==
                        UINT64_C(0x8123456789abcdef));
            if (is_64 && case_index == 65)
                assert(result.x[6] ==
                        UINT64_C(0x02468acf13579bde));
            if (is_64 && case_index == 66)
                assert(result.x[6] ==
                        UINT64_C(0x8000000000000000));
            shift_case_count++;
        }
    }
    assert(shift_case_count == 102);

    assert(encode_lslv(false, 0, 0, 0) ==
            UINT32_C(0x1ac02000));
    assert(encode_lslv(false, 31, 31, 31) ==
            UINT32_C(0x1adf23ff));
    assert(encode_lslv(true, 0, 0, 0) ==
            UINT32_C(0x9ac02000));
    assert(encode_lslv(true, 31, 31, 31) ==
            UINT32_C(0x9adf23ff));

    static const struct {
        dword_t instruction;
        bool is_64;
        byte_t rn;
        byte_t rm;
        byte_t rd;
        qword_t rn_value;
        qword_t rm_value;
        qword_t expected;
    } roles[] = {
        {
            INSTRUCTION_LSLV_X5_X7_X1,
            true, 7, 1, 5,
            3, 4, 48,
        },
        {
            INSTRUCTION_LSLV_W0_W0_W1,
            false, 0, 1, 0,
            UINT64_C(0xaaaaaaaa00000003), 4, 48,
        },
        {
            UINT32_C(0x1ac42144),
            false, 10, 4, 4,
            UINT64_C(0xaaaaaaaa80000004), 4,
            UINT32_C(0x40),
        },
        {
            UINT32_C(0x1ac523e6),
            false, 31, 5, 6,
            0, 1, 0,
        },
        {
            UINT32_C(0x9adf2086),
            true, 4, 31, 6,
            UINT64_C(0x8123456789abcdef), 0,
            UINT64_C(0x8123456789abcdef),
        },
        {
            UINT32_C(0x1ac5209f),
            false, 4, 5, 31,
            UINT64_C(0xaaaaaaaa89abcdef), 8, 0,
        },
        {
            UINT32_C(0x9ac42086),
            true, 4, 4, 6,
            UINT64_C(0x8000000000000001),
            UINT64_C(0x8000000000000001), 2,
        },
        {
            UINT32_C(0x1ac720e7),
            false, 7, 7, 7,
            UINT64_C(0xaaaaaaaa80000004),
            UINT64_C(0xaaaaaaaa80000004),
            UINT32_C(0x40),
        },
    };
    _Static_assert(array_size(roles) == 8,
            "LSLV 寄存器角色专例数量必须保持稳定");
    for (unsigned index = 0; index < array_size(roles); index++) {
        assert(encode_lslv(roles[index].is_64,
                roles[index].rm, roles[index].rn,
                roles[index].rd) == roles[index].instruction);
        if (roles[index].rn == roles[index].rm)
            assert(roles[index].rn_value ==
                    roles[index].rm_value);

        struct cpu_state initial;
        init_differential_cpu(&initial);
        initial.sp = DATA_PAGE + 0x381;
        if (roles[index].rd != 31)
            initial.x[roles[index].rd] = UINT64_MAX;
        if (roles[index].rn != 31)
            initial.x[roles[index].rn] =
                    roles[index].rn_value;
        if (roles[index].rm != 31)
            initial.x[roles[index].rm] =
                    roles[index].rm_value;
        aarch64_set_exclusive(&initial,
                DATA_PAGE + 0x60, 8, false,
                UINT64_C(0x3344), 0, NULL, 4, 6, 8);

        struct test_fixture c_fixture;
        struct test_fixture threaded_fixture;
        init_fixture(&c_fixture);
        init_fixture(&threaded_fixture);
        write_instruction(&c_fixture.tlb,
                CODE_PAGE, roles[index].instruction);
        write_instruction(&threaded_fixture.tlb,
                CODE_PAGE, roles[index].instruction);
        memset(c_fixture.memory.data, 0xa5,
                sizeof(c_fixture.memory.data));
        memset(threaded_fixture.memory.data, 0xa5,
                sizeof(threaded_fixture.memory.data));
        byte_t expected_data[GUEST_MEMORY_PAGE_SIZE];
        memcpy(expected_data, c_fixture.memory.data,
                sizeof(expected_data));

        struct cpu_state result =
                run_fast_differential_fixtures(
                        roles[index].instruction, initial,
                        AARCH64_STEP_RETIRED,
                        &c_fixture, &threaded_fixture, NULL);
        struct cpu_state expected_cpu = initial;
        expected_cpu.pc = CODE_PAGE + 4;
        expected_cpu.cycle++;
        if (roles[index].rd != 31)
            expected_cpu.x[roles[index].rd] =
                    roles[index].expected;
        assert_cpu_equal(&result, &expected_cpu);
        assert(memcmp(c_fixture.memory.data,
                expected_data, sizeof(expected_data)) == 0);
        assert(memcmp(threaded_fixture.memory.data,
                expected_data, sizeof(expected_data)) == 0);
    }
    assert(shift_case_count + array_size(roles) == 110);
}

static void test_fast_madd_differential(void) {
    assert(encode_madd(false, 0, 0, 0, 0) ==
            UINT32_C(0x1b000000));
    assert(encode_madd(false, 31, 31, 31, 31) ==
            UINT32_C(0x1b1f7fff));
    assert(encode_madd(true, 0, 0, 0, 0) ==
            UINT32_C(0x9b000000));
    assert(encode_madd(true, 31, 31, 31, 31) ==
            UINT32_C(0x9b1f7fff));

    static const struct {
        dword_t instruction;
        bool is_64;
        byte_t rd;
        byte_t rn;
        byte_t rm;
        byte_t ra;
        qword_t rn_value;
        qword_t rm_value;
        qword_t ra_value;
        qword_t expected;
    } cases[] = {
        {
            INSTRUCTION_MADD_W0_W1_W2_W3,
            false, 0, 1, 2, 3,
            UINT64_C(0xaaaaaaaaffffffff),
            UINT64_C(0xbbbbbbbb00000002),
            UINT64_C(0xcccccccc00000003),
            1,
        },
        {
            INSTRUCTION_MADD_X4_X5_X6_X7,
            true, 4, 5, 6, 7,
            UINT64_MAX, 2, 3, 1,
        },
        {
            INSTRUCTION_MUL_X6_X6_X0,
            true, 6, 6, 0, 31,
            7, 9, 0, 63,
        },
        {
            INSTRUCTION_MUL_W2_W0_W2,
            false, 2, 0, 2, 31,
            UINT64_C(0xaaaaaaaa00000003),
            UINT64_C(0xbbbbbbbb80000001),
            0, UINT32_C(0x80000003),
        },
        {
            UINT32_C(0x9b0b2549),
            true, 9, 10, 11, 9,
            6, 7, 8, 50,
        },
        {
            UINT32_C(0x9b092928),
            true, 8, 9, 9, 10,
            4, 4, 5, 21,
        },
        {
            UINT32_C(0x9b0a2528),
            true, 8, 9, 10, 9,
            4, 5, 4, 24,
        },
        {
            UINT32_C(0x9b0a2928),
            true, 8, 9, 10, 10,
            4, 5, 5, 25,
        },
        {
            UINT32_C(0x9b071ce7),
            true, 7, 7, 7, 7,
            3, 3, 3, 12,
        },
        {
            UINT32_C(0x9b092be8),
            true, 8, 31, 9, 10,
            0, 7, 13, 13,
        },
        {
            UINT32_C(0x9b1f2928),
            true, 8, 9, 31, 10,
            7, 0, 13, 13,
        },
        {
            UINT32_C(0x9b0c357f),
            true, 31, 11, 12, 13,
            6, 7, 8, 0,
        },
    };
    _Static_assert(array_size(cases) == 12,
            "MADD 绝对差分专例数量必须保持稳定");
    for (unsigned index = 0; index < array_size(cases); index++) {
        assert(encode_madd(cases[index].is_64,
                cases[index].rn, cases[index].rm,
                cases[index].ra, cases[index].rd) ==
                cases[index].instruction);
        if (cases[index].rn == cases[index].rm)
            assert(cases[index].rn_value == cases[index].rm_value);
        if (cases[index].rn == cases[index].ra)
            assert(cases[index].rn_value == cases[index].ra_value);
        if (cases[index].rm == cases[index].ra)
            assert(cases[index].rm_value == cases[index].ra_value);

        struct cpu_state initial;
        init_differential_cpu(&initial);
        initial.sp = DATA_PAGE + 0x381;
        if (cases[index].rd != 31)
            initial.x[cases[index].rd] = UINT64_MAX;
        if (cases[index].rn != 31)
            initial.x[cases[index].rn] = cases[index].rn_value;
        if (cases[index].rm != 31)
            initial.x[cases[index].rm] = cases[index].rm_value;
        if (cases[index].ra != 31)
            initial.x[cases[index].ra] = cases[index].ra_value;
        aarch64_set_exclusive(&initial,
                DATA_PAGE + 0x60, 8, false,
                UINT64_C(0x3344), 0, NULL, 4, 6, 8);

        struct test_fixture c_fixture;
        struct test_fixture threaded_fixture;
        init_fixture(&c_fixture);
        init_fixture(&threaded_fixture);
        write_instruction(&c_fixture.tlb,
                CODE_PAGE, cases[index].instruction);
        write_instruction(&threaded_fixture.tlb,
                CODE_PAGE, cases[index].instruction);
        memset(c_fixture.memory.data, 0xa5,
                sizeof(c_fixture.memory.data));
        memset(threaded_fixture.memory.data, 0xa5,
                sizeof(threaded_fixture.memory.data));
        byte_t expected_data[GUEST_MEMORY_PAGE_SIZE];
        memcpy(expected_data, c_fixture.memory.data,
                sizeof(expected_data));

        struct cpu_state result =
                run_fast_differential_fixtures(
                        cases[index].instruction, initial,
                        AARCH64_STEP_RETIRED,
                        &c_fixture, &threaded_fixture, NULL);
        struct cpu_state expected_cpu = initial;
        expected_cpu.pc = CODE_PAGE + 4;
        expected_cpu.cycle++;
        if (cases[index].rd != 31)
            expected_cpu.x[cases[index].rd] =
                    cases[index].expected;
        assert_cpu_equal(&result, &expected_cpu);
        assert(memcmp(c_fixture.memory.data,
                expected_data, sizeof(expected_data)) == 0);
        assert(memcmp(threaded_fixture.memory.data,
                expected_data, sizeof(expected_data)) == 0);
    }
}

static void test_fast_msub_differential(void) {
    assert(encode_msub(false, 0, 0, 0, 0) ==
            UINT32_C(0x1b008000));
    assert(encode_msub(false, 31, 31, 31, 31) ==
            UINT32_C(0x1b1fffff));
    assert(encode_msub(true, 0, 0, 0, 0) ==
            UINT32_C(0x9b008000));
    assert(encode_msub(true, 31, 31, 31, 31) ==
            UINT32_C(0x9b1fffff));

    static const struct {
        dword_t instruction;
        bool is_64;
        byte_t rd;
        byte_t rn;
        byte_t rm;
        byte_t ra;
        qword_t rn_value;
        qword_t rm_value;
        qword_t ra_value;
        qword_t expected;
    } cases[] = {
        {
            INSTRUCTION_MSUB_W6_W6_W5_W10,
            false, 6, 6, 5, 10,
            UINT64_C(0xaaaaaaaa00000003),
            UINT64_C(0xbbbbbbbb00000004),
            UINT64_C(0xcccccccc00000020),
            UINT32_C(0x00000014),
        },
        {
            UINT32_C(0x9b069ca4),
            true, 4, 5, 6, 7,
            UINT64_MAX, 2, 3, 5,
        },
        {
            UINT32_C(0x9b00fcc8),
            true, 8, 6, 0, 31,
            UINT64_MAX, 2, 0, 2,
        },
        {
            UINT32_C(0x1b02fc02),
            false, 2, 0, 2, 31,
            UINT64_C(0xaaaaaaaa00000003),
            UINT64_C(0xbbbbbbbb80000001),
            0, UINT32_C(0x7ffffffd),
        },
        {
            UINT32_C(0x9b0ba549),
            true, 9, 10, 11, 9,
            6, 7, 50, 8,
        },
        {
            UINT32_C(0x9b09a928),
            true, 8, 9, 9, 10,
            4, 4, 5, UINT64_C(0xfffffffffffffff5),
        },
        {
            UINT32_C(0x9b0aa528),
            true, 8, 9, 10, 9,
            4, 5, 4, UINT64_C(0xfffffffffffffff0),
        },
        {
            UINT32_C(0x9b0aa928),
            true, 8, 9, 10, 10,
            4, 5, 5, UINT64_C(0xfffffffffffffff1),
        },
        {
            UINT32_C(0x9b079ce7),
            true, 7, 7, 7, 7,
            3, 3, 3, UINT64_C(0xfffffffffffffffa),
        },
        {
            UINT32_C(0x9b09abe8),
            true, 8, 31, 9, 10,
            0, 7, 13, 13,
        },
        {
            UINT32_C(0x9b1fa928),
            true, 8, 9, 31, 10,
            7, 0, 13, 13,
        },
        {
            UINT32_C(0x9b0cb57f),
            true, 31, 11, 12, 13,
            6, 7, 8, UINT64_C(0xffffffffffffffde),
        },
    };
    _Static_assert(array_size(cases) == 12,
            "MSUB 绝对差分专例数量必须保持稳定");
    for (unsigned index = 0; index < array_size(cases); index++) {
        assert(encode_msub(cases[index].is_64,
                cases[index].rn, cases[index].rm,
                cases[index].ra, cases[index].rd) ==
                cases[index].instruction);
        if (cases[index].rn == cases[index].rm)
            assert(cases[index].rn_value == cases[index].rm_value);
        if (cases[index].rn == cases[index].ra)
            assert(cases[index].rn_value == cases[index].ra_value);
        if (cases[index].rm == cases[index].ra)
            assert(cases[index].rm_value == cases[index].ra_value);

        struct cpu_state initial;
        init_differential_cpu(&initial);
        initial.sp = DATA_PAGE + 0x381;
        if (cases[index].rd != 31)
            initial.x[cases[index].rd] = UINT64_MAX;
        if (cases[index].rn != 31)
            initial.x[cases[index].rn] = cases[index].rn_value;
        if (cases[index].rm != 31)
            initial.x[cases[index].rm] = cases[index].rm_value;
        if (cases[index].ra != 31)
            initial.x[cases[index].ra] = cases[index].ra_value;
        aarch64_set_exclusive(&initial,
                DATA_PAGE + 0x60, 8, false,
                UINT64_C(0x3344), 0, NULL, 4, 6, 8);

        qword_t mask = cases[index].is_64 ?
                UINT64_MAX : UINT32_MAX;
        qword_t rn_value = cases[index].rn == 31 ?
                0 : initial.x[cases[index].rn] & mask;
        qword_t rm_value = cases[index].rm == 31 ?
                0 : initial.x[cases[index].rm] & mask;
        qword_t ra_value = cases[index].ra == 31 ?
                0 : initial.x[cases[index].ra] & mask;
        qword_t reference = (qword_t) (
                (__uint128_t) ra_value -
                (__uint128_t) rn_value * rm_value) & mask;
        assert(reference == cases[index].expected);

        struct test_fixture c_fixture;
        struct test_fixture threaded_fixture;
        init_fixture(&c_fixture);
        init_fixture(&threaded_fixture);
        write_instruction(&c_fixture.tlb,
                CODE_PAGE, cases[index].instruction);
        write_instruction(&threaded_fixture.tlb,
                CODE_PAGE, cases[index].instruction);
        memset(c_fixture.memory.data, 0xa5,
                sizeof(c_fixture.memory.data));
        memset(threaded_fixture.memory.data, 0xa5,
                sizeof(threaded_fixture.memory.data));
        byte_t expected_data[GUEST_MEMORY_PAGE_SIZE];
        memcpy(expected_data, c_fixture.memory.data,
                sizeof(expected_data));

        struct cpu_state result =
                run_fast_differential_fixtures(
                        cases[index].instruction, initial,
                        AARCH64_STEP_RETIRED,
                        &c_fixture, &threaded_fixture, NULL);
        struct cpu_state expected_cpu = initial;
        expected_cpu.pc = CODE_PAGE + 4;
        expected_cpu.cycle++;
        if (cases[index].rd != 31)
            expected_cpu.x[cases[index].rd] =
                    cases[index].expected;
        assert_cpu_equal(&result, &expected_cpu);
        assert(memcmp(c_fixture.memory.data,
                expected_data, sizeof(expected_data)) == 0);
        assert(memcmp(threaded_fixture.memory.data,
                expected_data, sizeof(expected_data)) == 0);
    }
}

static void test_fast_umaddl_differential(void) {
    assert(encode_umaddl(0, 0, 0, 0) ==
            UINT32_C(0x9ba00000));
    assert(encode_umaddl(31, 31, 31, 31) ==
            UINT32_C(0x9bbf7fff));

    static const struct {
        dword_t instruction;
        byte_t rd;
        byte_t rn;
        byte_t rm;
        byte_t ra;
        qword_t rn_value;
        qword_t rm_value;
        qword_t ra_value;
        qword_t expected;
    } cases[] = {
        {
            INSTRUCTION_UMADDL_X1_W0_W11_XZR,
            1, 0, 11, 31,
            UINT64_C(0xdeadbeef80000001),
            UINT64_C(0xcafebabe00000002),
            0, UINT64_C(0x0000000100000002),
        },
        {
            INSTRUCTION_UMADDL_X14_W15_W16_X17,
            14, 15, 16, 17,
            UINT64_MAX, UINT64_MAX, UINT64_MAX,
            UINT64_C(0xfffffffe00000000),
        },
        {
            UINT32_C(0x9ba92be8),
            8, 31, 9, 10,
            0, 7, 13, 13,
        },
        {
            UINT32_C(0x9bbf2928),
            8, 9, 31, 10,
            7, 0, 13, 13,
        },
        {
            UINT32_C(0x9bac357f),
            31, 11, 12, 13,
            6, 7, 8, 50,
        },
        {
            UINT32_C(0x9ba92908),
            8, 8, 9, 10,
            4, 5, 6, 26,
        },
        {
            UINT32_C(0x9ba82928),
            8, 9, 8, 10,
            4, 5, 6, 26,
        },
        {
            UINT32_C(0x9bab2549),
            9, 10, 11, 9,
            6, 7, 8, 50,
        },
        {
            UINT32_C(0x9ba92928),
            8, 9, 9, 10,
            4, 4, 5, 21,
        },
        {
            UINT32_C(0x9baa2528),
            8, 9, 10, 9,
            UINT64_C(0xaaaaaaaa00000004), 5,
            UINT64_C(0xaaaaaaaa00000004),
            UINT64_C(0xaaaaaaaa00000018),
        },
        {
            UINT32_C(0x9baa2928),
            8, 9, 10, 10,
            4, UINT64_C(0xbbbbbbbb00000005),
            UINT64_C(0xbbbbbbbb00000005),
            UINT64_C(0xbbbbbbbb00000019),
        },
        {
            UINT32_C(0x9ba71ce7),
            7, 7, 7, 7,
            UINT64_C(0xaaaaaaaa00000003),
            UINT64_C(0xaaaaaaaa00000003),
            UINT64_C(0xaaaaaaaa00000003),
            UINT64_C(0xaaaaaaaa0000000c),
        },
    };
    _Static_assert(array_size(cases) == 12,
            "UMADDL 绝对差分专例数量必须保持稳定");
    for (unsigned index = 0; index < array_size(cases); index++) {
        assert(encode_umaddl(cases[index].rn, cases[index].rm,
                cases[index].ra, cases[index].rd) ==
                cases[index].instruction);
        if (cases[index].rn == cases[index].rm)
            assert(cases[index].rn_value == cases[index].rm_value);
        if (cases[index].rn == cases[index].ra)
            assert(cases[index].rn_value == cases[index].ra_value);
        if (cases[index].rm == cases[index].ra)
            assert(cases[index].rm_value == cases[index].ra_value);

        struct cpu_state initial;
        init_differential_cpu(&initial);
        initial.sp = DATA_PAGE + 0x381;
        if (cases[index].rd != 31)
            initial.x[cases[index].rd] = UINT64_MAX;
        if (cases[index].rn != 31)
            initial.x[cases[index].rn] = cases[index].rn_value;
        if (cases[index].rm != 31)
            initial.x[cases[index].rm] = cases[index].rm_value;
        if (cases[index].ra != 31)
            initial.x[cases[index].ra] = cases[index].ra_value;
        aarch64_set_exclusive(&initial,
                DATA_PAGE + 0x60, 8, false,
                UINT64_C(0x3344), 0, NULL, 4, 6, 8);

        qword_t rn_value = cases[index].rn == 31 ?
                0 : initial.x[cases[index].rn];
        qword_t rm_value = cases[index].rm == 31 ?
                0 : initial.x[cases[index].rm];
        qword_t ra_value = cases[index].ra == 31 ?
                0 : initial.x[cases[index].ra];
        qword_t reference = (qword_t) (
                (__uint128_t) (dword_t) rn_value *
                (dword_t) rm_value + (__uint128_t) ra_value);
        assert(reference == cases[index].expected);

        struct test_fixture c_fixture;
        struct test_fixture threaded_fixture;
        init_fixture(&c_fixture);
        init_fixture(&threaded_fixture);
        write_instruction(&c_fixture.tlb,
                CODE_PAGE, cases[index].instruction);
        write_instruction(&threaded_fixture.tlb,
                CODE_PAGE, cases[index].instruction);
        memset(c_fixture.memory.data, 0xa5,
                sizeof(c_fixture.memory.data));
        memset(threaded_fixture.memory.data, 0xa5,
                sizeof(threaded_fixture.memory.data));
        byte_t expected_data[GUEST_MEMORY_PAGE_SIZE];
        memcpy(expected_data, c_fixture.memory.data,
                sizeof(expected_data));

        struct cpu_state result =
                run_fast_differential_fixtures(
                        cases[index].instruction, initial,
                        AARCH64_STEP_RETIRED,
                        &c_fixture, &threaded_fixture, NULL);
        struct cpu_state expected_cpu = initial;
        expected_cpu.pc = CODE_PAGE + 4;
        expected_cpu.cycle++;
        if (cases[index].rd != 31)
            expected_cpu.x[cases[index].rd] =
                    cases[index].expected;
        assert_cpu_equal(&result, &expected_cpu);
        assert(memcmp(c_fixture.memory.data,
                expected_data, sizeof(expected_data)) == 0);
        assert(memcmp(threaded_fixture.memory.data,
                expected_data, sizeof(expected_data)) == 0);
    }
}

static void test_fast_smaddl_differential(void) {
    assert(encode_smaddl(0, 0, 0, 0) ==
            UINT32_C(0x9b200000));
    assert(encode_smaddl(31, 31, 31, 31) ==
            UINT32_C(0x9b3f7fff));

    static const struct {
        dword_t instruction;
        byte_t rd;
        byte_t rn;
        byte_t rm;
        byte_t ra;
        qword_t rn_value;
        qword_t rm_value;
        qword_t ra_value;
        qword_t expected;
    } cases[] = {
        {
            INSTRUCTION_SMADDL_X0_W0_W1_XZR,
            0, 0, 1, 31,
            UINT64_C(0xaaaaaaaafffffffd),
            UINT64_C(0xbbbbbbbb00000007),
            0, UINT64_C(0xffffffffffffffeb),
        },
        {
            INSTRUCTION_SMADDL_X6_W7_W8_X9,
            6, 7, 8, 9,
            UINT64_C(0xffffffff80000000),
            UINT64_MAX, UINT64_MAX,
            UINT64_C(0x000000007fffffff),
        },
        {
            UINT32_C(0x9b292bea),
            10, 31, 9, 10,
            0, UINT64_C(0xccccccccffffffff),
            UINT64_C(0x8123456789abcdef),
            UINT64_C(0x8123456789abcdef),
        },
        {
            UINT32_C(0x9b3f1ca6),
            6, 5, 31, 7,
            UINT64_C(0xaaaaaaaaffffffff), 0,
            UINT64_C(0x0123456789abcdef),
            UINT64_C(0x0123456789abcdef),
        },
        {
            UINT32_C(0x9b251885),
            5, 4, 5, 6,
            UINT64_C(0xaaaaaaaa00000003),
            UINT64_C(0xbbbbbbbb00000005),
            UINT64_C(0xfffffffffffffff8), 7,
        },
        {
            UINT32_C(0x9b2c357f),
            31, 11, 12, 13,
            UINT64_C(0xaaaaaaaafffffffe),
            UINT64_C(0xbbbbbbbb00000003),
            UINT64_C(0x0123456789abcdef),
            UINT64_C(0x0123456789abcde9),
        },
    };
    _Static_assert(array_size(cases) == 6,
            "SMADDL 符号、累加与寄存器角色专例数量必须保持稳定");

    for (unsigned index = 0; index < array_size(cases); index++) {
        assert(encode_smaddl(cases[index].rn, cases[index].rm,
                cases[index].ra, cases[index].rd) ==
                cases[index].instruction);
        struct aarch64_decoded decoded;
        assert(aarch64_decode(cases[index].instruction, &decoded));
        assert(decoded.opcode == AARCH64_OP_SMADDL);
        assert(decoded.width == 64);
        assert(decoded.operands.data_processing_3source.rd ==
                cases[index].rd);
        assert(decoded.operands.data_processing_3source.rn ==
                cases[index].rn);
        assert(decoded.operands.data_processing_3source.rm ==
                cases[index].rm);
        assert(decoded.operands.data_processing_3source.ra ==
                cases[index].ra);
        if (cases[index].rn == cases[index].rm)
            assert(cases[index].rn_value == cases[index].rm_value);
        if (cases[index].rn == cases[index].ra)
            assert(cases[index].rn_value == cases[index].ra_value);
        if (cases[index].rm == cases[index].ra)
            assert(cases[index].rm_value == cases[index].ra_value);

        struct cpu_state initial;
        init_differential_cpu(&initial);
        initial.sp = DATA_PAGE + 0x381;
        if (cases[index].rd != 31)
            initial.x[cases[index].rd] = UINT64_MAX;
        if (cases[index].rn != 31)
            initial.x[cases[index].rn] = cases[index].rn_value;
        if (cases[index].rm != 31)
            initial.x[cases[index].rm] = cases[index].rm_value;
        if (cases[index].ra != 31)
            initial.x[cases[index].ra] = cases[index].ra_value;
        aarch64_set_exclusive(&initial,
                DATA_PAGE + 0x60, 8, false,
                UINT64_C(0x3344), 0, NULL, 4, 6, 8);

        sqword_t rn_value = cases[index].rn == 31 ?
                0 : (dword_t) initial.x[cases[index].rn];
        sqword_t rm_value = cases[index].rm == 31 ?
                0 : (dword_t) initial.x[cases[index].rm];
        if (rn_value >= INT64_C(0x80000000))
            rn_value -= INT64_C(0x100000000);
        if (rm_value >= INT64_C(0x80000000))
            rm_value -= INT64_C(0x100000000);
        qword_t ra_value = cases[index].ra == 31 ?
                0 : initial.x[cases[index].ra];
        qword_t reference =
                ra_value + (qword_t) (rn_value * rm_value);
        assert(reference == cases[index].expected);

        struct test_fixture c_fixture;
        struct test_fixture threaded_fixture;
        init_fixture(&c_fixture);
        init_fixture(&threaded_fixture);
        write_instruction(&c_fixture.tlb,
                CODE_PAGE, cases[index].instruction);
        write_instruction(&threaded_fixture.tlb,
                CODE_PAGE, cases[index].instruction);
        memset(c_fixture.memory.data, 0xa5,
                sizeof(c_fixture.memory.data));
        memset(threaded_fixture.memory.data, 0xa5,
                sizeof(threaded_fixture.memory.data));
        byte_t expected_data[GUEST_MEMORY_PAGE_SIZE];
        memcpy(expected_data, c_fixture.memory.data,
                sizeof(expected_data));

        struct cpu_state result =
                run_fast_differential_fixtures(
                        cases[index].instruction, initial,
                        AARCH64_STEP_RETIRED,
                        &c_fixture, &threaded_fixture, NULL);
        struct cpu_state expected_cpu = initial;
        expected_cpu.pc = CODE_PAGE + 4;
        expected_cpu.cycle++;
        if (cases[index].rd != 31)
            expected_cpu.x[cases[index].rd] =
                    cases[index].expected;
        assert_cpu_equal(&result, &expected_cpu);
        assert(memcmp(c_fixture.memory.data,
                expected_data, sizeof(expected_data)) == 0);
        assert(memcmp(threaded_fixture.memory.data,
                expected_data, sizeof(expected_data)) == 0);
    }
}

static void test_multiply_add_sibling_fallback(void) {
    static const struct {
        dword_t instruction;
        enum aarch64_opcode opcode;
        qword_t rn_value;
        qword_t rm_value;
        qword_t ra_value;
        qword_t expected;
    } cases[] = {
        {
            INSTRUCTION_SMSUBL_X10_W11_W12_X13,
            AARCH64_OP_SMSUBL,
            1, 1, UINT64_C(0x8000000000000000),
            UINT64_C(0x7fffffffffffffff),
        },
        {
            INSTRUCTION_UMSUBL_X18_W19_W20_X21,
            AARCH64_OP_UMSUBL,
            UINT64_MAX, UINT64_MAX, 0,
            UINT64_C(0x00000001ffffffff),
        },
        {
            INSTRUCTION_SMULH_X8_X9_X10,
            AARCH64_OP_SMULH,
            UINT64_C(0x8000000000000000), 2, 0,
            UINT64_MAX,
        },
        {
            INSTRUCTION_UMULH_X11_X12_X13,
            AARCH64_OP_UMULH,
            UINT64_MAX, UINT64_MAX, 0,
            UINT64_C(0xfffffffffffffffe),
        },
    };
    _Static_assert(array_size(cases) == 4,
            "三源乘法 sibling 指令数量必须保持稳定");

    struct test_fixture fixture;
    init_fixture(&fixture);
    memset(fixture.memory.data, 0xa5,
            sizeof(fixture.memory.data));
    byte_t expected_data[GUEST_MEMORY_PAGE_SIZE];
    memcpy(expected_data, fixture.memory.data,
            sizeof(expected_data));
    for (unsigned index = 0; index < array_size(cases); index++) {
        struct aarch64_decoded decoded;
        assert(aarch64_decode(cases[index].instruction, &decoded));
        assert(decoded.opcode == cases[index].opcode);
        write_instruction(&fixture.tlb,
                CODE_PAGE + index * 4, cases[index].instruction);
    }

    struct aarch64_runner runner;
    assert(aarch64_runner_init_backend(
            &runner, &fixture.tlb, AARCH64_BACKEND_THREADED));
    struct cpu_state cpu;
    init_differential_cpu(&cpu);
    aarch64_set_exclusive(&cpu,
            DATA_PAGE + 0x60, 8, false,
            UINT64_C(0x3344), 0, NULL, 4, 6, 8);

    for (unsigned index = 0; index < array_size(cases); index++) {
        struct aarch64_decoded decoded;
        assert(aarch64_decode(cases[index].instruction, &decoded));
        byte_t rn = decoded.operands.data_processing_3source.rn;
        byte_t rm = decoded.operands.data_processing_3source.rm;
        byte_t ra = decoded.operands.data_processing_3source.ra;
        byte_t rd = decoded.operands.data_processing_3source.rd;
        if (rn != 31)
            cpu.x[rn] = cases[index].rn_value;
        if (rm != 31)
            cpu.x[rm] = cases[index].rm_value;
        if (ra != 31)
            cpu.x[ra] = cases[index].ra_value;

        struct cpu_state expected_cpu = cpu;
        expected_cpu.pc = CODE_PAGE + (index + 1) * 4;
        expected_cpu.cycle++;
        if (rd != 31)
            expected_cpu.x[rd] = cases[index].expected;
        assert(run_at(&runner, &cpu,
                CODE_PAGE + index * 4).stop ==
                AARCH64_STEP_RETIRED);
        assert_cpu_equal(&cpu, &expected_cpu);
        assert(memcmp(fixture.memory.data,
                expected_data, sizeof(expected_data)) == 0);
        assert_stats(&runner, 0, index + 1, 0, index + 1);
    }
}

static void test_data_processing_2source_sibling_fallback(void) {
    struct test_fixture fixture;
    init_fixture(&fixture);
    const dword_t instructions[] = {
        INSTRUCTION_UDIV_W0_W1_W2,
        INSTRUCTION_SDIV_X9_X10_X11,
        INSTRUCTION_RORV_X0_X1_X2,
    };
    const enum aarch64_opcode opcodes[] = {
        AARCH64_OP_UDIV,
        AARCH64_OP_SDIV,
        AARCH64_OP_RORV,
    };
    _Static_assert(array_size(instructions) == array_size(opcodes),
            "寄存器双源 sibling 指令与 opcode 必须一一对应");
    for (unsigned index = 0; index < array_size(instructions); index++) {
        struct aarch64_decoded decoded;
        assert(aarch64_decode(instructions[index], &decoded));
        assert(decoded.opcode == opcodes[index]);
        write_instruction(&fixture.tlb,
                CODE_PAGE + index * 4, instructions[index]);
    }

    struct aarch64_runner runner;
    assert(aarch64_runner_init_backend(
            &runner, &fixture.tlb, AARCH64_BACKEND_THREADED));
    struct cpu_state cpu;
    init_differential_cpu(&cpu);
    aarch64_set_exclusive(&cpu,
            DATA_PAGE + 0x60, 8, false,
            UINT64_C(0x3344), 0, NULL, 4, 6, 8);
    qword_t expected_sp = cpu.sp;
    dword_t expected_nzcv = cpu.nzcv;
    dword_t expected_fpcr = cpu.fpcr;
    dword_t expected_fpsr = cpu.fpsr;

    cpu.x[1] = UINT64_C(0xffffffff00000064);
    cpu.x[2] = 7;
    assert(run_at(&runner, &cpu, CODE_PAGE).stop ==
            AARCH64_STEP_RETIRED);
    assert(cpu.x[0] == 14);

    cpu.x[10] = 100;
    cpu.x[11] = UINT64_C(0xfffffffffffffff9);
    assert(run_at(&runner, &cpu, CODE_PAGE + 4).stop ==
            AARCH64_STEP_RETIRED);
    assert(cpu.x[9] == UINT64_C(0xfffffffffffffff2));

    cpu.x[1] = UINT64_C(0x0123456789abcdef);
    cpu.x[2] = 8;
    assert(run_at(&runner, &cpu, CODE_PAGE + 8).stop ==
            AARCH64_STEP_RETIRED);
    assert(cpu.x[0] == UINT64_C(0xef0123456789abcd));

    assert(cpu.sp == expected_sp);
    assert(cpu.nzcv == expected_nzcv);
    assert(cpu.fpcr == expected_fpcr);
    assert(cpu.fpsr == expected_fpsr);
    assert(cpu.exclusive.valid);
    assert(cpu.exclusive.address == DATA_PAGE + 0x60);
    assert(cpu.exclusive.write_epoch == 6);
    assert(cpu.exclusive.sync_identity == 8);
    assert_stats(&runner, 0, array_size(instructions),
            0, array_size(instructions));
}

static void test_fast_extract_differential(void) {
    for (unsigned width_index = 0; width_index < 2; width_index++) {
        bool is_64 = width_index != 0;
        byte_t width = is_64 ? 64 : 32;
        for (byte_t lsb = 0; lsb < width; lsb++) {
            struct cpu_state initial;
            init_differential_cpu(&initial);
            initial.x[4] = UINT64_C(0x8123456789abcdef);
            initial.x[5] = UINT64_C(0xfedcba9886543211);
            struct cpu_state result = run_fast_differential(
                    encode_extract(is_64, lsb, 5, 4, 6),
                    initial, AARCH64_STEP_RETIRED);
            assert(result.pc == CODE_PAGE + 4);
            assert(result.cycle == initial.cycle + 1);
            assert(result.nzcv == initial.nzcv);
            assert(result.fpcr == initial.fpcr);
            assert(result.fpsr == initial.fpsr);
            assert(result.x[4] == initial.x[4]);
            assert(result.x[5] == initial.x[5]);
        }
    }

    struct cpu_state initial;
    init_differential_cpu(&initial);
    initial.x[1] = UINT64_C(0xffffffff89abcdef);
    aarch64_set_exclusive(&initial, DATA_PAGE + 0x60, 8, false,
            UINT64_C(0x3344), 0, NULL, 4, 6, 8);
    assert(encode_extract(false, 19, 1, 1, 3) ==
            INSTRUCTION_EXTR_W3_W1_W1_19);
    struct cpu_state result = run_fast_differential(
            INSTRUCTION_EXTR_W3_W1_W1_19,
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[3] == UINT64_C(0x0000000079bdf135));
    assert(result.x[1] == initial.x[1]);
    assert(result.sp == initial.sp);
    assert(result.nzcv == initial.nzcv);
    assert(result.fpcr == initial.fpcr);
    assert(result.fpsr == initial.fpsr);
    assert(result.exclusive.valid);
    assert(result.exclusive.address == DATA_PAGE + 0x60);
    assert(result.exclusive.write_epoch == 6);
    assert(result.exclusive.sync_identity == 8);

    init_differential_cpu(&initial);
    initial.x[1] = UINT64_C(0xffffffff89abcdef);
    assert(encode_extract(false, 29, 1, 1, 2) ==
            INSTRUCTION_EXTR_W2_W1_W1_29);
    result = run_fast_differential(
            INSTRUCTION_EXTR_W2_W1_W1_29,
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[2] == UINT64_C(0x000000004d5e6f7c));
    assert(result.x[1] == initial.x[1]);

    init_differential_cpu(&initial);
    initial.x[4] = UINT64_C(0x0123456789abcdef);
    initial.x[5] = UINT64_C(0xfedcba9876543210);
    result = run_fast_differential(
            encode_extract(true, 17, 5, 4, 6),
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[6] == UINT64_C(0xe6f7ff6e5d4c3b2a));
    assert(result.x[4] == initial.x[4]);
    assert(result.x[5] == initial.x[5]);

    init_differential_cpu(&initial);
    initial.x[4] = UINT64_C(0x0123456789abcdef);
    initial.x[5] = UINT64_C(0xfedcba9876543210);
    result = run_fast_differential(
            encode_extract(true, 17, 5, 4, 4),
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[4] == UINT64_C(0xe6f7ff6e5d4c3b2a));
    assert(result.x[5] == initial.x[5]);

    init_differential_cpu(&initial);
    initial.x[4] = UINT64_C(0x0123456789abcdef);
    initial.x[5] = UINT64_C(0xfedcba9876543210);
    result = run_fast_differential(
            encode_extract(true, 17, 5, 4, 5),
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[4] == initial.x[4]);
    assert(result.x[5] == UINT64_C(0xe6f7ff6e5d4c3b2a));

    init_differential_cpu(&initial);
    initial.x[5] = UINT64_C(0xfedcba9876543210);
    result = run_fast_differential(
            encode_extract(true, 8, 5, 31, 6),
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[6] == UINT64_C(0x00fedcba98765432));
    assert(result.x[5] == initial.x[5]);
    assert(result.sp == initial.sp);

    init_differential_cpu(&initial);
    initial.x[4] = UINT64_C(0x0123456789abcdef);
    result = run_fast_differential(
            encode_extract(true, 8, 31, 4, 6),
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[6] == UINT64_C(0xef00000000000000));
    assert(result.x[4] == initial.x[4]);
    assert(result.sp == initial.sp);

    init_differential_cpu(&initial);
    initial.x[4] = UINT64_C(0x0123456789abcdef);
    initial.x[5] = UINT64_C(0xfedcba9876543210);
    result = run_fast_differential(
            encode_extract(true, 17, 5, 4, 31),
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[4] == initial.x[4]);
    assert(result.x[5] == initial.x[5]);
    assert(result.sp == initial.sp);
}

static void test_extract_invalid_encodings(void) {
    const dword_t instructions[] = {
        UINT32_C(0x13828020),
        UINT32_C(0x13c20020),
        UINT32_C(0x93820020),
    };
    struct test_fixture c_fixture;
    struct test_fixture threaded_fixture;
    init_fixture(&c_fixture);
    init_fixture(&threaded_fixture);
    for (unsigned index = 0; index < array_size(instructions); index++) {
        struct aarch64_decoded decoded;
        assert(!aarch64_decode(instructions[index], &decoded));
        write_instruction(&c_fixture.tlb, CODE_PAGE + index * 4,
                instructions[index]);
        write_instruction(&threaded_fixture.tlb, CODE_PAGE + index * 4,
                instructions[index]);
    }

    struct aarch64_runner c_runner;
    struct aarch64_runner threaded_runner;
    assert(aarch64_runner_init_backend(
            &c_runner, &c_fixture.tlb, AARCH64_BACKEND_C));
    assert(aarch64_runner_init_backend(&threaded_runner,
            &threaded_fixture.tlb, AARCH64_BACKEND_THREADED));
    struct cpu_state c_cpu;
    struct cpu_state threaded_cpu;
    init_differential_cpu(&c_cpu);
    init_differential_cpu(&threaded_cpu);
    for (unsigned index = 0; index < array_size(instructions); index++) {
        qword_t cycle = c_cpu.cycle;
        struct aarch64_step_result c_result =
                run_at(&c_runner, &c_cpu, CODE_PAGE + index * 4);
        struct aarch64_step_result threaded_result =
                run_at(&threaded_runner, &threaded_cpu,
                        CODE_PAGE + index * 4);
        assert(c_result.stop == AARCH64_STEP_UNDEFINED);
        assert_step_equal(&c_result, &threaded_result);
        assert_cpu_equal(&c_cpu, &threaded_cpu);
        assert_memory_equal(&c_fixture.memory,
                &threaded_fixture.memory);
        assert(c_cpu.pc == CODE_PAGE + index * 4);
        assert(c_cpu.cycle == cycle);
        assert_stats(&threaded_runner, 0, index + 1, 0, 0);
    }
}

static void test_fast_orr_shifted_differential(void) {
    const enum aarch64_shift_type shift_types[] = {
        AARCH64_SHIFT_LSL,
        AARCH64_SHIFT_LSR,
        AARCH64_SHIFT_ASR,
        AARCH64_SHIFT_ROR,
    };
    for (unsigned width_index = 0; width_index < 2; width_index++) {
        bool is_64 = width_index != 0;
        byte_t width = is_64 ? 64 : 32;
        const byte_t shifts[] = {0, 1, (byte_t) (width - 1)};
        for (unsigned type_index = 0;
                type_index < array_size(shift_types); type_index++) {
            for (unsigned shift_index = 0;
                    shift_index < array_size(shifts); shift_index++) {
                for (unsigned invert = 0; invert < 2; invert++) {
                    struct cpu_state initial;
                    init_differential_cpu(&initial);
                    initial.x[4] = UINT64_C(0x8123456789abcdef);
                    initial.x[5] = UINT64_C(0xfedcba9876543210);
                    struct cpu_state result = run_fast_differential(
                            encode_orr_shifted(is_64, invert != 0,
                                    shift_types[type_index],
                                    shifts[shift_index], 5, 4, 6),
                            initial, AARCH64_STEP_RETIRED);
                    assert(result.pc == CODE_PAGE + 4);
                    assert(result.cycle == initial.cycle + 1);
                    assert(result.nzcv == initial.nzcv);
                    assert(result.x[4] == initial.x[4]);
                    assert(result.x[5] == initial.x[5]);
                }
            }
        }
    }

    struct cpu_state initial;
    init_differential_cpu(&initial);
    initial.x[0] = UINT64_C(0x0123456789abcdef);
    aarch64_set_exclusive(&initial, DATA_PAGE + 0x40, 8, false,
            UINT64_C(0x1122), 0, NULL, 3, 5, 7);
    assert(encode_orr_shifted(true, false, AARCH64_SHIFT_LSL,
            0, 0, 31, 3) == INSTRUCTION_ORR_X3_XZR_X0);
    struct cpu_state result = run_fast_differential(
            INSTRUCTION_ORR_X3_XZR_X0,
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[3] == UINT64_C(0x0123456789abcdef));
    assert(result.sp == initial.sp);
    assert(result.nzcv == initial.nzcv);
    assert(result.fpcr == initial.fpcr);
    assert(result.fpsr == initial.fpsr);
    assert(result.exclusive.valid);
    assert(result.exclusive.address == DATA_PAGE + 0x40);
    assert(result.exclusive.write_epoch == 5);
    assert(result.exclusive.sync_identity == 7);

    init_differential_cpu(&initial);
    initial.x[3] = UINT64_C(0x0123456789abcdef);
    result = run_fast_differential(encode_orr_shifted(
            true, true, AARCH64_SHIFT_ROR, 8, 3, 31, 2),
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[2] == UINT64_C(0x10fedcba98765432));
    assert(result.nzcv == initial.nzcv);

    init_differential_cpu(&initial);
    initial.x[4] = 1;
    initial.x[5] = 2;
    result = run_fast_differential(encode_orr_shifted(
            true, false, AARCH64_SHIFT_LSL, 4, 5, 4, 4),
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[4] == UINT64_C(0x21));

    init_differential_cpu(&initial);
    initial.x[4] = UINT64_C(0x80);
    initial.x[5] = 3;
    result = run_fast_differential(encode_orr_shifted(
            true, false, AARCH64_SHIFT_LSR, 1, 5, 4, 5),
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[5] == UINT64_C(0x81));

    init_differential_cpu(&initial);
    initial.x[4] = 1;
    initial.x[5] = UINT64_C(0xffffffff80000000);
    result = run_fast_differential(encode_orr_shifted(
            false, false, AARCH64_SHIFT_LSR, 31, 5, 4, 5),
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[5] == 1);

    init_differential_cpu(&initial);
    initial.x[5] = UINT64_C(0x8000000000000000);
    result = run_fast_differential(encode_orr_shifted(
            true, false, AARCH64_SHIFT_ASR, 63, 5, 31, 6),
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[6] == UINT64_MAX);

    init_differential_cpu(&initial);
    initial.x[4] = UINT64_C(0x123456789abcdef0);
    result = run_fast_differential(encode_orr_shifted(
            true, false, AARCH64_SHIFT_LSL, 0, 31, 4, 31),
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[4] == initial.x[4]);
    assert(result.sp == initial.sp);

    init_differential_cpu(&initial);
    result = run_fast_differential(encode_orr_shifted(
            false, true, AARCH64_SHIFT_ROR, 0, 31, 31, 6),
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[6] == UINT32_MAX);
}

static void test_fast_eor_shifted_differential(void) {
    const enum aarch64_shift_type shift_types[] = {
        AARCH64_SHIFT_LSL,
        AARCH64_SHIFT_LSR,
        AARCH64_SHIFT_ASR,
        AARCH64_SHIFT_ROR,
    };
    for (unsigned width_index = 0; width_index < 2; width_index++) {
        bool is_64 = width_index != 0;
        byte_t width = is_64 ? 64 : 32;
        const byte_t shifts[] = {0, 1, (byte_t) (width - 1)};
        for (unsigned type_index = 0;
                type_index < array_size(shift_types); type_index++) {
            for (unsigned shift_index = 0;
                    shift_index < array_size(shifts); shift_index++) {
                for (unsigned invert = 0; invert < 2; invert++) {
                    struct cpu_state initial;
                    init_differential_cpu(&initial);
                    initial.x[4] = UINT64_C(0x8123456789abcdef);
                    initial.x[5] = UINT64_C(0xfedcba9886543211);
                    struct cpu_state result = run_fast_differential(
                            encode_eor_shifted(is_64, invert != 0,
                                    shift_types[type_index],
                                    shifts[shift_index], 5, 4, 6),
                            initial, AARCH64_STEP_RETIRED);
                    assert(result.pc == CODE_PAGE + 4);
                    assert(result.cycle == initial.cycle + 1);
                    assert(result.nzcv == initial.nzcv);
                    assert(result.fpcr == initial.fpcr);
                    assert(result.fpsr == initial.fpsr);
                    assert(result.x[4] == initial.x[4]);
                    assert(result.x[5] == initial.x[5]);
                }
            }
        }
    }

    struct cpu_state initial;
    init_differential_cpu(&initial);
    initial.x[3] = UINT64_C(0xf0f0f0f0f0f0f0f0);
    initial.x[6] = UINT64_C(0xff00ff00ff00ff00);
    aarch64_set_exclusive(&initial, DATA_PAGE + 0x40, 8, false,
            UINT64_C(0x1122), 0, NULL, 3, 5, 7);
    assert(encode_eor_shifted(true, false, AARCH64_SHIFT_LSL,
            0, 6, 3, 5) == INSTRUCTION_EOR_X5_X3_X6);
    struct cpu_state result = run_fast_differential(
            INSTRUCTION_EOR_X5_X3_X6,
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[5] == UINT64_C(0x0ff00ff00ff00ff0));
    assert(result.sp == initial.sp);
    assert(result.nzcv == initial.nzcv);
    assert(result.fpcr == initial.fpcr);
    assert(result.fpsr == initial.fpsr);
    assert(result.exclusive.valid);
    assert(result.exclusive.address == DATA_PAGE + 0x40);
    assert(result.exclusive.write_epoch == 5);
    assert(result.exclusive.sync_identity == 7);

    init_differential_cpu(&initial);
    initial.x[4] = 1;
    result = run_fast_differential(encode_eor_shifted(
            true, true, AARCH64_SHIFT_LSL, 0, 31, 4, 6),
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[6] == UINT64_MAX - 1);

    init_differential_cpu(&initial);
    initial.x[4] = UINT64_C(0x123456789abcdef0);
    result = run_fast_differential(encode_eor_shifted(
            true, false, AARCH64_SHIFT_LSL, 0, 4, 4, 6),
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[6] == 0);

    init_differential_cpu(&initial);
    initial.x[4] = UINT64_C(0xff);
    initial.x[5] = UINT64_C(0x0f);
    result = run_fast_differential(encode_eor_shifted(
            true, false, AARCH64_SHIFT_LSL, 0, 5, 4, 4),
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[4] == UINT64_C(0xf0));

    init_differential_cpu(&initial);
    initial.x[4] = UINT64_C(0xff);
    initial.x[5] = UINT64_C(0x0f);
    result = run_fast_differential(encode_eor_shifted(
            true, false, AARCH64_SHIFT_LSL, 0, 5, 4, 5),
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[5] == UINT64_C(0xf0));

    init_differential_cpu(&initial);
    initial.x[4] = UINT64_C(0xaaaaaaaaffff0000);
    initial.x[5] = UINT64_C(0x5555555500ff00ff);
    initial.x[6] = UINT64_MAX;
    result = run_fast_differential(encode_eor_shifted(
            false, false, AARCH64_SHIFT_LSL, 0, 5, 4, 6),
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[6] == UINT64_C(0x00000000ff0000ff));

    init_differential_cpu(&initial);
    initial.x[5] = UINT64_C(0x123456789abcdef0);
    result = run_fast_differential(encode_eor_shifted(
            true, false, AARCH64_SHIFT_LSL, 0, 5, 31, 6),
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[6] == initial.x[5]);
    assert(result.sp == initial.sp);

    init_differential_cpu(&initial);
    initial.x[4] = UINT64_C(0x123456789abcdef0);
    result = run_fast_differential(encode_eor_shifted(
            true, false, AARCH64_SHIFT_ROR, 17, 31, 4, 6),
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[6] == initial.x[4]);
    assert(result.sp == initial.sp);

    init_differential_cpu(&initial);
    initial.x[4] = UINT64_C(0x123456789abcdef0);
    initial.x[5] = UINT64_C(0x0fedcba987654321);
    result = run_fast_differential(encode_eor_shifted(
            true, false, AARCH64_SHIFT_LSL, 4, 5, 4, 31),
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[4] == initial.x[4]);
    assert(result.x[5] == initial.x[5]);
    assert(result.sp == initial.sp);
}

static void test_fast_ands_shifted_differential(void) {
    const enum aarch64_shift_type shift_types[] = {
        AARCH64_SHIFT_LSL,
        AARCH64_SHIFT_LSR,
        AARCH64_SHIFT_ASR,
        AARCH64_SHIFT_ROR,
    };
    for (unsigned width_index = 0; width_index < 2; width_index++) {
        bool is_64 = width_index != 0;
        byte_t width = is_64 ? 64 : 32;
        qword_t mask = is_64 ? UINT64_MAX : UINT32_MAX;
        const byte_t shifts[] = {0, 1, (byte_t) (width - 1)};
        for (unsigned type_index = 0;
                type_index < array_size(shift_types); type_index++) {
            for (unsigned shift_index = 0;
                    shift_index < array_size(shifts); shift_index++) {
                for (unsigned invert = 0; invert < 2; invert++) {
                    struct cpu_state initial;
                    init_differential_cpu(&initial);
                    initial.x[4] = UINT64_C(0x8123456789abcdef);
                    initial.x[5] = UINT64_C(0xfedcba9886543211);
                    initial.x[6] = UINT64_MAX;
                    initial.nzcv = UINT32_C(0xf0000000);

                    byte_t shift = shifts[shift_index];
                    qword_t left = initial.x[4] & mask;
                    qword_t right = initial.x[5] & mask;
                    if (shift != 0) {
                        if (shift_types[type_index] ==
                                AARCH64_SHIFT_LSL) {
                            right = right << shift & mask;
                        } else if (shift_types[type_index] ==
                                AARCH64_SHIFT_LSR) {
                            right >>= shift;
                        } else if (shift_types[type_index] ==
                                AARCH64_SHIFT_ASR) {
                            qword_t sign =
                                    UINT64_C(1) << (width - 1);
                            right >>= shift;
                            if (initial.x[5] & sign)
                                right |= mask << (width - shift);
                        } else {
                            right = (right >> shift |
                                    right << (width - shift)) & mask;
                        }
                    }
                    if (invert != 0)
                        right = ~right & mask;
                    qword_t expected = left & right;
                    qword_t sign = UINT64_C(1) << (width - 1);
                    dword_t expected_nzcv =
                            (expected & sign ?
                                    UINT32_C(1) << 31 : 0) |
                            (expected == 0 ?
                                    UINT32_C(1) << 30 : 0);

                    struct cpu_state result = run_fast_differential(
                            encode_ands_shifted(is_64, invert != 0,
                                    shift_types[type_index], shift,
                                    5, 4, 6),
                            initial, AARCH64_STEP_RETIRED);
                    assert(result.x[6] == expected);
                    assert(result.x[4] == initial.x[4]);
                    assert(result.x[5] == initial.x[5]);
                    assert(result.sp == initial.sp);
                    assert(result.pc == CODE_PAGE + 4);
                    assert(result.cycle == initial.cycle + 1);
                    assert(result.nzcv == expected_nzcv);
                    assert(result.fpcr == initial.fpcr);
                    assert(result.fpsr == initial.fpsr);
                }
            }
        }
    }

    struct cpu_state initial;
    init_differential_cpu(&initial);
    initial.x[0] = UINT64_C(0x8000000000000000);
    initial.x[5] = UINT64_MAX;
    initial.nzcv = UINT32_C(0xf0000000);
    aarch64_set_exclusive(&initial, DATA_PAGE + 0x40, 8, false,
            UINT64_C(0x1122), 0, NULL, 3, 5, 7);
    assert(encode_ands_shifted(true, false, AARCH64_SHIFT_LSL,
            0, 5, 0, 31) == INSTRUCTION_ANDS_XZR_X0_X5);
    struct cpu_state result = run_fast_differential(
            INSTRUCTION_ANDS_XZR_X0_X5,
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[0] == initial.x[0]);
    assert(result.x[5] == initial.x[5]);
    assert(result.sp == initial.sp);
    assert(result.nzcv == UINT32_C(0x80000000));
    assert(result.exclusive.valid);
    assert(result.exclusive.address == DATA_PAGE + 0x40);
    assert(result.exclusive.write_epoch == 5);
    assert(result.exclusive.sync_identity == 7);

    init_differential_cpu(&initial);
    initial.x[0] = UINT64_C(0x8000000000000000);
    initial.x[2] = UINT64_MAX;
    initial.nzcv = UINT32_C(0xf0000000);
    assert(encode_ands_shifted(false, false, AARCH64_SHIFT_LSL,
            0, 2, 0, 31) == INSTRUCTION_ANDS_WZR_W0_W2);
    result = run_fast_differential(
            INSTRUCTION_ANDS_WZR_W0_W2,
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[0] == initial.x[0]);
    assert(result.x[2] == initial.x[2]);
    assert(result.sp == initial.sp);
    assert(result.nzcv == UINT32_C(0x40000000));

    init_differential_cpu(&initial);
    initial.x[1] = UINT64_C(0x55aa55aa55aa55aa);
    initial.x[2] = UINT64_C(0xff00ff00ff00ff00);
    initial.x[0] = UINT64_MAX;
    initial.nzcv = UINT32_C(0xf0000000);
    assert(encode_ands_shifted(true, true, AARCH64_SHIFT_LSL,
            0, 2, 1, 0) == UINT32_C(0xea220020));
    result = run_fast_differential(UINT32_C(0xea220020),
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[0] == UINT64_C(0x00aa00aa00aa00aa));
    assert(result.x[1] == initial.x[1]);
    assert(result.x[2] == initial.x[2]);
    assert(result.nzcv == 0);

    init_differential_cpu(&initial);
    initial.x[1] = UINT64_C(0x55aa55aa55aa55aa);
    initial.x[2] = UINT64_C(0xff00ff00ff00ff00);
    initial.x[0] = UINT64_MAX;
    initial.nzcv = UINT32_C(0xf0000000);
    assert(encode_ands_shifted(true, false, AARCH64_SHIFT_LSL,
            0, 2, 1, 0) == UINT32_C(0xea020020));
    result = run_fast_differential(UINT32_C(0xea020020),
            initial, AARCH64_STEP_RETIRED);
    assert(result.x[0] == UINT64_C(0x5500550055005500));
    assert(result.x[1] == initial.x[1]);
    assert(result.x[2] == initial.x[2]);
    assert(result.nzcv == 0);
}

static void test_fast_load_imm12_differential(void) {
    const struct {
        dword_t instruction;
        byte_t size;
        byte_t rn;
        byte_t rt;
        word_t offset;
        qword_t source;
        qword_t expected;
    } cases[] = {
        {UINT32_C(0x39401ca4), 1, 5, 4, 7,
                UINT64_C(0x80), UINT64_C(0x80)},
        {UINT32_C(0x79400ce6), 2, 7, 6, 6,
                UINT64_C(0x8001), UINT64_C(0x8001)},
        {UINT32_C(0xb9400c62), 4, 3, 2, 12,
                UINT64_C(0x80000003), UINT64_C(0x80000003)},
        {UINT32_C(0xf9400be9), 8, 31, 9, 16,
                UINT64_C(0x8877665544332211),
                UINT64_C(0x8877665544332211)},
        {UINT32_C(0x39c0156a), 1, 11, 10, 5,
                UINT64_C(0x80), UINT64_C(0x00000000ffffff80)},
        {UINT32_C(0x39800d49), 1, 10, 9, 3,
                UINT64_C(0x80), UINT64_C(0xffffffffffffff80)},
        {UINT32_C(0x79c0098b), 2, 12, 11, 4,
                UINT64_C(0x8001), UINT64_C(0x00000000ffff8001)},
        {UINT32_C(0x79800dac), 2, 13, 12, 6,
                UINT64_C(0x8001), UINT64_C(0xffffffffffff8001)},
        {UINT32_C(0xb98009cd), 4, 14, 13, 8,
                UINT64_C(0x80000003), UINT64_C(0xffffffff80000003)},
    };
    for (unsigned index = 0; index < array_size(cases); index++) {
        struct test_fixture c_fixture;
        struct test_fixture threaded_fixture;
        init_fixture(&c_fixture);
        init_fixture(&threaded_fixture);
        size_t data_offset = 0x100 + cases[index].offset;
        put_value(c_fixture.memory.data + data_offset,
                cases[index].size, cases[index].source);
        put_value(threaded_fixture.memory.data + data_offset,
                cases[index].size, cases[index].source);
        write_instruction(&c_fixture.tlb,
                CODE_PAGE, cases[index].instruction);
        write_instruction(&threaded_fixture.tlb,
                CODE_PAGE, cases[index].instruction);

        struct cpu_state initial;
        init_differential_cpu(&initial);
        initial.x[cases[index].rt] = UINT64_MAX;
        qword_t base = DATA_PAGE + 0x100;
        if (cases[index].rn == 31)
            initial.sp = base;
        else
            initial.x[cases[index].rn] = base;
        aarch64_set_exclusive(&initial, DATA_PAGE + 0x300, 8, false,
                UINT64_C(0x1122), 0, NULL, 3, 5, 7);

        struct cpu_state result = run_fast_differential_fixtures(
                cases[index].instruction, initial,
                AARCH64_STEP_RETIRED,
                &c_fixture, &threaded_fixture, NULL);
        assert(result.x[cases[index].rt] == cases[index].expected);
        if (cases[index].rn == 31)
            assert(result.sp == base);
        else
            assert(result.x[cases[index].rn] == base);
        assert(result.pc == CODE_PAGE + 4);
        assert(result.cycle == initial.cycle + 1);
        assert(result.exclusive.valid);
        assert(result.exclusive.write_epoch == 5);
        assert(result.exclusive.sync_identity == 7);
    }

    struct test_fixture c_fixture;
    struct test_fixture threaded_fixture;
    init_fixture(&c_fixture);
    init_fixture(&threaded_fixture);
    const dword_t maximum_offset = UINT32_C(0xf97ffc20);
    const size_t target_offset = 0x180;
    const qword_t maximum_value = UINT64_C(0x0123456789abcdef);
    put_value(c_fixture.memory.data + target_offset, 8, maximum_value);
    put_value(threaded_fixture.memory.data + target_offset,
            8, maximum_value);
    write_instruction(&c_fixture.tlb, CODE_PAGE, maximum_offset);
    write_instruction(&threaded_fixture.tlb, CODE_PAGE, maximum_offset);
    struct cpu_state initial;
    init_differential_cpu(&initial);
    initial.x[0] = UINT64_MAX;
    initial.x[1] = DATA_PAGE + target_offset - UINT64_C(4095) * 8;
    struct cpu_state result = run_fast_differential_fixtures(
            maximum_offset, initial, AARCH64_STEP_RETIRED,
            &c_fixture, &threaded_fixture, NULL);
    assert(result.x[0] == maximum_value);
    assert(result.x[1] == initial.x[1]);

    init_fixture(&c_fixture);
    init_fixture(&threaded_fixture);
    const dword_t cross_page_alias = UINT32_C(0xf9400021);
    put_value(c_fixture.memory.primary_code +
            GUEST_MEMORY_PAGE_SIZE - 4, 4, UINT32_C(0x44332211));
    put_value(threaded_fixture.memory.primary_code +
            GUEST_MEMORY_PAGE_SIZE - 4, 4, UINT32_C(0x44332211));
    put_value(c_fixture.memory.data, 4, UINT32_C(0x88776655));
    put_value(threaded_fixture.memory.data, 4, UINT32_C(0x88776655));
    write_instruction(&c_fixture.tlb, CODE_PAGE, cross_page_alias);
    write_instruction(&threaded_fixture.tlb,
            CODE_PAGE, cross_page_alias);
    init_differential_cpu(&initial);
    initial.x[1] = DATA_PAGE - 4;
    result = run_fast_differential_fixtures(
            cross_page_alias, initial, AARCH64_STEP_RETIRED,
            &c_fixture, &threaded_fixture, NULL);
    assert(result.x[1] == UINT64_C(0x8877665544332211));
}

static void test_fast_load_imm12_faults(void) {
    struct test_fixture c_fixture;
    struct test_fixture threaded_fixture;
    init_fixture(&c_fixture);
    init_fixture(&threaded_fixture);
    const dword_t xzr_unmapped = UINT32_C(0xf940015f);
    write_instruction(&c_fixture.tlb, CODE_PAGE, xzr_unmapped);
    write_instruction(&threaded_fixture.tlb, CODE_PAGE, xzr_unmapped);
    struct cpu_state initial;
    init_differential_cpu(&initial);
    initial.x[10] = DATA_PAGE + GUEST_MEMORY_PAGE_SIZE;
    aarch64_set_exclusive(&initial, DATA_PAGE + 0x300, 8, false,
            UINT64_C(0x1122), 0, NULL, 3, 5, 7);
    struct aarch64_step_result step;
    struct cpu_state result = run_fast_differential_fixtures(
            xzr_unmapped, initial, AARCH64_STEP_DATA_FAULT,
            &c_fixture, &threaded_fixture, &step);
    assert(step.fault.address == initial.x[10]);
    assert(step.fault.access == GUEST_MEMORY_READ);
    assert(step.fault.kind == GUEST_MEMORY_FAULT_UNMAPPED);
    assert(result.x[10] == initial.x[10]);
    assert(result.pc == CODE_PAGE);
    assert(result.cycle == initial.cycle);
    assert(!result.exclusive.valid);

    init_fixture(&c_fixture);
    init_fixture(&threaded_fixture);
    write_instruction(&c_fixture.tlb, CODE_PAGE, INSTRUCTION_LDR_X2);
    write_instruction(&threaded_fixture.tlb,
            CODE_PAGE, INSTRUCTION_LDR_X2);
    c_fixture.memory.code_permissions = GUEST_MEMORY_EXECUTE;
    threaded_fixture.memory.code_permissions = GUEST_MEMORY_EXECUTE;
    guest_address_space_changed(&c_fixture.space);
    guest_address_space_changed(&threaded_fixture.space);
    init_differential_cpu(&initial);
    initial.x[1] = CODE_PAGE + 0x100;
    initial.x[2] = UINT64_C(0x123456789abcdef0);
    aarch64_set_exclusive(&initial, DATA_PAGE + 0x300, 8, false,
            UINT64_C(0x1122), 0, NULL, 3, 5, 7);
    result = run_fast_differential_fixtures(
            INSTRUCTION_LDR_X2, initial, AARCH64_STEP_DATA_FAULT,
            &c_fixture, &threaded_fixture, &step);
    assert(step.fault.address == initial.x[1]);
    assert(step.fault.access == GUEST_MEMORY_READ);
    assert(step.fault.kind == GUEST_MEMORY_FAULT_PERMISSION);
    assert(result.x[2] == initial.x[2]);
    assert(result.pc == CODE_PAGE);
    assert(result.cycle == initial.cycle);
    assert(!result.exclusive.valid);
}

static void test_fast_load_imm9_differential(void) {
    const struct {
        byte_t size_shift;
        byte_t operation;
        byte_t width;
        bool signed_load;
        qword_t source;
        qword_t expected;
    } transfers[] = {
        {0, 1, 32, false, UINT64_C(0x80), UINT64_C(0x80)},
        {0, 2, 64, true, UINT64_C(0x80),
                UINT64_C(0xffffffffffffff80)},
        {0, 3, 32, true, UINT64_C(0x80),
                UINT64_C(0x00000000ffffff80)},
        {1, 1, 32, false, UINT64_C(0x8001), UINT64_C(0x8001)},
        {1, 2, 64, true, UINT64_C(0x8001),
                UINT64_C(0xffffffffffff8001)},
        {1, 3, 32, true, UINT64_C(0x8001),
                UINT64_C(0x00000000ffff8001)},
        {2, 1, 32, false, UINT64_C(0x80000003),
                UINT64_C(0x0000000080000003)},
        {2, 2, 64, true, UINT64_C(0x80000003),
                UINT64_C(0xffffffff80000003)},
        {3, 1, 64, false, UINT64_C(0x8877665544332211),
                UINT64_C(0x8877665544332211)},
    };
    const enum aarch64_address_mode address_modes[] = {
        AARCH64_ADDRESS_OFFSET,
        AARCH64_ADDRESS_POST_INDEX,
        AARCH64_ADDRESS_PRE_INDEX,
    };
    const int64_t offsets[] = {-256, 0, 255};
    const size_t target_offset = 0x300;
    unsigned case_count = 0;

    for (unsigned transfer_index = 0;
            transfer_index < array_size(transfers); transfer_index++) {
        byte_t size = (byte_t) (
                1U << transfers[transfer_index].size_shift);
        for (unsigned mode_index = 0;
                mode_index < array_size(address_modes); mode_index++) {
            for (unsigned offset_index = 0;
                    offset_index < array_size(offsets); offset_index++) {
                int64_t offset = offsets[offset_index];
                enum aarch64_address_mode address_mode =
                        address_modes[mode_index];
                dword_t instruction = encode_scalar_load_imm9(
                        transfers[transfer_index].size_shift,
                        transfers[transfer_index].operation,
                        address_mode, offset, 5, 4);
                struct aarch64_decoded decoded;
                assert(aarch64_decode(instruction, &decoded));
                assert(decoded.opcode == AARCH64_OP_LOAD_IMM9);
                assert(decoded.width == transfers[transfer_index].width);
                assert(decoded.operands.load_store.size == size);
                assert(decoded.operands.load_store.offset == offset);
                assert(decoded.operands.load_store.address_mode ==
                        address_mode);
                assert(decoded.operands.load_store.signed_load ==
                        transfers[transfer_index].signed_load);

                struct test_fixture c_fixture;
                struct test_fixture threaded_fixture;
                init_fixture(&c_fixture);
                init_fixture(&threaded_fixture);
                put_value(c_fixture.memory.data + target_offset,
                        size, transfers[transfer_index].source);
                put_value(threaded_fixture.memory.data + target_offset,
                        size, transfers[transfer_index].source);
                write_instruction(&c_fixture.tlb, CODE_PAGE, instruction);
                write_instruction(&threaded_fixture.tlb,
                        CODE_PAGE, instruction);

                struct cpu_state initial;
                init_differential_cpu(&initial);
                initial.x[4] = UINT64_MAX;
                guest_addr_t base = DATA_PAGE + target_offset;
                if (address_mode != AARCH64_ADDRESS_POST_INDEX)
                    base -= (qword_t) offset;
                initial.x[5] = base;
                aarch64_set_exclusive(&initial,
                        DATA_PAGE + 0x40, 8, false,
                        UINT64_C(0x1122), 0, NULL, 3, 5, 7);

                struct cpu_state result =
                        run_fast_differential_fixtures(
                                instruction, initial,
                                AARCH64_STEP_RETIRED,
                                &c_fixture, &threaded_fixture, NULL);
                guest_addr_t expected_base =
                        address_mode == AARCH64_ADDRESS_OFFSET ?
                        base : base + (qword_t) offset;
                assert(result.x[4] ==
                        transfers[transfer_index].expected);
                assert(result.x[5] == expected_base);
                assert(result.pc == CODE_PAGE + 4);
                assert(result.cycle == initial.cycle + 1);
                assert(result.nzcv == initial.nzcv);
                assert(result.fpcr == initial.fpcr);
                assert(result.fpsr == initial.fpsr);
                assert(result.exclusive.valid);
                assert(result.exclusive.write_epoch == 5);
                assert(result.exclusive.sync_identity == 7);
                case_count++;
            }
        }
    }
    assert(case_count == 81);

    assert(encode_scalar_load_imm9(3, 1,
            AARCH64_ADDRESS_POST_INDEX, 8, 3, 2) ==
            INSTRUCTION_LDR_X2_X3_POST_8);
    assert(encode_scalar_load_imm9(0, 1,
            AARCH64_ADDRESS_OFFSET, -4, 0, 1) ==
            INSTRUCTION_LDURB_W1_X0_NEG_4);
    const struct {
        dword_t instruction;
        byte_t size;
        size_t target_offset;
        byte_t rn;
        byte_t rt;
        guest_addr_t base;
        guest_addr_t expected_base;
        qword_t source;
        qword_t expected;
    } profile_cases[] = {
        {
            INSTRUCTION_LDR_X2_X3_POST_8, 8, 0x100, 3, 2,
            DATA_PAGE + 0x100, DATA_PAGE + 0x108,
            UINT64_C(0x0123456789abcdef),
            UINT64_C(0x0123456789abcdef),
        },
        {
            INSTRUCTION_LDURB_W1_X0_NEG_4, 1, 0x40, 0, 1,
            DATA_PAGE + 0x44, DATA_PAGE + 0x44,
            UINT64_C(0x5a), UINT64_C(0x5a),
        },
    };
    for (unsigned index = 0;
            index < array_size(profile_cases); index++) {
        struct test_fixture c_fixture;
        struct test_fixture threaded_fixture;
        init_fixture(&c_fixture);
        init_fixture(&threaded_fixture);
        put_value(c_fixture.memory.data +
                profile_cases[index].target_offset,
                profile_cases[index].size,
                profile_cases[index].source);
        put_value(threaded_fixture.memory.data +
                profile_cases[index].target_offset,
                profile_cases[index].size,
                profile_cases[index].source);
        write_instruction(&c_fixture.tlb, CODE_PAGE,
                profile_cases[index].instruction);
        write_instruction(&threaded_fixture.tlb, CODE_PAGE,
                profile_cases[index].instruction);
        struct cpu_state initial;
        init_differential_cpu(&initial);
        initial.x[profile_cases[index].rn] =
                profile_cases[index].base;
        initial.x[profile_cases[index].rt] = UINT64_MAX;
        struct cpu_state result = run_fast_differential_fixtures(
                profile_cases[index].instruction, initial,
                AARCH64_STEP_RETIRED,
                &c_fixture, &threaded_fixture, NULL);
        assert(result.x[profile_cases[index].rt] ==
                profile_cases[index].expected);
        assert(result.x[profile_cases[index].rn] ==
                profile_cases[index].expected_base);
    }
}

static void test_fast_load_imm9_aliases(void) {
    const enum aarch64_address_mode address_modes[] = {
        AARCH64_ADDRESS_OFFSET,
        AARCH64_ADDRESS_POST_INDEX,
        AARCH64_ADDRESS_PRE_INDEX,
    };
    const int64_t offsets[] = {-8, 8, -8};
    const guest_addr_t bases[] = {
        DATA_PAGE + 0x208,
        DATA_PAGE + 0x200,
        DATA_PAGE + 0x208,
    };
    const guest_addr_t expected_bases[] = {
        DATA_PAGE + 0x208,
        DATA_PAGE + 0x208,
        DATA_PAGE + 0x200,
    };
    const qword_t value = UINT64_C(0x0123456789abcdef);
    unsigned case_count = 0;

    for (unsigned index = 0; index < array_size(address_modes); index++) {
        struct test_fixture c_fixture;
        struct test_fixture threaded_fixture;
        init_fixture(&c_fixture);
        init_fixture(&threaded_fixture);
        put_value(c_fixture.memory.data + 0x200, 8, value);
        put_value(threaded_fixture.memory.data + 0x200, 8, value);
        dword_t instruction = encode_scalar_load_imm9(
                3, 1, address_modes[index], offsets[index], 31, 4);
        write_instruction(&c_fixture.tlb, CODE_PAGE, instruction);
        write_instruction(&threaded_fixture.tlb, CODE_PAGE, instruction);
        struct cpu_state initial;
        init_differential_cpu(&initial);
        initial.sp = bases[index];
        initial.x[4] = UINT64_MAX;
        struct cpu_state result = run_fast_differential_fixtures(
                instruction, initial, AARCH64_STEP_RETIRED,
                &c_fixture, &threaded_fixture, NULL);
        assert(result.x[4] == value);
        assert(result.sp == expected_bases[index]);
        case_count++;
    }

    for (unsigned index = 0; index < array_size(address_modes); index++) {
        struct test_fixture c_fixture;
        struct test_fixture threaded_fixture;
        init_fixture(&c_fixture);
        init_fixture(&threaded_fixture);
        put_value(c_fixture.memory.data + 0x200, 8, value);
        put_value(threaded_fixture.memory.data + 0x200, 8, value);
        dword_t instruction = encode_scalar_load_imm9(
                3, 1, address_modes[index], offsets[index], 5, 31);
        write_instruction(&c_fixture.tlb, CODE_PAGE, instruction);
        write_instruction(&threaded_fixture.tlb, CODE_PAGE, instruction);
        struct cpu_state initial;
        init_differential_cpu(&initial);
        initial.x[5] = bases[index];
        qword_t registers[array_size(initial.x)];
        memcpy(registers, initial.x, sizeof(registers));
        struct cpu_state result = run_fast_differential_fixtures(
                instruction, initial, AARCH64_STEP_RETIRED,
                &c_fixture, &threaded_fixture, NULL);
        for (unsigned reg = 0; reg < array_size(result.x); reg++) {
            qword_t expected = reg == 5 ?
                    expected_bases[index] : registers[reg];
            assert(result.x[reg] == expected);
        }
        case_count++;
    }

    {
        struct test_fixture c_fixture;
        struct test_fixture threaded_fixture;
        init_fixture(&c_fixture);
        init_fixture(&threaded_fixture);
        put_value(c_fixture.memory.data + 0x200, 8, value);
        put_value(threaded_fixture.memory.data + 0x200, 8, value);
        dword_t instruction = encode_scalar_load_imm9(
                3, 1, AARCH64_ADDRESS_OFFSET, -8, 5, 5);
        write_instruction(&c_fixture.tlb, CODE_PAGE, instruction);
        write_instruction(&threaded_fixture.tlb, CODE_PAGE, instruction);
        struct cpu_state initial;
        init_differential_cpu(&initial);
        initial.x[5] = DATA_PAGE + 0x208;
        struct cpu_state result = run_fast_differential_fixtures(
                instruction, initial, AARCH64_STEP_RETIRED,
                &c_fixture, &threaded_fixture, NULL);
        assert(result.x[5] == value);
        case_count++;
    }

    for (unsigned index = 1; index < array_size(address_modes); index++) {
        struct test_fixture c_fixture;
        struct test_fixture threaded_fixture;
        init_fixture(&c_fixture);
        init_fixture(&threaded_fixture);
        put_value(c_fixture.memory.data + 0x200, 8, value);
        put_value(threaded_fixture.memory.data + 0x200, 8, value);
        dword_t instruction = encode_scalar_load_imm9(
                3, 1, address_modes[index], offsets[index], 31, 31);
        write_instruction(&c_fixture.tlb, CODE_PAGE, instruction);
        write_instruction(&threaded_fixture.tlb, CODE_PAGE, instruction);
        struct cpu_state initial;
        init_differential_cpu(&initial);
        initial.sp = bases[index];
        struct cpu_state result = run_fast_differential_fixtures(
                instruction, initial, AARCH64_STEP_RETIRED,
                &c_fixture, &threaded_fixture, NULL);
        assert(result.sp == expected_bases[index]);
        case_count++;
    }
    assert(case_count == 9);
}

static void test_fast_load_imm9_faults(void) {
    {
        struct test_fixture c_fixture;
        struct test_fixture threaded_fixture;
        init_fixture(&c_fixture);
        init_fixture(&threaded_fixture);
        dword_t instruction = encode_scalar_load_imm9(
                3, 1, AARCH64_ADDRESS_POST_INDEX, 8, 3, 2);
        write_instruction(&c_fixture.tlb, CODE_PAGE, instruction);
        write_instruction(&threaded_fixture.tlb, CODE_PAGE, instruction);
        struct cpu_state initial;
        init_differential_cpu(&initial);
        initial.x[2] = UINT64_C(0x123456789abcdef0);
        initial.x[3] = DATA_PAGE + GUEST_MEMORY_PAGE_SIZE - 4;
        aarch64_set_exclusive(&initial,
                DATA_PAGE + 0x40, 8, false,
                UINT64_C(0x1122), 0, NULL, 3, 5, 7);
        struct aarch64_step_result step;
        struct cpu_state result = run_fast_differential_fixtures(
                instruction, initial, AARCH64_STEP_DATA_FAULT,
                &c_fixture, &threaded_fixture, &step);
        assert(step.fault.address == DATA_PAGE + GUEST_MEMORY_PAGE_SIZE);
        assert(step.fault.access == GUEST_MEMORY_READ);
        assert(step.fault.kind == GUEST_MEMORY_FAULT_UNMAPPED);
        assert(result.x[2] == initial.x[2]);
        assert(result.x[3] == initial.x[3]);
        assert(result.pc == CODE_PAGE);
        assert(result.cycle == initial.cycle);
        assert(!result.exclusive.valid);
    }

    {
        struct test_fixture c_fixture;
        struct test_fixture threaded_fixture;
        init_fixture(&c_fixture);
        init_fixture(&threaded_fixture);
        dword_t instruction = encode_scalar_load_imm9(
                3, 1, AARCH64_ADDRESS_PRE_INDEX, -8, 31, 4);
        write_instruction(&c_fixture.tlb, CODE_PAGE, instruction);
        write_instruction(&threaded_fixture.tlb, CODE_PAGE, instruction);
        c_fixture.memory.code_permissions = GUEST_MEMORY_EXECUTE;
        threaded_fixture.memory.code_permissions = GUEST_MEMORY_EXECUTE;
        guest_address_space_changed(&c_fixture.space);
        guest_address_space_changed(&threaded_fixture.space);
        struct cpu_state initial;
        init_differential_cpu(&initial);
        initial.x[4] = UINT64_C(0x123456789abcdef0);
        initial.sp = CODE_PAGE + 0x108;
        aarch64_set_exclusive(&initial,
                DATA_PAGE + 0x40, 8, false,
                UINT64_C(0x1122), 0, NULL, 3, 5, 7);
        struct aarch64_step_result step;
        struct cpu_state result = run_fast_differential_fixtures(
                instruction, initial, AARCH64_STEP_DATA_FAULT,
                &c_fixture, &threaded_fixture, &step);
        assert(step.fault.address == CODE_PAGE + 0x100);
        assert(step.fault.access == GUEST_MEMORY_READ);
        assert(step.fault.kind == GUEST_MEMORY_FAULT_PERMISSION);
        assert(result.x[4] == initial.x[4]);
        assert(result.sp == initial.sp);
        assert(result.pc == CODE_PAGE);
        assert(result.cycle == initial.cycle);
        assert(!result.exclusive.valid);
    }

    {
        struct test_fixture c_fixture;
        struct test_fixture threaded_fixture;
        init_fixture(&c_fixture);
        init_fixture(&threaded_fixture);
        dword_t instruction = encode_scalar_load_imm9(
                0, 1, AARCH64_ADDRESS_OFFSET, 0, 5, 31);
        write_instruction(&c_fixture.tlb, CODE_PAGE, instruction);
        write_instruction(&threaded_fixture.tlb, CODE_PAGE, instruction);
        struct cpu_state initial;
        init_differential_cpu(&initial);
        initial.x[5] = UINT64_C(1) << 48;
        aarch64_set_exclusive(&initial,
                DATA_PAGE + 0x40, 8, false,
                UINT64_C(0x1122), 0, NULL, 3, 5, 7);
        struct aarch64_step_result step;
        struct cpu_state result = run_fast_differential_fixtures(
                instruction, initial, AARCH64_STEP_DATA_FAULT,
                &c_fixture, &threaded_fixture, &step);
        assert(step.fault.address == initial.x[5]);
        assert(step.fault.access == GUEST_MEMORY_READ);
        assert(step.fault.kind == GUEST_MEMORY_FAULT_ADDRESS_SIZE);
        assert(result.x[5] == initial.x[5]);
        assert(result.pc == CODE_PAGE);
        assert(result.cycle == initial.cycle);
        assert(!result.exclusive.valid);
    }
}

static void test_fast_store_imm9_differential(void) {
    const enum aarch64_address_mode address_modes[] = {
        AARCH64_ADDRESS_OFFSET,
        AARCH64_ADDRESS_POST_INDEX,
        AARCH64_ADDRESS_PRE_INDEX,
    };
    const int64_t offsets[] = {-256, 0, 255};
    const qword_t source = UINT64_C(0x8877665544332211);
    const size_t target_offset = 0x300;
    unsigned case_count = 0;

    for (byte_t size_shift = 0; size_shift < 4; size_shift++) {
        byte_t size = (byte_t) (1U << size_shift);
        byte_t width = size_shift == 3 ? 64 : 32;
        for (unsigned mode_index = 0;
                mode_index < array_size(address_modes); mode_index++) {
            for (unsigned offset_index = 0;
                    offset_index < array_size(offsets); offset_index++) {
                enum aarch64_address_mode address_mode =
                        address_modes[mode_index];
                int64_t offset = offsets[offset_index];
                dword_t instruction = encode_scalar_store_imm9(
                        size_shift, address_mode, offset, 5, 4);
                struct aarch64_decoded decoded;
                assert(aarch64_decode(instruction, &decoded));
                assert(decoded.opcode == AARCH64_OP_STORE_IMM9);
                assert(decoded.width == width);
                assert(decoded.operands.load_store.size == size);
                assert(decoded.operands.load_store.offset == offset);
                assert(decoded.operands.load_store.address_mode ==
                        address_mode);
                assert(!decoded.operands.load_store.signed_load);
                assert(decoded.operands.load_store.rn == 5);
                assert(decoded.operands.load_store.rt == 4);

                struct test_fixture c_fixture;
                struct test_fixture threaded_fixture;
                init_fixture(&c_fixture);
                init_fixture(&threaded_fixture);
                memset(c_fixture.memory.data, 0xa5,
                        sizeof(c_fixture.memory.data));
                memset(threaded_fixture.memory.data, 0xa5,
                        sizeof(threaded_fixture.memory.data));
                write_instruction(&c_fixture.tlb, CODE_PAGE, instruction);
                write_instruction(&threaded_fixture.tlb,
                        CODE_PAGE, instruction);

                struct cpu_state initial;
                init_differential_cpu(&initial);
                initial.x[4] = source;
                guest_addr_t base = DATA_PAGE + target_offset;
                if (address_mode != AARCH64_ADDRESS_POST_INDEX)
                    base -= (qword_t) offset;
                initial.x[5] = base;
                aarch64_set_exclusive(&initial,
                        DATA_PAGE + 0x40, 8, false,
                        UINT64_C(0x1122), 0, NULL, 3, 5, 7);

                struct cpu_state result =
                        run_fast_differential_fixtures(
                                instruction, initial,
                                AARCH64_STEP_RETIRED,
                                &c_fixture, &threaded_fixture, NULL);
                for (byte_t byte = 0; byte < size; byte++) {
                    assert(c_fixture.memory.data[target_offset + byte] ==
                            (byte_t) (source >> (byte * 8)));
                }
                assert(c_fixture.memory.data[target_offset - 1] == 0xa5);
                assert(c_fixture.memory.data[
                        target_offset + size] == 0xa5);
                guest_addr_t expected_base =
                        address_mode == AARCH64_ADDRESS_OFFSET ?
                        base : base + (qword_t) offset;
                assert(result.x[4] == source);
                assert(result.x[5] == expected_base);
                assert(result.pc == CODE_PAGE + 4);
                assert(result.cycle == initial.cycle + 1);
                assert(result.nzcv == initial.nzcv);
                assert(result.fpcr == initial.fpcr);
                assert(result.fpsr == initial.fpsr);
                assert(result.exclusive.valid);
                assert(result.exclusive.write_epoch == 5);
                assert(result.exclusive.sync_identity == 7);
                case_count++;
            }
        }
    }
    assert(case_count == 36);

    assert(encode_scalar_store_imm9(3,
            AARCH64_ADDRESS_POST_INDEX, 8, 2, 31) ==
            INSTRUCTION_STR_XZR_X2_POST_8);
    assert(encode_scalar_store_imm9(1,
            AARCH64_ADDRESS_OFFSET, -2, 7, 31) ==
            INSTRUCTION_STURH_WZR_X7_NEG_2);
    const struct {
        dword_t instruction;
        byte_t size;
        byte_t rn;
        guest_addr_t base;
        guest_addr_t expected_base;
        size_t target_offset;
    } profile_cases[] = {
        {
            INSTRUCTION_STR_XZR_X2_POST_8, 8, 2,
            DATA_PAGE + 0x180, DATA_PAGE + 0x188, 0x180,
        },
        {
            INSTRUCTION_STURH_WZR_X7_NEG_2, 2, 7,
            DATA_PAGE + 0x202, DATA_PAGE + 0x202, 0x200,
        },
    };
    for (unsigned index = 0;
            index < array_size(profile_cases); index++) {
        struct test_fixture c_fixture;
        struct test_fixture threaded_fixture;
        init_fixture(&c_fixture);
        init_fixture(&threaded_fixture);
        memset(c_fixture.memory.data + profile_cases[index].target_offset,
                0xa5, profile_cases[index].size);
        memset(threaded_fixture.memory.data +
                profile_cases[index].target_offset,
                0xa5, profile_cases[index].size);
        write_instruction(&c_fixture.tlb, CODE_PAGE,
                profile_cases[index].instruction);
        write_instruction(&threaded_fixture.tlb, CODE_PAGE,
                profile_cases[index].instruction);
        struct cpu_state initial;
        init_differential_cpu(&initial);
        initial.x[profile_cases[index].rn] =
                profile_cases[index].base;
        struct cpu_state result = run_fast_differential_fixtures(
                profile_cases[index].instruction, initial,
                AARCH64_STEP_RETIRED,
                &c_fixture, &threaded_fixture, NULL);
        for (byte_t byte = 0; byte < profile_cases[index].size; byte++) {
            assert(c_fixture.memory.data[
                    profile_cases[index].target_offset + byte] == 0);
        }
        assert(result.x[profile_cases[index].rn] ==
                profile_cases[index].expected_base);
    }
}

static void test_fast_store_imm9_aliases(void) {
    const enum aarch64_address_mode address_modes[] = {
        AARCH64_ADDRESS_OFFSET,
        AARCH64_ADDRESS_POST_INDEX,
        AARCH64_ADDRESS_PRE_INDEX,
    };
    const int64_t offsets[] = {-8, 8, -8};
    const guest_addr_t bases[] = {
        DATA_PAGE + 0x248,
        DATA_PAGE + 0x240,
        DATA_PAGE + 0x248,
    };
    const guest_addr_t expected_bases[] = {
        DATA_PAGE + 0x248,
        DATA_PAGE + 0x248,
        DATA_PAGE + 0x240,
    };

    for (unsigned index = 0; index < array_size(address_modes); index++) {
        struct test_fixture c_fixture;
        struct test_fixture threaded_fixture;
        init_fixture(&c_fixture);
        init_fixture(&threaded_fixture);
        memset(c_fixture.memory.data + 0x240, 0xa5, 8);
        memset(threaded_fixture.memory.data + 0x240, 0xa5, 8);
        dword_t instruction = encode_scalar_store_imm9(
                3, address_modes[index], offsets[index], 31, 31);
        write_instruction(&c_fixture.tlb, CODE_PAGE, instruction);
        write_instruction(&threaded_fixture.tlb, CODE_PAGE, instruction);
        struct cpu_state initial;
        init_differential_cpu(&initial);
        initial.sp = bases[index];
        struct cpu_state result = run_fast_differential_fixtures(
                instruction, initial, AARCH64_STEP_RETIRED,
                &c_fixture, &threaded_fixture, NULL);
        const byte_t zeros[8] = {0};
        assert(memcmp(c_fixture.memory.data + 0x240,
                zeros, sizeof(zeros)) == 0);
        assert(result.sp == expected_bases[index]);
    }

    {
        struct test_fixture c_fixture;
        struct test_fixture threaded_fixture;
        init_fixture(&c_fixture);
        init_fixture(&threaded_fixture);
        memset(c_fixture.memory.data + 0x280, 0xa5, 8);
        memset(threaded_fixture.memory.data + 0x280, 0xa5, 8);
        dword_t instruction = encode_scalar_store_imm9(
                3, AARCH64_ADDRESS_OFFSET, 8, 5, 5);
        write_instruction(&c_fixture.tlb, CODE_PAGE, instruction);
        write_instruction(&threaded_fixture.tlb, CODE_PAGE, instruction);
        struct cpu_state initial;
        init_differential_cpu(&initial);
        initial.x[5] = DATA_PAGE + 0x278;
        struct cpu_state result = run_fast_differential_fixtures(
                instruction, initial, AARCH64_STEP_RETIRED,
                &c_fixture, &threaded_fixture, NULL);
        for (byte_t byte = 0; byte < 8; byte++) {
            assert(c_fixture.memory.data[0x280 + byte] ==
                    (byte_t) (initial.x[5] >> (byte * 8)));
        }
        assert(result.x[5] == initial.x[5]);
    }

    const enum aarch64_address_mode writeback_modes[] = {
        AARCH64_ADDRESS_POST_INDEX,
        AARCH64_ADDRESS_PRE_INDEX,
    };
    for (unsigned index = 0;
            index < array_size(writeback_modes); index++) {
        dword_t instruction = encode_scalar_store_imm9(
                3, writeback_modes[index], 8, 5, 4);
        instruction = (instruction & ~UINT32_C(0x1f)) | 5;
        struct aarch64_decoded decoded;
        assert(!aarch64_decode(instruction, &decoded));
    }

    {
        struct test_fixture c_fixture;
        struct test_fixture threaded_fixture;
        init_fixture(&c_fixture);
        init_fixture(&threaded_fixture);
        memset(c_fixture.memory.primary_code +
                GUEST_MEMORY_PAGE_SIZE - 5, 0xa5, 5);
        memset(threaded_fixture.memory.primary_code +
                GUEST_MEMORY_PAGE_SIZE - 5, 0xa5, 5);
        memset(c_fixture.memory.data, 0xa5, 9);
        memset(threaded_fixture.memory.data, 0xa5, 9);
        dword_t instruction = encode_scalar_store_imm9(
                3, AARCH64_ADDRESS_PRE_INDEX, -4, 3, 4);
        write_instruction(&c_fixture.tlb, CODE_PAGE, instruction);
        write_instruction(&threaded_fixture.tlb, CODE_PAGE, instruction);
        struct cpu_state initial;
        init_differential_cpu(&initial);
        initial.x[3] = DATA_PAGE;
        initial.x[4] = UINT64_C(0x8877665544332211);
        struct cpu_state result = run_fast_differential_fixtures(
                instruction, initial, AARCH64_STEP_RETIRED,
                &c_fixture, &threaded_fixture, NULL);
        for (byte_t byte = 0; byte < 4; byte++) {
            assert(c_fixture.memory.primary_code[
                    GUEST_MEMORY_PAGE_SIZE - 4 + byte] ==
                    (byte_t) (initial.x[4] >> (byte * 8)));
            assert(c_fixture.memory.data[byte] ==
                    (byte_t) (initial.x[4] >> ((byte + 4) * 8)));
        }
        assert(c_fixture.memory.primary_code[
                GUEST_MEMORY_PAGE_SIZE - 5] == 0xa5);
        assert(c_fixture.memory.data[4] == 0xa5);
        assert(result.x[3] == DATA_PAGE - 4);
    }
}

static void test_fast_store_imm9_faults(void) {
    {
        struct test_fixture c_fixture;
        struct test_fixture threaded_fixture;
        init_fixture(&c_fixture);
        init_fixture(&threaded_fixture);
        memset(c_fixture.memory.data + GUEST_MEMORY_PAGE_SIZE - 4,
                0xa5, 4);
        memset(threaded_fixture.memory.data +
                GUEST_MEMORY_PAGE_SIZE - 4, 0xa5, 4);
        dword_t instruction = encode_scalar_store_imm9(
                3, AARCH64_ADDRESS_POST_INDEX, 8, 3, 4);
        write_instruction(&c_fixture.tlb, CODE_PAGE, instruction);
        write_instruction(&threaded_fixture.tlb, CODE_PAGE, instruction);
        struct cpu_state initial;
        init_differential_cpu(&initial);
        initial.x[3] = DATA_PAGE + GUEST_MEMORY_PAGE_SIZE - 4;
        initial.x[4] = UINT64_C(0x8877665544332211);
        aarch64_set_exclusive(&initial,
                DATA_PAGE + 0x40, 8, false,
                UINT64_C(0x1122), 0, NULL, 3, 5, 7);
        struct aarch64_step_result step;
        struct cpu_state result = run_fast_differential_fixtures(
                instruction, initial, AARCH64_STEP_DATA_FAULT,
                &c_fixture, &threaded_fixture, &step);
        assert(step.fault.address ==
                DATA_PAGE + GUEST_MEMORY_PAGE_SIZE);
        assert(step.fault.access == GUEST_MEMORY_WRITE);
        assert(step.fault.kind == GUEST_MEMORY_FAULT_UNMAPPED);
        for (byte_t byte = 0; byte < 4; byte++) {
            assert(c_fixture.memory.data[
                    GUEST_MEMORY_PAGE_SIZE - 4 + byte] == 0xa5);
        }
        assert(result.x[3] == initial.x[3]);
        assert(result.x[4] == initial.x[4]);
        assert(result.pc == CODE_PAGE);
        assert(result.cycle == initial.cycle);
        assert(!result.exclusive.valid);
    }

    {
        struct test_fixture c_fixture;
        struct test_fixture threaded_fixture;
        init_fixture(&c_fixture);
        init_fixture(&threaded_fixture);
        dword_t instruction = encode_scalar_store_imm9(
                3, AARCH64_ADDRESS_PRE_INDEX, -8, 31, 4);
        write_instruction(&c_fixture.tlb, CODE_PAGE, instruction);
        write_instruction(&threaded_fixture.tlb, CODE_PAGE, instruction);
        memset(c_fixture.memory.primary_code + 0x100, 0xa5, 8);
        memset(threaded_fixture.memory.primary_code + 0x100, 0xa5, 8);
        c_fixture.memory.code_permissions = GUEST_MEMORY_EXECUTE;
        threaded_fixture.memory.code_permissions = GUEST_MEMORY_EXECUTE;
        guest_address_space_changed(&c_fixture.space);
        guest_address_space_changed(&threaded_fixture.space);
        struct cpu_state initial;
        init_differential_cpu(&initial);
        initial.x[4] = UINT64_C(0x8877665544332211);
        initial.sp = CODE_PAGE + 0x108;
        aarch64_set_exclusive(&initial,
                DATA_PAGE + 0x40, 8, false,
                UINT64_C(0x1122), 0, NULL, 3, 5, 7);
        struct aarch64_step_result step;
        struct cpu_state result = run_fast_differential_fixtures(
                instruction, initial, AARCH64_STEP_DATA_FAULT,
                &c_fixture, &threaded_fixture, &step);
        assert(step.fault.address == CODE_PAGE + 0x100);
        assert(step.fault.access == GUEST_MEMORY_WRITE);
        assert(step.fault.kind == GUEST_MEMORY_FAULT_PERMISSION);
        for (byte_t byte = 0; byte < 8; byte++)
            assert(c_fixture.memory.primary_code[0x100 + byte] == 0xa5);
        assert(result.x[4] == initial.x[4]);
        assert(result.sp == initial.sp);
        assert(result.pc == CODE_PAGE);
        assert(result.cycle == initial.cycle);
        assert(!result.exclusive.valid);
    }

    {
        struct test_fixture c_fixture;
        struct test_fixture threaded_fixture;
        init_fixture(&c_fixture);
        init_fixture(&threaded_fixture);
        dword_t instruction = encode_scalar_store_imm9(
                3, AARCH64_ADDRESS_OFFSET, 0, 5, 4);
        write_instruction(&c_fixture.tlb, CODE_PAGE, instruction);
        write_instruction(&threaded_fixture.tlb, CODE_PAGE, instruction);
        struct cpu_state initial;
        init_differential_cpu(&initial);
        initial.x[4] = UINT64_C(0x8877665544332211);
        initial.x[5] = UINT64_C(1) << 48;
        aarch64_set_exclusive(&initial,
                DATA_PAGE + 0x40, 8, false,
                UINT64_C(0x1122), 0, NULL, 3, 5, 7);
        struct aarch64_step_result step;
        struct cpu_state result = run_fast_differential_fixtures(
                instruction, initial, AARCH64_STEP_DATA_FAULT,
                &c_fixture, &threaded_fixture, &step);
        assert(step.fault.address == initial.x[5]);
        assert(step.fault.access == GUEST_MEMORY_WRITE);
        assert(step.fault.kind == GUEST_MEMORY_FAULT_ADDRESS_SIZE);
        assert(result.x[4] == initial.x[4]);
        assert(result.x[5] == initial.x[5]);
        assert(result.pc == CODE_PAGE);
        assert(result.cycle == initial.cycle);
        assert(!result.exclusive.valid);
    }
}

static void test_fast_store_imm9_exclusive_invalidation(void) {
    struct test_fixture c_fixture;
    struct test_fixture threaded_fixture;
    init_fixture(&c_fixture);
    init_fixture(&threaded_fixture);
    const dword_t store_instruction = encode_scalar_store_imm9(
            3, AARCH64_ADDRESS_POST_INDEX, 0, 1, 0);
    const dword_t instructions[] = {
        INSTRUCTION_LDXR_X2_X1,
        store_instruction,
        INSTRUCTION_STXR_W4_X3_X1,
    };
    for (unsigned index = 0; index < array_size(instructions); index++) {
        write_instruction(&c_fixture.tlb,
                CODE_PAGE + index * 4, instructions[index]);
        write_instruction(&threaded_fixture.tlb,
                CODE_PAGE + index * 4, instructions[index]);
    }
    put_value(c_fixture.memory.data + 0x180, 8,
            UINT64_C(0x1020304050607080));
    put_value(threaded_fixture.memory.data + 0x180, 8,
            UINT64_C(0x1020304050607080));

    struct aarch64_runner c_runner;
    struct aarch64_runner threaded_runner;
    assert(aarch64_runner_init_backend(
            &c_runner, &c_fixture.tlb, AARCH64_BACKEND_C));
    assert(aarch64_runner_init_backend(&threaded_runner,
            &threaded_fixture.tlb, AARCH64_BACKEND_THREADED));
    struct cpu_state c_cpu;
    struct cpu_state threaded_cpu;
    init_differential_cpu(&c_cpu);
    init_differential_cpu(&threaded_cpu);
    c_cpu.x[0] = threaded_cpu.x[0] =
            UINT64_C(0x8877665544332211);
    c_cpu.x[1] = threaded_cpu.x[1] = DATA_PAGE + 0x180;
    c_cpu.x[3] = threaded_cpu.x[3] =
            UINT64_C(0xaabbccddeeff0011);
    c_cpu.x[4] = threaded_cpu.x[4] = UINT64_MAX;

    for (unsigned index = 0; index < array_size(instructions); index++) {
        struct aarch64_step_result c_result =
                aarch64_run_one(&c_runner, &c_cpu);
        struct aarch64_step_result threaded_result =
                aarch64_run_one(&threaded_runner, &threaded_cpu);
        assert(c_result.stop == AARCH64_STEP_RETIRED);
        assert_step_equal(&c_result, &threaded_result);
        assert_cpu_equal(&c_cpu, &threaded_cpu);
        assert_memory_equal(&c_fixture.memory,
                &threaded_fixture.memory);
        if (index == 0)
            assert(c_cpu.exclusive.valid);
        if (index == 1) {
            assert(c_cpu.exclusive.valid);
            assert(c_cpu.x[1] == DATA_PAGE + 0x180);
            assert_stats(&threaded_runner, 0, 2, 1, 1);
        }
    }
    assert(c_cpu.x[4] == 1);
    assert(!c_cpu.exclusive.valid);
    assert(c_cpu.x[2] == UINT64_C(0x1020304050607080));
    assert(c_cpu.x[3] == UINT64_C(0xaabbccddeeff0011));
    assert_stats(&threaded_runner, 0, 3, 1, 2);
    const byte_t expected[] = {
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
    };
    assert(memcmp(c_fixture.memory.data + 0x180,
            expected, sizeof(expected)) == 0);
}

static void test_fast_store_simd_imm9_differential(void) {
    static const union aarch64_vector_reg source = {
        .d = {
            UINT64_C(0x0123456789abcdef),
            UINT64_C(0xfedcba9876543210),
        },
    };
    static const struct {
        byte_t size_shift;
        enum aarch64_address_mode address_mode;
        int64_t offset;
        byte_t rn;
        byte_t rt;
        dword_t instruction;
        size_t target_offset;
    } cases[] = {
        {
            0, AARCH64_ADDRESS_PRE_INDEX, -256, 11, 10,
            UINT32_C(0x3c100d6a), 0x180,
        },
        {
            1, AARCH64_ADDRESS_POST_INDEX, 255, 12, 13,
            UINT32_C(0x7c0ff58d), 0x1c0,
        },
        {
            2, AARCH64_ADDRESS_OFFSET, 0, 14, 15,
            UINT32_C(0xbc0001cf), 0x200,
        },
        {
            3, AARCH64_ADDRESS_PRE_INDEX, 1, 16, 16,
            UINT32_C(0xfc001e10), 0x240,
        },
        {
            4, AARCH64_ADDRESS_OFFSET, 56, 0, 31,
            INSTRUCTION_STUR_Q31_X0_56, 0x280,
        },
        {
            4, AARCH64_ADDRESS_OFFSET, -16, 4, 0,
            INSTRUCTION_STUR_Q0_X4_NEG_16, 0x2c0,
        },
    };
    _Static_assert(array_size(cases) == 6,
            "STORE SIMD imm9 成功差分表必须保持六项");
    assert(encode_store_simd_imm9(0, AARCH64_ADDRESS_OFFSET,
            -256, 1, 0) == INSTRUCTION_STUR_B0_X1_NEG_256);

    for (unsigned index = 0; index < array_size(cases); index++) {
        byte_t size = (byte_t) (UINT8_C(1) << cases[index].size_shift);
        dword_t instruction = encode_store_simd_imm9(
                cases[index].size_shift, cases[index].address_mode,
                cases[index].offset, cases[index].rn, cases[index].rt);
        assert(instruction == cases[index].instruction);
        struct aarch64_decoded decoded;
        assert(aarch64_decode(instruction, &decoded));
        assert(decoded.opcode == AARCH64_OP_STORE_SIMD_IMM9);
        assert(decoded.width == size * 8);
        assert(decoded.operands.load_store.size == size);
        assert(decoded.operands.load_store.offset ==
                cases[index].offset);
        assert(decoded.operands.load_store.address_mode ==
                cases[index].address_mode);
        assert(decoded.operands.load_store.rn == cases[index].rn);
        assert(decoded.operands.load_store.rt == cases[index].rt);

        struct test_fixture c_fixture;
        struct test_fixture threaded_fixture;
        init_fixture(&c_fixture);
        init_fixture(&threaded_fixture);
        memset(c_fixture.memory.data, 0xa5,
                sizeof(c_fixture.memory.data));
        memset(threaded_fixture.memory.data, 0xa5,
                sizeof(threaded_fixture.memory.data));
        write_instruction(&c_fixture.tlb, CODE_PAGE, instruction);
        write_instruction(&threaded_fixture.tlb,
                CODE_PAGE, instruction);

        guest_addr_t target =
                DATA_PAGE + cases[index].target_offset;
        guest_addr_t base =
                cases[index].address_mode ==
                        AARCH64_ADDRESS_POST_INDEX ?
                target : target - (qword_t) cases[index].offset;
        struct cpu_state initial;
        init_differential_cpu(&initial);
        if (cases[index].rn == 31)
            initial.sp = base;
        else
            initial.x[cases[index].rn] = base;
        initial.v[cases[index].rt] = source;
        aarch64_set_exclusive(&initial, DATA_PAGE + 0x380,
                8, false, UINT64_C(0x1122), 0, NULL, 3, 5, 7);

        struct cpu_state result = run_fast_differential_fixtures(
                instruction, initial, AARCH64_STEP_RETIRED,
                &c_fixture, &threaded_fixture, NULL);
        struct cpu_state expected_cpu = initial;
        if (cases[index].address_mode != AARCH64_ADDRESS_OFFSET) {
            guest_addr_t adjusted =
                    base + (qword_t) cases[index].offset;
            if (cases[index].rn == 31)
                expected_cpu.sp = adjusted;
            else
                expected_cpu.x[cases[index].rn] = adjusted;
        }
        expected_cpu.pc = CODE_PAGE + 4;
        expected_cpu.cycle++;
        assert_cpu_equal(&result, &expected_cpu);

        byte_t expected_data[GUEST_MEMORY_PAGE_SIZE];
        memset(expected_data, 0xa5, sizeof(expected_data));
        memcpy(expected_data + cases[index].target_offset,
                source.b, size);
        assert(memcmp(c_fixture.memory.data,
                expected_data, sizeof(expected_data)) == 0);
        assert(memcmp(threaded_fixture.memory.data,
                expected_data, sizeof(expected_data)) == 0);
    }

    const dword_t cross_page = encode_store_simd_imm9(
            4, AARCH64_ADDRESS_POST_INDEX, -16, 31, 27);
    assert(cross_page == UINT32_C(0x3c9f07fb));
    struct test_fixture c_fixture;
    struct test_fixture threaded_fixture;
    init_fixture(&c_fixture);
    init_fixture(&threaded_fixture);
    memset(c_fixture.memory.primary_code +
            GUEST_MEMORY_PAGE_SIZE - 8, 0xa5, 8);
    memset(threaded_fixture.memory.primary_code +
            GUEST_MEMORY_PAGE_SIZE - 8, 0xa5, 8);
    memset(c_fixture.memory.data, 0xa5, 10);
    memset(threaded_fixture.memory.data, 0xa5, 10);
    write_instruction(&c_fixture.tlb, CODE_PAGE, cross_page);
    write_instruction(&threaded_fixture.tlb, CODE_PAGE, cross_page);
    struct cpu_state initial;
    init_differential_cpu(&initial);
    initial.sp = DATA_PAGE - 7;
    initial.v[27] = source;
    aarch64_set_exclusive(&initial, DATA_PAGE + 0x380,
            8, false, UINT64_C(0x1122), 0, NULL, 3, 5, 7);
    struct cpu_state result = run_fast_differential_fixtures(
            cross_page, initial, AARCH64_STEP_RETIRED,
            &c_fixture, &threaded_fixture, NULL);
    struct cpu_state expected_cpu = initial;
    expected_cpu.sp = DATA_PAGE - 23;
    expected_cpu.pc = CODE_PAGE + 4;
    expected_cpu.cycle++;
    assert_cpu_equal(&result, &expected_cpu);
    assert(c_fixture.memory.primary_code[
            GUEST_MEMORY_PAGE_SIZE - 8] == 0xa5);
    assert(memcmp(c_fixture.memory.primary_code +
            GUEST_MEMORY_PAGE_SIZE - 7, source.b, 7) == 0);
    assert(memcmp(c_fixture.memory.data, source.b + 7, 9) == 0);
    assert(c_fixture.memory.data[9] == 0xa5);
}

static void test_fast_store_simd_imm9_faults(void) {
    static const union aarch64_vector_reg source = {
        .d = {
            UINT64_C(0x8877665544332211),
            UINT64_C(0x1020304050607080),
        },
    };

    {
        const dword_t instruction = encode_store_simd_imm9(
                4, AARCH64_ADDRESS_POST_INDEX, 16, 4, 2);
        assert(instruction == UINT32_C(0x3c810482));
        struct test_fixture c_fixture;
        struct test_fixture threaded_fixture;
        init_fixture(&c_fixture);
        init_fixture(&threaded_fixture);
        memset(c_fixture.memory.data +
                GUEST_MEMORY_PAGE_SIZE - 8, 0xa5, 8);
        memset(threaded_fixture.memory.data +
                GUEST_MEMORY_PAGE_SIZE - 8, 0xa5, 8);
        write_instruction(&c_fixture.tlb, CODE_PAGE, instruction);
        write_instruction(&threaded_fixture.tlb,
                CODE_PAGE, instruction);
        struct cpu_state initial;
        init_differential_cpu(&initial);
        initial.x[4] =
                DATA_PAGE + GUEST_MEMORY_PAGE_SIZE - 8;
        initial.v[2] = source;
        aarch64_set_exclusive(&initial, DATA_PAGE + 0x300,
                8, false, UINT64_C(0x1122), 0, NULL, 3, 5, 7);
        struct aarch64_step_result step;
        struct cpu_state result = run_fast_differential_fixtures(
                instruction, initial, AARCH64_STEP_DATA_FAULT,
                &c_fixture, &threaded_fixture, &step);
        assert(step.fault.address ==
                DATA_PAGE + GUEST_MEMORY_PAGE_SIZE);
        assert(step.fault.access == GUEST_MEMORY_WRITE);
        assert(step.fault.kind == GUEST_MEMORY_FAULT_UNMAPPED);
        struct cpu_state expected_cpu = initial;
        expected_cpu.exclusive.valid = false;
        assert_cpu_equal(&result, &expected_cpu);
        for (byte_t byte = 0; byte < 8; byte++) {
            assert(c_fixture.memory.data[
                    GUEST_MEMORY_PAGE_SIZE - 8 + byte] == 0xa5);
        }
    }

    {
        const dword_t instruction = encode_store_simd_imm9(
                4, AARCH64_ADDRESS_PRE_INDEX, -16, 31, 3);
        assert(instruction == UINT32_C(0x3c9f0fe3));
        struct test_fixture c_fixture;
        struct test_fixture threaded_fixture;
        init_fixture(&c_fixture);
        init_fixture(&threaded_fixture);
        memset(c_fixture.memory.primary_code + 0x100, 0xa5, 16);
        memset(threaded_fixture.memory.primary_code +
                0x100, 0xa5, 16);
        write_instruction(&c_fixture.tlb, CODE_PAGE, instruction);
        write_instruction(&threaded_fixture.tlb,
                CODE_PAGE, instruction);
        c_fixture.memory.code_permissions = GUEST_MEMORY_EXECUTE;
        threaded_fixture.memory.code_permissions =
                GUEST_MEMORY_EXECUTE;
        guest_address_space_changed(&c_fixture.space);
        guest_address_space_changed(&threaded_fixture.space);
        struct cpu_state initial;
        init_differential_cpu(&initial);
        initial.sp = CODE_PAGE + 0x110;
        initial.v[3] = source;
        aarch64_set_exclusive(&initial, DATA_PAGE + 0x300,
                8, false, UINT64_C(0x1122), 0, NULL, 3, 5, 7);
        struct aarch64_step_result step;
        struct cpu_state result = run_fast_differential_fixtures(
                instruction, initial, AARCH64_STEP_DATA_FAULT,
                &c_fixture, &threaded_fixture, &step);
        assert(step.fault.address == CODE_PAGE + 0x100);
        assert(step.fault.access == GUEST_MEMORY_WRITE);
        assert(step.fault.kind == GUEST_MEMORY_FAULT_PERMISSION);
        struct cpu_state expected_cpu = initial;
        expected_cpu.exclusive.valid = false;
        assert_cpu_equal(&result, &expected_cpu);
        for (byte_t byte = 0; byte < 16; byte++)
            assert(c_fixture.memory.primary_code[0x100 + byte] == 0xa5);
    }

    {
        const dword_t instruction = encode_store_simd_imm9(
                0, AARCH64_ADDRESS_OFFSET, 0, 5, 6);
        assert(instruction == UINT32_C(0x3c0000a6));
        struct test_fixture c_fixture;
        struct test_fixture threaded_fixture;
        init_fixture(&c_fixture);
        init_fixture(&threaded_fixture);
        memset(c_fixture.memory.data, 0xa5,
                sizeof(c_fixture.memory.data));
        memset(threaded_fixture.memory.data, 0xa5,
                sizeof(threaded_fixture.memory.data));
        write_instruction(&c_fixture.tlb, CODE_PAGE, instruction);
        write_instruction(&threaded_fixture.tlb,
                CODE_PAGE, instruction);
        struct cpu_state initial;
        init_differential_cpu(&initial);
        initial.x[5] = UINT64_C(1) << 48;
        initial.v[6] = source;
        aarch64_set_exclusive(&initial, DATA_PAGE + 0x300,
                8, false, UINT64_C(0x1122), 0, NULL, 3, 5, 7);
        struct aarch64_step_result step;
        struct cpu_state result = run_fast_differential_fixtures(
                instruction, initial, AARCH64_STEP_DATA_FAULT,
                &c_fixture, &threaded_fixture, &step);
        assert(step.fault.address == (UINT64_C(1) << 48));
        assert(step.fault.access == GUEST_MEMORY_WRITE);
        assert(step.fault.kind ==
                GUEST_MEMORY_FAULT_ADDRESS_SIZE);
        struct cpu_state expected_cpu = initial;
        expected_cpu.exclusive.valid = false;
        assert_cpu_equal(&result, &expected_cpu);
        for (size_t byte = 0;
                byte < sizeof(c_fixture.memory.data); byte++) {
            assert(c_fixture.memory.data[byte] == 0xa5);
        }
    }
}

static void test_fast_store_simd_imm9_exclusive_invalidation(void) {
    struct test_fixture c_fixture;
    struct test_fixture threaded_fixture;
    init_fixture(&c_fixture);
    init_fixture(&threaded_fixture);
    const dword_t store = encode_store_simd_imm9(
            4, AARCH64_ADDRESS_OFFSET, 0, 1, 0);
    assert(store == UINT32_C(0x3c800020));
    const dword_t instructions[] = {
        INSTRUCTION_LDXR_X2_X1,
        store,
        INSTRUCTION_STXR_W4_X3_X1,
    };
    for (unsigned index = 0; index < array_size(instructions); index++) {
        write_instruction(&c_fixture.tlb,
                CODE_PAGE + index * 4, instructions[index]);
        write_instruction(&threaded_fixture.tlb,
                CODE_PAGE + index * 4, instructions[index]);
    }
    put_value(c_fixture.memory.data + 0x180, 8,
            UINT64_C(0x0102030405060708));
    put_value(threaded_fixture.memory.data + 0x180, 8,
            UINT64_C(0x0102030405060708));

    struct aarch64_runner c_runner;
    struct aarch64_runner threaded_runner;
    assert(aarch64_runner_init_backend(
            &c_runner, &c_fixture.tlb, AARCH64_BACKEND_C));
    assert(aarch64_runner_init_backend(&threaded_runner,
            &threaded_fixture.tlb, AARCH64_BACKEND_THREADED));
    struct cpu_state c_cpu;
    struct cpu_state threaded_cpu;
    init_differential_cpu(&c_cpu);
    init_differential_cpu(&threaded_cpu);
    c_cpu.x[1] = threaded_cpu.x[1] = DATA_PAGE + 0x180;
    c_cpu.x[3] = threaded_cpu.x[3] =
            UINT64_C(0xaabbccddeeff0011);
    c_cpu.x[4] = threaded_cpu.x[4] = UINT64_MAX;
    c_cpu.v[0].d[0] = threaded_cpu.v[0].d[0] =
            UINT64_C(0x8877665544332211);
    c_cpu.v[0].d[1] = threaded_cpu.v[0].d[1] =
            UINT64_C(0x1020304050607080);

    for (unsigned index = 0; index < array_size(instructions); index++) {
        struct aarch64_step_result c_result =
                aarch64_run_one(&c_runner, &c_cpu);
        struct aarch64_step_result threaded_result =
                aarch64_run_one(&threaded_runner, &threaded_cpu);
        assert(c_result.stop == AARCH64_STEP_RETIRED);
        assert_step_equal(&c_result, &threaded_result);
        assert_cpu_equal(&c_cpu, &threaded_cpu);
        assert_memory_equal(&c_fixture.memory,
                &threaded_fixture.memory);
        if (index == 0)
            assert(c_cpu.exclusive.valid);
        if (index == 1) {
            assert(c_cpu.exclusive.valid);
            assert(!guest_address_space_exclusive_matches(
                    &c_fixture.space, DATA_PAGE + 0x180,
                    c_cpu.exclusive.write_epoch));
            assert(!guest_address_space_exclusive_matches(
                    &threaded_fixture.space, DATA_PAGE + 0x180,
                    threaded_cpu.exclusive.write_epoch));
            assert_stats(&threaded_runner, 0, 2, 1, 1);
        }
    }
    assert(c_cpu.x[2] == UINT64_C(0x0102030405060708));
    assert(c_cpu.x[4] == 1);
    assert(!c_cpu.exclusive.valid);
    assert_stats(&threaded_runner, 0, 3, 1, 2);
    assert(memcmp(c_fixture.memory.data + 0x180,
            &c_cpu.v[0], sizeof(c_cpu.v[0])) == 0);
}

static void test_imm9_sibling_fallback(void) {
    const dword_t instructions[] = {
        INSTRUCTION_LDTR_X4_X3,
        INSTRUCTION_STTR_W5_X3_4,
        INSTRUCTION_LDUR_H2_SP_255,
    };
    const enum aarch64_opcode opcodes[] = {
        AARCH64_OP_LOAD_UNPRIVILEGED,
        AARCH64_OP_STORE_UNPRIVILEGED,
        AARCH64_OP_LOAD_SIMD_IMM9,
    };
    struct test_fixture fixture;
    init_fixture(&fixture);
    for (unsigned index = 0; index < array_size(instructions); index++) {
        struct aarch64_decoded decoded;
        assert(aarch64_decode(instructions[index], &decoded));
        assert(decoded.opcode == opcodes[index]);
        write_instruction(&fixture.tlb,
                CODE_PAGE + index * 4, instructions[index]);
    }
    put_value(fixture.memory.data + 0x100, 8,
            UINT64_C(0x0123456789abcdef));

    struct aarch64_runner runner;
    assert(aarch64_runner_init_backend(
            &runner, &fixture.tlb, AARCH64_BACKEND_THREADED));
    struct cpu_state cpu;
    init_differential_cpu(&cpu);
    cpu.x[2] = DATA_PAGE + 0x100;
    cpu.x[3] = DATA_PAGE + 0x100;
    cpu.x[5] = UINT64_C(0xffffffff89abcdef);
    cpu.sp = DATA_PAGE + 1;
    for (unsigned index = 0; index < array_size(instructions); index++) {
        assert(run_at(&runner, &cpu,
                CODE_PAGE + index * 4).stop == AARCH64_STEP_RETIRED);
    }
    assert_stats(&runner, 0, array_size(instructions),
            0, array_size(instructions));
}

static void test_fast_store_imm12_differential(void) {
    static const word_t immediates[] = {0, 1, UINT16_C(0xfff)};
    const qword_t source = UINT64_C(0x8877665544332211);
    const size_t target_offset = 0x203;
    unsigned case_count = 0;
    for (byte_t size_shift = 0; size_shift < 4; size_shift++) {
        byte_t size = (byte_t) (1U << size_shift);
        for (unsigned immediate_index = 0;
                immediate_index < array_size(immediates);
                immediate_index++) {
            word_t immediate = immediates[immediate_index];
            qword_t byte_offset = (qword_t) immediate << size_shift;
            dword_t instruction = encode_store_imm12(
                    size_shift, immediate, 5, 4);
            struct test_fixture c_fixture;
            struct test_fixture threaded_fixture;
            init_fixture(&c_fixture);
            init_fixture(&threaded_fixture);
            memset(c_fixture.memory.data, 0xa5,
                    sizeof(c_fixture.memory.data));
            memset(threaded_fixture.memory.data, 0xa5,
                    sizeof(threaded_fixture.memory.data));
            write_instruction(&c_fixture.tlb, CODE_PAGE, instruction);
            write_instruction(&threaded_fixture.tlb,
                    CODE_PAGE, instruction);

            struct cpu_state initial;
            init_differential_cpu(&initial);
            initial.x[4] = source;
            initial.x[5] = DATA_PAGE + target_offset - byte_offset;
            aarch64_set_exclusive(&initial, DATA_PAGE + 0x300,
                    8, false, UINT64_C(0x1122), 0, NULL, 3, 5, 7);
            struct cpu_state result = run_fast_differential_fixtures(
                    instruction, initial, AARCH64_STEP_RETIRED,
                    &c_fixture, &threaded_fixture, NULL);

            for (byte_t byte = 0; byte < size; byte++) {
                assert(c_fixture.memory.data[target_offset + byte] ==
                        (byte_t) (source >> (byte * 8)));
            }
            assert(c_fixture.memory.data[target_offset - 1] == 0xa5);
            assert(c_fixture.memory.data[target_offset + size] == 0xa5);
            assert(result.x[4] == source);
            assert(result.x[5] == initial.x[5]);
            assert(result.pc == CODE_PAGE + 4);
            assert(result.cycle == initial.cycle + 1);
            assert(result.nzcv == initial.nzcv);
            assert(result.fpcr == initial.fpcr);
            assert(result.fpsr == initial.fpsr);
            assert(result.exclusive.valid);
            assert(result.exclusive.write_epoch == 5);
            assert(result.exclusive.sync_identity == 7);
            case_count++;
        }
    }
    assert(case_count == 12);

    assert(encode_store_imm12(3, 0, 3, 5) ==
            INSTRUCTION_STR_X5_X3);
    assert(encode_store_imm12(2, 8, 1, 0) ==
            INSTRUCTION_STR_W0_X1_32);
    const struct {
        dword_t instruction;
        byte_t size;
        byte_t rn;
        byte_t rt;
        qword_t offset;
        qword_t value;
    } profile_cases[] = {
        {
            INSTRUCTION_STR_X5_X3, 8, 3, 5, 0,
            UINT64_C(0x1020304050607080),
        },
        {
            INSTRUCTION_STR_W0_X1_32, 4, 1, 0, 32,
            UINT64_C(0xffffffff89abcdef),
        },
    };
    for (unsigned index = 0;
            index < array_size(profile_cases); index++) {
        struct test_fixture c_fixture;
        struct test_fixture threaded_fixture;
        init_fixture(&c_fixture);
        init_fixture(&threaded_fixture);
        memset(c_fixture.memory.data, 0xa5,
                sizeof(c_fixture.memory.data));
        memset(threaded_fixture.memory.data, 0xa5,
                sizeof(threaded_fixture.memory.data));
        write_instruction(&c_fixture.tlb, CODE_PAGE,
                profile_cases[index].instruction);
        write_instruction(&threaded_fixture.tlb, CODE_PAGE,
                profile_cases[index].instruction);
        struct cpu_state initial;
        init_differential_cpu(&initial);
        initial.x[profile_cases[index].rn] =
                DATA_PAGE + 0x280 - profile_cases[index].offset;
        initial.x[profile_cases[index].rt] =
                profile_cases[index].value;
        run_fast_differential_fixtures(
                profile_cases[index].instruction, initial,
                AARCH64_STEP_RETIRED,
                &c_fixture, &threaded_fixture, NULL);
        for (byte_t byte = 0;
                byte < profile_cases[index].size; byte++) {
            assert(c_fixture.memory.data[0x280 + byte] ==
                    (byte_t) (profile_cases[index].value >>
                            (byte * 8)));
        }
        assert(c_fixture.memory.data[
                0x280 + profile_cases[index].size] == 0xa5);
    }

    {
        struct test_fixture c_fixture;
        struct test_fixture threaded_fixture;
        init_fixture(&c_fixture);
        init_fixture(&threaded_fixture);
        memset(c_fixture.memory.data, 0xa5,
                sizeof(c_fixture.memory.data));
        memset(threaded_fixture.memory.data, 0xa5,
                sizeof(threaded_fixture.memory.data));
        dword_t instruction =
                encode_store_imm12(3, UINT16_C(0xfff), 31, 31);
        write_instruction(&c_fixture.tlb, CODE_PAGE, instruction);
        write_instruction(&threaded_fixture.tlb,
                CODE_PAGE, instruction);
        struct cpu_state initial;
        init_differential_cpu(&initial);
        initial.sp = DATA_PAGE + 0x300 - UINT64_C(0x7ff8);
        struct cpu_state result = run_fast_differential_fixtures(
                instruction, initial, AARCH64_STEP_RETIRED,
                &c_fixture, &threaded_fixture, NULL);
        const byte_t zeros[8] = {0};
        assert(memcmp(c_fixture.memory.data + 0x300,
                zeros, sizeof(zeros)) == 0);
        assert(result.sp == initial.sp);
    }

    {
        struct test_fixture c_fixture;
        struct test_fixture threaded_fixture;
        init_fixture(&c_fixture);
        init_fixture(&threaded_fixture);
        memset(c_fixture.memory.data, 0xa5,
                sizeof(c_fixture.memory.data));
        memset(threaded_fixture.memory.data, 0xa5,
                sizeof(threaded_fixture.memory.data));
        dword_t instruction = encode_store_imm12(3, 5, 1, 1);
        write_instruction(&c_fixture.tlb, CODE_PAGE, instruction);
        write_instruction(&threaded_fixture.tlb,
                CODE_PAGE, instruction);
        struct cpu_state initial;
        init_differential_cpu(&initial);
        initial.x[1] = DATA_PAGE + 0x280 - 40;
        struct cpu_state result = run_fast_differential_fixtures(
                instruction, initial, AARCH64_STEP_RETIRED,
                &c_fixture, &threaded_fixture, NULL);
        for (byte_t byte = 0; byte < 8; byte++) {
            assert(c_fixture.memory.data[0x280 + byte] ==
                    (byte_t) (initial.x[1] >> (byte * 8)));
        }
        assert(result.x[1] == initial.x[1]);
    }

    {
        struct test_fixture c_fixture;
        struct test_fixture threaded_fixture;
        init_fixture(&c_fixture);
        init_fixture(&threaded_fixture);
        memset(c_fixture.memory.primary_code +
                GUEST_MEMORY_PAGE_SIZE - 5, 0xa5, 5);
        memset(threaded_fixture.memory.primary_code +
                GUEST_MEMORY_PAGE_SIZE - 5, 0xa5, 5);
        memset(c_fixture.memory.data, 0xa5,
                sizeof(c_fixture.memory.data));
        memset(threaded_fixture.memory.data, 0xa5,
                sizeof(threaded_fixture.memory.data));
        write_instruction(&c_fixture.tlb,
                CODE_PAGE, INSTRUCTION_STR_X5_X3);
        write_instruction(&threaded_fixture.tlb,
                CODE_PAGE, INSTRUCTION_STR_X5_X3);
        struct cpu_state initial;
        init_differential_cpu(&initial);
        initial.x[3] = DATA_PAGE - 4;
        initial.x[5] = source;
        run_fast_differential_fixtures(
                INSTRUCTION_STR_X5_X3, initial,
                AARCH64_STEP_RETIRED,
                &c_fixture, &threaded_fixture, NULL);
        for (byte_t byte = 0; byte < 4; byte++) {
            assert(c_fixture.memory.primary_code[
                    GUEST_MEMORY_PAGE_SIZE - 4 + byte] ==
                    (byte_t) (source >> (byte * 8)));
            assert(c_fixture.memory.data[byte] ==
                    (byte_t) (source >> ((byte + 4) * 8)));
        }
        assert(c_fixture.memory.primary_code[
                GUEST_MEMORY_PAGE_SIZE - 5] == 0xa5);
        assert(c_fixture.memory.data[4] == 0xa5);
    }
}

static void test_fast_store_imm12_faults(void) {
    struct test_fixture c_fixture;
    struct test_fixture threaded_fixture;
    init_fixture(&c_fixture);
    init_fixture(&threaded_fixture);
    memset(c_fixture.memory.data + GUEST_MEMORY_PAGE_SIZE - 4,
            0xa5, 4);
    memset(threaded_fixture.memory.data + GUEST_MEMORY_PAGE_SIZE - 4,
            0xa5, 4);
    write_instruction(&c_fixture.tlb,
            CODE_PAGE, INSTRUCTION_STR_X5_X3);
    write_instruction(&threaded_fixture.tlb,
            CODE_PAGE, INSTRUCTION_STR_X5_X3);
    struct cpu_state initial;
    init_differential_cpu(&initial);
    initial.x[3] = DATA_PAGE + GUEST_MEMORY_PAGE_SIZE - 4;
    initial.x[5] = UINT64_C(0x8877665544332211);
    aarch64_set_exclusive(&initial, DATA_PAGE + 0x300,
            8, false, UINT64_C(0x1122), 0, NULL, 3, 5, 7);
    struct aarch64_step_result step;
    struct cpu_state result = run_fast_differential_fixtures(
            INSTRUCTION_STR_X5_X3, initial,
            AARCH64_STEP_DATA_FAULT,
            &c_fixture, &threaded_fixture, &step);
    assert(step.fault.address == DATA_PAGE + GUEST_MEMORY_PAGE_SIZE);
    assert(step.fault.access == GUEST_MEMORY_WRITE);
    assert(step.fault.kind == GUEST_MEMORY_FAULT_UNMAPPED);
    for (byte_t byte = 0; byte < 4; byte++)
        assert(c_fixture.memory.data[
                GUEST_MEMORY_PAGE_SIZE - 4 + byte] == 0xa5);
    assert(result.x[3] == initial.x[3]);
    assert(result.x[5] == initial.x[5]);
    assert(result.pc == CODE_PAGE);
    assert(result.cycle == initial.cycle);
    assert(!result.exclusive.valid);

    init_fixture(&c_fixture);
    init_fixture(&threaded_fixture);
    write_instruction(&c_fixture.tlb,
            CODE_PAGE, INSTRUCTION_STR_W0_X1_32);
    write_instruction(&threaded_fixture.tlb,
            CODE_PAGE, INSTRUCTION_STR_W0_X1_32);
    memset(c_fixture.memory.primary_code + 0x100, 0xa5, 4);
    memset(threaded_fixture.memory.primary_code + 0x100, 0xa5, 4);
    c_fixture.memory.code_permissions = GUEST_MEMORY_EXECUTE;
    threaded_fixture.memory.code_permissions = GUEST_MEMORY_EXECUTE;
    guest_address_space_changed(&c_fixture.space);
    guest_address_space_changed(&threaded_fixture.space);
    init_differential_cpu(&initial);
    initial.x[0] = UINT64_C(0xffffffff89abcdef);
    initial.x[1] = CODE_PAGE + 0x100 - 32;
    aarch64_set_exclusive(&initial, DATA_PAGE + 0x300,
            8, false, UINT64_C(0x1122), 0, NULL, 3, 5, 7);
    result = run_fast_differential_fixtures(
            INSTRUCTION_STR_W0_X1_32, initial,
            AARCH64_STEP_DATA_FAULT,
            &c_fixture, &threaded_fixture, &step);
    assert(step.fault.address == CODE_PAGE + 0x100);
    assert(step.fault.access == GUEST_MEMORY_WRITE);
    assert(step.fault.kind == GUEST_MEMORY_FAULT_PERMISSION);
    for (byte_t byte = 0; byte < 4; byte++)
        assert(c_fixture.memory.primary_code[0x100 + byte] == 0xa5);
    assert(result.x[0] == initial.x[0]);
    assert(result.x[1] == initial.x[1]);
    assert(result.pc == CODE_PAGE);
    assert(result.cycle == initial.cycle);
    assert(!result.exclusive.valid);

    init_fixture(&c_fixture);
    init_fixture(&threaded_fixture);
    write_instruction(&c_fixture.tlb,
            CODE_PAGE, INSTRUCTION_STR_X5_X3);
    write_instruction(&threaded_fixture.tlb,
            CODE_PAGE, INSTRUCTION_STR_X5_X3);
    init_differential_cpu(&initial);
    initial.x[3] = (UINT64_C(1) << 48) - 4;
    initial.x[5] = UINT64_C(0x8877665544332211);
    aarch64_set_exclusive(&initial, DATA_PAGE + 0x300,
            8, false, UINT64_C(0x1122), 0, NULL, 3, 5, 7);
    result = run_fast_differential_fixtures(
            INSTRUCTION_STR_X5_X3, initial,
            AARCH64_STEP_DATA_FAULT,
            &c_fixture, &threaded_fixture, &step);
    assert(step.fault.address == (UINT64_C(1) << 48));
    assert(step.fault.access == GUEST_MEMORY_WRITE);
    assert(step.fault.kind == GUEST_MEMORY_FAULT_ADDRESS_SIZE);
    assert(result.pc == CODE_PAGE);
    assert(result.cycle == initial.cycle);
    assert(!result.exclusive.valid);
}

static void test_fast_store_imm12_exclusive_invalidation(void) {
    struct test_fixture c_fixture;
    struct test_fixture threaded_fixture;
    init_fixture(&c_fixture);
    init_fixture(&threaded_fixture);
    const dword_t instructions[] = {
        INSTRUCTION_LDXR_X2_X1,
        INSTRUCTION_STR_X0,
        INSTRUCTION_STXR_W4_X3_X1,
    };
    for (unsigned index = 0; index < array_size(instructions); index++) {
        write_instruction(&c_fixture.tlb,
                CODE_PAGE + index * 4, instructions[index]);
        write_instruction(&threaded_fixture.tlb,
                CODE_PAGE + index * 4, instructions[index]);
    }
    put_value(c_fixture.memory.data + 0x180, 8,
            UINT64_C(0x1020304050607080));
    put_value(threaded_fixture.memory.data + 0x180, 8,
            UINT64_C(0x1020304050607080));

    struct aarch64_runner c_runner;
    struct aarch64_runner threaded_runner;
    assert(aarch64_runner_init_backend(
            &c_runner, &c_fixture.tlb, AARCH64_BACKEND_C));
    assert(aarch64_runner_init_backend(&threaded_runner,
            &threaded_fixture.tlb, AARCH64_BACKEND_THREADED));
    struct cpu_state c_cpu;
    struct cpu_state threaded_cpu;
    init_differential_cpu(&c_cpu);
    init_differential_cpu(&threaded_cpu);
    c_cpu.x[0] = threaded_cpu.x[0] =
            UINT64_C(0x8877665544332211);
    c_cpu.x[1] = threaded_cpu.x[1] = DATA_PAGE + 0x180;
    c_cpu.x[3] = threaded_cpu.x[3] =
            UINT64_C(0xaabbccddeeff0011);
    c_cpu.x[4] = threaded_cpu.x[4] = UINT64_MAX;

    for (unsigned index = 0; index < array_size(instructions); index++) {
        struct aarch64_step_result c_result =
                aarch64_run_one(&c_runner, &c_cpu);
        struct aarch64_step_result threaded_result =
                aarch64_run_one(&threaded_runner, &threaded_cpu);
        assert(c_result.stop == AARCH64_STEP_RETIRED);
        assert_step_equal(&c_result, &threaded_result);
        assert_cpu_equal(&c_cpu, &threaded_cpu);
        assert_memory_equal(&c_fixture.memory,
                &threaded_fixture.memory);
        if (index == 0)
            assert(c_cpu.exclusive.valid);
        if (index == 1) {
            assert(c_cpu.exclusive.valid);
            assert_stats(&threaded_runner, 0, 2, 1, 1);
        }
    }
    assert(c_cpu.x[4] == 1);
    assert(!c_cpu.exclusive.valid);
    assert(c_cpu.x[2] == UINT64_C(0x1020304050607080));
    assert(c_cpu.x[3] == UINT64_C(0xaabbccddeeff0011));
    assert_stats(&threaded_runner, 0, 3, 1, 2);
    const byte_t expected[] = {
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
    };
    assert(memcmp(c_fixture.memory.data + 0x180,
            expected, sizeof(expected)) == 0);
}

static void run_fast_store_simd_imm12_case(byte_t size_shift,
        word_t immediate, byte_t rn, byte_t rt, dword_t expected_word) {
    static const union aarch64_vector_reg source = {
        .d = {
            UINT64_C(0x0123456789abcdef),
            UINT64_C(0xfedcba9876543210),
        },
    };
    const size_t target_offset = 0x203;
    byte_t size = (byte_t) (UINT8_C(1) << size_shift);
    qword_t byte_offset = (qword_t) immediate * size;
    dword_t instruction = encode_store_simd_imm12(
            size_shift, immediate, rn, rt);
    if (expected_word != 0)
        assert(instruction == expected_word);

    struct test_fixture c_fixture;
    struct test_fixture threaded_fixture;
    init_fixture(&c_fixture);
    init_fixture(&threaded_fixture);
    memset(c_fixture.memory.data, 0xa5,
            sizeof(c_fixture.memory.data));
    memset(threaded_fixture.memory.data, 0xa5,
            sizeof(threaded_fixture.memory.data));
    write_instruction(&c_fixture.tlb, CODE_PAGE, instruction);
    write_instruction(&threaded_fixture.tlb, CODE_PAGE, instruction);

    struct cpu_state initial;
    init_differential_cpu(&initial);
    initial.v[rt] = source;
    guest_addr_t base = DATA_PAGE + target_offset - byte_offset;
    if (rn == 31)
        initial.sp = base;
    else
        initial.x[rn] = base;
    aarch64_set_exclusive(&initial, DATA_PAGE + 0x380,
            8, false, UINT64_C(0x1122), 0, NULL, 3, 5, 7);

    struct cpu_state result = run_fast_differential_fixtures(
            instruction, initial, AARCH64_STEP_RETIRED,
            &c_fixture, &threaded_fixture, NULL);
    assert(memcmp(c_fixture.memory.data + target_offset,
            source.b, size) == 0);
    assert(c_fixture.memory.data[target_offset - 1] == 0xa5);
    assert(c_fixture.memory.data[target_offset + size] == 0xa5);
    if (rn == 31)
        assert(result.sp == base);
    else
        assert(result.x[rn] == base);
    assert(memcmp(&result.v[rt], &source, sizeof(source)) == 0);
    assert(result.pc == CODE_PAGE + 4);
    assert(result.cycle == initial.cycle + 1);
    assert(result.nzcv == initial.nzcv);
    assert(result.fpcr == initial.fpcr);
    assert(result.fpsr == initial.fpsr);
    assert(result.exclusive.valid);
    assert(result.exclusive.address == DATA_PAGE + 0x380);
    assert(result.exclusive.write_epoch == 5);
    assert(result.exclusive.sync_identity == 7);
}

static void test_fast_store_simd_imm12_differential(void) {
    static const word_t immediates[] = {0, 1, UINT16_C(0xfff)};
    unsigned case_count = 0;
    for (byte_t size_shift = 0; size_shift <= 4; size_shift++) {
        for (unsigned immediate_index = 0;
                immediate_index < array_size(immediates);
                immediate_index++) {
            run_fast_store_simd_imm12_case(size_shift,
                    immediates[immediate_index], 5, 4, 0);
            case_count++;
        }
    }
    assert(case_count == 15);

    run_fast_store_simd_imm12_case(4, 0, 0, 0,
            INSTRUCTION_STR_Q0_X0);
    run_fast_store_simd_imm12_case(4, 2, 0, 31,
            INSTRUCTION_STR_Q31_X0_32);
    run_fast_store_simd_imm12_case(4, UINT16_C(0xfff), 31, 31,
            UINT32_C(0x3dbfffff));
    run_fast_store_simd_imm12_case(4, 1, 7, 7,
            UINT32_C(0x3d8004e7));

    static const union aarch64_vector_reg source = {
        .d = {
            UINT64_C(0x8877665544332211),
            UINT64_C(0x1020304050607080),
        },
    };
    const dword_t cross_page =
            encode_store_simd_imm12(4, 0, 4, 2);
    assert(cross_page == UINT32_C(0x3d800082));
    struct test_fixture c_fixture;
    struct test_fixture threaded_fixture;
    init_fixture(&c_fixture);
    init_fixture(&threaded_fixture);
    memset(c_fixture.memory.primary_code +
            GUEST_MEMORY_PAGE_SIZE - 8, 0xa5, 8);
    memset(threaded_fixture.memory.primary_code +
            GUEST_MEMORY_PAGE_SIZE - 8, 0xa5, 8);
    memset(c_fixture.memory.data, 0xa5, 10);
    memset(threaded_fixture.memory.data, 0xa5, 10);
    write_instruction(&c_fixture.tlb, CODE_PAGE, cross_page);
    write_instruction(&threaded_fixture.tlb, CODE_PAGE, cross_page);
    struct cpu_state initial;
    init_differential_cpu(&initial);
    initial.x[4] = DATA_PAGE - 7;
    initial.v[2] = source;
    aarch64_set_exclusive(&initial, DATA_PAGE + 0x380,
            8, false, UINT64_C(0x1122), 0, NULL, 3, 5, 7);
    struct cpu_state result = run_fast_differential_fixtures(
            cross_page, initial, AARCH64_STEP_RETIRED,
            &c_fixture, &threaded_fixture, NULL);
    assert(c_fixture.memory.primary_code[
            GUEST_MEMORY_PAGE_SIZE - 8] == 0xa5);
    assert(memcmp(c_fixture.memory.primary_code +
            GUEST_MEMORY_PAGE_SIZE - 7, source.b, 7) == 0);
    assert(memcmp(c_fixture.memory.data, source.b + 7, 9) == 0);
    assert(c_fixture.memory.data[9] == 0xa5);
    assert(result.x[4] == DATA_PAGE - 7);
    assert(memcmp(&result.v[2], &source, sizeof(source)) == 0);
    assert(result.exclusive.valid);
}

static void test_fast_store_simd_imm12_faults(void) {
    static const union aarch64_vector_reg source = {
        .d = {
            UINT64_C(0x0123456789abcdef),
            UINT64_C(0xfedcba9876543210),
        },
    };
    const dword_t instruction =
            encode_store_simd_imm12(4, 0, 4, 2);
    struct test_fixture c_fixture;
    struct test_fixture threaded_fixture;
    init_fixture(&c_fixture);
    init_fixture(&threaded_fixture);
    memset(c_fixture.memory.data + GUEST_MEMORY_PAGE_SIZE - 8,
            0xa5, 8);
    memset(threaded_fixture.memory.data + GUEST_MEMORY_PAGE_SIZE - 8,
            0xa5, 8);
    write_instruction(&c_fixture.tlb, CODE_PAGE, instruction);
    write_instruction(&threaded_fixture.tlb, CODE_PAGE, instruction);
    struct cpu_state initial;
    init_differential_cpu(&initial);
    initial.x[4] = DATA_PAGE + GUEST_MEMORY_PAGE_SIZE - 8;
    initial.v[2] = source;
    aarch64_set_exclusive(&initial, DATA_PAGE + 0x300,
            8, false, UINT64_C(0x1122), 0, NULL, 3, 5, 7);
    struct aarch64_step_result step;
    struct cpu_state result = run_fast_differential_fixtures(
            instruction, initial, AARCH64_STEP_DATA_FAULT,
            &c_fixture, &threaded_fixture, &step);
    assert(step.fault.address == DATA_PAGE + GUEST_MEMORY_PAGE_SIZE);
    assert(step.fault.access == GUEST_MEMORY_WRITE);
    assert(step.fault.kind == GUEST_MEMORY_FAULT_UNMAPPED);
    for (byte_t byte = 0; byte < 8; byte++) {
        assert(c_fixture.memory.data[
                GUEST_MEMORY_PAGE_SIZE - 8 + byte] == 0xa5);
    }
    assert(result.x[4] == initial.x[4]);
    assert(memcmp(&result.v[2], &source, sizeof(source)) == 0);
    assert(result.pc == CODE_PAGE);
    assert(result.cycle == initial.cycle);
    assert(!result.exclusive.valid);
    assert(result.exclusive.write_epoch == 5);
    assert(result.exclusive.sync_identity == 7);

    const dword_t permission =
            encode_store_simd_imm12(4, 1, 4, 2);
    init_fixture(&c_fixture);
    init_fixture(&threaded_fixture);
    memset(c_fixture.memory.primary_code + 0x100, 0xa5, 16);
    memset(threaded_fixture.memory.primary_code + 0x100, 0xa5, 16);
    write_instruction(&c_fixture.tlb, CODE_PAGE, permission);
    write_instruction(&threaded_fixture.tlb, CODE_PAGE, permission);
    c_fixture.memory.code_permissions = GUEST_MEMORY_EXECUTE;
    threaded_fixture.memory.code_permissions = GUEST_MEMORY_EXECUTE;
    guest_address_space_changed(&c_fixture.space);
    guest_address_space_changed(&threaded_fixture.space);
    init_differential_cpu(&initial);
    initial.x[4] = CODE_PAGE + 0x100 - 16;
    initial.v[2] = source;
    aarch64_set_exclusive(&initial, DATA_PAGE + 0x300,
            8, false, UINT64_C(0x1122), 0, NULL, 3, 5, 7);
    result = run_fast_differential_fixtures(
            permission, initial, AARCH64_STEP_DATA_FAULT,
            &c_fixture, &threaded_fixture, &step);
    assert(step.fault.address == CODE_PAGE + 0x100);
    assert(step.fault.access == GUEST_MEMORY_WRITE);
    assert(step.fault.kind == GUEST_MEMORY_FAULT_PERMISSION);
    for (byte_t byte = 0; byte < 16; byte++)
        assert(c_fixture.memory.primary_code[0x100 + byte] == 0xa5);
    assert(result.x[4] == initial.x[4]);
    assert(result.pc == CODE_PAGE);
    assert(result.cycle == initial.cycle);
    assert(!result.exclusive.valid);

    init_fixture(&c_fixture);
    init_fixture(&threaded_fixture);
    write_instruction(&c_fixture.tlb, CODE_PAGE, instruction);
    write_instruction(&threaded_fixture.tlb, CODE_PAGE, instruction);
    init_differential_cpu(&initial);
    initial.x[4] = (UINT64_C(1) << 48) - 8;
    initial.v[2] = source;
    aarch64_set_exclusive(&initial, DATA_PAGE + 0x300,
            8, false, UINT64_C(0x1122), 0, NULL, 3, 5, 7);
    result = run_fast_differential_fixtures(
            instruction, initial, AARCH64_STEP_DATA_FAULT,
            &c_fixture, &threaded_fixture, &step);
    assert(step.fault.address == (UINT64_C(1) << 48));
    assert(step.fault.access == GUEST_MEMORY_WRITE);
    assert(step.fault.kind == GUEST_MEMORY_FAULT_ADDRESS_SIZE);
    assert(result.x[4] == initial.x[4]);
    assert(result.pc == CODE_PAGE);
    assert(result.cycle == initial.cycle);
    assert(!result.exclusive.valid);
}

static void test_fast_store_simd_imm12_exclusive_invalidation(void) {
    struct test_fixture c_fixture;
    struct test_fixture threaded_fixture;
    init_fixture(&c_fixture);
    init_fixture(&threaded_fixture);
    const dword_t store =
            encode_store_simd_imm12(4, 0, 1, 0);
    assert(store == UINT32_C(0x3d800020));
    const dword_t instructions[] = {
        INSTRUCTION_LDXR_X2_X1,
        store,
        INSTRUCTION_STXR_W4_X3_X1,
    };
    for (unsigned index = 0; index < array_size(instructions); index++) {
        write_instruction(&c_fixture.tlb,
                CODE_PAGE + index * 4, instructions[index]);
        write_instruction(&threaded_fixture.tlb,
                CODE_PAGE + index * 4, instructions[index]);
    }
    put_value(c_fixture.memory.data + 0x180, 8,
            UINT64_C(0x0102030405060708));
    put_value(threaded_fixture.memory.data + 0x180, 8,
            UINT64_C(0x0102030405060708));

    struct aarch64_runner c_runner;
    struct aarch64_runner threaded_runner;
    assert(aarch64_runner_init_backend(
            &c_runner, &c_fixture.tlb, AARCH64_BACKEND_C));
    assert(aarch64_runner_init_backend(&threaded_runner,
            &threaded_fixture.tlb, AARCH64_BACKEND_THREADED));
    struct cpu_state c_cpu;
    struct cpu_state threaded_cpu;
    init_differential_cpu(&c_cpu);
    init_differential_cpu(&threaded_cpu);
    c_cpu.x[1] = threaded_cpu.x[1] = DATA_PAGE + 0x180;
    c_cpu.x[3] = threaded_cpu.x[3] =
            UINT64_C(0xaabbccddeeff0011);
    c_cpu.x[4] = threaded_cpu.x[4] = UINT64_MAX;
    c_cpu.v[0].d[0] = threaded_cpu.v[0].d[0] =
            UINT64_C(0x8877665544332211);
    c_cpu.v[0].d[1] = threaded_cpu.v[0].d[1] =
            UINT64_C(0x1020304050607080);

    for (unsigned index = 0; index < array_size(instructions); index++) {
        struct aarch64_step_result c_result =
                aarch64_run_one(&c_runner, &c_cpu);
        struct aarch64_step_result threaded_result =
                aarch64_run_one(&threaded_runner, &threaded_cpu);
        assert(c_result.stop == AARCH64_STEP_RETIRED);
        assert_step_equal(&c_result, &threaded_result);
        assert_cpu_equal(&c_cpu, &threaded_cpu);
        assert_memory_equal(&c_fixture.memory,
                &threaded_fixture.memory);
        if (index == 0)
            assert(c_cpu.exclusive.valid);
        if (index == 1) {
            assert(c_cpu.exclusive.valid);
            assert(!guest_address_space_exclusive_matches(
                    &c_fixture.space, DATA_PAGE + 0x180,
                    c_cpu.exclusive.write_epoch));
            assert(!guest_address_space_exclusive_matches(
                    &threaded_fixture.space, DATA_PAGE + 0x180,
                    threaded_cpu.exclusive.write_epoch));
            assert_stats(&threaded_runner, 0, 2, 1, 1);
        }
    }
    assert(c_cpu.x[2] == UINT64_C(0x0102030405060708));
    assert(c_cpu.x[4] == 1);
    assert(!c_cpu.exclusive.valid);
    assert_stats(&threaded_runner, 0, 3, 1, 2);
    assert(memcmp(c_fixture.memory.data + 0x180,
            &c_cpu.v[0], sizeof(c_cpu.v[0])) == 0);
}

static void run_fast_load_simd_imm12_case(byte_t size_shift,
        word_t immediate, byte_t rn, byte_t rt,
        dword_t expected_word) {
    static const union aarch64_vector_reg source = {
        .d = {
            UINT64_C(0x0123456789abcdef),
            UINT64_C(0xfedcba9876543210),
        },
    };
    const size_t target_offset = 0x243;
    byte_t size = (byte_t) (UINT8_C(1) << size_shift);
    qword_t byte_offset = (qword_t) immediate * size;
    dword_t instruction = encode_load_simd_imm12(
            size_shift, immediate, rn, rt);
    if (expected_word != 0)
        assert(instruction == expected_word);

    struct aarch64_decoded decoded;
    assert(aarch64_decode(instruction, &decoded));
    assert(decoded.opcode == AARCH64_OP_LOAD_SIMD_IMM12);
    assert(decoded.width == size * 8);
    assert(decoded.operands.load_store.size == size);
    assert(decoded.operands.load_store.rn == rn);
    assert(decoded.operands.load_store.rt == rt);
    assert(decoded.operands.load_store.offset == (int64_t) byte_offset);
    assert(decoded.operands.load_store.address_mode ==
            AARCH64_ADDRESS_OFFSET);

    struct test_fixture c_fixture;
    struct test_fixture threaded_fixture;
    init_fixture(&c_fixture);
    init_fixture(&threaded_fixture);
    memset(c_fixture.memory.data, 0xa5,
            sizeof(c_fixture.memory.data));
    memset(threaded_fixture.memory.data, 0xa5,
            sizeof(threaded_fixture.memory.data));
    memcpy(c_fixture.memory.data + target_offset, source.b, size);
    memcpy(threaded_fixture.memory.data + target_offset,
            source.b, size);
    write_instruction(&c_fixture.tlb, CODE_PAGE, instruction);
    write_instruction(&threaded_fixture.tlb, CODE_PAGE, instruction);
    struct test_memory expected_memory = c_fixture.memory;

    struct cpu_state initial;
    init_differential_cpu(&initial);
    memset(initial.v[rt].b, 0x5a, sizeof(initial.v[rt].b));
    guest_addr_t base = DATA_PAGE + target_offset - byte_offset;
    if (rn == 31)
        initial.sp = base;
    else
        initial.x[rn] = base;
    aarch64_set_exclusive(&initial, DATA_PAGE + 0x380,
            8, false, UINT64_C(0x1122), 0, NULL, 3, 5, 7);

    struct cpu_state expected = initial;
    expected.v[rt] = (union aarch64_vector_reg) {0};
    memcpy(expected.v[rt].b, source.b, size);
    expected.pc += 4;
    expected.cycle++;
    struct cpu_state result = run_fast_differential_fixtures(
            instruction, initial, AARCH64_STEP_RETIRED,
            &c_fixture, &threaded_fixture, NULL);
    assert_cpu_equal(&result, &expected);
    assert_memory_equal(&c_fixture.memory, &expected_memory);
    assert_memory_equal(&threaded_fixture.memory, &expected_memory);
}

static void test_fast_load_simd_imm12_differential(void) {
    static const word_t immediates[] = {0, 1, UINT16_C(0xfff)};
    static const dword_t expected_words[][3] = {
        {
            UINT32_C(0x3d4000a4),
            UINT32_C(0x3d4004a4),
            UINT32_C(0x3d7ffca4),
        },
        {
            UINT32_C(0x7d4000a4),
            UINT32_C(0x7d4004a4),
            UINT32_C(0x7d7ffca4),
        },
        {
            UINT32_C(0xbd4000a4),
            UINT32_C(0xbd4004a4),
            UINT32_C(0xbd7ffca4),
        },
        {
            UINT32_C(0xfd4000a4),
            UINT32_C(0xfd4004e7),
            UINT32_C(0xfd7ffca4),
        },
        {
            UINT32_C(0x3dc000a4),
            UINT32_C(0x3dc004a4),
            UINT32_C(0x3dffffff),
        },
    };
    _Static_assert(array_size(expected_words) == 5,
            "LOAD SIMD imm12 必须覆盖五种合法宽度");
    unsigned case_count = 0;
    for (byte_t size_shift = 0;
            size_shift < array_size(expected_words); size_shift++) {
        for (unsigned immediate_index = 0;
                immediate_index < array_size(immediates);
                immediate_index++) {
            byte_t rn = 5;
            byte_t rt = 4;
            if (size_shift == 3 && immediate_index == 1) {
                rn = 7;
                rt = 7;
            } else if (size_shift == 4 && immediate_index == 2) {
                rn = 31;
                rt = 31;
            }
            run_fast_load_simd_imm12_case(size_shift,
                    immediates[immediate_index], rn, rt,
                    expected_words[size_shift][immediate_index]);
            case_count++;
        }
    }
    assert(case_count == 15);

    run_fast_load_simd_imm12_case(
            4, UINT16_C(0xd6), 0, 31,
            INSTRUCTION_LDR_Q31_X0_D60);

    const dword_t cross_page =
            encode_load_simd_imm12(4, 0, 4, 2);
    assert(cross_page == UINT32_C(0x3dc00082));
    byte_t source[sizeof(union aarch64_vector_reg)];
    for (byte_t index = 0; index < sizeof(source); index++)
        source[index] = (byte_t) (0x21 + index * 3);
    struct test_fixture c_fixture;
    struct test_fixture threaded_fixture;
    init_fixture(&c_fixture);
    init_fixture(&threaded_fixture);
    memcpy(c_fixture.memory.primary_code +
            GUEST_MEMORY_PAGE_SIZE - 7, source, 7);
    memcpy(c_fixture.memory.data, source + 7, sizeof(source) - 7);
    memcpy(threaded_fixture.memory.primary_code +
            GUEST_MEMORY_PAGE_SIZE - 7, source, 7);
    memcpy(threaded_fixture.memory.data,
            source + 7, sizeof(source) - 7);
    write_instruction(&c_fixture.tlb, CODE_PAGE, cross_page);
    write_instruction(&threaded_fixture.tlb, CODE_PAGE, cross_page);
    struct test_memory expected_memory = c_fixture.memory;
    struct cpu_state initial;
    init_differential_cpu(&initial);
    initial.x[4] = DATA_PAGE - 7;
    memset(initial.v[2].b, 0x5a, sizeof(initial.v[2].b));
    aarch64_set_exclusive(&initial, DATA_PAGE + 0x380,
            8, false, UINT64_C(0x1122), 0, NULL, 3, 5, 7);
    struct cpu_state expected = initial;
    memcpy(expected.v[2].b, source, sizeof(source));
    expected.pc += 4;
    expected.cycle++;
    struct cpu_state result = run_fast_differential_fixtures(
            cross_page, initial, AARCH64_STEP_RETIRED,
            &c_fixture, &threaded_fixture, NULL);
    assert_cpu_equal(&result, &expected);
    assert_memory_equal(&c_fixture.memory, &expected_memory);
    assert_memory_equal(&threaded_fixture.memory, &expected_memory);
}

static void test_fast_load_simd_imm12_faults(void) {
    const dword_t unscaled =
            encode_load_simd_imm12(4, 0, 4, 2);
    const dword_t scaled =
            encode_load_simd_imm12(4, 1, 4, 2);
    struct test_fixture c_fixture;
    struct test_fixture threaded_fixture;
    struct cpu_state initial;
    struct cpu_state expected;
    struct cpu_state result;
    struct aarch64_step_result step;

    init_fixture(&c_fixture);
    init_fixture(&threaded_fixture);
    write_instruction(&c_fixture.tlb, CODE_PAGE, unscaled);
    write_instruction(&threaded_fixture.tlb, CODE_PAGE, unscaled);
    struct test_memory expected_memory = c_fixture.memory;
    init_differential_cpu(&initial);
    initial.x[4] = DATA_PAGE + GUEST_MEMORY_PAGE_SIZE - 8;
    memset(initial.v[2].b, 0x5a, sizeof(initial.v[2].b));
    aarch64_set_exclusive(&initial, DATA_PAGE + 0x380,
            8, false, UINT64_C(0x1122), 0, NULL, 3, 5, 7);
    expected = initial;
    expected.exclusive.valid = false;
    result = run_fast_differential_fixtures(
            unscaled, initial, AARCH64_STEP_DATA_FAULT,
            &c_fixture, &threaded_fixture, &step);
    assert(step.fault.address == DATA_PAGE + GUEST_MEMORY_PAGE_SIZE);
    assert(step.fault.access == GUEST_MEMORY_READ);
    assert(step.fault.kind == GUEST_MEMORY_FAULT_UNMAPPED);
    assert_cpu_equal(&result, &expected);
    assert_memory_equal(&c_fixture.memory, &expected_memory);
    assert_memory_equal(&threaded_fixture.memory, &expected_memory);

    init_fixture(&c_fixture);
    init_fixture(&threaded_fixture);
    write_instruction(&c_fixture.tlb, CODE_PAGE, scaled);
    write_instruction(&threaded_fixture.tlb, CODE_PAGE, scaled);
    c_fixture.memory.code_permissions = GUEST_MEMORY_EXECUTE;
    threaded_fixture.memory.code_permissions = GUEST_MEMORY_EXECUTE;
    guest_address_space_changed(&c_fixture.space);
    guest_address_space_changed(&threaded_fixture.space);
    expected_memory = c_fixture.memory;
    init_differential_cpu(&initial);
    initial.x[4] = CODE_PAGE + 0x100 - 16;
    memset(initial.v[2].b, 0x5a, sizeof(initial.v[2].b));
    aarch64_set_exclusive(&initial, DATA_PAGE + 0x380,
            8, false, UINT64_C(0x1122), 0, NULL, 3, 5, 7);
    expected = initial;
    expected.exclusive.valid = false;
    result = run_fast_differential_fixtures(
            scaled, initial, AARCH64_STEP_DATA_FAULT,
            &c_fixture, &threaded_fixture, &step);
    assert(step.fault.address == CODE_PAGE + 0x100);
    assert(step.fault.access == GUEST_MEMORY_READ);
    assert(step.fault.kind == GUEST_MEMORY_FAULT_PERMISSION);
    assert_cpu_equal(&result, &expected);
    assert_memory_equal(&c_fixture.memory, &expected_memory);
    assert_memory_equal(&threaded_fixture.memory, &expected_memory);

    init_fixture(&c_fixture);
    init_fixture(&threaded_fixture);
    write_instruction(&c_fixture.tlb, CODE_PAGE, scaled);
    write_instruction(&threaded_fixture.tlb, CODE_PAGE, scaled);
    expected_memory = c_fixture.memory;
    init_differential_cpu(&initial);
    initial.x[4] = (UINT64_C(1) << 48) - 24;
    memset(initial.v[2].b, 0x5a, sizeof(initial.v[2].b));
    aarch64_set_exclusive(&initial, DATA_PAGE + 0x380,
            8, false, UINT64_C(0x1122), 0, NULL, 3, 5, 7);
    expected = initial;
    expected.exclusive.valid = false;
    result = run_fast_differential_fixtures(
            scaled, initial, AARCH64_STEP_DATA_FAULT,
            &c_fixture, &threaded_fixture, &step);
    assert(step.fault.address == (UINT64_C(1) << 48));
    assert(step.fault.access == GUEST_MEMORY_READ);
    assert(step.fault.kind == GUEST_MEMORY_FAULT_ADDRESS_SIZE);
    assert_cpu_equal(&result, &expected);
    assert_memory_equal(&c_fixture.memory, &expected_memory);
    assert_memory_equal(&threaded_fixture.memory, &expected_memory);
}

static void test_fast_load_simd_imm12_preserves_exclusive(void) {
    const dword_t instructions[] = {
        INSTRUCTION_LDXR_X2_X1,
        INSTRUCTION_LDR_Q31_X0_D60,
        INSTRUCTION_STXR_W6_X3_X1,
    };
    static const union aarch64_vector_reg source = {
        .d = {
            UINT64_C(0x0123456789abcdef),
            UINT64_C(0xfedcba9876543210),
        },
    };
    struct test_fixture c_fixture;
    struct test_fixture threaded_fixture;
    init_fixture(&c_fixture);
    init_fixture(&threaded_fixture);
    for (unsigned index = 0; index < array_size(instructions); index++) {
        write_instruction(&c_fixture.tlb,
                CODE_PAGE + index * 4, instructions[index]);
        write_instruction(&threaded_fixture.tlb,
                CODE_PAGE + index * 4, instructions[index]);
    }
    memcpy(c_fixture.memory.data + 0x180,
            source.b, sizeof(source.b));
    memcpy(threaded_fixture.memory.data + 0x180,
            source.b, sizeof(source.b));

    struct aarch64_runner c_runner;
    struct aarch64_runner threaded_runner;
    assert(aarch64_runner_init_backend(
            &c_runner, &c_fixture.tlb, AARCH64_BACKEND_C));
    assert(aarch64_runner_init_backend(&threaded_runner,
            &threaded_fixture.tlb, AARCH64_BACKEND_THREADED));
    struct cpu_state c_cpu;
    struct cpu_state threaded_cpu;
    init_differential_cpu(&c_cpu);
    init_differential_cpu(&threaded_cpu);
    c_cpu.x[0] = threaded_cpu.x[0] =
            DATA_PAGE + 0x180 - UINT64_C(0xd60);
    c_cpu.x[1] = threaded_cpu.x[1] = DATA_PAGE + 0x180;
    c_cpu.x[3] = threaded_cpu.x[3] =
            UINT64_C(0xaabbccddeeff0011);
    c_cpu.x[6] = threaded_cpu.x[6] = UINT64_MAX;
    memset(c_cpu.v[31].b, 0x5a, sizeof(c_cpu.v[31].b));
    threaded_cpu.v[31] = c_cpu.v[31];

    for (unsigned index = 0; index < array_size(instructions); index++) {
        struct aarch64_step_result c_result =
                aarch64_run_one(&c_runner, &c_cpu);
        struct aarch64_step_result threaded_result =
                aarch64_run_one(&threaded_runner, &threaded_cpu);
        assert(c_result.stop == AARCH64_STEP_RETIRED);
        assert_step_equal(&c_result, &threaded_result);
        assert_cpu_equal(&c_cpu, &threaded_cpu);
        assert_memory_equal(&c_fixture.memory,
                &threaded_fixture.memory);
        if (index == 0) {
            assert(c_cpu.exclusive.valid);
            assert_stats(&threaded_runner, 0, 1, 0, 1);
        } else if (index == 1) {
            assert(memcmp(c_cpu.v[31].b,
                    source.b, sizeof(source.b)) == 0);
            assert(c_cpu.exclusive.valid);
            assert(guest_address_space_exclusive_matches(
                    &c_fixture.space, DATA_PAGE + 0x180,
                    c_cpu.exclusive.write_epoch));
            assert(guest_address_space_exclusive_matches(
                    &threaded_fixture.space, DATA_PAGE + 0x180,
                    threaded_cpu.exclusive.write_epoch));
            assert_stats(&threaded_runner, 0, 2, 1, 1);
        }
    }
    assert(c_cpu.x[6] == 0);
    assert(!c_cpu.exclusive.valid);
    assert(memcmp(c_cpu.v[31].b,
            source.b, sizeof(source.b)) == 0);
    assert_stats(&threaded_runner, 0, 3, 1, 2);
    byte_t expected_store[8];
    put_value(expected_store, 8,
            UINT64_C(0xaabbccddeeff0011));
    assert(memcmp(c_fixture.memory.data + 0x180,
            expected_store, sizeof(expected_store)) == 0);
}

static void run_fast_store_pair_case(bool is_64,
        enum aarch64_address_mode address_mode, int64_t offset,
        byte_t rn, byte_t rt, byte_t rt2, dword_t expected_word) {
    const qword_t first_source = UINT64_C(0x8877665589abcdef);
    const qword_t second_source = UINT64_C(0x1122334401234567);
    const guest_addr_t target = DATA_PAGE + 0x300;
    byte_t size = is_64 ? 8 : 4;
    dword_t instruction = encode_store_pair(
            is_64, address_mode, offset, rn, rt, rt2);
    if (expected_word != 0)
        assert(instruction == expected_word);

    struct test_fixture c_fixture;
    struct test_fixture threaded_fixture;
    init_fixture(&c_fixture);
    init_fixture(&threaded_fixture);
    memset(c_fixture.memory.data, 0xa5,
            sizeof(c_fixture.memory.data));
    memset(threaded_fixture.memory.data, 0xa5,
            sizeof(threaded_fixture.memory.data));
    write_instruction(&c_fixture.tlb, CODE_PAGE, instruction);
    write_instruction(&threaded_fixture.tlb, CODE_PAGE, instruction);

    struct cpu_state initial;
    init_differential_cpu(&initial);
    if (rt != 31)
        initial.x[rt] = first_source;
    if (rt2 != 31)
        initial.x[rt2] = second_source;
    guest_addr_t base = address_mode == AARCH64_ADDRESS_POST_INDEX ?
            target : target - (qword_t) offset;
    if (rn == 31)
        initial.sp = base;
    else
        initial.x[rn] = base;
    qword_t first = rt == 31 ? 0 : initial.x[rt];
    qword_t second = rt2 == 31 ? 0 : initial.x[rt2];
    if (!is_64) {
        first &= UINT32_MAX;
        second &= UINT32_MAX;
    }
    aarch64_set_exclusive(&initial, DATA_PAGE + 0x80,
            8, false, UINT64_C(0x1122), 0, NULL, 3, 5, 7);

    struct cpu_state result = run_fast_differential_fixtures(
            instruction, initial, AARCH64_STEP_RETIRED,
            &c_fixture, &threaded_fixture, NULL);
    byte_t expected[16] = {0};
    put_value(expected, size, first);
    put_value(expected + size, size, second);
    assert(memcmp(c_fixture.memory.data + 0x300,
            expected, (size_t) size * 2) == 0);
    assert(c_fixture.memory.data[0x2ff] == 0xa5);
    assert(c_fixture.memory.data[0x300 + size * 2] == 0xa5);

    guest_addr_t expected_base = base;
    if (address_mode != AARCH64_ADDRESS_OFFSET)
        expected_base += (qword_t) offset;
    if (rn == 31)
        assert(result.sp == expected_base);
    else
        assert(result.x[rn] == expected_base);
    assert(result.pc == CODE_PAGE + 4);
    assert(result.cycle == initial.cycle + 1);
    assert(result.nzcv == initial.nzcv);
    assert(result.fpcr == initial.fpcr);
    assert(result.fpsr == initial.fpsr);
    assert(result.exclusive.valid);
    assert(result.exclusive.write_epoch == 5);
    assert(result.exclusive.sync_identity == 7);
}

static void test_fast_store_pair_differential(void) {
    static const enum aarch64_address_mode address_modes[] = {
        AARCH64_ADDRESS_OFFSET,
        AARCH64_ADDRESS_PRE_INDEX,
        AARCH64_ADDRESS_POST_INDEX,
    };
    static const int8_t scaled_offsets[] = {-64, -1, 0, 1, 63};
    unsigned case_count = 0;
    for (unsigned width_index = 0; width_index < 2; width_index++) {
        bool is_64 = width_index != 0;
        byte_t size = is_64 ? 8 : 4;
        for (unsigned mode_index = 0;
                mode_index < array_size(address_modes); mode_index++) {
            for (unsigned offset_index = 0;
                    offset_index < array_size(scaled_offsets);
                    offset_index++) {
                run_fast_store_pair_case(is_64,
                        address_modes[mode_index],
                        (int64_t) scaled_offsets[offset_index] * size,
                        5, 4, 6, 0);
                case_count++;
            }
        }
    }
    assert(case_count == 30);

    run_fast_store_pair_case(true, AARCH64_ADDRESS_PRE_INDEX,
            -352, 31, 29, 30, INSTRUCTION_STP_X29_X30_SP_PRE_352);
    run_fast_store_pair_case(true, AARCH64_ADDRESS_PRE_INDEX,
            -16, 31, 29, 30, INSTRUCTION_STP_X29_X30_SP_PRE_16);
    run_fast_store_pair_case(
            true, AARCH64_ADDRESS_OFFSET, 0, 4, 4, 6, 0);
    run_fast_store_pair_case(
            true, AARCH64_ADDRESS_OFFSET, 0, 6, 4, 6, 0);
    run_fast_store_pair_case(
            true, AARCH64_ADDRESS_OFFSET, 0, 5, 6, 6, 0);
    run_fast_store_pair_case(
            true, AARCH64_ADDRESS_OFFSET, 0, 31, 31, 31, 0);

    struct test_fixture c_fixture;
    struct test_fixture threaded_fixture;
    init_fixture(&c_fixture);
    init_fixture(&threaded_fixture);
    const dword_t cross_page = encode_store_pair(
            true, AARCH64_ADDRESS_OFFSET, 0, 4, 5, 6);
    memset(c_fixture.memory.primary_code +
            GUEST_MEMORY_PAGE_SIZE - 8, 0xa5, 8);
    memset(threaded_fixture.memory.primary_code +
            GUEST_MEMORY_PAGE_SIZE - 8, 0xa5, 8);
    memset(c_fixture.memory.data, 0xa5, 10);
    memset(threaded_fixture.memory.data, 0xa5, 10);
    write_instruction(&c_fixture.tlb, CODE_PAGE, cross_page);
    write_instruction(&threaded_fixture.tlb, CODE_PAGE, cross_page);
    struct cpu_state initial;
    init_differential_cpu(&initial);
    initial.x[4] = DATA_PAGE - 7;
    initial.x[5] = UINT64_C(0x8877665544332211);
    initial.x[6] = UINT64_C(0x1020304050607080);
    run_fast_differential_fixtures(
            cross_page, initial, AARCH64_STEP_RETIRED,
            &c_fixture, &threaded_fixture, NULL);
    byte_t expected[16];
    put_value(expected, 8, initial.x[5]);
    put_value(expected + 8, 8, initial.x[6]);
    assert(c_fixture.memory.primary_code[
            GUEST_MEMORY_PAGE_SIZE - 8] == 0xa5);
    assert(memcmp(c_fixture.memory.primary_code +
            GUEST_MEMORY_PAGE_SIZE - 7, expected, 7) == 0);
    assert(memcmp(c_fixture.memory.data, expected + 7, 9) == 0);
    assert(c_fixture.memory.data[9] == 0xa5);
}

static void test_fast_store_pair_faults(void) {
    const dword_t post_index = encode_store_pair(
            true, AARCH64_ADDRESS_POST_INDEX, 16, 5, 4, 6);
    struct test_fixture c_fixture;
    struct test_fixture threaded_fixture;
    init_fixture(&c_fixture);
    init_fixture(&threaded_fixture);
    memset(c_fixture.memory.data + GUEST_MEMORY_PAGE_SIZE - 8,
            0xa5, 8);
    memset(threaded_fixture.memory.data + GUEST_MEMORY_PAGE_SIZE - 8,
            0xa5, 8);
    write_instruction(&c_fixture.tlb, CODE_PAGE, post_index);
    write_instruction(&threaded_fixture.tlb, CODE_PAGE, post_index);
    struct cpu_state initial;
    init_differential_cpu(&initial);
    initial.x[4] = UINT64_C(0x8877665544332211);
    initial.x[5] = DATA_PAGE + GUEST_MEMORY_PAGE_SIZE - 8;
    initial.x[6] = UINT64_C(0x1020304050607080);
    aarch64_set_exclusive(&initial, DATA_PAGE + 0x80,
            8, false, UINT64_C(0x1122), 0, NULL, 3, 5, 7);
    struct aarch64_step_result step;
    struct cpu_state result = run_fast_differential_fixtures(
            post_index, initial, AARCH64_STEP_DATA_FAULT,
            &c_fixture, &threaded_fixture, &step);
    assert(step.fault.address == DATA_PAGE + GUEST_MEMORY_PAGE_SIZE);
    assert(step.fault.access == GUEST_MEMORY_WRITE);
    assert(step.fault.kind == GUEST_MEMORY_FAULT_UNMAPPED);
    for (byte_t byte = 0; byte < 8; byte++) {
        assert(c_fixture.memory.data[
                GUEST_MEMORY_PAGE_SIZE - 8 + byte] == 0xa5);
    }
    assert(result.x[4] == initial.x[4]);
    assert(result.x[5] == initial.x[5]);
    assert(result.x[6] == initial.x[6]);
    assert(result.pc == CODE_PAGE);
    assert(result.cycle == initial.cycle);
    assert(!result.exclusive.valid);

    const dword_t pre_index = encode_store_pair(
            true, AARCH64_ADDRESS_PRE_INDEX, -16, 5, 4, 6);
    init_fixture(&c_fixture);
    init_fixture(&threaded_fixture);
    memset(c_fixture.memory.primary_code + 0x100, 0xa5, 16);
    memset(threaded_fixture.memory.primary_code + 0x100, 0xa5, 16);
    write_instruction(&c_fixture.tlb, CODE_PAGE, pre_index);
    write_instruction(&threaded_fixture.tlb, CODE_PAGE, pre_index);
    c_fixture.memory.code_permissions = GUEST_MEMORY_EXECUTE;
    threaded_fixture.memory.code_permissions = GUEST_MEMORY_EXECUTE;
    guest_address_space_changed(&c_fixture.space);
    guest_address_space_changed(&threaded_fixture.space);
    init_differential_cpu(&initial);
    initial.x[4] = UINT64_C(0x8877665544332211);
    initial.x[5] = CODE_PAGE + 0x110;
    initial.x[6] = UINT64_C(0x1020304050607080);
    aarch64_set_exclusive(&initial, DATA_PAGE + 0x80,
            8, false, UINT64_C(0x1122), 0, NULL, 3, 5, 7);
    result = run_fast_differential_fixtures(
            pre_index, initial, AARCH64_STEP_DATA_FAULT,
            &c_fixture, &threaded_fixture, &step);
    assert(step.fault.address == CODE_PAGE + 0x100);
    assert(step.fault.access == GUEST_MEMORY_WRITE);
    assert(step.fault.kind == GUEST_MEMORY_FAULT_PERMISSION);
    for (byte_t byte = 0; byte < 16; byte++)
        assert(c_fixture.memory.primary_code[0x100 + byte] == 0xa5);
    assert(result.x[4] == initial.x[4]);
    assert(result.x[5] == initial.x[5]);
    assert(result.x[6] == initial.x[6]);
    assert(result.pc == CODE_PAGE);
    assert(result.cycle == initial.cycle);
    assert(!result.exclusive.valid);

    const dword_t address_size = encode_store_pair(
            true, AARCH64_ADDRESS_OFFSET, 0, 5, 4, 6);
    init_fixture(&c_fixture);
    init_fixture(&threaded_fixture);
    write_instruction(&c_fixture.tlb, CODE_PAGE, address_size);
    write_instruction(&threaded_fixture.tlb,
            CODE_PAGE, address_size);
    init_differential_cpu(&initial);
    initial.x[4] = UINT64_C(0x8877665544332211);
    initial.x[5] = (UINT64_C(1) << 48) - 8;
    initial.x[6] = UINT64_C(0x1020304050607080);
    aarch64_set_exclusive(&initial, DATA_PAGE + 0x80,
            8, false, UINT64_C(0x1122), 0, NULL, 3, 5, 7);
    result = run_fast_differential_fixtures(
            address_size, initial, AARCH64_STEP_DATA_FAULT,
            &c_fixture, &threaded_fixture, &step);
    assert(step.fault.address == (UINT64_C(1) << 48));
    assert(step.fault.access == GUEST_MEMORY_WRITE);
    assert(step.fault.kind == GUEST_MEMORY_FAULT_ADDRESS_SIZE);
    assert(result.x[4] == initial.x[4]);
    assert(result.x[5] == initial.x[5]);
    assert(result.x[6] == initial.x[6]);
    assert(result.pc == CODE_PAGE);
    assert(result.cycle == initial.cycle);
    assert(!result.exclusive.valid);
}

static void test_fast_store_pair_exclusive_invalidation(void) {
    struct test_fixture c_fixture;
    struct test_fixture threaded_fixture;
    init_fixture(&c_fixture);
    init_fixture(&threaded_fixture);
    const dword_t instructions[] = {
        INSTRUCTION_LDXR_X2_X1,
        encode_store_pair(true,
                AARCH64_ADDRESS_OFFSET, 0, 1, 5, 6),
        INSTRUCTION_STXR_W4_X3_X1,
    };
    for (unsigned index = 0; index < array_size(instructions); index++) {
        write_instruction(&c_fixture.tlb,
                CODE_PAGE + index * 4, instructions[index]);
        write_instruction(&threaded_fixture.tlb,
                CODE_PAGE + index * 4, instructions[index]);
    }
    put_value(c_fixture.memory.data + 0x180, 8,
            UINT64_C(0x0102030405060708));
    put_value(threaded_fixture.memory.data + 0x180, 8,
            UINT64_C(0x0102030405060708));

    struct aarch64_runner c_runner;
    struct aarch64_runner threaded_runner;
    assert(aarch64_runner_init_backend(
            &c_runner, &c_fixture.tlb, AARCH64_BACKEND_C));
    assert(aarch64_runner_init_backend(&threaded_runner,
            &threaded_fixture.tlb, AARCH64_BACKEND_THREADED));
    struct cpu_state c_cpu;
    struct cpu_state threaded_cpu;
    init_differential_cpu(&c_cpu);
    init_differential_cpu(&threaded_cpu);
    c_cpu.x[1] = threaded_cpu.x[1] = DATA_PAGE + 0x180;
    c_cpu.x[3] = threaded_cpu.x[3] =
            UINT64_C(0xaabbccddeeff0011);
    c_cpu.x[4] = threaded_cpu.x[4] = UINT64_MAX;
    c_cpu.x[5] = threaded_cpu.x[5] =
            UINT64_C(0x8877665544332211);
    c_cpu.x[6] = threaded_cpu.x[6] =
            UINT64_C(0x1020304050607080);

    for (unsigned index = 0; index < array_size(instructions); index++) {
        struct aarch64_step_result c_result =
                aarch64_run_one(&c_runner, &c_cpu);
        struct aarch64_step_result threaded_result =
                aarch64_run_one(&threaded_runner, &threaded_cpu);
        assert(c_result.stop == AARCH64_STEP_RETIRED);
        assert_step_equal(&c_result, &threaded_result);
        assert_cpu_equal(&c_cpu, &threaded_cpu);
        assert_memory_equal(&c_fixture.memory,
                &threaded_fixture.memory);
        if (index == 0)
            assert(c_cpu.exclusive.valid);
        if (index == 1) {
            assert(c_cpu.exclusive.valid);
            assert_stats(&threaded_runner, 0, 2, 1, 1);
        }
    }
    assert(c_cpu.x[2] == UINT64_C(0x0102030405060708));
    assert(c_cpu.x[4] == 1);
    assert(!c_cpu.exclusive.valid);
    assert_stats(&threaded_runner, 0, 3, 1, 2);
    byte_t expected[16];
    put_value(expected, 8, c_cpu.x[5]);
    put_value(expected + 8, 8, c_cpu.x[6]);
    assert(memcmp(c_fixture.memory.data + 0x180,
            expected, sizeof(expected)) == 0);
}

static void run_fast_store_simd_pair_case(byte_t operation, byte_t mode,
        int64_t offset, byte_t rn, byte_t rt, byte_t rt2,
        dword_t expected_word) {
    const union aarch64_vector_reg first_source = {
        .d = {
            UINT64_C(0x0123456789abcdef),
            UINT64_C(0xfedcba9876543210),
        },
    };
    const union aarch64_vector_reg second_source = {
        .d = {
            UINT64_C(0x8877665544332211),
            UINT64_C(0x1020304050607080),
        },
    };
    const guest_addr_t target = DATA_PAGE + 0x300;
    byte_t size = (byte_t) (UINT8_C(1) << (operation + 2));
    dword_t instruction = encode_store_simd_pair(
            operation, mode, offset, rn, rt, rt2);
    if (expected_word != 0)
        assert(instruction == expected_word);

    struct test_fixture c_fixture;
    struct test_fixture threaded_fixture;
    init_fixture(&c_fixture);
    init_fixture(&threaded_fixture);
    memset(c_fixture.memory.data, 0xa5,
            sizeof(c_fixture.memory.data));
    memset(threaded_fixture.memory.data, 0xa5,
            sizeof(threaded_fixture.memory.data));
    write_instruction(&c_fixture.tlb, CODE_PAGE, instruction);
    write_instruction(&threaded_fixture.tlb, CODE_PAGE, instruction);

    struct cpu_state initial;
    init_differential_cpu(&initial);
    initial.v[rt] = first_source;
    initial.v[rt2] = second_source;
    enum aarch64_address_mode address_mode = AARCH64_ADDRESS_OFFSET;
    if (mode == 1)
        address_mode = AARCH64_ADDRESS_POST_INDEX;
    else if (mode == 3)
        address_mode = AARCH64_ADDRESS_PRE_INDEX;
    guest_addr_t base = address_mode == AARCH64_ADDRESS_POST_INDEX ?
            target : target - (qword_t) offset;
    if (rn == 31)
        initial.sp = base;
    else
        initial.x[rn] = base;
    union aarch64_vector_reg first = initial.v[rt];
    union aarch64_vector_reg second = initial.v[rt2];
    aarch64_set_exclusive(&initial, DATA_PAGE + 0x80,
            8, false, UINT64_C(0x1122), 0, NULL, 3, 5, 7);

    struct cpu_state result = run_fast_differential_fixtures(
            instruction, initial, AARCH64_STEP_RETIRED,
            &c_fixture, &threaded_fixture, NULL);
    byte_t expected[2 * sizeof(union aarch64_vector_reg)] = {0};
    memcpy(expected, first.b, size);
    memcpy(expected + size, second.b, size);
    assert(memcmp(c_fixture.memory.data + 0x300,
            expected, (size_t) size * 2) == 0);
    assert(c_fixture.memory.data[0x2ff] == 0xa5);
    assert(c_fixture.memory.data[0x300 + size * 2] == 0xa5);

    guest_addr_t expected_base = base;
    if (address_mode != AARCH64_ADDRESS_OFFSET)
        expected_base += (qword_t) offset;
    if (rn == 31)
        assert(result.sp == expected_base);
    else
        assert(result.x[rn] == expected_base);
    assert(result.pc == CODE_PAGE + 4);
    assert(result.cycle == initial.cycle + 1);
    assert(result.nzcv == initial.nzcv);
    assert(result.fpcr == initial.fpcr);
    assert(result.fpsr == initial.fpsr);
    assert(result.exclusive.valid);
    assert(result.exclusive.write_epoch == 5);
    assert(result.exclusive.sync_identity == 7);
}

static void test_fast_store_simd_pair_differential(void) {
    static const int8_t scaled_offsets[] = {-64, -1, 0, 1, 63};
    unsigned case_count = 0;
    for (byte_t operation = 0; operation < 3; operation++) {
        byte_t size = (byte_t) (UINT8_C(1) << (operation + 2));
        for (byte_t mode = 0; mode < 4; mode++) {
            for (unsigned offset_index = 0;
                    offset_index < array_size(scaled_offsets);
                    offset_index++) {
                run_fast_store_simd_pair_case(operation, mode,
                        (int64_t) scaled_offsets[offset_index] * size,
                        5, 4, 6, 0);
                case_count++;
            }
        }
    }
    assert(case_count == 60);

    run_fast_store_simd_pair_case(2, 2, 32, 3, 0, 0,
            INSTRUCTION_STP_Q0_Q0_X3_32);
    run_fast_store_simd_pair_case(2, 2, 0, 0, 31, 31,
            INSTRUCTION_STP_Q31_Q31_X0);
    run_fast_store_simd_pair_case(2, 3, -32, 31, 31, 30,
            INSTRUCTION_STP_Q31_Q30_SP_PRE_32);
    run_fast_store_simd_pair_case(2, 1, 32, 31, 0, 1,
            INSTRUCTION_STP_Q0_Q1_SP_POST_32);
    run_fast_store_simd_pair_case(2, 2, 0, 7, 7, 7, 0);

    struct test_fixture c_fixture;
    struct test_fixture threaded_fixture;
    init_fixture(&c_fixture);
    init_fixture(&threaded_fixture);
    const dword_t cross_page = encode_store_simd_pair(
            2, 2, 0, 4, 2, 3);
    memset(c_fixture.memory.primary_code +
            GUEST_MEMORY_PAGE_SIZE - 16, 0xa5, 16);
    memset(threaded_fixture.memory.primary_code +
            GUEST_MEMORY_PAGE_SIZE - 16, 0xa5, 16);
    memset(c_fixture.memory.data, 0xa5, 18);
    memset(threaded_fixture.memory.data, 0xa5, 18);
    write_instruction(&c_fixture.tlb, CODE_PAGE, cross_page);
    write_instruction(&threaded_fixture.tlb, CODE_PAGE, cross_page);
    struct cpu_state initial;
    init_differential_cpu(&initial);
    initial.x[4] = DATA_PAGE - 15;
    initial.v[2].d[0] = UINT64_C(0x0123456789abcdef);
    initial.v[2].d[1] = UINT64_C(0xfedcba9876543210);
    initial.v[3].d[0] = UINT64_C(0x8877665544332211);
    initial.v[3].d[1] = UINT64_C(0x1020304050607080);
    run_fast_differential_fixtures(
            cross_page, initial, AARCH64_STEP_RETIRED,
            &c_fixture, &threaded_fixture, NULL);
    byte_t expected[2 * sizeof(union aarch64_vector_reg)];
    memcpy(expected, initial.v[2].b, sizeof(initial.v[2]));
    memcpy(expected + sizeof(initial.v[2]),
            initial.v[3].b, sizeof(initial.v[3]));
    assert(c_fixture.memory.primary_code[
            GUEST_MEMORY_PAGE_SIZE - 16] == 0xa5);
    assert(memcmp(c_fixture.memory.primary_code +
            GUEST_MEMORY_PAGE_SIZE - 15, expected, 15) == 0);
    assert(memcmp(c_fixture.memory.data,
            expected + 15, sizeof(expected) - 15) == 0);
    assert(c_fixture.memory.data[sizeof(expected) - 15] == 0xa5);
}

static void test_fast_store_simd_pair_faults(void) {
    const dword_t post_index = encode_store_simd_pair(
            2, 1, 32, 5, 0, 1);
    struct test_fixture c_fixture;
    struct test_fixture threaded_fixture;
    init_fixture(&c_fixture);
    init_fixture(&threaded_fixture);
    memset(c_fixture.memory.data + GUEST_MEMORY_PAGE_SIZE - 16,
            0xa5, 16);
    memset(threaded_fixture.memory.data + GUEST_MEMORY_PAGE_SIZE - 16,
            0xa5, 16);
    write_instruction(&c_fixture.tlb, CODE_PAGE, post_index);
    write_instruction(&threaded_fixture.tlb, CODE_PAGE, post_index);
    struct cpu_state initial;
    init_differential_cpu(&initial);
    initial.x[5] = DATA_PAGE + GUEST_MEMORY_PAGE_SIZE - 16;
    initial.v[1].d[0] = UINT64_C(0x8877665544332211);
    initial.v[1].d[1] = UINT64_C(0x1020304050607080);
    aarch64_set_exclusive(&initial, DATA_PAGE + 0x80,
            8, false, UINT64_C(0x1122), 0, NULL, 3, 5, 7);
    struct aarch64_step_result step;
    struct cpu_state result = run_fast_differential_fixtures(
            post_index, initial, AARCH64_STEP_DATA_FAULT,
            &c_fixture, &threaded_fixture, &step);
    assert(step.fault.address == DATA_PAGE + GUEST_MEMORY_PAGE_SIZE);
    assert(step.fault.access == GUEST_MEMORY_WRITE);
    assert(step.fault.kind == GUEST_MEMORY_FAULT_UNMAPPED);
    for (byte_t index = 0; index < 16; index++) {
        assert(c_fixture.memory.data[
                GUEST_MEMORY_PAGE_SIZE - 16 + index] == 0xa5);
    }
    assert(result.x[5] == initial.x[5]);
    assert(result.pc == CODE_PAGE);
    assert(result.cycle == initial.cycle);
    assert(!result.exclusive.valid);

    const dword_t pre_index = encode_store_simd_pair(
            2, 3, -32, 5, 0, 1);
    init_fixture(&c_fixture);
    init_fixture(&threaded_fixture);
    memset(c_fixture.memory.primary_code + 0x100, 0xa5, 32);
    memset(threaded_fixture.memory.primary_code + 0x100, 0xa5, 32);
    write_instruction(&c_fixture.tlb, CODE_PAGE, pre_index);
    write_instruction(&threaded_fixture.tlb, CODE_PAGE, pre_index);
    c_fixture.memory.code_permissions = GUEST_MEMORY_EXECUTE;
    threaded_fixture.memory.code_permissions = GUEST_MEMORY_EXECUTE;
    guest_address_space_changed(&c_fixture.space);
    guest_address_space_changed(&threaded_fixture.space);
    init_differential_cpu(&initial);
    initial.x[5] = CODE_PAGE + 0x120;
    initial.v[1].d[0] = UINT64_C(0x8877665544332211);
    initial.v[1].d[1] = UINT64_C(0x1020304050607080);
    aarch64_set_exclusive(&initial, DATA_PAGE + 0x80,
            8, false, UINT64_C(0x1122), 0, NULL, 3, 5, 7);
    result = run_fast_differential_fixtures(
            pre_index, initial, AARCH64_STEP_DATA_FAULT,
            &c_fixture, &threaded_fixture, &step);
    assert(step.fault.address == CODE_PAGE + 0x100);
    assert(step.fault.access == GUEST_MEMORY_WRITE);
    assert(step.fault.kind == GUEST_MEMORY_FAULT_PERMISSION);
    for (byte_t index = 0; index < 32; index++)
        assert(c_fixture.memory.primary_code[0x100 + index] == 0xa5);
    assert(result.x[5] == initial.x[5]);
    assert(result.pc == CODE_PAGE);
    assert(result.cycle == initial.cycle);
    assert(!result.exclusive.valid);

    const dword_t address_size = encode_store_simd_pair(
            2, 2, 0, 5, 0, 1);
    init_fixture(&c_fixture);
    init_fixture(&threaded_fixture);
    write_instruction(&c_fixture.tlb, CODE_PAGE, address_size);
    write_instruction(&threaded_fixture.tlb, CODE_PAGE, address_size);
    init_differential_cpu(&initial);
    initial.x[5] = (UINT64_C(1) << 48) - 16;
    initial.v[1].d[0] = UINT64_C(0x8877665544332211);
    initial.v[1].d[1] = UINT64_C(0x1020304050607080);
    aarch64_set_exclusive(&initial, DATA_PAGE + 0x80,
            8, false, UINT64_C(0x1122), 0, NULL, 3, 5, 7);
    result = run_fast_differential_fixtures(
            address_size, initial, AARCH64_STEP_DATA_FAULT,
            &c_fixture, &threaded_fixture, &step);
    assert(step.fault.address == (UINT64_C(1) << 48));
    assert(step.fault.access == GUEST_MEMORY_WRITE);
    assert(step.fault.kind == GUEST_MEMORY_FAULT_ADDRESS_SIZE);
    assert(result.x[5] == initial.x[5]);
    assert(result.pc == CODE_PAGE);
    assert(result.cycle == initial.cycle);
    assert(!result.exclusive.valid);
}

static void test_fast_store_simd_pair_exclusive_invalidation(void) {
    struct test_fixture c_fixture;
    struct test_fixture threaded_fixture;
    init_fixture(&c_fixture);
    init_fixture(&threaded_fixture);
    const dword_t instructions[] = {
        INSTRUCTION_LDXR_X2_X1,
        encode_store_simd_pair(2, 2, 0, 1, 0, 1),
        INSTRUCTION_STXR_W4_X3_X1,
    };
    for (unsigned index = 0; index < array_size(instructions); index++) {
        write_instruction(&c_fixture.tlb,
                CODE_PAGE + index * 4, instructions[index]);
        write_instruction(&threaded_fixture.tlb,
                CODE_PAGE + index * 4, instructions[index]);
    }
    put_value(c_fixture.memory.data + 0x180, 8,
            UINT64_C(0x0102030405060708));
    put_value(threaded_fixture.memory.data + 0x180, 8,
            UINT64_C(0x0102030405060708));

    struct aarch64_runner c_runner;
    struct aarch64_runner threaded_runner;
    assert(aarch64_runner_init_backend(
            &c_runner, &c_fixture.tlb, AARCH64_BACKEND_C));
    assert(aarch64_runner_init_backend(&threaded_runner,
            &threaded_fixture.tlb, AARCH64_BACKEND_THREADED));
    struct cpu_state c_cpu;
    struct cpu_state threaded_cpu;
    init_differential_cpu(&c_cpu);
    init_differential_cpu(&threaded_cpu);
    c_cpu.x[1] = threaded_cpu.x[1] = DATA_PAGE + 0x180;
    c_cpu.x[3] = threaded_cpu.x[3] =
            UINT64_C(0xaabbccddeeff0011);
    c_cpu.x[4] = threaded_cpu.x[4] = UINT64_MAX;
    c_cpu.v[1].d[0] = threaded_cpu.v[1].d[0] =
            UINT64_C(0x8877665544332211);
    c_cpu.v[1].d[1] = threaded_cpu.v[1].d[1] =
            UINT64_C(0x1020304050607080);

    for (unsigned index = 0; index < array_size(instructions); index++) {
        struct aarch64_step_result c_result =
                aarch64_run_one(&c_runner, &c_cpu);
        struct aarch64_step_result threaded_result =
                aarch64_run_one(&threaded_runner, &threaded_cpu);
        assert(c_result.stop == AARCH64_STEP_RETIRED);
        assert_step_equal(&c_result, &threaded_result);
        assert_cpu_equal(&c_cpu, &threaded_cpu);
        assert_memory_equal(&c_fixture.memory,
                &threaded_fixture.memory);
        if (index == 0)
            assert(c_cpu.exclusive.valid);
        if (index == 1) {
            assert(c_cpu.exclusive.valid);
            assert(!guest_address_space_exclusive_matches(
                    &c_fixture.space, DATA_PAGE + 0x180,
                    c_cpu.exclusive.write_epoch));
            assert(!guest_address_space_exclusive_matches(
                    &threaded_fixture.space, DATA_PAGE + 0x180,
                    threaded_cpu.exclusive.write_epoch));
            assert_stats(&threaded_runner, 0, 2, 1, 1);
        }
    }
    assert(c_cpu.x[2] == UINT64_C(0x0102030405060708));
    assert(c_cpu.x[4] == 1);
    assert(!c_cpu.exclusive.valid);
    assert_stats(&threaded_runner, 0, 3, 1, 2);
    byte_t expected[2 * sizeof(union aarch64_vector_reg)];
    memcpy(expected, c_cpu.v[0].b, sizeof(c_cpu.v[0]));
    memcpy(expected + sizeof(c_cpu.v[0]),
            c_cpu.v[1].b, sizeof(c_cpu.v[1]));
    assert(memcmp(c_fixture.memory.data + 0x180,
            expected, sizeof(expected)) == 0);
}

static void run_fast_load_simd_pair_case(byte_t operation, byte_t mode,
        int64_t offset, byte_t rn, byte_t rt, byte_t rt2,
        dword_t expected_word) {
    static const union aarch64_vector_reg first_source = {
        .d = {
            UINT64_C(0x0123456789abcdef),
            UINT64_C(0xfedcba9876543210),
        },
    };
    static const union aarch64_vector_reg second_source = {
        .d = {
            UINT64_C(0x8877665544332211),
            UINT64_C(0x1020304050607080),
        },
    };
    const guest_addr_t target = DATA_PAGE + 0x300;
    byte_t size = (byte_t) (UINT8_C(1) << (operation + 2));
    dword_t instruction = encode_load_simd_pair(
            operation, mode, offset, rn, rt, rt2);
    if (expected_word != 0)
        assert(instruction == expected_word);

    struct test_fixture c_fixture;
    struct test_fixture threaded_fixture;
    init_fixture(&c_fixture);
    init_fixture(&threaded_fixture);
    memset(c_fixture.memory.data, 0xa5,
            sizeof(c_fixture.memory.data));
    memset(threaded_fixture.memory.data, 0xa5,
            sizeof(threaded_fixture.memory.data));
    memcpy(c_fixture.memory.data + 0x300, first_source.b, size);
    memcpy(c_fixture.memory.data + 0x300 + size,
            second_source.b, size);
    memcpy(threaded_fixture.memory.data,
            c_fixture.memory.data, sizeof(c_fixture.memory.data));
    write_instruction(&c_fixture.tlb, CODE_PAGE, instruction);
    write_instruction(&threaded_fixture.tlb, CODE_PAGE, instruction);
    byte_t expected_data[GUEST_MEMORY_PAGE_SIZE];
    memcpy(expected_data, c_fixture.memory.data,
            sizeof(expected_data));

    struct cpu_state initial;
    init_differential_cpu(&initial);
    memset(initial.v[rt].b, 0x5a, sizeof(initial.v[rt].b));
    memset(initial.v[rt2].b, 0xc3, sizeof(initial.v[rt2].b));
    enum aarch64_address_mode address_mode = AARCH64_ADDRESS_OFFSET;
    if (mode == 1)
        address_mode = AARCH64_ADDRESS_POST_INDEX;
    else if (mode == 3)
        address_mode = AARCH64_ADDRESS_PRE_INDEX;
    guest_addr_t base = address_mode == AARCH64_ADDRESS_POST_INDEX ?
            target : target - (qword_t) offset;
    if (rn == 31)
        initial.sp = base;
    else
        initial.x[rn] = base;
    aarch64_set_exclusive(&initial, DATA_PAGE + 0x80,
            8, false, UINT64_C(0x1122), 0, NULL, 3, 5, 7);

    struct cpu_state result = run_fast_differential_fixtures(
            instruction, initial, AARCH64_STEP_RETIRED,
            &c_fixture, &threaded_fixture, NULL);
    union aarch64_vector_reg expected_first = {0};
    union aarch64_vector_reg expected_second = {0};
    memcpy(expected_first.b, first_source.b, size);
    memcpy(expected_second.b, second_source.b, size);
    assert(memcmp(result.v[rt].b,
            expected_first.b, sizeof(expected_first.b)) == 0);
    assert(memcmp(result.v[rt2].b,
            expected_second.b, sizeof(expected_second.b)) == 0);
    guest_addr_t expected_base = base;
    if (address_mode != AARCH64_ADDRESS_OFFSET)
        expected_base += (qword_t) offset;
    if (rn == 31)
        assert(result.sp == expected_base);
    else
        assert(result.x[rn] == expected_base);
    assert(result.pc == CODE_PAGE + 4);
    assert(result.cycle == initial.cycle + 1);
    assert(result.nzcv == initial.nzcv);
    assert(result.fpcr == initial.fpcr);
    assert(result.fpsr == initial.fpsr);
    assert(result.exclusive.address == initial.exclusive.address);
    assert(result.exclusive.value_low == initial.exclusive.value_low);
    assert(result.exclusive.value_high == initial.exclusive.value_high);
    assert(result.exclusive.address_space ==
            initial.exclusive.address_space);
    assert(result.exclusive.mapping_epoch ==
            initial.exclusive.mapping_epoch);
    assert(result.exclusive.write_epoch == initial.exclusive.write_epoch);
    assert(result.exclusive.sync_identity ==
            initial.exclusive.sync_identity);
    assert(result.exclusive.size == initial.exclusive.size);
    assert(result.exclusive.pair == initial.exclusive.pair);
    assert(result.exclusive.valid);
    assert(memcmp(c_fixture.memory.data,
            expected_data, sizeof(expected_data)) == 0);
}

static void test_fast_load_simd_pair_differential(void) {
    unsigned case_count = 0;
    for (byte_t operation = 0; operation < 3; operation++) {
        byte_t size = (byte_t) (UINT8_C(1) << (operation + 2));
        const int64_t offsets[] = {
            -64 * (int64_t) size,
            size,
            63 * (int64_t) size,
            -(int64_t) size,
        };
        for (byte_t mode = 0; mode < 4; mode++) {
            byte_t rn = 5;
            byte_t rt = 4;
            byte_t rt2 = 6;
            if ((operation == 0 && mode == 1) ||
                    (operation == 1 && mode == 3)) {
                rn = 31;
            } else if (operation == 2 && mode == 3) {
                rn = 7;
                rt = 7;
                rt2 = 8;
            }
            run_fast_load_simd_pair_case(operation, mode,
                    offsets[mode], rn, rt, rt2, 0);
            case_count++;
        }
    }
    assert(case_count == 12);

    run_fast_load_simd_pair_case(2, 2, 0, 1, 30, 31,
            INSTRUCTION_LDP_Q30_Q31_X1);
    run_fast_load_simd_pair_case(2, 2, 80, 31, 30, 29,
            INSTRUCTION_LDP_Q30_Q29_SP_80);

    struct test_fixture c_fixture;
    struct test_fixture threaded_fixture;
    init_fixture(&c_fixture);
    init_fixture(&threaded_fixture);
    const dword_t cross_page = encode_load_simd_pair(
            2, 2, 0, 4, 2, 3);
    byte_t pair_bytes[2 * sizeof(union aarch64_vector_reg)];
    for (byte_t index = 0; index < sizeof(pair_bytes); index++)
        pair_bytes[index] = (byte_t) (0x21 + index * 3);
    memcpy(c_fixture.memory.primary_code +
            GUEST_MEMORY_PAGE_SIZE - 15, pair_bytes, 15);
    memcpy(c_fixture.memory.data, pair_bytes + 15,
            sizeof(pair_bytes) - 15);
    memcpy(threaded_fixture.memory.primary_code +
            GUEST_MEMORY_PAGE_SIZE - 15, pair_bytes, 15);
    memcpy(threaded_fixture.memory.data, pair_bytes + 15,
            sizeof(pair_bytes) - 15);
    write_instruction(&c_fixture.tlb, CODE_PAGE, cross_page);
    write_instruction(&threaded_fixture.tlb, CODE_PAGE, cross_page);
    byte_t expected_primary[GUEST_MEMORY_PAGE_SIZE];
    byte_t expected_data[GUEST_MEMORY_PAGE_SIZE];
    memcpy(expected_primary, c_fixture.memory.primary_code,
            sizeof(expected_primary));
    memcpy(expected_data, c_fixture.memory.data,
            sizeof(expected_data));
    struct cpu_state initial;
    init_differential_cpu(&initial);
    initial.x[4] = DATA_PAGE - 15;
    memset(initial.v[2].b, 0x5a, sizeof(initial.v[2].b));
    memset(initial.v[3].b, 0xc3, sizeof(initial.v[3].b));
    aarch64_set_exclusive(&initial, DATA_PAGE + 0x80,
            8, false, UINT64_C(0x1122), 0, NULL, 3, 5, 7);
    struct cpu_state result = run_fast_differential_fixtures(
            cross_page, initial, AARCH64_STEP_RETIRED,
            &c_fixture, &threaded_fixture, NULL);
    assert(memcmp(result.v[2].b, pair_bytes,
            sizeof(result.v[2].b)) == 0);
    assert(memcmp(result.v[3].b,
            pair_bytes + sizeof(result.v[2].b),
            sizeof(result.v[3].b)) == 0);
    assert(result.x[4] == initial.x[4]);
    assert(result.exclusive.valid);
    assert(memcmp(c_fixture.memory.primary_code,
            expected_primary, sizeof(expected_primary)) == 0);
    assert(memcmp(c_fixture.memory.data,
            expected_data, sizeof(expected_data)) == 0);
}

static void test_fast_load_simd_pair_faults(void) {
    struct test_fixture c_fixture;
    struct test_fixture threaded_fixture;
    struct cpu_state initial;
    struct cpu_state result;
    struct aarch64_step_result step;

    const dword_t post_index = encode_load_simd_pair(
            2, 1, 32, 5, 4, 6);
    init_fixture(&c_fixture);
    init_fixture(&threaded_fixture);
    write_instruction(&c_fixture.tlb, CODE_PAGE, post_index);
    write_instruction(&threaded_fixture.tlb, CODE_PAGE, post_index);
    init_differential_cpu(&initial);
    initial.x[5] = DATA_PAGE + GUEST_MEMORY_PAGE_SIZE - 16;
    memset(initial.v[4].b, 0x5a, sizeof(initial.v[4].b));
    memset(initial.v[6].b, 0xc3, sizeof(initial.v[6].b));
    aarch64_set_exclusive(&initial, DATA_PAGE + 0x80,
            8, false, UINT64_C(0x1122), 0, NULL, 3, 5, 7);
    result = run_fast_differential_fixtures(
            post_index, initial, AARCH64_STEP_DATA_FAULT,
            &c_fixture, &threaded_fixture, &step);
    assert(step.fault.address == DATA_PAGE + GUEST_MEMORY_PAGE_SIZE);
    assert(step.fault.access == GUEST_MEMORY_READ);
    assert(step.fault.kind == GUEST_MEMORY_FAULT_UNMAPPED);
    assert(memcmp(result.v[4].b,
            initial.v[4].b, sizeof(result.v[4].b)) == 0);
    assert(memcmp(result.v[6].b,
            initial.v[6].b, sizeof(result.v[6].b)) == 0);
    assert(result.x[5] == initial.x[5]);
    assert(result.pc == CODE_PAGE);
    assert(result.cycle == initial.cycle);
    assert(!result.exclusive.valid);

    const dword_t pre_index = encode_load_simd_pair(
            2, 3, -32, 5, 4, 6);
    init_fixture(&c_fixture);
    init_fixture(&threaded_fixture);
    write_instruction(&c_fixture.tlb, CODE_PAGE, pre_index);
    write_instruction(&threaded_fixture.tlb, CODE_PAGE, pre_index);
    c_fixture.memory.code_permissions = GUEST_MEMORY_EXECUTE;
    threaded_fixture.memory.code_permissions = GUEST_MEMORY_EXECUTE;
    guest_address_space_changed(&c_fixture.space);
    guest_address_space_changed(&threaded_fixture.space);
    init_differential_cpu(&initial);
    initial.x[5] = CODE_PAGE + 0x120;
    memset(initial.v[4].b, 0x5a, sizeof(initial.v[4].b));
    memset(initial.v[6].b, 0xc3, sizeof(initial.v[6].b));
    aarch64_set_exclusive(&initial, DATA_PAGE + 0x80,
            8, false, UINT64_C(0x1122), 0, NULL, 3, 5, 7);
    result = run_fast_differential_fixtures(
            pre_index, initial, AARCH64_STEP_DATA_FAULT,
            &c_fixture, &threaded_fixture, &step);
    assert(step.fault.address == CODE_PAGE + 0x100);
    assert(step.fault.access == GUEST_MEMORY_READ);
    assert(step.fault.kind == GUEST_MEMORY_FAULT_PERMISSION);
    assert(memcmp(result.v[4].b,
            initial.v[4].b, sizeof(result.v[4].b)) == 0);
    assert(memcmp(result.v[6].b,
            initial.v[6].b, sizeof(result.v[6].b)) == 0);
    assert(result.x[5] == initial.x[5]);
    assert(result.pc == CODE_PAGE);
    assert(result.cycle == initial.cycle);
    assert(!result.exclusive.valid);

    const dword_t address_size = encode_load_simd_pair(
            2, 2, 0, 5, 4, 6);
    init_fixture(&c_fixture);
    init_fixture(&threaded_fixture);
    write_instruction(&c_fixture.tlb, CODE_PAGE, address_size);
    write_instruction(&threaded_fixture.tlb, CODE_PAGE, address_size);
    init_differential_cpu(&initial);
    initial.x[5] = (UINT64_C(1) << 48) - 16;
    memset(initial.v[4].b, 0x5a, sizeof(initial.v[4].b));
    memset(initial.v[6].b, 0xc3, sizeof(initial.v[6].b));
    aarch64_set_exclusive(&initial, DATA_PAGE + 0x80,
            8, false, UINT64_C(0x1122), 0, NULL, 3, 5, 7);
    result = run_fast_differential_fixtures(
            address_size, initial, AARCH64_STEP_DATA_FAULT,
            &c_fixture, &threaded_fixture, &step);
    assert(step.fault.address == (UINT64_C(1) << 48));
    assert(step.fault.access == GUEST_MEMORY_READ);
    assert(step.fault.kind == GUEST_MEMORY_FAULT_ADDRESS_SIZE);
    assert(memcmp(result.v[4].b,
            initial.v[4].b, sizeof(result.v[4].b)) == 0);
    assert(memcmp(result.v[6].b,
            initial.v[6].b, sizeof(result.v[6].b)) == 0);
    assert(result.x[5] == initial.x[5]);
    assert(result.pc == CODE_PAGE);
    assert(result.cycle == initial.cycle);
    assert(!result.exclusive.valid);
}

static void test_fast_load_simd_pair_preserves_exclusive(void) {
    struct test_fixture c_fixture;
    struct test_fixture threaded_fixture;
    init_fixture(&c_fixture);
    init_fixture(&threaded_fixture);
    const dword_t instructions[] = {
        INSTRUCTION_LDXR_X2_X1,
        INSTRUCTION_LDP_Q30_Q31_X1,
        INSTRUCTION_STXR_W6_X3_X1,
    };
    for (unsigned index = 0; index < array_size(instructions); index++) {
        write_instruction(&c_fixture.tlb,
                CODE_PAGE + index * 4, instructions[index]);
        write_instruction(&threaded_fixture.tlb,
                CODE_PAGE + index * 4, instructions[index]);
    }
    byte_t source[2 * sizeof(union aarch64_vector_reg)];
    for (byte_t index = 0; index < sizeof(source); index++)
        source[index] = (byte_t) (0x31 + index * 5);
    memcpy(c_fixture.memory.data + 0x180, source, sizeof(source));
    memcpy(threaded_fixture.memory.data + 0x180,
            source, sizeof(source));

    struct aarch64_runner c_runner;
    struct aarch64_runner threaded_runner;
    assert(aarch64_runner_init_backend(
            &c_runner, &c_fixture.tlb, AARCH64_BACKEND_C));
    assert(aarch64_runner_init_backend(&threaded_runner,
            &threaded_fixture.tlb, AARCH64_BACKEND_THREADED));
    struct cpu_state c_cpu;
    struct cpu_state threaded_cpu;
    init_differential_cpu(&c_cpu);
    init_differential_cpu(&threaded_cpu);
    c_cpu.x[1] = threaded_cpu.x[1] = DATA_PAGE + 0x180;
    c_cpu.x[3] = threaded_cpu.x[3] =
            UINT64_C(0xaabbccddeeff0011);
    memset(c_cpu.v[30].b, 0x5a, sizeof(c_cpu.v[30].b));
    memset(c_cpu.v[31].b, 0xc3, sizeof(c_cpu.v[31].b));
    threaded_cpu.v[30] = c_cpu.v[30];
    threaded_cpu.v[31] = c_cpu.v[31];

    for (unsigned index = 0; index < array_size(instructions); index++) {
        struct aarch64_step_result c_result =
                aarch64_run_one(&c_runner, &c_cpu);
        struct aarch64_step_result threaded_result =
                aarch64_run_one(&threaded_runner, &threaded_cpu);
        assert(c_result.stop == AARCH64_STEP_RETIRED);
        assert_step_equal(&c_result, &threaded_result);
        assert_cpu_equal(&c_cpu, &threaded_cpu);
        assert_memory_equal(&c_fixture.memory,
                &threaded_fixture.memory);
        if (index == 1) {
            assert(c_cpu.exclusive.valid);
            assert(guest_address_space_exclusive_matches(
                    &c_fixture.space, DATA_PAGE + 0x180,
                    c_cpu.exclusive.write_epoch));
            assert(guest_address_space_exclusive_matches(
                    &threaded_fixture.space, DATA_PAGE + 0x180,
                    threaded_cpu.exclusive.write_epoch));
            assert(memcmp(c_cpu.v[30].b,
                    source, sizeof(c_cpu.v[30].b)) == 0);
            assert(memcmp(c_cpu.v[31].b,
                    source + sizeof(c_cpu.v[30].b),
                    sizeof(c_cpu.v[31].b)) == 0);
            assert_stats(&threaded_runner, 0, 2, 1, 1);
        }
    }
    assert(c_cpu.x[6] == 0);
    assert(!c_cpu.exclusive.valid);
    assert_stats(&threaded_runner, 0, 3, 1, 2);
    byte_t expected[8];
    put_value(expected, sizeof(expected), c_cpu.x[3]);
    assert(memcmp(c_fixture.memory.data + 0x180,
            expected, sizeof(expected)) == 0);
}

static void run_fast_advsimd_movi_case(bool q, bool op,
        byte_t cmode, byte_t immediate, byte_t rd,
        dword_t expected_word, qword_t expected_immediate) {
    dword_t instruction = encode_advsimd_immediate(
            q, op, cmode, immediate, rd);
    assert(instruction == expected_word);
    struct aarch64_decoded decoded;
    assert(aarch64_decode(instruction, &decoded));
    assert(decoded.opcode == AARCH64_OP_ADVSIMD_MOVI);
    assert(decoded.width == (q ? 128 : 64));
    assert(decoded.operands.advsimd_immediate.rd == rd);
    assert(decoded.operands.advsimd_immediate.immediate ==
            expected_immediate);

    struct test_fixture c_fixture;
    struct test_fixture threaded_fixture;
    init_fixture(&c_fixture);
    init_fixture(&threaded_fixture);
    memset(c_fixture.memory.data, 0xa5,
            sizeof(c_fixture.memory.data));
    memcpy(threaded_fixture.memory.data,
            c_fixture.memory.data, sizeof(c_fixture.memory.data));
    write_instruction(&c_fixture.tlb, CODE_PAGE, instruction);
    write_instruction(&threaded_fixture.tlb, CODE_PAGE, instruction);
    struct test_memory expected_memory = c_fixture.memory;

    struct cpu_state initial;
    init_differential_cpu(&initial);
    initial.v[rd].d[0] = UINT64_C(0x5a5a5a5a5a5a5a5a);
    initial.v[rd].d[1] = UINT64_C(0xc3c3c3c3c3c3c3c3);
    aarch64_set_exclusive(&initial, DATA_PAGE + 0x80,
            8, false, UINT64_C(0x1122), 0, NULL, 3, 5, 7);

    struct cpu_state result = run_fast_differential_fixtures(
            instruction, initial, AARCH64_STEP_RETIRED,
            &c_fixture, &threaded_fixture, NULL);
    struct cpu_state expected = initial;
    expected.v[rd].d[0] = expected_immediate;
    expected.v[rd].d[1] = q ? expected_immediate : 0;
    expected.pc += 4;
    expected.cycle++;
    assert_cpu_equal(&result, &expected);
    assert_memory_equal(&c_fixture.memory, &expected_memory);
    assert_memory_equal(&threaded_fixture.memory, &expected_memory);
}

static void test_fast_advsimd_movi_differential(void) {
    assert(encode_advsimd_immediate(
            false, false, 0, 0, 0) == UINT32_C(0x0f000400));
    assert(encode_advsimd_immediate(
            true, true, 14, 0xff, 31) == UINT32_C(0x6f07e7ff));
    static const struct {
        bool q;
        bool op;
        byte_t cmode;
        byte_t immediate;
        byte_t rd;
        dword_t word;
        qword_t expected;
    } cases[] = {
        {true, false, 0, 0x00, 31, UINT32_C(0x4f00041f),
                UINT64_C(0x0000000000000000)},
        {false, false, 2, 0xff, 2, UINT32_C(0x0f0727e2),
                UINT64_C(0x0000ff000000ff00)},
        {true, false, 4, 0x81, 3, UINT32_C(0x4f044423),
                UINT64_C(0x0081000000810000)},
        {false, false, 6, 0x34, 4, UINT32_C(0x0f016684),
                UINT64_C(0x3400000034000000)},
        {true, false, 8, 0x5a, 5, UINT32_C(0x4f028745),
                UINT64_C(0x005a005a005a005a)},
        {false, false, 10, 0x12, 6, UINT32_C(0x0f00a646),
                UINT64_C(0x1200120012001200)},
        {true, false, 12, 0x56, 7, UINT32_C(0x4f02c6c7),
                UINT64_C(0x000056ff000056ff)},
        {false, false, 13, 0x78, 8, UINT32_C(0x0f03d708),
                UINT64_C(0x0078ffff0078ffff)},
        {true, false, 14, 0xa5, 9, UINT32_C(0x4f05e4a9),
                UINT64_C(0xa5a5a5a5a5a5a5a5)},
        {false, true, 14, 0x81, 10, UINT32_C(0x2f04e42a),
                UINT64_C(0xff000000000000ff)},
        {true, false, 0, 0x00, 27, INSTRUCTION_MOVI_V27_4S_0,
                UINT64_C(0x0000000000000000)},
    };
    _Static_assert(array_size(cases) == 11,
            "MOVI 差分门必须覆盖十种立即数形式和真实画像代表");

    for (unsigned index = 0; index < array_size(cases); index++) {
        run_fast_advsimd_movi_case(cases[index].q, cases[index].op,
                cases[index].cmode, cases[index].immediate,
                cases[index].rd, cases[index].word,
                cases[index].expected);
    }
}

static void test_advsimd_movi_sibling_fallback(void) {
    static const dword_t instructions[] = {
        INSTRUCTION_MVNI_V9_8H,
        INSTRUCTION_ORR_V12_8H,
        INSTRUCTION_BIC_V14_4H,
    };
    static const enum aarch64_opcode opcodes[] = {
        AARCH64_OP_ADVSIMD_MVNI,
        AARCH64_OP_ADVSIMD_ORR_IMMEDIATE,
        AARCH64_OP_ADVSIMD_BIC_IMMEDIATE,
    };
    struct test_fixture fixture;
    init_fixture(&fixture);
    for (unsigned index = 0; index < array_size(instructions); index++) {
        struct aarch64_decoded decoded;
        assert(aarch64_decode(instructions[index], &decoded));
        assert(decoded.opcode == opcodes[index]);
        write_instruction(&fixture.tlb,
                CODE_PAGE + index * 4, instructions[index]);
    }
    struct test_memory expected_memory = fixture.memory;
    struct aarch64_runner runner;
    assert(aarch64_runner_init_backend(
            &runner, &fixture.tlb, AARCH64_BACKEND_THREADED));
    struct cpu_state cpu;
    init_differential_cpu(&cpu);
    aarch64_set_exclusive(&cpu, DATA_PAGE + 0x80,
            8, false, UINT64_C(0x1122), 0, NULL, 3, 5, 7);

    cpu.v[9].d[0] = UINT64_C(0x0123456789abcdef);
    cpu.v[9].d[1] = UINT64_C(0xfedcba9876543210);
    struct cpu_state expected = cpu;
    expected.v[9].d[0] = UINT64_C(0x65ff65ff65ff65ff);
    expected.v[9].d[1] = UINT64_C(0x65ff65ff65ff65ff);
    expected.pc = CODE_PAGE + 4;
    expected.cycle++;
    assert(run_at(&runner, &cpu, CODE_PAGE).stop ==
            AARCH64_STEP_RETIRED);
    assert_cpu_equal(&cpu, &expected);
    assert_stats(&runner, 0, 1, 0, 1);

    cpu.v[12].d[0] = UINT64_C(0x00ff00ff00ff00ff);
    cpu.v[12].d[1] = UINT64_C(0x0001000100010001);
    expected = cpu;
    expected.v[12].d[0] = UINT64_C(0x12ff12ff12ff12ff);
    expected.v[12].d[1] = UINT64_C(0x1201120112011201);
    expected.pc = CODE_PAGE + 8;
    expected.cycle++;
    assert(run_at(&runner, &cpu, CODE_PAGE + 4).stop ==
            AARCH64_STEP_RETIRED);
    assert_cpu_equal(&cpu, &expected);
    assert_stats(&runner, 0, 2, 0, 2);

    cpu.v[14].d[0] = UINT64_MAX;
    cpu.v[14].d[1] = UINT64_C(0x0123456789abcdef);
    expected = cpu;
    expected.v[14].d[0] = UINT64_C(0xffa9ffa9ffa9ffa9);
    expected.v[14].d[1] = 0;
    expected.pc = CODE_PAGE + 12;
    expected.cycle++;
    assert(run_at(&runner, &cpu, CODE_PAGE + 8).stop ==
            AARCH64_STEP_RETIRED);
    assert_cpu_equal(&cpu, &expected);
    assert_stats(&runner, 0, 3, 0, 3);
    assert_memory_equal(&fixture.memory, &expected_memory);
}

static void test_fast_load_pair_differential(void) {
    const qword_t first_source = UINT64_C(0x8877665589abcdef);
    const qword_t second_source = UINT64_C(0x1122334401234567);
    const guest_addr_t target = DATA_PAGE + 0x300;
    const struct {
        byte_t operation;
        enum aarch64_address_mode address_mode;
        int64_t offset;
        byte_t rn;
        byte_t rt;
        byte_t rt2;
        dword_t expected_word;
    } cases[] = {
        {0, AARCH64_ADDRESS_OFFSET, 252, 5, 4, 6, 0},
        {0, AARCH64_ADDRESS_PRE_INDEX, -256, 7, 8, 9, 0},
        {0, AARCH64_ADDRESS_POST_INDEX, 4, 31, 10, 11, 0},
        {2, AARCH64_ADDRESS_OFFSET, 400, 31, 6, 1,
                INSTRUCTION_LDP_X6_X1_SP_400},
        {2, AARCH64_ADDRESS_POST_INDEX, 16, 31, 29, 30,
                INSTRUCTION_LDP_X29_X30_SP_POST_16},
        {2, AARCH64_ADDRESS_PRE_INDEX, -512, 12, 13, 14, 0},
        {1, AARCH64_ADDRESS_OFFSET, 252, 15, 16, 17, 0},
        {1, AARCH64_ADDRESS_PRE_INDEX, -256, 18, 19, 20, 0},
        {2, AARCH64_ADDRESS_OFFSET, 0, 4, 4, 21, 0},
        {2, AARCH64_ADDRESS_OFFSET, 0, 22, 23, 22, 0},
        {2, AARCH64_ADDRESS_OFFSET, 0, 24, 31, 25, 0},
    };

    for (unsigned index = 0; index < array_size(cases); index++) {
        byte_t size = cases[index].operation == 2 ? 8 : 4;
        dword_t instruction = encode_load_pair(
                cases[index].operation, cases[index].address_mode,
                cases[index].offset, cases[index].rn,
                cases[index].rt, cases[index].rt2);
        if (cases[index].expected_word != 0)
            assert(instruction == cases[index].expected_word);

        struct test_fixture c_fixture;
        struct test_fixture threaded_fixture;
        init_fixture(&c_fixture);
        init_fixture(&threaded_fixture);
        put_value(c_fixture.memory.data + 0x300,
                size, first_source);
        put_value(c_fixture.memory.data + 0x300 + size,
                size, second_source);
        put_value(threaded_fixture.memory.data + 0x300,
                size, first_source);
        put_value(threaded_fixture.memory.data + 0x300 + size,
                size, second_source);
        write_instruction(&c_fixture.tlb, CODE_PAGE, instruction);
        write_instruction(&threaded_fixture.tlb,
                CODE_PAGE, instruction);

        struct cpu_state initial;
        init_differential_cpu(&initial);
        if (cases[index].rt != 31)
            initial.x[cases[index].rt] = UINT64_MAX;
        if (cases[index].rt2 != 31)
            initial.x[cases[index].rt2] = UINT64_MAX;
        guest_addr_t base =
                cases[index].address_mode ==
                        AARCH64_ADDRESS_POST_INDEX ?
                target : target - (qword_t) cases[index].offset;
        if (cases[index].rn == 31)
            initial.sp = base;
        else
            initial.x[cases[index].rn] = base;
        aarch64_set_exclusive(&initial, DATA_PAGE + 0x80,
                8, false, UINT64_C(0x1122), 0, NULL, 3, 5, 7);

        struct cpu_state result = run_fast_differential_fixtures(
                instruction, initial, AARCH64_STEP_RETIRED,
                &c_fixture, &threaded_fixture, NULL);
        qword_t expected_first = first_source;
        qword_t expected_second = second_source;
        if (cases[index].operation != 2) {
            expected_first &= UINT32_MAX;
            expected_second &= UINT32_MAX;
        }
        if (cases[index].operation == 1) {
            qword_t sign = UINT64_C(1) << 31;
            expected_first = (expected_first ^ sign) - sign;
            expected_second = (expected_second ^ sign) - sign;
        }
        if (cases[index].rt != 31)
            assert(result.x[cases[index].rt] == expected_first);
        if (cases[index].rt2 != 31)
            assert(result.x[cases[index].rt2] == expected_second);

        guest_addr_t expected_base = base;
        if (cases[index].address_mode != AARCH64_ADDRESS_OFFSET)
            expected_base += (qword_t) cases[index].offset;
        if (cases[index].rn == 31) {
            assert(result.sp == expected_base);
        } else if (cases[index].rn != cases[index].rt &&
                cases[index].rn != cases[index].rt2) {
            assert(result.x[cases[index].rn] == expected_base);
        }
        assert(result.pc == CODE_PAGE + 4);
        assert(result.cycle == initial.cycle + 1);
        assert(result.exclusive.valid);
        assert(result.exclusive.write_epoch == 5);
        assert(result.exclusive.sync_identity == 7);
    }

    struct test_fixture c_fixture;
    struct test_fixture threaded_fixture;
    init_fixture(&c_fixture);
    init_fixture(&threaded_fixture);
    const dword_t cross_page = encode_load_pair(
            2, AARCH64_ADDRESS_OFFSET, 0, 4, 6, 7);
    byte_t pair_bytes[16];
    put_value(pair_bytes, 8, first_source);
    put_value(pair_bytes + 8, 8, second_source);
    memcpy(c_fixture.memory.primary_code +
            GUEST_MEMORY_PAGE_SIZE - 5, pair_bytes, 5);
    memcpy(c_fixture.memory.data, pair_bytes + 5, 11);
    memcpy(threaded_fixture.memory.primary_code +
            GUEST_MEMORY_PAGE_SIZE - 5, pair_bytes, 5);
    memcpy(threaded_fixture.memory.data, pair_bytes + 5, 11);
    write_instruction(&c_fixture.tlb, CODE_PAGE, cross_page);
    write_instruction(&threaded_fixture.tlb, CODE_PAGE, cross_page);
    struct cpu_state initial;
    init_differential_cpu(&initial);
    initial.x[4] = DATA_PAGE - 5;
    struct cpu_state result = run_fast_differential_fixtures(
            cross_page, initial, AARCH64_STEP_RETIRED,
            &c_fixture, &threaded_fixture, NULL);
    assert(result.x[6] == first_source);
    assert(result.x[7] == second_source);
}

static void test_fast_load_pair_faults(void) {
    const dword_t post_index = encode_load_pair(
            2, AARCH64_ADDRESS_POST_INDEX, 16, 5, 4, 6);
    struct test_fixture c_fixture;
    struct test_fixture threaded_fixture;
    init_fixture(&c_fixture);
    init_fixture(&threaded_fixture);
    write_instruction(&c_fixture.tlb, CODE_PAGE, post_index);
    write_instruction(&threaded_fixture.tlb, CODE_PAGE, post_index);
    struct cpu_state initial;
    init_differential_cpu(&initial);
    initial.x[4] = UINT64_C(0x1020304050607080);
    initial.x[5] = DATA_PAGE + GUEST_MEMORY_PAGE_SIZE - 8;
    initial.x[6] = UINT64_C(0x8877665544332211);
    aarch64_set_exclusive(&initial, DATA_PAGE + 0x80,
            8, false, UINT64_C(0x1122), 0, NULL, 3, 5, 7);
    struct aarch64_step_result step;
    struct cpu_state result = run_fast_differential_fixtures(
            post_index, initial, AARCH64_STEP_DATA_FAULT,
            &c_fixture, &threaded_fixture, &step);
    assert(step.fault.address == DATA_PAGE + GUEST_MEMORY_PAGE_SIZE);
    assert(step.fault.access == GUEST_MEMORY_READ);
    assert(step.fault.kind == GUEST_MEMORY_FAULT_UNMAPPED);
    assert(result.x[4] == initial.x[4]);
    assert(result.x[5] == initial.x[5]);
    assert(result.x[6] == initial.x[6]);
    assert(result.pc == CODE_PAGE);
    assert(result.cycle == initial.cycle);
    assert(!result.exclusive.valid);

    const dword_t pre_index = encode_load_pair(
            2, AARCH64_ADDRESS_PRE_INDEX, -16, 5, 4, 6);
    init_fixture(&c_fixture);
    init_fixture(&threaded_fixture);
    write_instruction(&c_fixture.tlb, CODE_PAGE, pre_index);
    write_instruction(&threaded_fixture.tlb, CODE_PAGE, pre_index);
    c_fixture.memory.code_permissions = GUEST_MEMORY_EXECUTE;
    threaded_fixture.memory.code_permissions = GUEST_MEMORY_EXECUTE;
    guest_address_space_changed(&c_fixture.space);
    guest_address_space_changed(&threaded_fixture.space);
    init_differential_cpu(&initial);
    initial.x[4] = UINT64_C(0x1020304050607080);
    initial.x[5] = CODE_PAGE + 0x110;
    initial.x[6] = UINT64_C(0x8877665544332211);
    aarch64_set_exclusive(&initial, DATA_PAGE + 0x80,
            8, false, UINT64_C(0x1122), 0, NULL, 3, 5, 7);
    result = run_fast_differential_fixtures(
            pre_index, initial, AARCH64_STEP_DATA_FAULT,
            &c_fixture, &threaded_fixture, &step);
    assert(step.fault.address == CODE_PAGE + 0x100);
    assert(step.fault.access == GUEST_MEMORY_READ);
    assert(step.fault.kind == GUEST_MEMORY_FAULT_PERMISSION);
    assert(result.x[4] == initial.x[4]);
    assert(result.x[5] == initial.x[5]);
    assert(result.x[6] == initial.x[6]);
    assert(result.pc == CODE_PAGE);
    assert(result.cycle == initial.cycle);
    assert(!result.exclusive.valid);

    const dword_t address_size = encode_load_pair(
            2, AARCH64_ADDRESS_OFFSET, 0, 5, 4, 6);
    init_fixture(&c_fixture);
    init_fixture(&threaded_fixture);
    write_instruction(&c_fixture.tlb, CODE_PAGE, address_size);
    write_instruction(&threaded_fixture.tlb,
            CODE_PAGE, address_size);
    init_differential_cpu(&initial);
    initial.x[4] = UINT64_C(0x1020304050607080);
    initial.x[5] = (UINT64_C(1) << 48) - 8;
    initial.x[6] = UINT64_C(0x8877665544332211);
    aarch64_set_exclusive(&initial, DATA_PAGE + 0x80,
            8, false, UINT64_C(0x1122), 0, NULL, 3, 5, 7);
    result = run_fast_differential_fixtures(
            address_size, initial, AARCH64_STEP_DATA_FAULT,
            &c_fixture, &threaded_fixture, &step);
    assert(step.fault.address == (UINT64_C(1) << 48));
    assert(step.fault.access == GUEST_MEMORY_READ);
    assert(step.fault.kind == GUEST_MEMORY_FAULT_ADDRESS_SIZE);
    assert(result.x[4] == initial.x[4]);
    assert(result.x[5] == initial.x[5]);
    assert(result.x[6] == initial.x[6]);
    assert(result.pc == CODE_PAGE);
    assert(result.cycle == initial.cycle);
    assert(!result.exclusive.valid);
}

static void test_fast_load_pair_preserves_exclusive(void) {
    struct test_fixture c_fixture;
    struct test_fixture threaded_fixture;
    init_fixture(&c_fixture);
    init_fixture(&threaded_fixture);
    const dword_t instructions[] = {
        INSTRUCTION_LDXR_X2_X1,
        INSTRUCTION_LDP_X4_X5_X1,
        INSTRUCTION_STXR_W6_X3_X1,
    };
    for (unsigned index = 0; index < array_size(instructions); index++) {
        write_instruction(&c_fixture.tlb,
                CODE_PAGE + index * 4, instructions[index]);
        write_instruction(&threaded_fixture.tlb,
                CODE_PAGE + index * 4, instructions[index]);
    }
    put_value(c_fixture.memory.data + 0x180, 8,
            UINT64_C(0x1020304050607080));
    put_value(c_fixture.memory.data + 0x188, 8,
            UINT64_C(0x8877665544332211));
    memcpy(threaded_fixture.memory.data + 0x180,
            c_fixture.memory.data + 0x180, 16);

    struct aarch64_runner c_runner;
    struct aarch64_runner threaded_runner;
    assert(aarch64_runner_init_backend(
            &c_runner, &c_fixture.tlb, AARCH64_BACKEND_C));
    assert(aarch64_runner_init_backend(&threaded_runner,
            &threaded_fixture.tlb, AARCH64_BACKEND_THREADED));
    struct cpu_state c_cpu;
    struct cpu_state threaded_cpu;
    init_differential_cpu(&c_cpu);
    init_differential_cpu(&threaded_cpu);
    c_cpu.x[1] = threaded_cpu.x[1] = DATA_PAGE + 0x180;
    c_cpu.x[3] = threaded_cpu.x[3] =
            UINT64_C(0xaabbccddeeff0011);

    for (unsigned index = 0; index < array_size(instructions); index++) {
        struct aarch64_step_result c_result =
                aarch64_run_one(&c_runner, &c_cpu);
        struct aarch64_step_result threaded_result =
                aarch64_run_one(&threaded_runner, &threaded_cpu);
        assert(c_result.stop == AARCH64_STEP_RETIRED);
        assert_step_equal(&c_result, &threaded_result);
        assert_cpu_equal(&c_cpu, &threaded_cpu);
        assert_memory_equal(&c_fixture.memory,
                &threaded_fixture.memory);
        if (index == 1) {
            assert(c_cpu.exclusive.valid);
            assert_stats(&threaded_runner, 0, 2, 1, 1);
        }
    }
    assert(c_cpu.x[2] == UINT64_C(0x1020304050607080));
    assert(c_cpu.x[4] == UINT64_C(0x1020304050607080));
    assert(c_cpu.x[5] == UINT64_C(0x8877665544332211));
    assert(c_cpu.x[6] == 0);
    assert(!c_cpu.exclusive.valid);
    assert_stats(&threaded_runner, 0, 3, 1, 2);
    const byte_t expected[] = {
        0x11, 0x00, 0xff, 0xee, 0xdd, 0xcc, 0xbb, 0xaa,
    };
    assert(memcmp(c_fixture.memory.data + 0x180,
            expected, sizeof(expected)) == 0);
}

static void test_load_pair_sibling_fallback(void) {
    struct test_fixture fixture;
    init_fixture(&fixture);
    const dword_t instructions[] = {
        INSTRUCTION_LDXP_X4_X5_X1,
    };
    const enum aarch64_opcode opcodes[] = {
        AARCH64_OP_LDXP,
    };
    _Static_assert(array_size(instructions) == 1,
            "配对加载 sibling 必须继续锁定 exclusive load");
    for (unsigned index = 0; index < array_size(instructions); index++) {
        struct aarch64_decoded decoded;
        assert(aarch64_decode(instructions[index], &decoded));
        assert(decoded.opcode == opcodes[index]);
        write_instruction(&fixture.tlb,
                CODE_PAGE + index * 4, instructions[index]);
    }
    struct aarch64_runner runner;
    assert(aarch64_runner_init_backend(
            &runner, &fixture.tlb, AARCH64_BACKEND_THREADED));
    struct cpu_state cpu;
    init_differential_cpu(&cpu);
    cpu.x[1] = DATA_PAGE;
    for (unsigned index = 0; index < array_size(instructions); index++) {
        assert(run_at(&runner, &cpu, CODE_PAGE + index * 4).stop ==
                AARCH64_STEP_RETIRED);
    }
    assert_stats(&runner, 0, array_size(instructions),
            0, array_size(instructions));
}

static void test_fast_load_register_offset_differential(void) {
    static const enum aarch64_extend_type extend_types[] = {
        AARCH64_EXTEND_UXTW,
        AARCH64_EXTEND_UXTX,
        AARCH64_EXTEND_SXTW,
        AARCH64_EXTEND_SXTX,
    };
    static const qword_t index_values[] = {
        UINT64_C(0xabcdef0100000003),
        UINT64_C(3),
        UINT64_C(0x01234567fffffffd),
        UINT64_C(0xfffffffffffffffd),
    };
    static const qword_t sources[] = {
        UINT64_C(0x81),
        UINT64_C(0x8002),
        UINT64_C(0x80000003),
        UINT64_C(0x8877665544332211),
    };
    static const qword_t expected[4][4] = {
        {0, 0, 0, 0},
        {
            UINT64_C(0x81),
            UINT64_C(0x8002),
            UINT64_C(0x80000003),
            UINT64_C(0x8877665544332211),
        },
        {
            UINT64_C(0xffffffffffffff81),
            UINT64_C(0xffffffffffff8002),
            UINT64_C(0xffffffff80000003),
            0,
        },
        {
            UINT64_C(0x00000000ffffff81),
            UINT64_C(0x00000000ffff8002),
            0,
            0,
        },
    };

    unsigned case_count = 0;
    for (byte_t size_shift = 0; size_shift < 4; size_shift++) {
        for (byte_t operation = 1; operation < 4; operation++) {
            bool valid = operation == 1 ||
                    (operation == 2 && size_shift < 3) ||
                    (operation == 3 && size_shift < 2);
            if (!valid)
                continue;
            for (unsigned extend_index = 0;
                    extend_index < array_size(extend_types);
                    extend_index++) {
                enum aarch64_extend_type extend_type =
                        extend_types[extend_index];
                bool negative = extend_type == AARCH64_EXTEND_SXTW ||
                        extend_type == AARCH64_EXTEND_SXTX;
                for (unsigned scaled_index = 0;
                        scaled_index < 2; scaled_index++) {
                    bool scaled = scaled_index != 0;
                    byte_t size = (byte_t) (1U << size_shift);
                    size_t magnitude = (size_t) 3 <<
                            (scaled ? size_shift : 0);
                    size_t target_offset = negative ?
                            0x400 - magnitude : 0x400 + magnitude;
                    dword_t instruction = encode_scalar_register_offset(
                            size_shift, operation, extend_type, scaled,
                            6, 5, 4);

                    struct test_fixture c_fixture;
                    struct test_fixture threaded_fixture;
                    init_fixture(&c_fixture);
                    init_fixture(&threaded_fixture);
                    put_value(c_fixture.memory.data + target_offset,
                            size, sources[size_shift]);
                    put_value(threaded_fixture.memory.data + target_offset,
                            size, sources[size_shift]);
                    write_instruction(&c_fixture.tlb,
                            CODE_PAGE, instruction);
                    write_instruction(&threaded_fixture.tlb,
                            CODE_PAGE, instruction);

                    struct cpu_state initial;
                    init_differential_cpu(&initial);
                    initial.x[4] = UINT64_MAX;
                    initial.x[5] = DATA_PAGE + 0x400;
                    initial.x[6] = index_values[extend_index];
                    aarch64_set_exclusive(&initial,
                            DATA_PAGE + 0x60, 8, false,
                            UINT64_C(0x3344), 0, NULL, 4, 6, 8);
                    struct cpu_state result =
                            run_fast_differential_fixtures(
                                    instruction, initial,
                                    AARCH64_STEP_RETIRED,
                                    &c_fixture, &threaded_fixture, NULL);
                    assert(result.x[4] ==
                            expected[operation][size_shift]);
                    assert(result.x[5] == initial.x[5]);
                    assert(result.x[6] == initial.x[6]);
                    assert(result.pc == CODE_PAGE + 4);
                    assert(result.cycle == initial.cycle + 1);
                    assert(result.nzcv == initial.nzcv);
                    assert(result.fpcr == initial.fpcr);
                    assert(result.fpsr == initial.fpsr);
                    assert(result.exclusive.valid);
                    assert(result.exclusive.address ==
                            DATA_PAGE + 0x60);
                    assert(result.exclusive.write_epoch == 6);
                    assert(result.exclusive.sync_identity == 8);
                    case_count++;
                }
            }
        }
    }
    assert(case_count == 72);

    assert(encode_scalar_register_offset(3, 1, AARCH64_EXTEND_UXTX,
            true, 2, 3, 4) == INSTRUCTION_LDR_X4_X3_X2_LSL_3);
    assert(encode_scalar_register_offset(3, 1, AARCH64_EXTEND_UXTX,
            false, 1, 0, 3) == INSTRUCTION_LDR_X3_X0_X1);
    const struct {
        dword_t instruction;
        byte_t rt;
        byte_t rn;
        byte_t rm;
        size_t target_offset;
    } profile_cases[] = {
        {INSTRUCTION_LDR_X4_X3_X2_LSL_3, 4, 3, 2, 24},
        {INSTRUCTION_LDR_X3_X0_X1, 3, 0, 1, 3},
    };
    for (unsigned index = 0;
            index < array_size(profile_cases); index++) {
        struct test_fixture c_fixture;
        struct test_fixture threaded_fixture;
        init_fixture(&c_fixture);
        init_fixture(&threaded_fixture);
        const size_t base_offset = 0x180;
        const qword_t value = UINT64_C(0x1020304050607080);
        put_value(c_fixture.memory.data + base_offset +
                profile_cases[index].target_offset, 8, value);
        put_value(threaded_fixture.memory.data + base_offset +
                profile_cases[index].target_offset, 8, value);
        write_instruction(&c_fixture.tlb, CODE_PAGE,
                profile_cases[index].instruction);
        write_instruction(&threaded_fixture.tlb, CODE_PAGE,
                profile_cases[index].instruction);
        struct cpu_state initial;
        init_differential_cpu(&initial);
        initial.x[profile_cases[index].rn] = DATA_PAGE + base_offset;
        initial.x[profile_cases[index].rm] = 3;
        initial.x[profile_cases[index].rt] = UINT64_MAX;
        struct cpu_state result = run_fast_differential_fixtures(
                profile_cases[index].instruction, initial,
                AARCH64_STEP_RETIRED,
                &c_fixture, &threaded_fixture, NULL);
        assert(result.x[profile_cases[index].rt] == value);
    }
}

static void test_fast_load_register_offset_aliases(void) {
    {
        struct test_fixture c_fixture;
        struct test_fixture threaded_fixture;
        init_fixture(&c_fixture);
        init_fixture(&threaded_fixture);
        put_value(c_fixture.memory.primary_code +
                GUEST_MEMORY_PAGE_SIZE - 4,
                4, UINT32_C(0x44332211));
        put_value(threaded_fixture.memory.primary_code +
                GUEST_MEMORY_PAGE_SIZE - 4,
                4, UINT32_C(0x44332211));
        put_value(c_fixture.memory.data,
                4, UINT32_C(0x88776655));
        put_value(threaded_fixture.memory.data,
                4, UINT32_C(0x88776655));
        dword_t instruction = encode_scalar_register_offset(
                3, 1, AARCH64_EXTEND_UXTX, false, 2, 1, 1);
        write_instruction(&c_fixture.tlb, CODE_PAGE, instruction);
        write_instruction(&threaded_fixture.tlb,
                CODE_PAGE, instruction);
        struct cpu_state initial;
        init_differential_cpu(&initial);
        initial.x[1] = DATA_PAGE - 4;
        initial.x[2] = 0;
        struct cpu_state result = run_fast_differential_fixtures(
                instruction, initial, AARCH64_STEP_RETIRED,
                &c_fixture, &threaded_fixture, NULL);
        assert(result.x[1] == UINT64_C(0x8877665544332211));
        assert(result.x[2] == 0);
    }

    {
        struct test_fixture c_fixture;
        struct test_fixture threaded_fixture;
        init_fixture(&c_fixture);
        init_fixture(&threaded_fixture);
        const qword_t value = UINT64_C(0xa1a2a3a4a5a6a7a8);
        put_value(c_fixture.memory.data + 0x308, 8, value);
        put_value(threaded_fixture.memory.data + 0x308, 8, value);
        dword_t instruction = encode_scalar_register_offset(
                3, 1, AARCH64_EXTEND_UXTX, false, 2, 31, 2);
        write_instruction(&c_fixture.tlb, CODE_PAGE, instruction);
        write_instruction(&threaded_fixture.tlb,
                CODE_PAGE, instruction);
        struct cpu_state initial;
        init_differential_cpu(&initial);
        initial.x[2] = 8;
        struct cpu_state result = run_fast_differential_fixtures(
                instruction, initial, AARCH64_STEP_RETIRED,
                &c_fixture, &threaded_fixture, NULL);
        assert(result.x[2] == value);
        assert(result.sp == initial.sp);
    }

    {
        struct test_fixture c_fixture;
        struct test_fixture threaded_fixture;
        init_fixture(&c_fixture);
        init_fixture(&threaded_fixture);
        const guest_addr_t target = DATA_PAGE + 0x280;
        const qword_t value = UINT64_C(0xb1b2b3b4b5b6b7b8);
        put_value(c_fixture.memory.data + 0x280, 8, value);
        put_value(threaded_fixture.memory.data + 0x280, 8, value);
        dword_t instruction = encode_scalar_register_offset(
                3, 1, AARCH64_EXTEND_UXTX, false, 1, 1, 1);
        write_instruction(&c_fixture.tlb, CODE_PAGE, instruction);
        write_instruction(&threaded_fixture.tlb,
                CODE_PAGE, instruction);
        struct cpu_state initial;
        init_differential_cpu(&initial);
        initial.x[1] = target / 2;
        struct cpu_state result = run_fast_differential_fixtures(
                instruction, initial, AARCH64_STEP_RETIRED,
                &c_fixture, &threaded_fixture, NULL);
        assert(result.x[1] == value);
    }

    {
        struct test_fixture c_fixture;
        struct test_fixture threaded_fixture;
        init_fixture(&c_fixture);
        init_fixture(&threaded_fixture);
        put_value(c_fixture.memory.data + 0x200, 8,
                UINT64_C(0xc1c2c3c4c5c6c7c8));
        put_value(threaded_fixture.memory.data + 0x200, 8,
                UINT64_C(0xc1c2c3c4c5c6c7c8));
        dword_t instruction = encode_scalar_register_offset(
                3, 1, AARCH64_EXTEND_UXTX, false, 31, 5, 31);
        write_instruction(&c_fixture.tlb, CODE_PAGE, instruction);
        write_instruction(&threaded_fixture.tlb,
                CODE_PAGE, instruction);
        struct cpu_state initial;
        init_differential_cpu(&initial);
        initial.x[5] = DATA_PAGE + 0x200;
        struct cpu_state result = run_fast_differential_fixtures(
                instruction, initial, AARCH64_STEP_RETIRED,
                &c_fixture, &threaded_fixture, NULL);
        assert(result.x[5] == initial.x[5]);
        assert(result.sp == initial.sp);
    }
}

static void test_fast_load_register_offset_faults(void) {
    struct test_fixture c_fixture;
    struct test_fixture threaded_fixture;
    init_fixture(&c_fixture);
    init_fixture(&threaded_fixture);
    dword_t instruction = encode_scalar_register_offset(
            3, 1, AARCH64_EXTEND_SXTW, false, 2, 10, 31);
    write_instruction(&c_fixture.tlb, CODE_PAGE, instruction);
    write_instruction(&threaded_fixture.tlb, CODE_PAGE, instruction);
    struct cpu_state initial;
    init_differential_cpu(&initial);
    initial.x[2] = UINT64_C(0x01234567fffffffd);
    initial.x[10] = DATA_PAGE + GUEST_MEMORY_PAGE_SIZE - 1;
    aarch64_set_exclusive(&initial, DATA_PAGE + 0x300, 8, false,
            UINT64_C(0x1122), 0, NULL, 3, 5, 7);
    struct aarch64_step_result step;
    struct cpu_state result = run_fast_differential_fixtures(
            instruction, initial, AARCH64_STEP_DATA_FAULT,
            &c_fixture, &threaded_fixture, &step);
    assert(step.fault.address == DATA_PAGE + GUEST_MEMORY_PAGE_SIZE);
    assert(step.fault.access == GUEST_MEMORY_READ);
    assert(step.fault.kind == GUEST_MEMORY_FAULT_UNMAPPED);
    assert(result.x[2] == initial.x[2]);
    assert(result.x[10] == initial.x[10]);
    assert(result.pc == CODE_PAGE);
    assert(result.cycle == initial.cycle);
    assert(!result.exclusive.valid);

    init_fixture(&c_fixture);
    init_fixture(&threaded_fixture);
    instruction = encode_scalar_register_offset(
            3, 1, AARCH64_EXTEND_UXTX, false, 2, 1, 2);
    write_instruction(&c_fixture.tlb, CODE_PAGE, instruction);
    write_instruction(&threaded_fixture.tlb, CODE_PAGE, instruction);
    c_fixture.memory.code_permissions = GUEST_MEMORY_EXECUTE;
    threaded_fixture.memory.code_permissions = GUEST_MEMORY_EXECUTE;
    guest_address_space_changed(&c_fixture.space);
    guest_address_space_changed(&threaded_fixture.space);
    init_differential_cpu(&initial);
    initial.x[1] = CODE_PAGE + 0xf8;
    initial.x[2] = 8;
    aarch64_set_exclusive(&initial, DATA_PAGE + 0x300, 8, false,
            UINT64_C(0x1122), 0, NULL, 3, 5, 7);
    result = run_fast_differential_fixtures(
            instruction, initial, AARCH64_STEP_DATA_FAULT,
            &c_fixture, &threaded_fixture, &step);
    assert(step.fault.address == CODE_PAGE + 0x100);
    assert(step.fault.access == GUEST_MEMORY_READ);
    assert(step.fault.kind == GUEST_MEMORY_FAULT_PERMISSION);
    assert(result.x[1] == initial.x[1]);
    assert(result.x[2] == initial.x[2]);
    assert(result.pc == CODE_PAGE);
    assert(result.cycle == initial.cycle);
    assert(!result.exclusive.valid);
}

static void test_fast_store_register_offset_differential(void) {
    static const enum aarch64_extend_type extend_types[] = {
        AARCH64_EXTEND_UXTW,
        AARCH64_EXTEND_UXTX,
        AARCH64_EXTEND_SXTW,
        AARCH64_EXTEND_SXTX,
    };
    static const qword_t index_values[] = {
        UINT64_C(0xabcdef0100000003),
        UINT64_C(3),
        UINT64_C(0x01234567fffffffd),
        UINT64_C(0xfffffffffffffffd),
    };
    const qword_t source = UINT64_C(0x8877665544332211);
    unsigned case_count = 0;

    for (byte_t size_shift = 0; size_shift < 4; size_shift++) {
        for (unsigned extend_index = 0;
                extend_index < array_size(extend_types);
                extend_index++) {
            enum aarch64_extend_type extend_type =
                    extend_types[extend_index];
            bool negative = extend_type == AARCH64_EXTEND_SXTW ||
                    extend_type == AARCH64_EXTEND_SXTX;
            for (unsigned scaled_index = 0;
                    scaled_index < 2; scaled_index++) {
                bool scaled = scaled_index != 0;
                byte_t size = (byte_t) (1U << size_shift);
                byte_t shift = scaled ? size_shift : 0;
                size_t magnitude = (size_t) 3 << shift;
                size_t target_offset = negative ?
                        0x400 - magnitude : 0x400 + magnitude;
                dword_t instruction = encode_scalar_register_offset(
                        size_shift, 0, extend_type, scaled, 6, 5, 4);

                struct test_fixture c_fixture;
                struct test_fixture threaded_fixture;
                init_fixture(&c_fixture);
                init_fixture(&threaded_fixture);
                memset(c_fixture.memory.data, 0xa5,
                        sizeof(c_fixture.memory.data));
                memset(threaded_fixture.memory.data, 0xa5,
                        sizeof(threaded_fixture.memory.data));
                write_instruction(&c_fixture.tlb,
                        CODE_PAGE, instruction);
                write_instruction(&threaded_fixture.tlb,
                        CODE_PAGE, instruction);

                struct cpu_state initial;
                init_differential_cpu(&initial);
                initial.x[4] = source;
                initial.x[5] = DATA_PAGE + 0x400;
                initial.x[6] = index_values[extend_index];
                aarch64_set_exclusive(&initial,
                        DATA_PAGE + 0x60, 8, false,
                        UINT64_C(0x3344), 0, NULL, 4, 6, 8);
                struct cpu_state result =
                        run_fast_differential_fixtures(
                                instruction, initial,
                                AARCH64_STEP_RETIRED,
                                &c_fixture, &threaded_fixture, NULL);

                for (byte_t byte = 0; byte < size; byte++) {
                    assert(c_fixture.memory.data[
                            target_offset + byte] ==
                            (byte_t) (source >> (byte * 8)));
                }
                assert(c_fixture.memory.data[target_offset - 1] == 0xa5);
                assert(c_fixture.memory.data[
                        target_offset + size] == 0xa5);
                assert(result.x[4] == source);
                assert(result.x[5] == initial.x[5]);
                assert(result.x[6] == initial.x[6]);
                assert(result.pc == CODE_PAGE + 4);
                assert(result.cycle == initial.cycle + 1);
                assert(result.nzcv == initial.nzcv);
                assert(result.fpcr == initial.fpcr);
                assert(result.fpsr == initial.fpsr);
                assert(result.exclusive.valid);
                assert(result.exclusive.write_epoch == 6);
                assert(result.exclusive.sync_identity == 8);
                case_count++;
            }
        }
    }
    assert(case_count == 32);

    assert(encode_scalar_register_offset(3, 0,
            AARCH64_EXTEND_UXTX, true, 3, 7, 6) ==
            INSTRUCTION_STR_X6_X7_X3_LSL_3);
    assert(encode_scalar_register_offset(3, 0,
            AARCH64_EXTEND_UXTX, true, 3, 1, 31) ==
            INSTRUCTION_STR_XZR_X1_X3_LSL_3);
    const struct {
        dword_t instruction;
        byte_t rn;
        byte_t rt;
        qword_t value;
    } profile_cases[] = {
        {
            INSTRUCTION_STR_X6_X7_X3_LSL_3, 7, 6,
            UINT64_C(0x1020304050607080),
        },
        {
            INSTRUCTION_STR_XZR_X1_X3_LSL_3, 1, 31,
            0,
        },
    };
    for (unsigned index = 0;
            index < array_size(profile_cases); index++) {
        struct test_fixture c_fixture;
        struct test_fixture threaded_fixture;
        init_fixture(&c_fixture);
        init_fixture(&threaded_fixture);
        memset(c_fixture.memory.data, 0xa5,
                sizeof(c_fixture.memory.data));
        memset(threaded_fixture.memory.data, 0xa5,
                sizeof(threaded_fixture.memory.data));
        write_instruction(&c_fixture.tlb, CODE_PAGE,
                profile_cases[index].instruction);
        write_instruction(&threaded_fixture.tlb, CODE_PAGE,
                profile_cases[index].instruction);
        struct cpu_state initial;
        init_differential_cpu(&initial);
        initial.x[3] = 3;
        initial.x[profile_cases[index].rn] =
                DATA_PAGE + 0x280 - 24;
        if (profile_cases[index].rt != 31)
            initial.x[profile_cases[index].rt] =
                    profile_cases[index].value;
        struct cpu_state result = run_fast_differential_fixtures(
                profile_cases[index].instruction, initial,
                AARCH64_STEP_RETIRED,
                &c_fixture, &threaded_fixture, NULL);
        for (byte_t byte = 0; byte < 8; byte++) {
            assert(c_fixture.memory.data[0x280 + byte] ==
                    (byte_t) (profile_cases[index].value >>
                            (byte * 8)));
        }
        assert(result.x[3] == initial.x[3]);
        assert(result.x[profile_cases[index].rn] ==
                initial.x[profile_cases[index].rn]);
        assert(result.sp == initial.sp);
    }
}

static void test_fast_store_register_offset_boundaries(void) {
    {
        struct test_fixture c_fixture;
        struct test_fixture threaded_fixture;
        init_fixture(&c_fixture);
        init_fixture(&threaded_fixture);
        memset(c_fixture.memory.data + 0x280, 0xa5, 8);
        memset(threaded_fixture.memory.data + 0x280, 0xa5, 8);
        dword_t instruction = encode_scalar_register_offset(
                3, 0, AARCH64_EXTEND_UXTX, false, 31, 31, 31);
        write_instruction(&c_fixture.tlb, CODE_PAGE, instruction);
        write_instruction(&threaded_fixture.tlb,
                CODE_PAGE, instruction);
        struct cpu_state initial;
        init_differential_cpu(&initial);
        initial.sp = DATA_PAGE + 0x280;
        struct cpu_state result = run_fast_differential_fixtures(
                instruction, initial, AARCH64_STEP_RETIRED,
                &c_fixture, &threaded_fixture, NULL);
        const byte_t zeros[8] = {0};
        assert(memcmp(c_fixture.memory.data + 0x280,
                zeros, sizeof(zeros)) == 0);
        assert(result.sp == initial.sp);
    }

    {
        struct test_fixture c_fixture;
        struct test_fixture threaded_fixture;
        init_fixture(&c_fixture);
        init_fixture(&threaded_fixture);
        dword_t instruction = encode_scalar_register_offset(
                3, 0, AARCH64_EXTEND_UXTX, false, 5, 5, 5);
        write_instruction(&c_fixture.tlb, CODE_PAGE, instruction);
        write_instruction(&threaded_fixture.tlb,
                CODE_PAGE, instruction);
        struct cpu_state initial;
        init_differential_cpu(&initial);
        initial.x[5] = (UINT64_C(1) << 63) + DATA_PAGE / 2;
        struct cpu_state result = run_fast_differential_fixtures(
                instruction, initial, AARCH64_STEP_RETIRED,
                &c_fixture, &threaded_fixture, NULL);
        for (byte_t byte = 0; byte < 8; byte++) {
            assert(c_fixture.memory.data[byte] ==
                    (byte_t) (initial.x[5] >> (byte * 8)));
        }
        assert(result.x[5] == initial.x[5]);
    }

    {
        struct test_fixture c_fixture;
        struct test_fixture threaded_fixture;
        init_fixture(&c_fixture);
        init_fixture(&threaded_fixture);
        dword_t instruction = encode_scalar_register_offset(
                3, 0, AARCH64_EXTEND_SXTX, false, 2, 1, 4);
        write_instruction(&c_fixture.tlb, CODE_PAGE, instruction);
        write_instruction(&threaded_fixture.tlb,
                CODE_PAGE, instruction);
        const guest_addr_t target = DATA_PAGE + 0x280;
        const qword_t index = UINT64_C(0x0000000100000003);
        const qword_t source = UINT64_C(0x8877665544332211);
        struct cpu_state initial;
        init_differential_cpu(&initial);
        initial.x[1] = target - index;
        initial.x[2] = index;
        initial.x[4] = source;
        struct cpu_state result = run_fast_differential_fixtures(
                instruction, initial, AARCH64_STEP_RETIRED,
                &c_fixture, &threaded_fixture, NULL);
        for (byte_t byte = 0; byte < 8; byte++) {
            assert(c_fixture.memory.data[0x280 + byte] ==
                    (byte_t) (source >> (byte * 8)));
        }
        assert(result.x[1] == initial.x[1]);
        assert(result.x[2] == index);
        assert(result.x[4] == source);
    }
}

static void test_fast_store_register_offset_faults(void) {
    struct test_fixture c_fixture;
    struct test_fixture threaded_fixture;
    init_fixture(&c_fixture);
    init_fixture(&threaded_fixture);
    memset(c_fixture.memory.data + GUEST_MEMORY_PAGE_SIZE - 4,
            0xa5, 4);
    memset(threaded_fixture.memory.data + GUEST_MEMORY_PAGE_SIZE - 4,
            0xa5, 4);
    dword_t instruction = encode_scalar_register_offset(
            3, 0, AARCH64_EXTEND_UXTX, false, 31, 5, 4);
    write_instruction(&c_fixture.tlb, CODE_PAGE, instruction);
    write_instruction(&threaded_fixture.tlb, CODE_PAGE, instruction);
    struct cpu_state initial;
    init_differential_cpu(&initial);
    initial.x[4] = UINT64_C(0x8877665544332211);
    initial.x[5] = DATA_PAGE + GUEST_MEMORY_PAGE_SIZE - 4;
    aarch64_set_exclusive(&initial, DATA_PAGE + 0x300,
            8, false, UINT64_C(0x1122), 0, NULL, 3, 5, 7);
    struct aarch64_step_result step;
    struct cpu_state result = run_fast_differential_fixtures(
            instruction, initial, AARCH64_STEP_DATA_FAULT,
            &c_fixture, &threaded_fixture, &step);
    assert(step.fault.address == DATA_PAGE + GUEST_MEMORY_PAGE_SIZE);
    assert(step.fault.access == GUEST_MEMORY_WRITE);
    assert(step.fault.kind == GUEST_MEMORY_FAULT_UNMAPPED);
    for (byte_t byte = 0; byte < 4; byte++) {
        assert(c_fixture.memory.data[
                GUEST_MEMORY_PAGE_SIZE - 4 + byte] == 0xa5);
    }
    assert(result.x[4] == initial.x[4]);
    assert(result.x[5] == initial.x[5]);
    assert(result.pc == CODE_PAGE);
    assert(result.cycle == initial.cycle);
    assert(!result.exclusive.valid);

    init_fixture(&c_fixture);
    init_fixture(&threaded_fixture);
    instruction = encode_scalar_register_offset(
            2, 0, AARCH64_EXTEND_UXTW, true, 2, 1, 4);
    write_instruction(&c_fixture.tlb, CODE_PAGE, instruction);
    write_instruction(&threaded_fixture.tlb, CODE_PAGE, instruction);
    memset(c_fixture.memory.primary_code + 0x100, 0xa5, 4);
    memset(threaded_fixture.memory.primary_code + 0x100, 0xa5, 4);
    c_fixture.memory.code_permissions = GUEST_MEMORY_EXECUTE;
    threaded_fixture.memory.code_permissions = GUEST_MEMORY_EXECUTE;
    guest_address_space_changed(&c_fixture.space);
    guest_address_space_changed(&threaded_fixture.space);
    init_differential_cpu(&initial);
    initial.x[1] = CODE_PAGE + 0xfc;
    initial.x[2] = 1;
    initial.x[4] = UINT64_C(0xffffffff89abcdef);
    aarch64_set_exclusive(&initial, DATA_PAGE + 0x300,
            8, false, UINT64_C(0x1122), 0, NULL, 3, 5, 7);
    result = run_fast_differential_fixtures(
            instruction, initial, AARCH64_STEP_DATA_FAULT,
            &c_fixture, &threaded_fixture, &step);
    assert(step.fault.address == CODE_PAGE + 0x100);
    assert(step.fault.access == GUEST_MEMORY_WRITE);
    assert(step.fault.kind == GUEST_MEMORY_FAULT_PERMISSION);
    for (byte_t byte = 0; byte < 4; byte++)
        assert(c_fixture.memory.primary_code[0x100 + byte] == 0xa5);
    assert(result.x[1] == initial.x[1]);
    assert(result.x[2] == initial.x[2]);
    assert(result.x[4] == initial.x[4]);
    assert(result.pc == CODE_PAGE);
    assert(result.cycle == initial.cycle);
    assert(!result.exclusive.valid);

    init_fixture(&c_fixture);
    init_fixture(&threaded_fixture);
    instruction = encode_scalar_register_offset(
            3, 0, AARCH64_EXTEND_UXTX, false, 31, 5, 4);
    write_instruction(&c_fixture.tlb, CODE_PAGE, instruction);
    write_instruction(&threaded_fixture.tlb, CODE_PAGE, instruction);
    init_differential_cpu(&initial);
    initial.x[4] = UINT64_C(0x8877665544332211);
    initial.x[5] = (UINT64_C(1) << 48) - 4;
    aarch64_set_exclusive(&initial, DATA_PAGE + 0x300,
            8, false, UINT64_C(0x1122), 0, NULL, 3, 5, 7);
    result = run_fast_differential_fixtures(
            instruction, initial, AARCH64_STEP_DATA_FAULT,
            &c_fixture, &threaded_fixture, &step);
    assert(step.fault.address == (UINT64_C(1) << 48));
    assert(step.fault.access == GUEST_MEMORY_WRITE);
    assert(step.fault.kind == GUEST_MEMORY_FAULT_ADDRESS_SIZE);
    assert(result.x[4] == initial.x[4]);
    assert(result.x[5] == initial.x[5]);
    assert(result.pc == CODE_PAGE);
    assert(result.cycle == initial.cycle);
    assert(!result.exclusive.valid);
}

static void test_fast_store_register_offset_exclusive_invalidation(void) {
    struct test_fixture c_fixture;
    struct test_fixture threaded_fixture;
    init_fixture(&c_fixture);
    init_fixture(&threaded_fixture);
    const dword_t instructions[] = {
        INSTRUCTION_LDXR_X2_X1,
        encode_scalar_register_offset(3, 0,
                AARCH64_EXTEND_UXTX, false, 31, 1, 0),
        INSTRUCTION_STXR_W4_X3_X1,
    };
    for (unsigned index = 0; index < array_size(instructions); index++) {
        write_instruction(&c_fixture.tlb,
                CODE_PAGE + index * 4, instructions[index]);
        write_instruction(&threaded_fixture.tlb,
                CODE_PAGE + index * 4, instructions[index]);
    }
    put_value(c_fixture.memory.data + 0x180, 8,
            UINT64_C(0x1020304050607080));
    put_value(threaded_fixture.memory.data + 0x180, 8,
            UINT64_C(0x1020304050607080));

    struct aarch64_runner c_runner;
    struct aarch64_runner threaded_runner;
    assert(aarch64_runner_init_backend(
            &c_runner, &c_fixture.tlb, AARCH64_BACKEND_C));
    assert(aarch64_runner_init_backend(&threaded_runner,
            &threaded_fixture.tlb, AARCH64_BACKEND_THREADED));
    struct cpu_state c_cpu;
    struct cpu_state threaded_cpu;
    init_differential_cpu(&c_cpu);
    init_differential_cpu(&threaded_cpu);
    c_cpu.x[0] = threaded_cpu.x[0] =
            UINT64_C(0x8877665544332211);
    c_cpu.x[1] = threaded_cpu.x[1] = DATA_PAGE + 0x180;
    c_cpu.x[3] = threaded_cpu.x[3] =
            UINT64_C(0xaabbccddeeff0011);
    c_cpu.x[4] = threaded_cpu.x[4] = UINT64_MAX;

    for (unsigned index = 0; index < array_size(instructions); index++) {
        struct aarch64_step_result c_result =
                aarch64_run_one(&c_runner, &c_cpu);
        struct aarch64_step_result threaded_result =
                aarch64_run_one(&threaded_runner, &threaded_cpu);
        assert(c_result.stop == AARCH64_STEP_RETIRED);
        assert_step_equal(&c_result, &threaded_result);
        assert_cpu_equal(&c_cpu, &threaded_cpu);
        assert_memory_equal(&c_fixture.memory,
                &threaded_fixture.memory);
        if (index == 1) {
            assert(c_cpu.exclusive.valid);
            assert_stats(&threaded_runner, 0, 2, 1, 1);
        }
    }
    assert(c_cpu.x[2] == UINT64_C(0x1020304050607080));
    assert(c_cpu.x[3] == UINT64_C(0xaabbccddeeff0011));
    assert(c_cpu.x[4] == 1);
    assert(!c_cpu.exclusive.valid);
    assert_stats(&threaded_runner, 0, 3, 1, 2);
    const byte_t expected[] = {
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
    };
    assert(memcmp(c_fixture.memory.data + 0x180,
            expected, sizeof(expected)) == 0);
}

static void test_simd_register_offset_sibling_fallback(void) {
    const dword_t instructions[] = {
        INSTRUCTION_STR_Q30_X21_X0,
        INSTRUCTION_LDR_Q11_X12_X13_SXTX_4,
    };
    const enum aarch64_opcode opcodes[] = {
        AARCH64_OP_STORE_SIMD_REGISTER_OFFSET,
        AARCH64_OP_LOAD_SIMD_REGISTER_OFFSET,
    };
    struct test_fixture fixture;
    init_fixture(&fixture);
    for (unsigned index = 0; index < array_size(instructions); index++) {
        struct aarch64_decoded decoded;
        assert(aarch64_decode(instructions[index], &decoded));
        assert(decoded.opcode == opcodes[index]);
        write_instruction(&fixture.tlb,
                CODE_PAGE + index * 4, instructions[index]);
    }
    static const union aarch64_vector_reg source = {
        .d = {
            UINT64_C(0x0123456789abcdef),
            UINT64_C(0xfedcba9876543210),
        },
    };
    memcpy(fixture.memory.data + 0x180,
            source.b, sizeof(source.b));
    struct aarch64_runner runner;
    assert(aarch64_runner_init_backend(
            &runner, &fixture.tlb, AARCH64_BACKEND_THREADED));
    struct cpu_state cpu;
    init_differential_cpu(&cpu);
    cpu.x[0] = 0;
    cpu.x[21] = DATA_PAGE;
    cpu.x[12] = DATA_PAGE + 0x180;
    cpu.x[13] = 0;
    memset(cpu.v[11].b, 0x5a, sizeof(cpu.v[11].b));
    for (unsigned index = 0; index < array_size(instructions); index++) {
        assert(aarch64_run_one(&runner, &cpu).stop ==
                AARCH64_STEP_RETIRED);
    }
    assert(memcmp(cpu.v[11].b,
            source.b, sizeof(source.b)) == 0);
    assert_stats(&runner, 0, 2, 0, 2);
}

static void test_fast_branch_differential(void) {
    struct cpu_state initial;
    struct cpu_state result;

    init_differential_cpu(&initial);
    result = run_fast_differential(
            encode_branch_immediate(false, 20),
            initial, AARCH64_STEP_RETIRED);
    assert(result.pc == CODE_PAGE + 20);

    init_differential_cpu(&initial);
    result = run_fast_differential(
            encode_branch_immediate(false, -12),
            initial, AARCH64_STEP_RETIRED);
    assert(result.pc == CODE_PAGE - 12);

    init_differential_cpu(&initial);
    result = run_fast_differential(
            encode_branch_immediate(true, 24),
            initial, AARCH64_STEP_RETIRED);
    assert(result.pc == CODE_PAGE + 24);
    assert(result.x[30] == CODE_PAGE + 4);

    init_differential_cpu(&initial);
    initial.x[5] = CODE_PAGE + 0x120;
    result = run_fast_differential(
            encode_branch_register(AARCH64_OP_BR, 5),
            initial, AARCH64_STEP_RETIRED);
    assert(result.pc == CODE_PAGE + 0x120);

    init_differential_cpu(&initial);
    initial.x[30] = CODE_PAGE + 0x180;
    result = run_fast_differential(
            encode_branch_register(AARCH64_OP_BLR, 30),
            initial, AARCH64_STEP_RETIRED);
    assert(result.pc == CODE_PAGE + 0x180);
    assert(result.x[30] == CODE_PAGE + 4);

    init_differential_cpu(&initial);
    initial.x[7] = CODE_PAGE + 0x1c0;
    result = run_fast_differential(
            encode_branch_register(AARCH64_OP_RET, 7),
            initial, AARCH64_STEP_RETIRED);
    assert(result.pc == CODE_PAGE + 0x1c0);

    init_differential_cpu(&initial);
    initial.nzcv = UINT32_C(0x40000000);
    result = run_fast_differential(
            encode_conditional_branch(24, 0),
            initial, AARCH64_STEP_RETIRED);
    assert(result.pc == CODE_PAGE + 24);

    init_differential_cpu(&initial);
    initial.nzcv = 0;
    result = run_fast_differential(
            encode_conditional_branch(24, 0),
            initial, AARCH64_STEP_RETIRED);
    assert(result.pc == CODE_PAGE + 4);

    init_differential_cpu(&initial);
    initial.x[3] = UINT64_C(0x1234567800000000);
    result = run_fast_differential(
            encode_compare_branch(false, false, 16, 3),
            initial, AARCH64_STEP_RETIRED);
    assert(result.pc == CODE_PAGE + 16);

    init_differential_cpu(&initial);
    initial.x[3] = UINT64_C(0x1234567800000000);
    result = run_fast_differential(
            encode_compare_branch(true, false, 16, 3),
            initial, AARCH64_STEP_RETIRED);
    assert(result.pc == CODE_PAGE + 4);

    init_differential_cpu(&initial);
    initial.x[4] = 1;
    result = run_fast_differential(
            encode_compare_branch(false, true, 16, 4),
            initial, AARCH64_STEP_RETIRED);
    assert(result.pc == CODE_PAGE + 16);

    init_differential_cpu(&initial);
    initial.x[4] = UINT64_C(0x100000000);
    result = run_fast_differential(
            encode_compare_branch(true, true, 16, 4),
            initial, AARCH64_STEP_RETIRED);
    assert(result.pc == CODE_PAGE + 16);

    init_differential_cpu(&initial);
    result = run_fast_differential(
            encode_compare_branch(true, false, 16, 31),
            initial, AARCH64_STEP_RETIRED);
    assert(result.pc == CODE_PAGE + 16);

    init_differential_cpu(&initial);
    result = run_fast_differential(
            encode_compare_branch(true, true, 16, 31),
            initial, AARCH64_STEP_RETIRED);
    assert(result.pc == CODE_PAGE + 4);

    init_differential_cpu(&initial);
    initial.x[5] = 0;
    result = run_fast_differential(
            encode_test_branch(false, 7, 12, 5),
            initial, AARCH64_STEP_RETIRED);
    assert(result.pc == CODE_PAGE + 12);

    init_differential_cpu(&initial);
    initial.x[5] = UINT64_C(1) << 7;
    result = run_fast_differential(
            encode_test_branch(false, 7, 12, 5),
            initial, AARCH64_STEP_RETIRED);
    assert(result.pc == CODE_PAGE + 4);

    init_differential_cpu(&initial);
    initial.x[6] = UINT64_C(1) << 42;
    result = run_fast_differential(
            encode_test_branch(true, 42, 12, 6),
            initial, AARCH64_STEP_RETIRED);
    assert(result.pc == CODE_PAGE + 12);

    init_differential_cpu(&initial);
    initial.x[6] = 0;
    result = run_fast_differential(
            encode_test_branch(true, 42, 12, 6),
            initial, AARCH64_STEP_RETIRED);
    assert(result.pc == CODE_PAGE + 4);
}

static void test_fast_svc_differential(void) {
    struct test_fixture c_fixture;
    struct test_fixture threaded_fixture;
    init_fixture(&c_fixture);
    init_fixture(&threaded_fixture);
    const dword_t instruction = UINT32_C(0xd4000541);
    write_instruction(&c_fixture.tlb, CODE_PAGE, instruction);
    write_instruction(&threaded_fixture.tlb, CODE_PAGE, instruction);

    struct aarch64_runner c_runner;
    struct aarch64_runner threaded_runner;
    assert(aarch64_runner_init_backend(
            &c_runner, &c_fixture.tlb, AARCH64_BACKEND_C));
    assert(aarch64_runner_init_backend(&threaded_runner,
            &threaded_fixture.tlb, AARCH64_BACKEND_THREADED));
    struct cpu_state c_cpu;
    struct cpu_state threaded_cpu;
    init_differential_cpu(&c_cpu);
    init_differential_cpu(&threaded_cpu);
    aarch64_set_exclusive(&c_cpu, DATA_PAGE + 0x100, 8, true,
            UINT64_C(0x1122334455667788),
            UINT64_C(0x99aabbccddeeff00), &c_fixture.space, 7, 11, 13);
    aarch64_set_exclusive(&threaded_cpu, DATA_PAGE + 0x100, 8, true,
            UINT64_C(0x1122334455667788),
            UINT64_C(0x99aabbccddeeff00), &threaded_fixture.space,
            7, 11, 13);

    struct aarch64_step_result c_result =
            aarch64_run_one(&c_runner, &c_cpu);
    struct aarch64_step_result threaded_result =
            aarch64_run_one(&threaded_runner, &threaded_cpu);
    assert(c_result.stop == AARCH64_STEP_SYSCALL);
    assert_step_equal(&c_result, &threaded_result);
    assert_cpu_equal(&c_cpu, &threaded_cpu);
    assert_memory_equal(&c_fixture.memory, &threaded_fixture.memory);
    assert(c_cpu.pc == CODE_PAGE + 4);
    assert(c_cpu.cycle == 10);
    assert(!c_cpu.exclusive.valid);
    assert(c_cpu.exclusive.address == DATA_PAGE + 0x100);
    assert(c_cpu.exclusive.size == 8);
    assert(c_cpu.exclusive.pair);
    assert(c_cpu.exclusive.value_low == UINT64_C(0x1122334455667788));
    assert(c_cpu.exclusive.value_high == UINT64_C(0x99aabbccddeeff00));
    assert(c_cpu.exclusive.address_space == &c_fixture.space);
    assert(threaded_cpu.exclusive.address_space == &threaded_fixture.space);
    assert(c_cpu.exclusive.mapping_epoch == 7);
    assert(c_cpu.exclusive.write_epoch == 11);
    assert(c_cpu.exclusive.sync_identity == 13);
    assert_stats(&threaded_runner, 0, 1, 1, 0);
}

static void test_fast_dispatch_structure(void) {
    struct test_fixture fixture;
    init_fixture(&fixture);
    const dword_t hot_instructions[] = {
        INSTRUCTION_ORR_X3_XZR_X0,
        encode_add_shifted(true, AARCH64_SHIFT_LSL, 0, 2, 0, 3),
        encode_eor_shifted(true, false, AARCH64_SHIFT_LSL,
                0, 2, 0, 4),
        INSTRUCTION_LDR_X2,
        INSTRUCTION_AND_X2_X2_X6,
        INSTRUCTION_EXTR_W3_W1_W1_19,
        encode_scalar_register_offset(3, 1, AARCH64_EXTEND_UXTX,
                false, 31, 31, 10),
        INSTRUCTION_STR_W0_X1_32,
        INSTRUCTION_NOP,
        encode_add_sub_immediate(true, false, false, 1, 0, 0),
        encode_add_sub_immediate(true, false, true, 1, 0, 0),
        encode_add_sub_immediate(true, true, false, 1, 0, 0),
        encode_add_sub_immediate(true, true, true, 1, 0, 0),
        encode_move_wide(true, AARCH64_OP_MOVN, 0, 1, 2),
        encode_move_wide(true, AARCH64_OP_MOVZ, 0, 1, 2),
        encode_move_wide(true, AARCH64_OP_MOVK, 0, 1, 2),
        encode_branch_immediate(false, 4),
        encode_branch_immediate(true, 4),
        encode_branch_register(AARCH64_OP_BR, 5),
        encode_branch_register(AARCH64_OP_BLR, 6),
        encode_branch_register(AARCH64_OP_RET, 7),
        encode_conditional_branch(4, 0),
        encode_compare_branch(true, false, 4, 8),
        encode_compare_branch(true, true, 4, 8),
        encode_test_branch(false, 7, 4, 9),
        encode_test_branch(true, 42, 4, 9),
        INSTRUCTION_LDP_X6_X1_SP_400,
        INSTRUCTION_STP_X5_X2_SP_16,
        INSTRUCTION_UBFM_X1_X1_1_63,
        INSTRUCTION_SUBS_XZR_X2_X5,
        INSTRUCTION_ADRP_X2_PLUS_122_PAGES,
        INSTRUCTION_AND_W0_W3_7FFFFFFF,
        INSTRUCTION_SUB_W0_W4_W3,
        INSTRUCTION_CCMP_W0_W19_4_NE,
        encode_scalar_register_offset(3, 0,
                AARCH64_EXTEND_UXTX, false, 31, 31, 31),
        INSTRUCTION_CSINC_X3_XZR_XZR_NE,
        INSTRUCTION_REV_W2_W2,
        INSTRUCTION_ADD_X0_X4_W0_UXTW_3,
        INSTRUCTION_LDR_X2_X3_POST_8,
        INSTRUCTION_CSEL_X1_X0_X1_EQ,
        INSTRUCTION_ANDS_W22_W2_7FFFFFFF,
        INSTRUCTION_STP_Q0_Q0_X3_32,
        INSTRUCTION_SXTW_X2_W2,
        INSTRUCTION_LSRV_W4_W10_W4,
        INSTRUCTION_MUL_X6_X6_X0,
        INSTRUCTION_LSLV_X5_X7_X1,
        INSTRUCTION_ADR_X1_PLUS_12,
        INSTRUCTION_ANDS_XZR_X0_X5,
        INSTRUCTION_STR_XZR_X2_POST_8,
        INSTRUCTION_ORR_W10_W10_1,
        INSTRUCTION_UMADDL_X1_W0_W11_XZR,
        INSTRUCTION_STR_Q0_X0,
        INSTRUCTION_SUBS_WZR_W2_W1_UXTB,
        INSTRUCTION_SUB_SP_SP_X0,
        INSTRUCTION_MSUB_W6_W6_W5_W10,
        INSTRUCTION_LDP_Q30_Q31_X1,
        INSTRUCTION_MOVI_V31_4S_0,
        INSTRUCTION_ASRV_W2_W23_W2,
        INSTRUCTION_STUR_Q31_X0_56,
        INSTRUCTION_BFM_X0_X1_52_51,
        INSTRUCTION_EOR_W0_W4_1,
        INSTRUCTION_SMADDL_X0_W0_W1_XZR,
        INSTRUCTION_LDR_Q31_X0_D60,
        UINT32_C(0xd4000541),
    };
    _Static_assert(array_size(hot_instructions) == 64,
            "threaded 热点结构门必须保持 64 个并行驻留 token");
    _Static_assert(array_size(hot_instructions) + 1 == 65,
            "threaded 热点结构门必须覆盖全部 65 个 fast opcode");
    bool seen_opcodes[AARCH64_OP_COUNT] = {false};
    for (unsigned index = 0; index < array_size(hot_instructions); index++) {
        struct aarch64_decoded decoded;
        assert(aarch64_decode(hot_instructions[index], &decoded));
        unsigned opcode = (unsigned) decoded.opcode;
        assert(opcode < array_size(seen_opcodes));
        assert(!seen_opcodes[opcode]);
        seen_opcodes[opcode] = true;
    }
    struct aarch64_decoded clz_decoded;
    assert(aarch64_decode(INSTRUCTION_CLZ_W2_W2, &clz_decoded));
    assert(clz_decoded.opcode == AARCH64_OP_CLZ);
    assert(!seen_opcodes[clz_decoded.opcode]);
    seen_opcodes[clz_decoded.opcode] = true;
    for (unsigned index = 0; index < array_size(hot_instructions); index++)
        write_instruction(&fixture.tlb, CODE_PAGE + index * 4,
                hot_instructions[index]);

    struct aarch64_runner runner;
    assert(aarch64_runner_init_backend(
            &runner, &fixture.tlb, AARCH64_BACKEND_THREADED));
    struct cpu_state cpu;
    init_differential_cpu(&cpu);
    cpu.x[5] = CODE_PAGE;
    cpu.x[6] = CODE_PAGE + 4;
    cpu.x[7] = CODE_PAGE + 8;
    cpu.x[8] = 1;
    cpu.x[9] = UINT64_C(1) << 42;
    const byte_t zero_store[8] = {0};

    for (unsigned index = 0; index < array_size(hot_instructions); index++) {
        if (index == 38) {
            put_value(fixture.memory.data + 0x100, 8,
                    UINT64_C(0x0123456789abcdef));
            cpu.x[2] = UINT64_MAX;
            cpu.x[3] = DATA_PAGE + 0x100;
            cpu.nzcv = UINT32_C(0x90000000);
        }
        if (index == 39) {
            cpu.x[0] = UINT64_C(0x1111222233334444);
            cpu.x[1] = UINT64_C(0x5555666677778888);
            cpu.nzcv = UINT32_C(0x40000000);
        }
        if (index == 40) {
            assert(cpu.x[2] == UINT64_C(0x0123456789abcdef));
            cpu.x[2] = UINT64_C(0xffffffff89abcdef);
            cpu.x[22] = UINT64_MAX;
            cpu.nzcv = UINT32_C(0xf0000000);
        }
        if (index == 41) {
            assert(cpu.x[3] == DATA_PAGE + 0x108);
            cpu.x[3] = DATA_PAGE + 0x500;
            cpu.v[0].d[0] = UINT64_C(0x0123456789abcdef);
            cpu.v[0].d[1] = UINT64_C(0xfedcba9876543210);
            memset(fixture.memory.data + 0x520, 0xa5, 32);
        }
        if (index == 42)
            cpu.x[2] = UINT64_C(0xaaaaaaaa80000001);
        if (index == 43) {
            cpu.x[4] = 4;
            cpu.x[10] = UINT64_C(0xaaaaaaaa80000040);
        }
        if (index == 44) {
            cpu.x[6] = 7;
            cpu.x[0] = 9;
        }
        if (index == 45) {
            cpu.x[7] = 3;
            cpu.x[1] = 4;
        }
        if (index == 47) {
            cpu.x[0] = 0;
            cpu.nzcv = UINT32_C(0xf0000000);
        }
        if (index == 48) {
            assert(cpu.x[2] == UINT64_C(0xffffffff80000001));
            memset(fixture.memory.data + 0x600,
                    0xa5, sizeof(zero_store));
            cpu.x[2] = DATA_PAGE + 0x600;
        }
        if (index == 49) {
            assert(cpu.x[10] == UINT64_C(0xaaaaaaaa80000040));
            cpu.x[10] = UINT64_C(0xbbbbbbbb80000040);
        }
        if (index == 50) {
            assert(cpu.x[1] == CODE_PAGE + 46 * 4 + 12);
            cpu.x[0] = UINT64_C(0xffffffff00000003);
            cpu.x[11] = UINT64_C(0xaaaaaaaa00000004);
            cpu.x[1] = UINT64_MAX;
            cpu.nzcv = UINT32_C(0x60000000);
        }
        if (index == 51) {
            assert(cpu.x[1] == 12);
            cpu.x[0] = DATA_PAGE + 0x700;
            cpu.v[0].d[0] = UINT64_C(0x0123456789abcdef);
            cpu.v[0].d[1] = UINT64_C(0xfedcba9876543210);
            memset(fixture.memory.data + 0x700,
                    0xa5, sizeof(cpu.v[0]));
        }
        if (index == 52) {
            assert(cpu.x[2] == DATA_PAGE + 0x608);
            cpu.x[1] = UINT64_C(0xaaaaaaaa00000080);
            cpu.x[2] = UINT64_C(0xbbbbbbbb00000080);
            cpu.nzcv = UINT32_C(0x10000000);
        }
        if (index == 53) {
            cpu.sp = UINT64_C(0x1000);
            cpu.x[0] = 8;
            cpu.nzcv = UINT32_C(0x30000000);
        }
        if (index == 54) {
            cpu.x[6] = UINT64_C(0xaaaaaaaa00000003);
            cpu.x[5] = UINT64_C(0xbbbbbbbb00000004);
            cpu.x[10] = UINT64_C(0xcccccccc00000020);
            cpu.nzcv = UINT32_C(0xa0000000);
        }
        if (index == 55) {
            for (byte_t offset = 0; offset < 32; offset++)
                fixture.memory.data[0x780 + offset] =
                        (byte_t) (0x21 + offset * 3);
            cpu.x[1] = DATA_PAGE + 0x780;
            memset(cpu.v[30].b, 0x5a, sizeof(cpu.v[30].b));
            memset(cpu.v[31].b, 0xc3, sizeof(cpu.v[31].b));
            cpu.nzcv = UINT32_C(0x50000000);
        }
        if (index == 56) {
            assert(memcmp(cpu.v[30].b,
                    fixture.memory.data + 0x780,
                    sizeof(cpu.v[30].b)) == 0);
            assert(memcmp(cpu.v[31].b,
                    fixture.memory.data + 0x790,
                    sizeof(cpu.v[31].b)) == 0);
            memset(cpu.v[31].b, 0x7e, sizeof(cpu.v[31].b));
            cpu.nzcv = UINT32_C(0x50000000);
        }
        if (index == 57) {
            assert(cpu.v[31].d[0] == 0);
            assert(cpu.v[31].d[1] == 0);
            cpu.x[23] = UINT64_C(0xaaaaaaaa81234567);
            cpu.x[2] = 3;
            cpu.nzcv = UINT32_C(0x50000000);
        }
        if (index == 58) {
            cpu.x[0] = DATA_PAGE + 0x788;
            cpu.v[31].d[0] = UINT64_C(0x1122334455667788);
            cpu.v[31].d[1] = UINT64_C(0x99aabbccddeeff00);
            memset(fixture.memory.data + 0x7c0, 0xa5,
                    sizeof(cpu.v[31]));
            cpu.nzcv = UINT32_C(0x50000000);
        }
        if (index == 59) {
            cpu.x[0] = UINT64_C(0xffff000000000abc);
            cpu.x[1] = UINT64_C(0x0123456789abcdef);
            cpu.nzcv = UINT32_C(0x50000000);
        }
        if (index == 60) {
            assert(cpu.x[0] == UINT64_C(0x3456789abcdefabc));
            cpu.nzcv = UINT32_C(0x50000000);
        }
        if (index == 61) {
            assert(cpu.x[0] == UINT64_C(0x0000000008000005));
            cpu.x[0] = UINT64_C(0xaaaaaaaafffffffd);
            cpu.x[1] = UINT64_C(0xbbbbbbbb00000007);
            cpu.nzcv = UINT32_C(0x50000000);
        }
        if (index == 62) {
            assert(cpu.x[0] == UINT64_C(0xffffffffffffffeb));
            put_value(fixture.memory.data + 0x900, 8,
                    UINT64_C(0x1122334455667788));
            put_value(fixture.memory.data + 0x908, 8,
                    UINT64_C(0x99aabbccddeeff00));
            cpu.x[0] = DATA_PAGE + 0x900 - UINT64_C(0xd60);
            memset(cpu.v[31].b, 0x5a, sizeof(cpu.v[31].b));
            cpu.nzcv = UINT32_C(0x50000000);
        }
        if (index == 63) {
            assert(cpu.x[0] ==
                    DATA_PAGE + 0x900 - UINT64_C(0xd60));
            assert(memcmp(cpu.v[31].b,
                    fixture.memory.data + 0x900,
                    sizeof(cpu.v[31].b)) == 0);
        }
        enum aarch64_step_stop expected =
                index + 1 == array_size(hot_instructions) ?
                AARCH64_STEP_SYSCALL : AARCH64_STEP_RETIRED;
        assert(run_at(&runner, &cpu, CODE_PAGE + index * 4).stop ==
                expected);
    }
    assert(cpu.x[2] == UINT32_C(0xf02468ac));
    assert(cpu.x[4] == UINT32_C(0x08000004));
    assert(cpu.x[0] == DATA_PAGE + 0x900 - UINT64_C(0xd60));
    assert(cpu.x[5] == UINT64_C(0xbbbbbbbb00000004));
    assert(cpu.x[6] == UINT32_C(0x00000014));
    assert(cpu.x[10] == UINT64_C(0xcccccccc00000020));
    assert(cpu.x[11] == UINT64_C(0xaaaaaaaa00000004));
    assert(cpu.x[3] == DATA_PAGE + 0x500);
    assert(cpu.x[1] == UINT64_C(0xbbbbbbbb00000007));
    assert(cpu.x[22] == UINT32_C(0x09abcdef));
    assert(cpu.x[23] == UINT64_C(0xaaaaaaaa81234567));
    assert(cpu.sp == UINT64_C(0x0ff8));
    assert(cpu.nzcv == UINT32_C(0x50000000));
    assert(memcmp(cpu.v[30].b,
            fixture.memory.data + 0x780, sizeof(cpu.v[30].b)) == 0);
    assert(cpu.v[31].d[0] == UINT64_C(0x1122334455667788));
    assert(cpu.v[31].d[1] == UINT64_C(0x99aabbccddeeff00));
    assert(memcmp(fixture.memory.data + 0x520,
            &cpu.v[0], sizeof(cpu.v[0])) == 0);
    assert(memcmp(fixture.memory.data + 0x530,
            &cpu.v[0], sizeof(cpu.v[0])) == 0);
    assert(memcmp(fixture.memory.data + 0x600,
            zero_store, sizeof(zero_store)) == 0);
    assert(memcmp(fixture.memory.data + 0x700,
            &cpu.v[0], sizeof(cpu.v[0])) == 0);
    assert(memcmp(fixture.memory.data + 0x7c0,
            &cpu.v[31], sizeof(cpu.v[31])) == 0);
    assert_stats(&runner, 0, array_size(hot_instructions),
            array_size(hot_instructions), 0);

    // 64 个 fast token 已填满缓存，复用执行完的末尾 SVC 槽。
    const guest_addr_t fallback_pc = CODE_PAGE +
            (array_size(hot_instructions) - 1) * 4;
    write_instruction(&fixture.tlb, fallback_pc, INSTRUCTION_CLZ_W2_W2);
    qword_t clz_sp = cpu.sp;
    dword_t clz_nzcv = cpu.nzcv;
    cpu.x[2] = UINT64_C(0xa5a5a5a500800000);
    assert(run_at(&runner, &cpu, fallback_pc).stop ==
            AARCH64_STEP_RETIRED);
    assert(cpu.x[2] == 8);
    assert(cpu.sp == clz_sp);
    assert(cpu.nzcv == clz_nzcv);
    assert(cpu.pc == fallback_pc + 4);
    assert_stats(&runner, 0, array_size(hot_instructions) + 1,
            array_size(hot_instructions) + 1, 0);

    cpu.x[2] = UINT64_C(0xa5a5a5a500000001);
    assert(run_at(&runner, &cpu, fallback_pc).stop ==
            AARCH64_STEP_RETIRED);
    assert(cpu.x[2] == 31);
    assert(cpu.sp == clz_sp);
    assert(cpu.nzcv == clz_nzcv);
    assert(cpu.pc == fallback_pc + 4);
    assert_stats(&runner, 1, array_size(hot_instructions) + 1,
            array_size(hot_instructions) + 2, 0);

    write_instruction(&fixture.tlb,
            fallback_pc, INSTRUCTION_ADDS_X22_SP_W23_SXTW_2);
    cpu.sp = UINT64_C(0x1000);
    cpu.x[23] = 1;
    cpu.x[22] = UINT64_MAX;
    cpu.nzcv = UINT32_C(0x30000000);
    assert(run_at(&runner, &cpu, fallback_pc).stop ==
            AARCH64_STEP_RETIRED);
    assert(cpu.x[22] == UINT64_C(0x1004));
    assert(cpu.sp == UINT64_C(0x1000));
    assert(cpu.x[23] == 1);
    assert(cpu.nzcv == 0);
    assert(cpu.pc == fallback_pc + 4);
    assert_stats(&runner, 1, array_size(hot_instructions) + 2,
            array_size(hot_instructions) + 2, 1);

    cpu.sp = UINT64_C(0x7fffffffffffffff);
    cpu.x[23] = 1;
    cpu.x[22] = 0;
    cpu.nzcv = 0;
    assert(run_at(&runner, &cpu, fallback_pc).stop ==
            AARCH64_STEP_RETIRED);
    assert(cpu.x[22] == UINT64_C(0x8000000000000003));
    assert(cpu.sp == UINT64_C(0x7fffffffffffffff));
    assert(cpu.x[23] == 1);
    assert(cpu.nzcv == UINT32_C(0x90000000));
    assert(cpu.pc == fallback_pc + 4);
    assert_stats(&runner, 2, array_size(hot_instructions) + 2,
            array_size(hot_instructions) + 2, 2);

    cpu.x[0] = UINT64_C(0x123456789abcdef0);
    cpu.x[3] = UINT64_C(0xdeadbeefdeadbeef);
    assert(run_at(&runner, &cpu, CODE_PAGE).stop ==
            AARCH64_STEP_RETIRED);
    assert(cpu.x[3] == UINT64_C(0x123456789abcdef0));
    assert_stats(&runner, 3, array_size(hot_instructions) + 2,
            array_size(hot_instructions) + 3, 2);

    cpu.x[0] = 20;
    cpu.x[2] = 10;
    cpu.x[3] = UINT64_C(0xdeadbeefdeadbeef);
    assert(run_at(&runner, &cpu, CODE_PAGE + 4).stop ==
            AARCH64_STEP_RETIRED);
    assert(cpu.x[3] == 30);
    assert_stats(&runner, 4, array_size(hot_instructions) + 2,
            array_size(hot_instructions) + 4, 2);

    cpu.x[0] = UINT64_C(0xff00);
    cpu.x[2] = UINT64_C(0x0ff0);
    cpu.x[4] = UINT64_C(0xdeadbeefdeadbeef);
    assert(run_at(&runner, &cpu, CODE_PAGE + 8).stop ==
            AARCH64_STEP_RETIRED);
    assert(cpu.x[4] == UINT64_C(0xf0f0));
    assert_stats(&runner, 5, array_size(hot_instructions) + 2,
            array_size(hot_instructions) + 5, 2);

    put_value(fixture.memory.data, 8,
            UINT64_C(0xfedcba9876543210));
    cpu.x[1] = DATA_PAGE;
    cpu.x[2] = UINT64_MAX;
    assert(run_at(&runner, &cpu, CODE_PAGE + 12).stop ==
            AARCH64_STEP_RETIRED);
    assert(cpu.x[2] == UINT64_C(0xfedcba9876543210));
    assert_stats(&runner, 6, array_size(hot_instructions) + 2,
            array_size(hot_instructions) + 6, 2);

    cpu.x[2] = UINT64_C(0xf0f0f0f0f0f0f0f0);
    cpu.x[6] = UINT64_C(0xff00ff00ff00ff00);
    assert(run_at(&runner, &cpu, CODE_PAGE + 16).stop ==
            AARCH64_STEP_RETIRED);
    assert(cpu.x[2] == UINT64_C(0xf000f000f000f000));
    assert_stats(&runner, 7, array_size(hot_instructions) + 2,
            array_size(hot_instructions) + 7, 2);

    cpu.x[1] = UINT64_C(0xffffffff89abcdef);
    cpu.x[3] = UINT64_MAX;
    assert(run_at(&runner, &cpu, CODE_PAGE + 20).stop ==
            AARCH64_STEP_RETIRED);
    assert(cpu.x[3] == UINT64_C(0x0000000079bdf135));
    assert_stats(&runner, 8, array_size(hot_instructions) + 2,
            array_size(hot_instructions) + 8, 2);

    cpu.sp = DATA_PAGE + 0x300;
    cpu.x[10] = UINT64_MAX;
    assert(run_at(&runner, &cpu, CODE_PAGE + 24).stop ==
            AARCH64_STEP_RETIRED);
    assert(cpu.x[10] == 0);
    assert_stats(&runner, 9, array_size(hot_instructions) + 2,
            array_size(hot_instructions) + 9, 2);

    memset(fixture.memory.data + 0x120, 0xa5, 4);
    cpu.x[0] = UINT64_C(0xffffffff89abcdef);
    cpu.x[1] = DATA_PAGE + 0x100;
    assert(run_at(&runner, &cpu, CODE_PAGE + 28).stop ==
            AARCH64_STEP_RETIRED);
    const byte_t expected_store[] = {0xef, 0xcd, 0xab, 0x89};
    assert(memcmp(fixture.memory.data + 0x120,
            expected_store, sizeof(expected_store)) == 0);
    assert_stats(&runner, 10, array_size(hot_instructions) + 2,
            array_size(hot_instructions) + 10, 2);

    const qword_t pair_first = UINT64_C(0x0123456789abcdef);
    const qword_t pair_second = UINT64_C(0xfedcba9876543210);
    put_value(fixture.memory.data + 0x490, 8, pair_first);
    put_value(fixture.memory.data + 0x498, 8, pair_second);
    assert(run_at(&runner, &cpu, CODE_PAGE + 26 * 4).stop ==
            AARCH64_STEP_RETIRED);
    assert(cpu.x[6] == pair_first);
    assert(cpu.x[1] == pair_second);
    assert_stats(&runner, 11, array_size(hot_instructions) + 2,
            array_size(hot_instructions) + 11, 2);

    cpu.x[5] = UINT64_C(0x8877665544332211);
    cpu.x[2] = UINT64_C(0x1020304050607080);
    cpu.sp = DATA_PAGE + 0x300;
    memset(fixture.memory.data + 0x310, 0xa5, 16);
    assert(run_at(&runner, &cpu, CODE_PAGE + 27 * 4).stop ==
            AARCH64_STEP_RETIRED);
    byte_t expected_pair_store[16];
    put_value(expected_pair_store, 8, cpu.x[5]);
    put_value(expected_pair_store + 8, 8, cpu.x[2]);
    assert(memcmp(fixture.memory.data + 0x310,
            expected_pair_store, sizeof(expected_pair_store)) == 0);
    assert(cpu.sp == DATA_PAGE + 0x300);
    assert_stats(&runner, 12, array_size(hot_instructions) + 2,
            array_size(hot_instructions) + 12, 2);

    cpu.x[1] = UINT64_C(0x8877665544332211);
    assert(run_at(&runner, &cpu, CODE_PAGE + 28 * 4).stop ==
            AARCH64_STEP_RETIRED);
    assert(cpu.x[1] == UINT64_C(0x443bb32aa2199108));
    assert_stats(&runner, 13, array_size(hot_instructions) + 2,
            array_size(hot_instructions) + 13, 2);

    cpu.x[2] = UINT64_C(0x8000000000000000);
    cpu.x[5] = 1;
    cpu.nzcv = UINT32_C(0x40000000);
    qword_t sp = cpu.sp;
    assert(run_at(&runner, &cpu, CODE_PAGE + 29 * 4).stop ==
            AARCH64_STEP_RETIRED);
    assert(cpu.x[2] == UINT64_C(0x8000000000000000));
    assert(cpu.x[5] == 1);
    assert(cpu.nzcv == UINT32_C(0x30000000));
    assert(cpu.sp == sp);
    assert_stats(&runner, 14, array_size(hot_instructions) + 2,
            array_size(hot_instructions) + 14, 2);

    cpu.x[2] = UINT64_C(0xdeadbeefdeadbeef);
    assert(run_at(&runner, &cpu, CODE_PAGE + 30 * 4).stop ==
            AARCH64_STEP_RETIRED);
    assert(cpu.x[2] == CODE_PAGE +
            UINT64_C(122) * GUEST_MEMORY_PAGE_SIZE);
    assert_stats(&runner, 15, array_size(hot_instructions) + 2,
            array_size(hot_instructions) + 15, 2);

    cpu.x[0] = UINT64_MAX;
    cpu.x[3] = UINT64_C(0xffffffff89abcdef);
    assert(run_at(&runner, &cpu, CODE_PAGE + 31 * 4).stop ==
            AARCH64_STEP_RETIRED);
    assert(cpu.x[0] == UINT64_C(0x0000000009abcdef));
    assert_stats(&runner, 16, array_size(hot_instructions) + 2,
            array_size(hot_instructions) + 16, 2);

    cpu.x[0] = UINT64_MAX;
    cpu.x[3] = UINT64_C(0xaaaaaaaa00000003);
    cpu.x[4] = UINT64_C(0xffffffff0000000a);
    assert(run_at(&runner, &cpu, CODE_PAGE + 32 * 4).stop ==
            AARCH64_STEP_RETIRED);
    assert(cpu.x[0] == 7);
    assert(cpu.x[3] == UINT64_C(0xaaaaaaaa00000003));
    assert(cpu.x[4] == UINT64_C(0xffffffff0000000a));
    assert_stats(&runner, 17, array_size(hot_instructions) + 2,
            array_size(hot_instructions) + 17, 2);

    cpu.x[0] = UINT32_C(0x80000000);
    cpu.x[19] = 1;
    cpu.nzcv = 0;
    qword_t ccmp_registers[array_size(cpu.x)];
    memcpy(ccmp_registers, cpu.x, sizeof(ccmp_registers));
    qword_t ccmp_sp = cpu.sp;
    assert(run_at(&runner, &cpu, CODE_PAGE + 33 * 4).stop ==
            AARCH64_STEP_RETIRED);
    assert(memcmp(cpu.x, ccmp_registers, sizeof(ccmp_registers)) == 0);
    assert(cpu.sp == ccmp_sp);
    assert(cpu.nzcv == UINT32_C(0x30000000));
    assert_stats(&runner, 18, array_size(hot_instructions) + 2,
            array_size(hot_instructions) + 18, 2);

    memset(fixture.memory.data + 0x280, 0xa5, 8);
    cpu.sp = DATA_PAGE + 0x280;
    assert(run_at(&runner, &cpu, CODE_PAGE + 34 * 4).stop ==
            AARCH64_STEP_RETIRED);
    const byte_t zeros[8] = {0};
    assert(memcmp(fixture.memory.data + 0x280,
            zeros, sizeof(zeros)) == 0);
    assert(cpu.sp == DATA_PAGE + 0x280);
    assert_stats(&runner, 19, array_size(hot_instructions) + 2,
            array_size(hot_instructions) + 19, 2);

    cpu.x[3] = UINT64_MAX;
    cpu.nzcv = UINT32_C(0x40000000);
    assert(run_at(&runner, &cpu, CODE_PAGE + 35 * 4).stop ==
            AARCH64_STEP_RETIRED);
    assert(cpu.x[3] == 1);
    assert(cpu.nzcv == UINT32_C(0x40000000));
    assert_stats(&runner, 20, array_size(hot_instructions) + 2,
            array_size(hot_instructions) + 20, 2);

    cpu.x[2] = UINT64_C(0xffffffff12345678);
    cpu.nzcv = UINT32_C(0x90000000);
    qword_t rev_sp = cpu.sp;
    assert(run_at(&runner, &cpu, CODE_PAGE + 36 * 4).stop ==
            AARCH64_STEP_RETIRED);
    assert(cpu.x[2] == UINT32_C(0x78563412));
    assert(cpu.sp == rev_sp);
    assert(cpu.nzcv == UINT32_C(0x90000000));
    assert_stats(&runner, 21, array_size(hot_instructions) + 2,
            array_size(hot_instructions) + 21, 2);

    cpu.x[0] = UINT64_C(0xffffffff00000003);
    cpu.x[4] = UINT64_C(0x1000);
    cpu.nzcv = UINT32_C(0x60000000);
    qword_t add_sp = cpu.sp;
    assert(run_at(&runner, &cpu, CODE_PAGE + 37 * 4).stop ==
            AARCH64_STEP_RETIRED);
    assert(cpu.x[0] == UINT64_C(0x1018));
    assert(cpu.x[4] == UINT64_C(0x1000));
    assert(cpu.sp == add_sp);
    assert(cpu.nzcv == UINT32_C(0x60000000));
    assert_stats(&runner, 22, array_size(hot_instructions) + 2,
            array_size(hot_instructions) + 22, 2);

    put_value(fixture.memory.data + 0x180, 8,
            UINT64_C(0xfedcba9876543210));
    cpu.x[2] = UINT64_MAX;
    cpu.x[3] = DATA_PAGE + 0x180;
    cpu.nzcv = UINT32_C(0x30000000);
    qword_t load_sp = cpu.sp;
    assert(run_at(&runner, &cpu, CODE_PAGE + 38 * 4).stop ==
            AARCH64_STEP_RETIRED);
    assert(cpu.x[2] == UINT64_C(0xfedcba9876543210));
    assert(cpu.x[3] == DATA_PAGE + 0x188);
    assert(cpu.sp == load_sp);
    assert(cpu.nzcv == UINT32_C(0x30000000));
    assert_stats(&runner, 23, array_size(hot_instructions) + 2,
            array_size(hot_instructions) + 23, 2);

    cpu.x[0] = UINT64_C(0x9999aaaabbbbcccc);
    cpu.x[1] = UINT64_C(0xddddeeeeffff0000);
    cpu.nzcv = UINT32_C(0x40000000);
    qword_t csel_sp = cpu.sp;
    assert(run_at(&runner, &cpu, CODE_PAGE + 39 * 4).stop ==
            AARCH64_STEP_RETIRED);
    assert(cpu.x[0] == UINT64_C(0x9999aaaabbbbcccc));
    assert(cpu.x[1] == UINT64_C(0x9999aaaabbbbcccc));
    assert(cpu.sp == csel_sp);
    assert(cpu.nzcv == UINT32_C(0x40000000));
    assert_stats(&runner, 24, array_size(hot_instructions) + 2,
            array_size(hot_instructions) + 24, 2);

    cpu.x[2] = UINT32_C(0x80000000);
    cpu.x[22] = UINT64_MAX;
    cpu.nzcv = UINT32_C(0xf0000000);
    qword_t ands_sp = cpu.sp;
    assert(run_at(&runner, &cpu, CODE_PAGE + 40 * 4).stop ==
            AARCH64_STEP_RETIRED);
    assert(cpu.x[2] == UINT32_C(0x80000000));
    assert(cpu.x[22] == 0);
    assert(cpu.sp == ands_sp);
    assert(cpu.nzcv == UINT32_C(0x40000000));
    assert_stats(&runner, 25, array_size(hot_instructions) + 2,
            array_size(hot_instructions) + 25, 2);

    cpu.x[3] = DATA_PAGE + 0x580;
    cpu.v[0].d[0] = UINT64_C(0x8877665544332211);
    cpu.v[0].d[1] = UINT64_C(0x1020304050607080);
    memset(fixture.memory.data + 0x5a0, 0xa5, 32);
    assert(run_at(&runner, &cpu, CODE_PAGE + 41 * 4).stop ==
            AARCH64_STEP_RETIRED);
    assert(memcmp(fixture.memory.data + 0x5a0,
            &cpu.v[0], sizeof(cpu.v[0])) == 0);
    assert(memcmp(fixture.memory.data + 0x5b0,
            &cpu.v[0], sizeof(cpu.v[0])) == 0);
    assert(cpu.x[3] == DATA_PAGE + 0x580);
    assert_stats(&runner, 26, array_size(hot_instructions) + 2,
            array_size(hot_instructions) + 26, 2);

    cpu.x[2] = UINT64_C(0xaaaaaaaa80000001);
    assert(run_at(&runner, &cpu, CODE_PAGE + 42 * 4).stop ==
            AARCH64_STEP_RETIRED);
    assert(cpu.x[2] == UINT64_C(0xffffffff80000001));
    assert_stats(&runner, 27, array_size(hot_instructions) + 2,
            array_size(hot_instructions) + 27, 2);

    cpu.x[4] = 4;
    cpu.x[10] = UINT64_C(0xaaaaaaaa80000040);
    assert(run_at(&runner, &cpu, CODE_PAGE + 43 * 4).stop ==
            AARCH64_STEP_RETIRED);
    assert(cpu.x[4] == UINT32_C(0x08000004));
    assert(cpu.x[10] == UINT64_C(0xaaaaaaaa80000040));
    assert_stats(&runner, 28, array_size(hot_instructions) + 2,
            array_size(hot_instructions) + 28, 2);

    cpu.x[6] = 5;
    cpu.x[0] = 11;
    assert(run_at(&runner, &cpu, CODE_PAGE + 44 * 4).stop ==
            AARCH64_STEP_RETIRED);
    assert(cpu.x[6] == 55);
    assert_stats(&runner, 29, array_size(hot_instructions) + 2,
            array_size(hot_instructions) + 29, 2);

    cpu.x[5] = UINT64_MAX;
    cpu.x[7] = 5;
    cpu.x[1] = 2;
    assert(run_at(&runner, &cpu, CODE_PAGE + 45 * 4).stop ==
            AARCH64_STEP_RETIRED);
    assert(cpu.x[5] == 20);
    assert_stats(&runner, 30, array_size(hot_instructions) + 2,
            array_size(hot_instructions) + 30, 2);

    cpu.x[1] = UINT64_MAX;
    assert(run_at(&runner, &cpu, CODE_PAGE + 46 * 4).stop ==
            AARCH64_STEP_RETIRED);
    assert(cpu.x[1] == CODE_PAGE + 46 * 4 + 12);
    assert_stats(&runner, 31, array_size(hot_instructions) + 2,
            array_size(hot_instructions) + 31, 2);

    cpu.x[0] = UINT64_C(0x8000000000000000);
    cpu.x[5] = UINT64_MAX;
    cpu.nzcv = UINT32_C(0xf0000000);
    assert(run_at(&runner, &cpu, CODE_PAGE + 47 * 4).stop ==
            AARCH64_STEP_RETIRED);
    assert(cpu.x[0] == UINT64_C(0x8000000000000000));
    assert(cpu.x[5] == UINT64_MAX);
    assert(cpu.nzcv == UINT32_C(0x80000000));
    assert_stats(&runner, 32, array_size(hot_instructions) + 2,
            array_size(hot_instructions) + 32, 2);

    memset(fixture.memory.data + 0x600, 0xa5, sizeof(zero_store));
    cpu.x[2] = DATA_PAGE + 0x600;
    cpu.nzcv = UINT32_C(0x30000000);
    assert(run_at(&runner, &cpu, CODE_PAGE + 48 * 4).stop ==
            AARCH64_STEP_RETIRED);
    assert(memcmp(fixture.memory.data + 0x600,
            zero_store, sizeof(zero_store)) == 0);
    assert(cpu.x[2] == DATA_PAGE + 0x608);
    assert(cpu.nzcv == UINT32_C(0x30000000));
    assert_stats(&runner, 33, array_size(hot_instructions) + 2,
            array_size(hot_instructions) + 33, 2);

    cpu.x[10] = UINT64_C(0xaaaaaaaa00000004);
    cpu.nzcv = UINT32_C(0x60000000);
    assert(run_at(&runner, &cpu, CODE_PAGE + 49 * 4).stop ==
            AARCH64_STEP_RETIRED);
    assert(cpu.x[10] == 5);
    assert(cpu.nzcv == UINT32_C(0x60000000));
    assert_stats(&runner, 34, array_size(hot_instructions) + 2,
            array_size(hot_instructions) + 34, 2);

    cpu.x[0] = UINT64_C(0xbbbbbbbb00000005);
    cpu.x[11] = UINT64_C(0xcccccccc00000006);
    cpu.x[1] = UINT64_MAX;
    cpu.nzcv = UINT32_C(0x30000000);
    assert(run_at(&runner, &cpu, CODE_PAGE + 50 * 4).stop ==
            AARCH64_STEP_RETIRED);
    assert(cpu.x[1] == 30);
    assert(cpu.nzcv == UINT32_C(0x30000000));
    assert_stats(&runner, 35, array_size(hot_instructions) + 2,
            array_size(hot_instructions) + 35, 2);

    cpu.x[0] = DATA_PAGE + 0x700;
    cpu.v[0].d[0] = UINT64_C(0x8877665544332211);
    cpu.v[0].d[1] = UINT64_C(0x1020304050607080);
    cpu.nzcv = UINT32_C(0xa0000000);
    assert(run_at(&runner, &cpu, CODE_PAGE + 51 * 4).stop ==
            AARCH64_STEP_RETIRED);
    assert(memcmp(fixture.memory.data + 0x700,
            &cpu.v[0], sizeof(cpu.v[0])) == 0);
    assert(cpu.x[0] == DATA_PAGE + 0x700);
    assert(cpu.nzcv == UINT32_C(0xa0000000));
    assert_stats(&runner, 36, array_size(hot_instructions) + 2,
            array_size(hot_instructions) + 36, 2);

    cpu.x[1] = UINT64_C(0xaaaaaaaa00000001);
    cpu.x[2] = UINT64_C(0xbbbbbbbb00000080);
    cpu.nzcv = UINT32_C(0xf0000000);
    assert(run_at(&runner, &cpu, CODE_PAGE + 52 * 4).stop ==
            AARCH64_STEP_RETIRED);
    assert(cpu.x[1] == UINT64_C(0xaaaaaaaa00000001));
    assert(cpu.x[2] == UINT64_C(0xbbbbbbbb00000080));
    assert(cpu.nzcv == UINT32_C(0x20000000));
    assert_stats(&runner, 37, array_size(hot_instructions) + 2,
            array_size(hot_instructions) + 37, 2);

    cpu.sp = UINT64_C(0x1000);
    cpu.x[0] = 8;
    cpu.nzcv = UINT32_C(0x30000000);
    assert(run_at(&runner, &cpu, CODE_PAGE + 53 * 4).stop ==
            AARCH64_STEP_RETIRED);
    assert(cpu.sp == UINT64_C(0x0ff8));
    assert(cpu.x[0] == 8);
    assert(cpu.nzcv == UINT32_C(0x30000000));
    assert_stats(&runner, 38, array_size(hot_instructions) + 2,
            array_size(hot_instructions) + 38, 2);

    cpu.x[6] = UINT64_C(0xaaaaaaaa00000003);
    cpu.x[5] = UINT64_C(0xbbbbbbbb00000004);
    cpu.x[10] = UINT64_C(0xcccccccc00000020);
    cpu.nzcv = UINT32_C(0xa0000000);
    qword_t msub_sp = cpu.sp;
    assert(run_at(&runner, &cpu, CODE_PAGE + 54 * 4).stop ==
            AARCH64_STEP_RETIRED);
    assert(cpu.x[6] == UINT32_C(0x00000014));
    assert(cpu.x[5] == UINT64_C(0xbbbbbbbb00000004));
    assert(cpu.x[10] == UINT64_C(0xcccccccc00000020));
    assert(cpu.sp == msub_sp);
    assert(cpu.nzcv == UINT32_C(0xa0000000));
    assert_stats(&runner, 39, array_size(hot_instructions) + 2,
            array_size(hot_instructions) + 39, 2);

    for (byte_t offset = 0; offset < 32; offset++)
        fixture.memory.data[0x7a0 + offset] =
                (byte_t) (0x81 + offset * 2);
    cpu.x[1] = DATA_PAGE + 0x7a0;
    memset(cpu.v[30].b, 0x5a, sizeof(cpu.v[30].b));
    memset(cpu.v[31].b, 0xc3, sizeof(cpu.v[31].b));
    cpu.nzcv = UINT32_C(0x50000000);
    assert(run_at(&runner, &cpu, CODE_PAGE + 55 * 4).stop ==
            AARCH64_STEP_RETIRED);
    assert(memcmp(cpu.v[30].b,
            fixture.memory.data + 0x7a0, sizeof(cpu.v[30].b)) == 0);
    assert(memcmp(cpu.v[31].b,
            fixture.memory.data + 0x7b0, sizeof(cpu.v[31].b)) == 0);
    assert(cpu.x[1] == DATA_PAGE + 0x7a0);
    assert(cpu.nzcv == UINT32_C(0x50000000));
    assert_stats(&runner, 40, array_size(hot_instructions) + 2,
            array_size(hot_instructions) + 40, 2);

    memset(cpu.v[31].b, 0x6d, sizeof(cpu.v[31].b));
    cpu.nzcv = UINT32_C(0x50000000);
    assert(run_at(&runner, &cpu, CODE_PAGE + 56 * 4).stop ==
            AARCH64_STEP_RETIRED);
    assert(cpu.v[31].d[0] == 0);
    assert(cpu.v[31].d[1] == 0);
    assert(cpu.nzcv == UINT32_C(0x50000000));
    assert_stats(&runner, 41, array_size(hot_instructions) + 2,
            array_size(hot_instructions) + 41, 2);

    cpu.x[23] = UINT64_C(0xaaaaaaaa80000004);
    cpu.x[2] = 4;
    cpu.nzcv = UINT32_C(0x50000000);
    qword_t asrv_sp = cpu.sp;
    assert(run_at(&runner, &cpu, CODE_PAGE + 57 * 4).stop ==
            AARCH64_STEP_RETIRED);
    assert(cpu.x[2] == UINT32_C(0xf8000000));
    assert(cpu.x[23] == UINT64_C(0xaaaaaaaa80000004));
    assert(cpu.sp == asrv_sp);
    assert(cpu.nzcv == UINT32_C(0x50000000));
    assert_stats(&runner, 42, array_size(hot_instructions) + 2,
            array_size(hot_instructions) + 42, 2);

    cpu.x[0] = DATA_PAGE + 0x7a8;
    cpu.v[31].d[0] = UINT64_C(0x0102030405060708);
    cpu.v[31].d[1] = UINT64_C(0xf1e2d3c4b5a69788);
    memset(fixture.memory.data + 0x7e0, 0xa5,
            sizeof(cpu.v[31]));
    cpu.nzcv = UINT32_C(0x50000000);
    qword_t store_simd_sp = cpu.sp;
    assert(run_at(&runner, &cpu, CODE_PAGE + 58 * 4).stop ==
            AARCH64_STEP_RETIRED);
    assert(memcmp(fixture.memory.data + 0x7e0,
            &cpu.v[31], sizeof(cpu.v[31])) == 0);
    assert(cpu.x[0] == DATA_PAGE + 0x7a8);
    assert(cpu.sp == store_simd_sp);
    assert(cpu.nzcv == UINT32_C(0x50000000));
    assert_stats(&runner, 43, array_size(hot_instructions) + 2,
            array_size(hot_instructions) + 43, 2);

    cpu.x[0] = UINT64_C(0x0000fffffffff123);
    cpu.x[1] = UINT64_C(0xfedcba9876543210);
    cpu.nzcv = UINT32_C(0x50000000);
    qword_t bfm_sp = cpu.sp;
    assert(run_at(&runner, &cpu, CODE_PAGE + 59 * 4).stop ==
            AARCH64_STEP_RETIRED);
    assert(cpu.x[0] == UINT64_C(0xcba9876543210123));
    assert(cpu.x[1] == UINT64_C(0xfedcba9876543210));
    assert(cpu.sp == bfm_sp);
    assert(cpu.nzcv == UINT32_C(0x50000000));
    assert_stats(&runner, 44, array_size(hot_instructions) + 2,
            array_size(hot_instructions) + 44, 2);

    cpu.x[0] = UINT64_MAX;
    cpu.x[4] = UINT64_C(0xffffffff89abcdef);
    cpu.nzcv = UINT32_C(0x50000000);
    qword_t eor_immediate_sp = cpu.sp;
    assert(run_at(&runner, &cpu, CODE_PAGE + 60 * 4).stop ==
            AARCH64_STEP_RETIRED);
    assert(cpu.x[0] == UINT64_C(0x0000000089abcdee));
    assert(cpu.x[4] == UINT64_C(0xffffffff89abcdef));
    assert(cpu.sp == eor_immediate_sp);
    assert(cpu.nzcv == UINT32_C(0x50000000));
    assert_stats(&runner, 45, array_size(hot_instructions) + 2,
            array_size(hot_instructions) + 45, 2);

    cpu.x[0] = UINT64_C(0xffffffff80000000);
    cpu.x[1] = UINT64_MAX;
    cpu.nzcv = UINT32_C(0x50000000);
    qword_t smaddl_sp = cpu.sp;
    assert(run_at(&runner, &cpu, CODE_PAGE + 61 * 4).stop ==
            AARCH64_STEP_RETIRED);
    assert(cpu.x[0] == UINT64_C(0x0000000080000000));
    assert(cpu.x[1] == UINT64_MAX);
    assert(cpu.sp == smaddl_sp);
    assert(cpu.nzcv == UINT32_C(0x50000000));
    assert_stats(&runner, 46, array_size(hot_instructions) + 2,
            array_size(hot_instructions) + 46, 2);

    put_value(fixture.memory.data + 0x940, 8,
            UINT64_C(0x1020304050607080));
    put_value(fixture.memory.data + 0x948, 8,
            UINT64_C(0x8877665544332211));
    cpu.x[0] = DATA_PAGE + 0x940 - UINT64_C(0xd60);
    memset(cpu.v[31].b, 0xc3, sizeof(cpu.v[31].b));
    cpu.nzcv = UINT32_C(0x50000000);
    qword_t load_simd_sp = cpu.sp;
    assert(run_at(&runner, &cpu, CODE_PAGE + 62 * 4).stop ==
            AARCH64_STEP_RETIRED);
    assert(cpu.v[31].d[0] == UINT64_C(0x1020304050607080));
    assert(cpu.v[31].d[1] == UINT64_C(0x8877665544332211));
    assert(cpu.x[0] == DATA_PAGE + 0x940 - UINT64_C(0xd60));
    assert(cpu.sp == load_simd_sp);
    assert(cpu.nzcv == UINT32_C(0x50000000));
    assert_stats(&runner, 47, array_size(hot_instructions) + 2,
            array_size(hot_instructions) + 47, 2);
    const struct aarch64_threaded_stats *stats =
            aarch64_runner_threaded_stats(&runner);
    assert(stats->cache_hits + stats->cache_misses ==
            stats->fast_dispatches + stats->c_fallbacks);
}

static void test_product_c_fallback(void) {
    struct test_fixture c_fixture;
    struct test_fixture threaded_fixture;
    init_fixture(&c_fixture);
    init_fixture(&threaded_fixture);
    write_instruction(&c_fixture.tlb,
            CODE_PAGE, INSTRUCTION_LDAR_X2_X1);
    write_instruction(&c_fixture.tlb,
            CODE_PAGE + 4, INSTRUCTION_STLR_X0_X1);
    write_instruction(&c_fixture.tlb,
            CODE_PAGE + 8, INSTRUCTION_PRFM_X0);
    write_instruction(&c_fixture.tlb,
            CODE_PAGE + 12, INSTRUCTION_STR_Q30_X21_X0);
    write_instruction(&c_fixture.tlb,
            CODE_PAGE + 16, INSTRUCTION_LD4_V28_X0);
    write_instruction(&c_fixture.tlb,
            CODE_PAGE + 20, INSTRUCTION_USHR_D28_D31_39);
    write_instruction(&c_fixture.tlb,
            CODE_PAGE + 24, INSTRUCTION_XTN_V28_4H_V28_4S);
    write_instruction(&c_fixture.tlb,
            CODE_PAGE + 28, INSTRUCTION_FCVT_D0_S0);
    write_instruction(&c_fixture.tlb,
            CODE_PAGE + 32, INSTRUCTION_MOV_S15_V31_S3);
    write_instruction(&c_fixture.tlb,
            CODE_PAGE + 36, INSTRUCTION_FDIV_S30_S0_S30);
    write_instruction(&c_fixture.tlb,
            CODE_PAGE + 40, INSTRUCTION_UCVTF_D31_D31);
    write_instruction(&c_fixture.tlb,
            CODE_PAGE + 44, INSTRUCTION_FCVTZU_X1_D31);
    write_instruction(&c_fixture.tlb,
            CODE_PAGE + 48, INSTRUCTION_SSHR_V30_2S_V31_2S_8);
    write_instruction(&c_fixture.tlb,
            CODE_PAGE + 52, INSTRUCTION_LD1_V31_B4_X1);
    write_instruction(&c_fixture.tlb,
            CODE_PAGE + 56, INSTRUCTION_FCSEL_D0_D0_D31_MI);
    write_instruction(&c_fixture.tlb,
            CODE_PAGE + 60, INSTRUCTION_FRINTM_D0_D0);
    write_instruction(&c_fixture.tlb,
            CODE_PAGE + 64, INSTRUCTION_FNEG_D0_D0);
    write_instruction(&c_fixture.tlb,
            CODE_PAGE + 68, INSTRUCTION_EXT_V0_V27_V30_8);
    write_instruction(&c_fixture.tlb,
            CODE_PAGE + 72, INSTRUCTION_MVN_V31_V30);
    write_instruction(&c_fixture.tlb,
            CODE_PAGE + 76, INSTRUCTION_USHR_V30_2D_V30_2D_6);
    write_instruction(&c_fixture.tlb,
            CODE_PAGE + 80, INSTRUCTION_ADDP_D31_V29_2D);
    write_instruction(&c_fixture.tlb,
            CODE_PAGE + 84, INSTRUCTION_FMADD_D31_D0_D15_D31);
    write_instruction(&c_fixture.tlb,
            CODE_PAGE + 88, INSTRUCTION_UADDLP_V31_8H_V31_16B);
    write_instruction(&c_fixture.tlb,
            CODE_PAGE + 92, INSTRUCTION_USHL_V31_8H_V31_8H_V24_8H);
    write_instruction(&c_fixture.tlb,
            CODE_PAGE + 96, INSTRUCTION_ADDV_H31_V31_8H);
    write_instruction(&c_fixture.tlb,
            CODE_PAGE + 100, INSTRUCTION_NEG_V29_2S_V31_2S);
    write_instruction(&c_fixture.tlb,
            CODE_PAGE + 104, INSTRUCTION_CNT_V31_8B_V31_8B);
    write_instruction(&c_fixture.tlb,
            CODE_PAGE + 108, INSTRUCTION_ADD_D31_D31_D30);
    write_instruction(&c_fixture.tlb,
            CODE_PAGE + 112, INSTRUCTION_SUB_V30_2D_V31_2D_V30_2D);
    write_instruction(&c_fixture.tlb,
            CODE_PAGE + 116, INSTRUCTION_CMGE_D31_D31_ZERO);
    write_instruction(&c_fixture.tlb,
            CODE_PAGE + 120, INSTRUCTION_CMGT_V30_2D_V31_2D_ZERO);
    write_instruction(&c_fixture.tlb,
            CODE_PAGE + 124, INSTRUCTION_ST1_V31_D1_X0);
    write_instruction(&c_fixture.tlb,
            CODE_PAGE + 128, INSTRUCTION_SHL_V31_2D_V31_2D_3);
    write_instruction(&c_fixture.tlb,
            CODE_PAGE + 132, INSTRUCTION_REV64_V30_2S_V30_2S);
    write_instruction(&c_fixture.tlb,
            CODE_PAGE + 136, INSTRUCTION_LD2_V30_V31_4S_X1);
    write_instruction(&c_fixture.tlb,
            CODE_PAGE + 140, INSTRUCTION_LD1R_V31_16B_X27);
    write_instruction(&c_fixture.tlb,
            CODE_PAGE + 144, INSTRUCTION_REV32_V31_16B_V31_16B);
    write_instruction(&c_fixture.tlb,
            CODE_PAGE + 148, INSTRUCTION_NEG_D31_D31);
    write_instruction(&c_fixture.tlb,
            CODE_PAGE + 152, INSTRUCTION_SSHR_D29_D31_3);
    write_instruction(&threaded_fixture.tlb,
            CODE_PAGE, INSTRUCTION_LDAR_X2_X1);
    write_instruction(&threaded_fixture.tlb,
            CODE_PAGE + 4, INSTRUCTION_STLR_X0_X1);
    write_instruction(&threaded_fixture.tlb,
            CODE_PAGE + 8, INSTRUCTION_PRFM_X0);
    write_instruction(&threaded_fixture.tlb,
            CODE_PAGE + 12, INSTRUCTION_STR_Q30_X21_X0);
    write_instruction(&threaded_fixture.tlb,
            CODE_PAGE + 16, INSTRUCTION_LD4_V28_X0);
    write_instruction(&threaded_fixture.tlb,
            CODE_PAGE + 20, INSTRUCTION_USHR_D28_D31_39);
    write_instruction(&threaded_fixture.tlb,
            CODE_PAGE + 24, INSTRUCTION_XTN_V28_4H_V28_4S);
    write_instruction(&threaded_fixture.tlb,
            CODE_PAGE + 28, INSTRUCTION_FCVT_D0_S0);
    write_instruction(&threaded_fixture.tlb,
            CODE_PAGE + 32, INSTRUCTION_MOV_S15_V31_S3);
    write_instruction(&threaded_fixture.tlb,
            CODE_PAGE + 36, INSTRUCTION_FDIV_S30_S0_S30);
    write_instruction(&threaded_fixture.tlb,
            CODE_PAGE + 40, INSTRUCTION_UCVTF_D31_D31);
    write_instruction(&threaded_fixture.tlb,
            CODE_PAGE + 44, INSTRUCTION_FCVTZU_X1_D31);
    write_instruction(&threaded_fixture.tlb,
            CODE_PAGE + 48, INSTRUCTION_SSHR_V30_2S_V31_2S_8);
    write_instruction(&threaded_fixture.tlb,
            CODE_PAGE + 52, INSTRUCTION_LD1_V31_B4_X1);
    write_instruction(&threaded_fixture.tlb,
            CODE_PAGE + 56, INSTRUCTION_FCSEL_D0_D0_D31_MI);
    write_instruction(&threaded_fixture.tlb,
            CODE_PAGE + 60, INSTRUCTION_FRINTM_D0_D0);
    write_instruction(&threaded_fixture.tlb,
            CODE_PAGE + 64, INSTRUCTION_FNEG_D0_D0);
    write_instruction(&threaded_fixture.tlb,
            CODE_PAGE + 68, INSTRUCTION_EXT_V0_V27_V30_8);
    write_instruction(&threaded_fixture.tlb,
            CODE_PAGE + 72, INSTRUCTION_MVN_V31_V30);
    write_instruction(&threaded_fixture.tlb,
            CODE_PAGE + 76, INSTRUCTION_USHR_V30_2D_V30_2D_6);
    write_instruction(&threaded_fixture.tlb,
            CODE_PAGE + 80, INSTRUCTION_ADDP_D31_V29_2D);
    write_instruction(&threaded_fixture.tlb,
            CODE_PAGE + 84, INSTRUCTION_FMADD_D31_D0_D15_D31);
    write_instruction(&threaded_fixture.tlb,
            CODE_PAGE + 88, INSTRUCTION_UADDLP_V31_8H_V31_16B);
    write_instruction(&threaded_fixture.tlb,
            CODE_PAGE + 92, INSTRUCTION_USHL_V31_8H_V31_8H_V24_8H);
    write_instruction(&threaded_fixture.tlb,
            CODE_PAGE + 96, INSTRUCTION_ADDV_H31_V31_8H);
    write_instruction(&threaded_fixture.tlb,
            CODE_PAGE + 100, INSTRUCTION_NEG_V29_2S_V31_2S);
    write_instruction(&threaded_fixture.tlb,
            CODE_PAGE + 104, INSTRUCTION_CNT_V31_8B_V31_8B);
    write_instruction(&threaded_fixture.tlb,
            CODE_PAGE + 108, INSTRUCTION_ADD_D31_D31_D30);
    write_instruction(&threaded_fixture.tlb,
            CODE_PAGE + 112, INSTRUCTION_SUB_V30_2D_V31_2D_V30_2D);
    write_instruction(&threaded_fixture.tlb,
            CODE_PAGE + 116, INSTRUCTION_CMGE_D31_D31_ZERO);
    write_instruction(&threaded_fixture.tlb,
            CODE_PAGE + 120, INSTRUCTION_CMGT_V30_2D_V31_2D_ZERO);
    write_instruction(&threaded_fixture.tlb,
            CODE_PAGE + 124, INSTRUCTION_ST1_V31_D1_X0);
    write_instruction(&threaded_fixture.tlb,
            CODE_PAGE + 128, INSTRUCTION_SHL_V31_2D_V31_2D_3);
    write_instruction(&threaded_fixture.tlb,
            CODE_PAGE + 132, INSTRUCTION_REV64_V30_2S_V30_2S);
    write_instruction(&threaded_fixture.tlb,
            CODE_PAGE + 136, INSTRUCTION_LD2_V30_V31_4S_X1);
    write_instruction(&threaded_fixture.tlb,
            CODE_PAGE + 140, INSTRUCTION_LD1R_V31_16B_X27);
    write_instruction(&threaded_fixture.tlb,
            CODE_PAGE + 144, INSTRUCTION_REV32_V31_16B_V31_16B);
    write_instruction(&threaded_fixture.tlb,
            CODE_PAGE + 148, INSTRUCTION_NEG_D31_D31);
    write_instruction(&threaded_fixture.tlb,
            CODE_PAGE + 152, INSTRUCTION_SSHR_D29_D31_3);

    const qword_t original = UINT64_C(0x8877665544332211);
    memcpy(c_fixture.memory.data, &original, sizeof(original));
    memcpy(threaded_fixture.memory.data, &original, sizeof(original));
    for (unsigned element = 0; element < 4; element++) {
        for (unsigned structure = 0; structure < 4; structure++) {
            dword_t value = UINT32_C(0x10203040) +
                    element * 0x100 + structure;
            size_t offset = 64 + (element * 4 + structure) * sizeof(value);
            memcpy(c_fixture.memory.data + offset, &value, sizeof(value));
            memcpy(threaded_fixture.memory.data + offset,
                    &value, sizeof(value));
        }
    }
    static const dword_t ld2_input[8] = {
        UINT32_C(0x11223344), UINT32_C(0x0badc0de),
        UINT32_C(0x55667788), UINT32_C(0x10203040),
        UINT32_C(0x99aabbcc), UINT32_C(0x7fffffff),
        UINT32_C(0xddeeff00), UINT32_C(0x80000001),
    };
    memcpy(c_fixture.memory.data + 160, ld2_input, sizeof(ld2_input));
    memcpy(threaded_fixture.memory.data + 160,
            ld2_input, sizeof(ld2_input));
    c_fixture.memory.data[224] = UINT8_C(0xa7);
    threaded_fixture.memory.data[224] = UINT8_C(0xa7);

    struct aarch64_runner c_runner;
    struct aarch64_runner threaded_runner;
    assert(aarch64_runner_init_backend(
            &c_runner, &c_fixture.tlb, AARCH64_BACKEND_C));
    assert(aarch64_runner_init_backend(&threaded_runner,
            &threaded_fixture.tlb, AARCH64_BACKEND_THREADED));
    struct cpu_state c_cpu;
    struct cpu_state threaded_cpu;
    init_differential_cpu(&c_cpu);
    init_differential_cpu(&threaded_cpu);
    c_cpu.x[0] = threaded_cpu.x[0] =
            UINT64_C(0x0123456789abcdef);
    c_cpu.x[1] = threaded_cpu.x[1] = DATA_PAGE;
    c_cpu.x[2] = threaded_cpu.x[2] = UINT64_MAX;

    struct aarch64_step_result c_result =
            aarch64_run_one(&c_runner, &c_cpu);
    struct aarch64_step_result threaded_result =
            aarch64_run_one(&threaded_runner, &threaded_cpu);
    assert(c_result.stop == AARCH64_STEP_RETIRED);
    assert_step_equal(&c_result, &threaded_result);
    assert_cpu_equal(&c_cpu, &threaded_cpu);
    assert_memory_equal(&c_fixture.memory, &threaded_fixture.memory);
    assert(c_cpu.x[2] == original);
    assert_stats(&threaded_runner, 0, 1, 0, 1);

    c_result = aarch64_run_one(&c_runner, &c_cpu);
    threaded_result = aarch64_run_one(&threaded_runner, &threaded_cpu);
    assert(c_result.stop == AARCH64_STEP_RETIRED);
    assert_step_equal(&c_result, &threaded_result);
    assert_cpu_equal(&c_cpu, &threaded_cpu);
    assert_memory_equal(&c_fixture.memory, &threaded_fixture.memory);
    qword_t stored;
    memcpy(&stored, c_fixture.memory.data, sizeof(stored));
    assert(stored == UINT64_C(0x0123456789abcdef));
    assert_stats(&threaded_runner, 0, 2, 0, 2);

    c_cpu.x[0] = threaded_cpu.x[0] = UINT64_MAX;
    c_result = aarch64_run_one(&c_runner, &c_cpu);
    threaded_result = aarch64_run_one(&threaded_runner, &threaded_cpu);
    assert(c_result.stop == AARCH64_STEP_RETIRED);
    assert_step_equal(&c_result, &threaded_result);
    assert_cpu_equal(&c_cpu, &threaded_cpu);
    assert_memory_equal(&c_fixture.memory, &threaded_fixture.memory);
    assert(c_cpu.x[0] == UINT64_MAX);
    assert_stats(&threaded_runner, 0, 3, 0, 3);

    c_cpu.x[0] = threaded_cpu.x[0] = 32;
    c_cpu.x[21] = threaded_cpu.x[21] = DATA_PAGE;
    c_cpu.v[30].d[0] = threaded_cpu.v[30].d[0] =
            UINT64_C(0x1122334455667788);
    c_cpu.v[30].d[1] = threaded_cpu.v[30].d[1] =
            UINT64_C(0x99aabbccddeeff00);
    c_result = aarch64_run_one(&c_runner, &c_cpu);
    threaded_result = aarch64_run_one(&threaded_runner, &threaded_cpu);
    assert(c_result.stop == AARCH64_STEP_RETIRED);
    assert_step_equal(&c_result, &threaded_result);
    assert_cpu_equal(&c_cpu, &threaded_cpu);
    assert_memory_equal(&c_fixture.memory, &threaded_fixture.memory);
    qword_t stored_low;
    qword_t stored_high;
    memcpy(&stored_low, c_fixture.memory.data + 32,
            sizeof(stored_low));
    memcpy(&stored_high, c_fixture.memory.data + 40,
            sizeof(stored_high));
    assert(stored_low == UINT64_C(0x1122334455667788));
    assert(stored_high == UINT64_C(0x99aabbccddeeff00));
    assert_stats(&threaded_runner, 0, 4, 0, 4);

    c_cpu.x[0] = threaded_cpu.x[0] = DATA_PAGE + 64;
    c_result = aarch64_run_one(&c_runner, &c_cpu);
    threaded_result = aarch64_run_one(&threaded_runner, &threaded_cpu);
    assert(c_result.stop == AARCH64_STEP_RETIRED);
    assert_step_equal(&c_result, &threaded_result);
    assert_cpu_equal(&c_cpu, &threaded_cpu);
    assert_memory_equal(&c_fixture.memory, &threaded_fixture.memory);
    for (unsigned structure = 0; structure < 4; structure++) {
        for (unsigned element = 0; element < 4; element++) {
            assert(c_cpu.v[28 + structure].s[element] ==
                    UINT32_C(0x10203040) +
                            element * 0x100 + structure);
        }
    }
    assert_stats(&threaded_runner, 0, 5, 0, 5);

    c_cpu.v[31].d[0] = threaded_cpu.v[31].d[0] =
            UINT64_C(0xfedcba9876543210);
    c_cpu.v[28].d[0] = threaded_cpu.v[28].d[0] = UINT64_MAX;
    c_cpu.v[28].d[1] = threaded_cpu.v[28].d[1] = UINT64_MAX;
    c_result = aarch64_run_one(&c_runner, &c_cpu);
    threaded_result = aarch64_run_one(&threaded_runner, &threaded_cpu);
    assert(c_result.stop == AARCH64_STEP_RETIRED);
    assert_step_equal(&c_result, &threaded_result);
    assert_cpu_equal(&c_cpu, &threaded_cpu);
    assert_memory_equal(&c_fixture.memory, &threaded_fixture.memory);
    assert(c_cpu.v[28].d[0] ==
            (UINT64_C(0xfedcba9876543210) >> 39));
    assert(c_cpu.v[28].d[1] == 0);
    assert_stats(&threaded_runner, 0, 6, 0, 6);

    c_cpu.v[28].s[0] = threaded_cpu.v[28].s[0] =
            UINT32_C(0x1111aaaa);
    c_cpu.v[28].s[1] = threaded_cpu.v[28].s[1] =
            UINT32_C(0x2222bbbb);
    c_cpu.v[28].s[2] = threaded_cpu.v[28].s[2] =
            UINT32_C(0x3333cccc);
    c_cpu.v[28].s[3] = threaded_cpu.v[28].s[3] =
            UINT32_C(0x4444dddd);
    c_result = aarch64_run_one(&c_runner, &c_cpu);
    threaded_result = aarch64_run_one(&threaded_runner, &threaded_cpu);
    assert(c_result.stop == AARCH64_STEP_RETIRED);
    assert_step_equal(&c_result, &threaded_result);
    assert_cpu_equal(&c_cpu, &threaded_cpu);
    assert_memory_equal(&c_fixture.memory, &threaded_fixture.memory);
    assert(c_cpu.v[28].d[0] == UINT64_C(0xddddccccbbbbaaaa));
    assert(c_cpu.v[28].d[1] == 0);
    assert_stats(&threaded_runner, 0, 7, 0, 7);

    c_cpu.v[0].s[0] = threaded_cpu.v[0].s[0] = UINT32_C(0x3fc00000);
    c_cpu.v[0].s[1] = threaded_cpu.v[0].s[1] = UINT32_MAX;
    c_cpu.v[0].d[1] = threaded_cpu.v[0].d[1] = UINT64_MAX;
    c_result = aarch64_run_one(&c_runner, &c_cpu);
    threaded_result = aarch64_run_one(&threaded_runner, &threaded_cpu);
    assert(c_result.stop == AARCH64_STEP_RETIRED);
    assert_step_equal(&c_result, &threaded_result);
    assert_cpu_equal(&c_cpu, &threaded_cpu);
    assert_memory_equal(&c_fixture.memory, &threaded_fixture.memory);
    assert(c_cpu.v[0].d[0] == UINT64_C(0x3ff8000000000000));
    assert(c_cpu.v[0].d[1] == 0);
    assert_stats(&threaded_runner, 0, 8, 0, 8);

    c_cpu.v[31].s[3] = threaded_cpu.v[31].s[3] =
            UINT32_C(0x55667788);
    c_cpu.v[15].q = threaded_cpu.v[15].q = ~(__uint128_t) 0;
    c_result = aarch64_run_one(&c_runner, &c_cpu);
    threaded_result = aarch64_run_one(&threaded_runner, &threaded_cpu);
    assert(c_result.stop == AARCH64_STEP_RETIRED);
    assert_step_equal(&c_result, &threaded_result);
    assert_cpu_equal(&c_cpu, &threaded_cpu);
    assert_memory_equal(&c_fixture.memory, &threaded_fixture.memory);
    assert(c_cpu.v[15].d[0] == UINT64_C(0x55667788));
    assert(c_cpu.v[15].d[1] == 0);
    assert_stats(&threaded_runner, 0, 9, 0, 9);

    c_cpu.v[0].s[0] = threaded_cpu.v[0].s[0] =
            UINT32_C(0x3f800000);
    c_cpu.v[30].s[0] = threaded_cpu.v[30].s[0] =
            UINT32_C(0x41800000);
    c_cpu.v[30].d[1] = threaded_cpu.v[30].d[1] = UINT64_MAX;
    c_result = aarch64_run_one(&c_runner, &c_cpu);
    threaded_result = aarch64_run_one(&threaded_runner, &threaded_cpu);
    assert(c_result.stop == AARCH64_STEP_RETIRED);
    assert_step_equal(&c_result, &threaded_result);
    assert_cpu_equal(&c_cpu, &threaded_cpu);
    assert_memory_equal(&c_fixture.memory, &threaded_fixture.memory);
    assert(c_cpu.v[30].d[0] == UINT64_C(0x3d800000));
    assert(c_cpu.v[30].d[1] == 0);
    assert_stats(&threaded_runner, 0, 10, 0, 10);

    c_cpu.v[31].d[0] = threaded_cpu.v[31].d[0] = UINT64_C(9);
    c_cpu.v[31].d[1] = threaded_cpu.v[31].d[1] = UINT64_MAX;
    c_result = aarch64_run_one(&c_runner, &c_cpu);
    threaded_result = aarch64_run_one(&threaded_runner, &threaded_cpu);
    assert(c_result.stop == AARCH64_STEP_RETIRED);
    assert_step_equal(&c_result, &threaded_result);
    assert_cpu_equal(&c_cpu, &threaded_cpu);
    assert_memory_equal(&c_fixture.memory, &threaded_fixture.memory);
    assert(c_cpu.v[31].d[0] == UINT64_C(0x4022000000000000));
    assert(c_cpu.v[31].d[1] == 0);
    assert_stats(&threaded_runner, 0, 11, 0, 11);

    c_cpu.v[31].d[0] = threaded_cpu.v[31].d[0] =
            UINT64_C(0x403e000000000000);
    c_cpu.v[31].d[1] = threaded_cpu.v[31].d[1] =
            UINT64_C(0x0123456789abcdef);
    c_cpu.x[1] = threaded_cpu.x[1] = UINT64_C(16);
    c_result = aarch64_run_one(&c_runner, &c_cpu);
    threaded_result = aarch64_run_one(&threaded_runner, &threaded_cpu);
    assert(c_result.stop == AARCH64_STEP_RETIRED);
    assert_step_equal(&c_result, &threaded_result);
    assert_cpu_equal(&c_cpu, &threaded_cpu);
    assert_memory_equal(&c_fixture.memory, &threaded_fixture.memory);
    assert(c_cpu.x[1] == UINT64_C(30));
    assert(c_cpu.v[31].d[0] == UINT64_C(0x403e000000000000));
    assert(c_cpu.v[31].d[1] == UINT64_C(0x0123456789abcdef));
    assert_stats(&threaded_runner, 0, 12, 0, 12);

    c_cpu.v[31].d[0] = threaded_cpu.v[31].d[0] = UINT64_C(1);
    c_cpu.v[31].d[1] = threaded_cpu.v[31].d[1] =
            UINT64_C(0x0123456789abcdef);
    c_cpu.v[30].d[0] = threaded_cpu.v[30].d[0] = UINT64_C(4);
    c_cpu.v[30].d[1] = threaded_cpu.v[30].d[1] = UINT64_C(1);
    c_result = aarch64_run_one(&c_runner, &c_cpu);
    threaded_result = aarch64_run_one(&threaded_runner, &threaded_cpu);
    assert(c_result.stop == AARCH64_STEP_RETIRED);
    assert_step_equal(&c_result, &threaded_result);
    assert_cpu_equal(&c_cpu, &threaded_cpu);
    assert_memory_equal(&c_fixture.memory, &threaded_fixture.memory);
    assert(c_cpu.v[30].d[0] == 0);
    assert(c_cpu.v[30].d[1] == 0);
    assert(c_cpu.v[31].d[0] == UINT64_C(1));
    assert(c_cpu.v[31].d[1] == UINT64_C(0x0123456789abcdef));
    assert_stats(&threaded_runner, 0, 13, 0, 13);

    c_fixture.memory.data[128] = 0xa5;
    threaded_fixture.memory.data[128] = 0xa5;
    c_cpu.x[1] = threaded_cpu.x[1] = DATA_PAGE + 128;
    c_cpu.v[31].d[0] = threaded_cpu.v[31].d[0] =
            UINT64_C(0x7766554433221100);
    c_cpu.v[31].d[1] = threaded_cpu.v[31].d[1] =
            UINT64_C(0xffeeddccbbaa9988);
    c_result = aarch64_run_one(&c_runner, &c_cpu);
    threaded_result = aarch64_run_one(&threaded_runner, &threaded_cpu);
    assert(c_result.stop == AARCH64_STEP_RETIRED);
    assert_step_equal(&c_result, &threaded_result);
    assert_cpu_equal(&c_cpu, &threaded_cpu);
    assert_memory_equal(&c_fixture.memory, &threaded_fixture.memory);
    assert(c_cpu.x[1] == DATA_PAGE + 128);
    assert(c_cpu.v[31].d[0] == UINT64_C(0x776655a533221100));
    assert(c_cpu.v[31].d[1] == UINT64_C(0xffeeddccbbaa9988));
    assert_stats(&threaded_runner, 0, 14, 0, 14);

    c_cpu.nzcv = threaded_cpu.nzcv = UINT32_C(0x60000000);
    c_cpu.v[0].d[0] = threaded_cpu.v[0].d[0] =
            UINT64_C(0x7ff0123456789abc);
    c_cpu.v[0].d[1] = threaded_cpu.v[0].d[1] =
            UINT64_C(0x1111222233334444);
    c_cpu.v[31].d[0] = threaded_cpu.v[31].d[0] =
            UINT64_C(0x0000000000000001);
    c_cpu.v[31].d[1] = threaded_cpu.v[31].d[1] =
            UINT64_C(0x5555666677778888);
    c_result = aarch64_run_one(&c_runner, &c_cpu);
    threaded_result = aarch64_run_one(&threaded_runner, &threaded_cpu);
    assert(c_result.stop == AARCH64_STEP_RETIRED);
    assert_step_equal(&c_result, &threaded_result);
    assert_cpu_equal(&c_cpu, &threaded_cpu);
    assert_memory_equal(&c_fixture.memory, &threaded_fixture.memory);
    assert(c_cpu.v[0].d[0] == UINT64_C(0x0000000000000001));
    assert(c_cpu.v[0].d[1] == 0);
    assert(c_cpu.v[31].d[0] == UINT64_C(0x0000000000000001));
    assert(c_cpu.v[31].d[1] == UINT64_C(0x5555666677778888));
    assert_stats(&threaded_runner, 0, 15, 0, 15);

    c_cpu.v[0].d[0] = threaded_cpu.v[0].d[0] =
            UINT64_C(0x42012e0be826d695);
    c_cpu.v[0].d[1] = threaded_cpu.v[0].d[1] =
            UINT64_C(0x0123456789abcdef);
    c_cpu.fpcr = threaded_cpu.fpcr = 0;
    c_cpu.fpsr = threaded_cpu.fpsr = AARCH64_FPSR_IXC;
    c_result = aarch64_run_one(&c_runner, &c_cpu);
    threaded_result = aarch64_run_one(&threaded_runner, &threaded_cpu);
    assert(c_result.stop == AARCH64_STEP_RETIRED);
    assert_step_equal(&c_result, &threaded_result);
    assert_cpu_equal(&c_cpu, &threaded_cpu);
    assert_memory_equal(&c_fixture.memory, &threaded_fixture.memory);
    assert(c_cpu.v[0].d[0] == UINT64_C(0x42012e0be8200000));
    assert(c_cpu.v[0].d[1] == 0);
    assert(c_cpu.fpsr == AARCH64_FPSR_IXC);
    assert_stats(&threaded_runner, 0, 16, 0, 16);

    c_cpu.v[0].d[0] = threaded_cpu.v[0].d[0] =
            UINT64_C(0x7ff0000000000000);
    c_cpu.v[0].d[1] = threaded_cpu.v[0].d[1] =
            UINT64_C(0x0123456789abcdef);
    c_cpu.fpcr = threaded_cpu.fpcr = 0;
    c_cpu.fpsr = threaded_cpu.fpsr = AARCH64_FPSR_IXC;
    c_result = aarch64_run_one(&c_runner, &c_cpu);
    threaded_result = aarch64_run_one(&threaded_runner, &threaded_cpu);
    assert(c_result.stop == AARCH64_STEP_RETIRED);
    assert_step_equal(&c_result, &threaded_result);
    assert_cpu_equal(&c_cpu, &threaded_cpu);
    assert_memory_equal(&c_fixture.memory, &threaded_fixture.memory);
    assert(c_cpu.v[0].d[0] == UINT64_C(0xfff0000000000000));
    assert(c_cpu.v[0].d[1] == 0);
    assert(c_cpu.fpsr == AARCH64_FPSR_IXC);
    assert_stats(&threaded_runner, 0, 17, 0, 17);

    c_cpu.v[27].d[0] = threaded_cpu.v[27].d[0] =
            UINT64_C(0x0706050403020100);
    c_cpu.v[27].d[1] = threaded_cpu.v[27].d[1] =
            UINT64_C(0x0f0e0d0c0b0a0908);
    c_cpu.v[30].d[0] = threaded_cpu.v[30].d[0] =
            UINT64_C(0x8786858483828180);
    c_cpu.v[30].d[1] = threaded_cpu.v[30].d[1] =
            UINT64_C(0x8f8e8d8c8b8a8988);
    const union aarch64_vector_reg source_n = c_cpu.v[27];
    const union aarch64_vector_reg source_m = c_cpu.v[30];
    c_result = aarch64_run_one(&c_runner, &c_cpu);
    threaded_result = aarch64_run_one(&threaded_runner, &threaded_cpu);
    assert(c_result.stop == AARCH64_STEP_RETIRED);
    assert_step_equal(&c_result, &threaded_result);
    assert_cpu_equal(&c_cpu, &threaded_cpu);
    assert_memory_equal(&c_fixture.memory, &threaded_fixture.memory);
    assert(c_cpu.v[0].d[0] == UINT64_C(0x0f0e0d0c0b0a0908));
    assert(c_cpu.v[0].d[1] == UINT64_C(0x8786858483828180));
    assert(memcmp(&c_cpu.v[27], &source_n, sizeof(source_n)) == 0);
    assert(memcmp(&c_cpu.v[30], &source_m, sizeof(source_m)) == 0);
    assert_stats(&threaded_runner, 0, 18, 0, 18);

    c_cpu.v[30].d[0] = threaded_cpu.v[30].d[0] =
            UINT64_C(0x0011223344556677);
    c_cpu.v[30].d[1] = threaded_cpu.v[30].d[1] =
            UINT64_C(0x8899aabbccddeeff);
    c_cpu.v[31].q = threaded_cpu.v[31].q = 0;
    const union aarch64_vector_reg not_source = c_cpu.v[30];
    c_result = aarch64_run_one(&c_runner, &c_cpu);
    threaded_result = aarch64_run_one(&threaded_runner, &threaded_cpu);
    assert(c_result.stop == AARCH64_STEP_RETIRED);
    assert_step_equal(&c_result, &threaded_result);
    assert_cpu_equal(&c_cpu, &threaded_cpu);
    assert_memory_equal(&c_fixture.memory, &threaded_fixture.memory);
    assert(c_cpu.v[31].d[0] == UINT64_C(0xffeeddccbbaa9988));
    assert(c_cpu.v[31].d[1] == UINT64_C(0x7766554433221100));
    assert(memcmp(&c_cpu.v[30], &not_source, sizeof(not_source)) == 0);
    assert_stats(&threaded_runner, 0, 19, 0, 19);

    const union aarch64_vector_reg not_result = c_cpu.v[31];
    c_result = aarch64_run_one(&c_runner, &c_cpu);
    threaded_result = aarch64_run_one(&threaded_runner, &threaded_cpu);
    assert(c_result.stop == AARCH64_STEP_RETIRED);
    assert_step_equal(&c_result, &threaded_result);
    assert_cpu_equal(&c_cpu, &threaded_cpu);
    assert_memory_equal(&c_fixture.memory, &threaded_fixture.memory);
    assert(c_cpu.v[30].d[0] == UINT64_C(0x00004488cd115599));
    assert(c_cpu.v[30].d[1] == UINT64_C(0x022266aaef3377bb));
    assert(memcmp(&c_cpu.v[31], &not_result, sizeof(not_result)) == 0);
    assert_stats(&threaded_runner, 0, 20, 0, 20);

    c_cpu.v[29].d[0] = threaded_cpu.v[29].d[0] =
            UINT64_C(0x7f7f7f7f7f7e7f7f);
    c_cpu.v[29].d[1] = threaded_cpu.v[29].d[1] =
            UINT64_C(0x7f7f7f7f7f7f7f7f);
    const union aarch64_vector_reg addp_source = c_cpu.v[29];
    c_result = aarch64_run_one(&c_runner, &c_cpu);
    threaded_result = aarch64_run_one(&threaded_runner, &threaded_cpu);
    assert(c_result.stop == AARCH64_STEP_RETIRED);
    assert_step_equal(&c_result, &threaded_result);
    assert_cpu_equal(&c_cpu, &threaded_cpu);
    assert_memory_equal(&c_fixture.memory, &threaded_fixture.memory);
    assert(c_cpu.v[31].d[0] == UINT64_C(0xfefefefefefdfefe));
    assert(c_cpu.v[31].d[1] == 0);
    assert(memcmp(&c_cpu.v[29], &addp_source, sizeof(addp_source)) == 0);
    assert_stats(&threaded_runner, 0, 21, 0, 21);

    c_cpu.v[0].d[0] = threaded_cpu.v[0].d[0] =
            UINT64_C(0x4000000000000000);
    c_cpu.v[15].d[0] = threaded_cpu.v[15].d[0] =
            UINT64_C(0x4008000000000000);
    c_cpu.v[31].d[0] = threaded_cpu.v[31].d[0] =
            UINT64_C(0x4024000000000000);
    c_result = aarch64_run_one(&c_runner, &c_cpu);
    threaded_result = aarch64_run_one(&threaded_runner, &threaded_cpu);
    assert(c_result.stop == AARCH64_STEP_RETIRED);
    assert_step_equal(&c_result, &threaded_result);
    assert_cpu_equal(&c_cpu, &threaded_cpu);
    assert_memory_equal(&c_fixture.memory, &threaded_fixture.memory);
    assert(c_cpu.v[31].d[0] == UINT64_C(0x4030000000000000));
    assert_stats(&threaded_runner, 0, 22, 0, 22);

    c_cpu.v[31].d[0] = threaded_cpu.v[31].d[0] =
            UINT64_C(0xffff807ffe01ff00);
    c_cpu.v[31].d[1] = threaded_cpu.v[31].d[1] =
            UINT64_C(0xff7fff0180800000);
    c_result = aarch64_run_one(&c_runner, &c_cpu);
    threaded_result = aarch64_run_one(&threaded_runner, &threaded_cpu);
    assert(c_result.stop == AARCH64_STEP_RETIRED);
    assert_step_equal(&c_result, &threaded_result);
    assert_cpu_equal(&c_cpu, &threaded_cpu);
    assert_memory_equal(&c_fixture.memory, &threaded_fixture.memory);
    assert(c_cpu.v[31].d[0] == UINT64_C(0x01fe00ff00ff00ff));
    assert(c_cpu.v[31].d[1] == UINT64_C(0x017e010001000000));
    assert_stats(&threaded_runner, 0, 23, 0, 23);

    /*
     * 每个半字只有低字节参与移位量解码；高字节使用不同噪声，
     * 同时覆盖零、正负、等于 lane 位宽及越界计数。
     */
    c_cpu.v[24].d[0] = threaded_cpu.v[24].d[0] =
            UINT64_C(0x3c08c3ff5a01a500);
    c_cpu.v[24].d[1] = threaded_cpu.v[24].d[1] =
            UINT64_C(0x2befd41181f07e10);
    const union aarch64_vector_reg ushl_shifts = c_cpu.v[24];
    c_result = aarch64_run_one(&c_runner, &c_cpu);
    threaded_result = aarch64_run_one(&threaded_runner, &threaded_cpu);
    assert(c_result.stop == AARCH64_STEP_RETIRED);
    assert_step_equal(&c_result, &threaded_result);
    assert_cpu_equal(&c_cpu, &threaded_cpu);
    assert_memory_equal(&c_fixture.memory, &threaded_fixture.memory);
    assert(c_cpu.v[31].d[0] == UINT64_C(0xfe00007f01fe00ff));
    assert(c_cpu.v[31].d[1] == 0);
    assert(memcmp(&c_cpu.v[24], &ushl_shifts, sizeof(ushl_shifts)) == 0);
    assert_stats(&threaded_runner, 0, 24, 0, 24);

    struct cpu_state addv_expected = c_cpu;
    addv_expected.v[31] = (union aarch64_vector_reg) {
        .h = {UINT16_C(0x017c)},
    };
    addv_expected.pc += 4;
    addv_expected.cycle++;
    c_result = aarch64_run_one(&c_runner, &c_cpu);
    threaded_result = aarch64_run_one(&threaded_runner, &threaded_cpu);
    assert(c_result.stop == AARCH64_STEP_RETIRED);
    assert_step_equal(&c_result, &threaded_result);
    assert_cpu_equal(&c_cpu, &threaded_cpu);
    assert_cpu_equal(&c_cpu, &addv_expected);
    assert_memory_equal(&c_fixture.memory, &threaded_fixture.memory);
    assert(memcmp(&c_cpu.v[24], &ushl_shifts, sizeof(ushl_shifts)) == 0);
    assert_stats(&threaded_runner, 0, 25, 0, 25);

    const union aarch64_vector_reg neg_source = c_cpu.v[31];
    struct cpu_state neg_expected = c_cpu;
    neg_expected.v[29] = (union aarch64_vector_reg) {
        .s = {UINT32_C(0xfffffe84), 0},
    };
    neg_expected.pc += 4;
    neg_expected.cycle++;
    c_result = aarch64_run_one(&c_runner, &c_cpu);
    threaded_result = aarch64_run_one(&threaded_runner, &threaded_cpu);
    assert(c_result.stop == AARCH64_STEP_RETIRED);
    assert_step_equal(&c_result, &threaded_result);
    assert_cpu_equal(&c_cpu, &threaded_cpu);
    assert_cpu_equal(&c_cpu, &neg_expected);
    assert_memory_equal(&c_fixture.memory, &threaded_fixture.memory);
    assert(memcmp(&c_cpu.v[31], &neg_source, sizeof(neg_source)) == 0);
    assert(memcmp(&c_cpu.v[24], &ushl_shifts, sizeof(ushl_shifts)) == 0);
    assert_stats(&threaded_runner, 0, 26, 0, 26);

    const union aarch64_vector_reg neg_result = c_cpu.v[29];
    struct cpu_state cnt_expected = c_cpu;
    cnt_expected.v[31] = (union aarch64_vector_reg) {
        .d = {UINT64_C(0x0000000000000105), 0},
    };
    cnt_expected.pc += 4;
    cnt_expected.cycle++;
    c_result = aarch64_run_one(&c_runner, &c_cpu);
    threaded_result = aarch64_run_one(&threaded_runner, &threaded_cpu);
    assert(c_result.stop == AARCH64_STEP_RETIRED);
    assert_step_equal(&c_result, &threaded_result);
    assert_cpu_equal(&c_cpu, &threaded_cpu);
    assert_cpu_equal(&c_cpu, &cnt_expected);
    assert_memory_equal(&c_fixture.memory, &threaded_fixture.memory);
    assert(memcmp(&c_cpu.v[29], &neg_result, sizeof(neg_result)) == 0);
    assert(memcmp(&c_cpu.v[24], &ushl_shifts, sizeof(ushl_shifts)) == 0);
    assert_stats(&threaded_runner, 0, 27, 0, 27);

    const union aarch64_vector_reg add_source = c_cpu.v[30];
    struct cpu_state add_expected = c_cpu;
    add_expected.v[31] = (union aarch64_vector_reg) {
        .d = {UINT64_C(0x00004488cd11569e), 0},
    };
    add_expected.pc += 4;
    add_expected.cycle++;
    c_result = aarch64_run_one(&c_runner, &c_cpu);
    threaded_result = aarch64_run_one(&threaded_runner, &threaded_cpu);
    assert(c_result.stop == AARCH64_STEP_RETIRED);
    assert_step_equal(&c_result, &threaded_result);
    assert_cpu_equal(&c_cpu, &threaded_cpu);
    assert_cpu_equal(&c_cpu, &add_expected);
    assert_memory_equal(&c_fixture.memory, &threaded_fixture.memory);
    assert(memcmp(&c_cpu.v[30], &add_source, sizeof(add_source)) == 0);
    assert(memcmp(&c_cpu.v[29], &neg_result, sizeof(neg_result)) == 0);
    assert(memcmp(&c_cpu.v[24], &ushl_shifts, sizeof(ushl_shifts)) == 0);
    assert_stats(&threaded_runner, 0, 28, 0, 28);

    const union aarch64_vector_reg sub_left = c_cpu.v[31];
    struct cpu_state sub_expected = c_cpu;
    sub_expected.v[30] = (union aarch64_vector_reg) {
        .d = {
            UINT64_C(0x0000000000000105),
            UINT64_C(0xfddd995510cc8845),
        },
    };
    sub_expected.pc += 4;
    sub_expected.cycle++;
    c_result = aarch64_run_one(&c_runner, &c_cpu);
    threaded_result = aarch64_run_one(&threaded_runner, &threaded_cpu);
    assert(c_result.stop == AARCH64_STEP_RETIRED);
    assert_step_equal(&c_result, &threaded_result);
    assert_cpu_equal(&c_cpu, &threaded_cpu);
    assert_cpu_equal(&c_cpu, &sub_expected);
    assert_memory_equal(&c_fixture.memory, &threaded_fixture.memory);
    assert(memcmp(&c_cpu.v[31], &sub_left, sizeof(sub_left)) == 0);
    assert(memcmp(&c_cpu.v[29], &neg_result, sizeof(neg_result)) == 0);
    assert(memcmp(&c_cpu.v[24], &ushl_shifts, sizeof(ushl_shifts)) == 0);
    assert_stats(&threaded_runner, 0, 29, 0, 29);

    const union aarch64_vector_reg sub_result = c_cpu.v[30];
    struct cpu_state cmge_expected = c_cpu;
    cmge_expected.v[31] = (union aarch64_vector_reg) {
        .d = {UINT64_MAX, 0},
    };
    cmge_expected.pc += 4;
    cmge_expected.cycle++;
    c_result = aarch64_run_one(&c_runner, &c_cpu);
    threaded_result = aarch64_run_one(&threaded_runner, &threaded_cpu);
    assert(c_result.stop == AARCH64_STEP_RETIRED);
    assert_step_equal(&c_result, &threaded_result);
    assert_cpu_equal(&c_cpu, &threaded_cpu);
    assert_cpu_equal(&c_cpu, &cmge_expected);
    assert_memory_equal(&c_fixture.memory, &threaded_fixture.memory);
    assert(memcmp(&c_cpu.v[30], &sub_result, sizeof(sub_result)) == 0);
    assert(memcmp(&c_cpu.v[29], &neg_result, sizeof(neg_result)) == 0);
    assert(memcmp(&c_cpu.v[24], &ushl_shifts, sizeof(ushl_shifts)) == 0);
    assert_stats(&threaded_runner, 0, 30, 0, 30);

    const union aarch64_vector_reg cmge_result = c_cpu.v[31];
    struct cpu_state cmgt_expected = c_cpu;
    cmgt_expected.v[30] = (union aarch64_vector_reg) {0};
    cmgt_expected.pc += 4;
    cmgt_expected.cycle++;
    c_result = aarch64_run_one(&c_runner, &c_cpu);
    threaded_result = aarch64_run_one(&threaded_runner, &threaded_cpu);
    assert(c_result.stop == AARCH64_STEP_RETIRED);
    assert_step_equal(&c_result, &threaded_result);
    assert_cpu_equal(&c_cpu, &threaded_cpu);
    assert_cpu_equal(&c_cpu, &cmgt_expected);
    assert_memory_equal(&c_fixture.memory, &threaded_fixture.memory);
    assert(memcmp(&c_cpu.v[31], &cmge_result, sizeof(cmge_result)) == 0);
    assert(memcmp(&c_cpu.v[29], &neg_result, sizeof(neg_result)) == 0);
    assert(memcmp(&c_cpu.v[24], &ushl_shifts, sizeof(ushl_shifts)) == 0);
    assert_stats(&threaded_runner, 0, 31, 0, 31);

    assert(c_cpu.x[0] == DATA_PAGE + 64);
    assert(c_cpu.v[31].d[0] == UINT64_MAX);
    assert(c_cpu.v[31].d[1] == 0);
    qword_t lane_store_before;
    memcpy(&lane_store_before,
            c_fixture.memory.data + 64, sizeof(lane_store_before));
    assert(lane_store_before == UINT64_C(0x1020304110203040));
    byte_t expected_data[GUEST_MEMORY_PAGE_SIZE];
    memcpy(expected_data,
            c_fixture.memory.data, sizeof(expected_data));
    memset(expected_data + 64, 0, sizeof(qword_t));
    struct cpu_state st1_expected = c_cpu;
    st1_expected.pc += 4;
    st1_expected.cycle++;
    c_result = aarch64_run_one(&c_runner, &c_cpu);
    threaded_result = aarch64_run_one(&threaded_runner, &threaded_cpu);
    assert(c_result.stop == AARCH64_STEP_RETIRED);
    assert_step_equal(&c_result, &threaded_result);
    assert_cpu_equal(&c_cpu, &threaded_cpu);
    assert_cpu_equal(&c_cpu, &st1_expected);
    assert_memory_equal(&c_fixture.memory, &threaded_fixture.memory);
    assert(memcmp(c_fixture.memory.data,
            expected_data, sizeof(expected_data)) == 0);
    assert(memcmp(&c_cpu.v[31], &cmge_result, sizeof(cmge_result)) == 0);
    assert(memcmp(&c_cpu.v[30],
            &cmgt_expected.v[30], sizeof(c_cpu.v[30])) == 0);
    assert(memcmp(&c_cpu.v[29], &neg_result, sizeof(neg_result)) == 0);
    assert(memcmp(&c_cpu.v[24], &ushl_shifts, sizeof(ushl_shifts)) == 0);
    assert_stats(&threaded_runner, 0, 32, 0, 32);

    struct cpu_state shl_expected = c_cpu;
    shl_expected.v[31] = (union aarch64_vector_reg) {
        .d = {UINT64_C(0xfffffffffffffff8), 0},
    };
    shl_expected.pc += 4;
    shl_expected.cycle++;
    c_result = aarch64_run_one(&c_runner, &c_cpu);
    threaded_result = aarch64_run_one(&threaded_runner, &threaded_cpu);
    assert(c_result.stop == AARCH64_STEP_RETIRED);
    assert_step_equal(&c_result, &threaded_result);
    assert_cpu_equal(&c_cpu, &threaded_cpu);
    assert_cpu_equal(&c_cpu, &shl_expected);
    assert_memory_equal(&c_fixture.memory, &threaded_fixture.memory);
    assert(memcmp(c_fixture.memory.data,
            expected_data, sizeof(expected_data)) == 0);
    assert(memcmp(&c_cpu.v[30],
            &cmgt_expected.v[30], sizeof(c_cpu.v[30])) == 0);
    assert(memcmp(&c_cpu.v[29], &neg_result, sizeof(neg_result)) == 0);
    assert(memcmp(&c_cpu.v[24], &ushl_shifts, sizeof(ushl_shifts)) == 0);
    assert_stats(&threaded_runner, 0, 33, 0, 33);

    c_cpu.v[30].d[0] = threaded_cpu.v[30].d[0] =
            UINT64_C(0x5566778811223344);
    c_cpu.v[30].d[1] = threaded_cpu.v[30].d[1] =
            UINT64_C(0xa5a5a5a5a5a5a5a5);
    struct cpu_state rev64_expected = c_cpu;
    rev64_expected.v[30] = (union aarch64_vector_reg) {
        .d = {UINT64_C(0x1122334455667788), 0},
    };
    rev64_expected.pc += 4;
    rev64_expected.cycle++;
    c_result = aarch64_run_one(&c_runner, &c_cpu);
    threaded_result = aarch64_run_one(&threaded_runner, &threaded_cpu);
    assert(c_result.stop == AARCH64_STEP_RETIRED);
    assert_step_equal(&c_result, &threaded_result);
    assert_cpu_equal(&c_cpu, &threaded_cpu);
    assert_cpu_equal(&c_cpu, &rev64_expected);
    assert_memory_equal(&c_fixture.memory, &threaded_fixture.memory);
    assert(memcmp(c_fixture.memory.data,
            expected_data, sizeof(expected_data)) == 0);
    assert(memcmp(&c_cpu.v[29], &neg_result, sizeof(neg_result)) == 0);
    assert(memcmp(&c_cpu.v[24], &ushl_shifts, sizeof(ushl_shifts)) == 0);
    assert_stats(&threaded_runner, 0, 34, 0, 34);

    c_cpu.x[1] = threaded_cpu.x[1] = DATA_PAGE + 160;
    c_cpu.v[30].q = threaded_cpu.v[30].q = ~(__uint128_t) 0;
    c_cpu.v[31].q = threaded_cpu.v[31].q = 0;
    struct cpu_state ld2_expected = c_cpu;
    ld2_expected.v[30] = (union aarch64_vector_reg) {
        .d = {
            UINT64_C(0x5566778811223344),
            UINT64_C(0xddeeff0099aabbcc),
        },
    };
    ld2_expected.v[31] = (union aarch64_vector_reg) {
        .d = {
            UINT64_C(0x102030400badc0de),
            UINT64_C(0x800000017fffffff),
        },
    };
    ld2_expected.pc += 4;
    ld2_expected.cycle++;
    c_result = aarch64_run_one(&c_runner, &c_cpu);
    threaded_result = aarch64_run_one(&threaded_runner, &threaded_cpu);
    assert(c_result.stop == AARCH64_STEP_RETIRED);
    assert_step_equal(&c_result, &threaded_result);
    assert_cpu_equal(&c_cpu, &threaded_cpu);
    assert_cpu_equal(&c_cpu, &ld2_expected);
    assert_memory_equal(&c_fixture.memory, &threaded_fixture.memory);
    assert(memcmp(c_fixture.memory.data,
            expected_data, sizeof(expected_data)) == 0);
    assert(memcmp(&c_cpu.v[29], &neg_result, sizeof(neg_result)) == 0);
    assert(memcmp(&c_cpu.v[24], &ushl_shifts, sizeof(ushl_shifts)) == 0);
    assert_stats(&threaded_runner, 0, 35, 0, 35);

    c_cpu.x[27] = threaded_cpu.x[27] = DATA_PAGE + 224;
    struct cpu_state ld1r_expected = c_cpu;
    ld1r_expected.v[31] = (union aarch64_vector_reg) {
        .d = {
            UINT64_C(0xa7a7a7a7a7a7a7a7),
            UINT64_C(0xa7a7a7a7a7a7a7a7),
        },
    };
    ld1r_expected.pc += 4;
    ld1r_expected.cycle++;
    c_result = aarch64_run_one(&c_runner, &c_cpu);
    threaded_result = aarch64_run_one(&threaded_runner, &threaded_cpu);
    assert(c_result.stop == AARCH64_STEP_RETIRED);
    assert_step_equal(&c_result, &threaded_result);
    assert_cpu_equal(&c_cpu, &threaded_cpu);
    assert_cpu_equal(&c_cpu, &ld1r_expected);
    assert_memory_equal(&c_fixture.memory, &threaded_fixture.memory);
    assert(memcmp(c_fixture.memory.data,
            expected_data, sizeof(expected_data)) == 0);
    assert(memcmp(&c_cpu.v[30],
            &ld2_expected.v[30], sizeof(c_cpu.v[30])) == 0);
    assert(memcmp(&c_cpu.v[29], &neg_result, sizeof(neg_result)) == 0);
    assert(memcmp(&c_cpu.v[24], &ushl_shifts, sizeof(ushl_shifts)) == 0);
    assert(c_cpu.x[27] == DATA_PAGE + 224);
    assert_stats(&threaded_runner, 0, 36, 0, 36);

    c_cpu.v[31].d[0] = threaded_cpu.v[31].d[0] =
            UINT64_C(0x7766554433221100);
    c_cpu.v[31].d[1] = threaded_cpu.v[31].d[1] =
            UINT64_C(0xffeeddccbbaa9988);
    struct cpu_state rev32_expected = c_cpu;
    rev32_expected.v[31] = (union aarch64_vector_reg) {
        .d = {
            UINT64_C(0x4455667700112233),
            UINT64_C(0xccddeeff8899aabb),
        },
    };
    rev32_expected.pc += 4;
    rev32_expected.cycle++;
    c_result = aarch64_run_one(&c_runner, &c_cpu);
    threaded_result = aarch64_run_one(&threaded_runner, &threaded_cpu);
    assert(c_result.stop == AARCH64_STEP_RETIRED);
    assert_step_equal(&c_result, &threaded_result);
    assert_cpu_equal(&c_cpu, &threaded_cpu);
    assert_cpu_equal(&c_cpu, &rev32_expected);
    assert_memory_equal(&c_fixture.memory, &threaded_fixture.memory);
    assert(memcmp(c_fixture.memory.data,
            expected_data, sizeof(expected_data)) == 0);
    assert(memcmp(&c_cpu.v[30],
            &ld2_expected.v[30], sizeof(c_cpu.v[30])) == 0);
    assert(memcmp(&c_cpu.v[29], &neg_result, sizeof(neg_result)) == 0);
    assert(memcmp(&c_cpu.v[24], &ushl_shifts, sizeof(ushl_shifts)) == 0);
    assert(c_cpu.x[27] == DATA_PAGE + 224);
    assert_stats(&threaded_runner, 0, 37, 0, 37);

    const union aarch64_vector_reg rev32_result = c_cpu.v[31];
    struct cpu_state scalar_neg_expected = c_cpu;
    scalar_neg_expected.v[31] = (union aarch64_vector_reg) {
        .d = {UINT64_C(0xbbaa9988ffeeddcd), 0},
    };
    scalar_neg_expected.pc += 4;
    scalar_neg_expected.cycle++;
    c_result = aarch64_run_one(&c_runner, &c_cpu);
    threaded_result = aarch64_run_one(&threaded_runner, &threaded_cpu);
    assert(c_result.stop == AARCH64_STEP_RETIRED);
    assert_step_equal(&c_result, &threaded_result);
    assert_cpu_equal(&c_cpu, &threaded_cpu);
    assert_cpu_equal(&c_cpu, &scalar_neg_expected);
    assert_memory_equal(&c_fixture.memory, &threaded_fixture.memory);
    assert(memcmp(c_fixture.memory.data,
            expected_data, sizeof(expected_data)) == 0);
    assert(rev32_result.d[0] == UINT64_C(0x4455667700112233));
    assert(rev32_result.d[1] == UINT64_C(0xccddeeff8899aabb));
    assert(memcmp(&c_cpu.v[30],
            &ld2_expected.v[30], sizeof(c_cpu.v[30])) == 0);
    assert(memcmp(&c_cpu.v[29], &neg_result, sizeof(neg_result)) == 0);
    assert(memcmp(&c_cpu.v[24], &ushl_shifts, sizeof(ushl_shifts)) == 0);
    assert(c_cpu.x[27] == DATA_PAGE + 224);
    assert_stats(&threaded_runner, 0, 38, 0, 38);

    const union aarch64_vector_reg scalar_neg_result = c_cpu.v[31];
    const union aarch64_vector_reg scalar_sshr_poison = {
        .d = {
            UINT64_C(0x0123456789abcdef),
            UINT64_C(0xfedcba9876543210),
        },
    };
    c_cpu.v[29] = scalar_sshr_poison;
    threaded_cpu.v[29] = scalar_sshr_poison;
    struct cpu_state scalar_sshr_expected = c_cpu;
    scalar_sshr_expected.v[29] = (union aarch64_vector_reg) {
        .d = {UINT64_C(0xf77553311ffddbb9), 0},
    };
    scalar_sshr_expected.pc += 4;
    scalar_sshr_expected.cycle++;
    c_result = aarch64_run_one(&c_runner, &c_cpu);
    threaded_result = aarch64_run_one(&threaded_runner, &threaded_cpu);
    assert(c_result.stop == AARCH64_STEP_RETIRED);
    assert_step_equal(&c_result, &threaded_result);
    assert_cpu_equal(&c_cpu, &threaded_cpu);
    assert_cpu_equal(&c_cpu, &scalar_sshr_expected);
    assert_memory_equal(&c_fixture.memory, &threaded_fixture.memory);
    assert(memcmp(c_fixture.memory.data,
            expected_data, sizeof(expected_data)) == 0);
    assert(scalar_neg_result.d[0] ==
            UINT64_C(0xbbaa9988ffeeddcd));
    assert(scalar_neg_result.d[1] == 0);
    assert(memcmp(&c_cpu.v[31],
            &scalar_neg_result, sizeof(scalar_neg_result)) == 0);
    assert(memcmp(&c_cpu.v[30],
            &ld2_expected.v[30], sizeof(c_cpu.v[30])) == 0);
    assert(memcmp(&c_cpu.v[24], &ushl_shifts, sizeof(ushl_shifts)) == 0);
    assert(c_cpu.x[27] == DATA_PAGE + 224);
    assert_stats(&threaded_runner, 0, 39, 0, 39);
}

static void test_c_and_threaded_differential(void) {
    struct test_fixture c_fixture;
    struct test_fixture threaded_fixture;
    init_fixture(&c_fixture);
    init_fixture(&threaded_fixture);
    write_differential_program(&c_fixture);
    write_differential_program(&threaded_fixture);
    memset(c_fixture.memory.data, 0xa5, sizeof(c_fixture.memory.data));
    memset(threaded_fixture.memory.data, 0xa5,
            sizeof(threaded_fixture.memory.data));

    struct aarch64_runner c_runner;
    struct aarch64_runner threaded_runner;
    assert(aarch64_runner_init_backend(
            &c_runner, &c_fixture.tlb, AARCH64_BACKEND_C));
    assert(aarch64_runner_init_backend(&threaded_runner,
            &threaded_fixture.tlb, AARCH64_BACKEND_THREADED));
    struct cpu_state c_cpu;
    struct cpu_state threaded_cpu;
    init_differential_cpu(&c_cpu);
    init_differential_cpu(&threaded_cpu);
    c_cpu.v[31].s[0] = threaded_cpu.v[31].s[0] =
            UINT32_C(0x80000001);
    c_cpu.v[31].s[1] = threaded_cpu.v[31].s[1] =
            UINT32_C(0xfedcba98);

    const enum aarch64_step_stop expected_stops[] = {
        AARCH64_STEP_RETIRED,
        AARCH64_STEP_RETIRED,
        AARCH64_STEP_RETIRED,
        AARCH64_STEP_RETIRED,
        AARCH64_STEP_RETIRED,
        AARCH64_STEP_RETIRED,
        AARCH64_STEP_SYSCALL,
    };
    for (unsigned step = 0; step < array_size(expected_stops); step++) {
        struct aarch64_step_result c_result =
                aarch64_run_one(&c_runner, &c_cpu);
        struct aarch64_step_result threaded_result =
                aarch64_run_one(&threaded_runner, &threaded_cpu);
        assert(c_result.stop == expected_stops[step]);
        assert_step_equal(&c_result, &threaded_result);
        assert_cpu_equal(&c_cpu, &threaded_cpu);
        assert_memory_equal(&c_fixture.memory,
                &threaded_fixture.memory);
        if (step == 5) {
            assert(c_cpu.v[31].d[0] ==
                    UINT64_C(0x0000000080000001));
            assert(c_cpu.v[31].d[1] ==
                    UINT64_C(0x00000000fedcba98));
            assert(c_cpu.pc == CODE_PAGE + 6 * 4);
            assert(c_cpu.cycle == 15);
            assert_stats(&threaded_runner, 0, 6, 5, 1);
        }
    }
    assert(c_cpu.x[0] == 8);
    assert(c_cpu.x[2] == 8);
    assert(c_cpu.x[3] == 16);
    assert(c_fixture.memory.data[0x80] == 8);
    assert_stats(&threaded_runner, 0, 7, 6, 1);

    struct aarch64_step_result c_undefined = run_at(
            &c_runner, &c_cpu, CODE_PAGE + 7 * 4);
    struct aarch64_step_result threaded_undefined = run_at(
            &threaded_runner, &threaded_cpu, CODE_PAGE + 7 * 4);
    assert(c_undefined.stop == AARCH64_STEP_UNDEFINED);
    assert_step_equal(&c_undefined, &threaded_undefined);
    assert_cpu_equal(&c_cpu, &threaded_cpu);
    assert_memory_equal(&c_fixture.memory, &threaded_fixture.memory);
    assert_stats(&threaded_runner, 0, 8, 6, 1);

    struct aarch64_step_result c_result = run_at(
            &c_runner, &c_cpu, CODE_PAGE);
    struct aarch64_step_result threaded_result = run_at(
            &threaded_runner, &threaded_cpu, CODE_PAGE);
    assert_step_equal(&c_result, &threaded_result);
    assert_cpu_equal(&c_cpu, &threaded_cpu);
    assert_memory_equal(&c_fixture.memory, &threaded_fixture.memory);
    assert_stats(&threaded_runner, 1, 8, 7, 1);
}

static void test_cache_keys_and_collision(void) {
    struct test_fixture fixture;
    init_fixture(&fixture);
    const guest_addr_t first = CODE_PAGE + 0x100;
    const guest_addr_t collision = first +
            AARCH64_THREADED_CACHE_SIZE * 4;
    write_instruction(&fixture.tlb, first, INSTRUCTION_NOP);
    write_instruction(&fixture.tlb, collision, INSTRUCTION_NOP);

    struct aarch64_runner runner;
    assert(aarch64_runner_init_backend(
            &runner, &fixture.tlb, AARCH64_BACKEND_THREADED));
    struct cpu_state cpu = {.x[0] = 3};
    assert(run_at(&runner, &cpu, first).stop == AARCH64_STEP_RETIRED);
    assert_stats(&runner, 0, 1, 1, 0);
    assert(run_at(&runner, &cpu, first).stop == AARCH64_STEP_RETIRED);
    assert_stats(&runner, 1, 1, 2, 0);
    assert(run_at(&runner, &cpu, collision).stop == AARCH64_STEP_RETIRED);
    assert(cpu.x[0] == 3);
    assert_stats(&runner, 1, 2, 3, 0);

    write_instruction(&fixture.tlb, collision, INSTRUCTION_ADD_X0);
    assert(run_at(&runner, &cpu, collision).stop == AARCH64_STEP_RETIRED);
    assert(cpu.x[0] == 4);
    assert_stats(&runner, 1, 3, 4, 0);
    assert(run_at(&runner, &cpu, collision).stop == AARCH64_STEP_RETIRED);
    assert(cpu.x[0] == 5);
    assert_stats(&runner, 2, 3, 5, 0);
    assert(run_at(&runner, &cpu, first).stop == AARCH64_STEP_RETIRED);
    assert_stats(&runner, 2, 4, 6, 0);
}

static void test_rwx_self_modifying_code(void) {
    struct test_fixture fixture;
    init_fixture(&fixture);
    const guest_addr_t pc = CODE_PAGE + 0x40;
    struct aarch64_runner runner;
    assert(aarch64_runner_init_backend(
            &runner, &fixture.tlb, AARCH64_BACKEND_THREADED));
    struct cpu_state cpu = {.x[0] = 10};

    write_instruction(&fixture.tlb, pc, INSTRUCTION_NOP);
    assert(run_at(&runner, &cpu, pc).stop == AARCH64_STEP_RETIRED);
    assert_stats(&runner, 0, 1, 1, 0);
    assert(run_at(&runner, &cpu, pc).stop == AARCH64_STEP_RETIRED);
    assert_stats(&runner, 1, 1, 2, 0);

    write_instruction(&fixture.tlb, pc, INSTRUCTION_ADD_X0);
    assert(run_at(&runner, &cpu, pc).stop == AARCH64_STEP_RETIRED);
    assert(cpu.x[0] == 11);
    assert_stats(&runner, 1, 2, 3, 0);

    write_instruction(&fixture.tlb, pc, INSTRUCTION_UNDEFINED);
    qword_t cycle = cpu.cycle;
    assert(run_at(&runner, &cpu, pc).stop == AARCH64_STEP_UNDEFINED);
    assert(cpu.pc == pc);
    assert(cpu.cycle == cycle);
    assert_stats(&runner, 1, 3, 3, 0);
    assert(run_at(&runner, &cpu, pc).stop == AARCH64_STEP_UNDEFINED);
    assert(cpu.pc == pc);
    assert(cpu.cycle == cycle);
    assert_stats(&runner, 2, 3, 3, 0);

    write_instruction(&fixture.tlb, pc, INSTRUCTION_NOP);
    assert(run_at(&runner, &cpu, pc).stop == AARCH64_STEP_RETIRED);
    assert(cpu.x[0] == 11);
    assert_stats(&runner, 2, 4, 4, 0);
}

static void test_mapping_invalidation(void) {
    struct test_fixture fixture;
    init_fixture(&fixture);
    const guest_addr_t pc = CODE_PAGE + 0x80;
    write_instruction(&fixture.tlb, pc, INSTRUCTION_NOP);
    put_instruction(fixture.memory.replacement_code + 0x80,
            INSTRUCTION_NOP);

    struct aarch64_runner runner;
    assert(aarch64_runner_init_backend(
            &runner, &fixture.tlb, AARCH64_BACKEND_THREADED));
    struct cpu_state cpu = {0};
    assert(run_at(&runner, &cpu, pc).stop == AARCH64_STEP_RETIRED);
    assert(run_at(&runner, &cpu, pc).stop == AARCH64_STEP_RETIRED);
    assert_stats(&runner, 1, 1, 2, 0);

    guest_address_space_changed(&fixture.space);
    assert(run_at(&runner, &cpu, pc).stop == AARCH64_STEP_RETIRED);
    assert_stats(&runner, 2, 1, 3, 0);

    fixture.memory.code_permissions =
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE;
    guest_address_space_changed(&fixture.space);
    qword_t cycle = cpu.cycle;
    struct aarch64_step_result result = run_at(&runner, &cpu, pc);
    assert(result.stop == AARCH64_STEP_FETCH_FAULT);
    assert(result.fault.kind == GUEST_MEMORY_FAULT_PERMISSION);
    assert(result.fault.address == pc);
    assert(cpu.pc == pc);
    assert(cpu.cycle == cycle);
    assert_stats(&runner, 2, 1, 3, 0);

    fixture.memory.code_permissions = GUEST_MEMORY_READ |
            GUEST_MEMORY_WRITE | GUEST_MEMORY_EXECUTE;
    guest_address_space_changed(&fixture.space);
    assert(run_at(&runner, &cpu, pc).stop == AARCH64_STEP_RETIRED);
    assert_stats(&runner, 3, 1, 4, 0);

    put_instruction(fixture.memory.replacement_code + 0x80,
            INSTRUCTION_ADD_X0);
    fixture.memory.code_mapping = fixture.memory.replacement_code;
    guest_address_space_changed(&fixture.space);
    assert(run_at(&runner, &cpu, pc).stop == AARCH64_STEP_RETIRED);
    assert(cpu.x[0] == 1);
    assert_stats(&runner, 3, 2, 5, 0);
}

static void test_shared_space_invalidation(void) {
    struct test_fixture fixture;
    init_fixture(&fixture);
    const guest_addr_t pc = CODE_PAGE + 0xc0;
    write_instruction(&fixture.tlb, pc, INSTRUCTION_NOP);

    struct guest_tlb second_tlb;
    guest_tlb_init(&second_tlb, &fixture.space);
    struct aarch64_runner runners[2];
    assert(aarch64_runner_init_backend(
            &runners[0], &fixture.tlb, AARCH64_BACKEND_THREADED));
    assert(aarch64_runner_init_backend(
            &runners[1], &second_tlb, AARCH64_BACKEND_THREADED));
    struct cpu_state cpus[2] = {{0}, {0}};
    for (unsigned index = 0; index < array_size(runners); index++) {
        assert(run_at(&runners[index], &cpus[index], pc).stop ==
                AARCH64_STEP_RETIRED);
        assert(run_at(&runners[index], &cpus[index], pc).stop ==
                AARCH64_STEP_RETIRED);
        assert_stats(&runners[index], 1, 1, 2, 0);
    }

    write_instruction(&fixture.tlb, pc, INSTRUCTION_ADD_X0);
    for (unsigned index = 0; index < array_size(runners); index++) {
        assert(run_at(&runners[index], &cpus[index], pc).stop ==
                AARCH64_STEP_RETIRED);
        assert(cpus[index].x[0] == 1);
        assert_stats(&runners[index], 1, 2, 3, 0);
    }
}

#if ISH_AARCH64_THREADED_PROFILE
static void test_profile_aggregation(void) {
    struct test_fixture first_fixture;
    struct test_fixture second_fixture;
    struct test_fixture c_fixture;
    init_fixture(&first_fixture);
    init_fixture(&second_fixture);
    init_fixture(&c_fixture);
    write_instruction(&first_fixture.tlb, CODE_PAGE,
            INSTRUCTION_CLZ_W2_W2);
    write_instruction(&first_fixture.tlb, CODE_PAGE + 4,
            INSTRUCTION_ADDS_X22_SP_W23_SXTW_2);
    write_instruction(&second_fixture.tlb, CODE_PAGE,
            INSTRUCTION_ADDS_X3);
    write_instruction(&second_fixture.tlb, CODE_PAGE + 4,
            INSTRUCTION_UNDEFINED);
    write_instruction(&second_fixture.tlb, CODE_PAGE + 8,
            INSTRUCTION_CLZ_W2_W2);
    write_instruction(&second_fixture.tlb, CODE_PAGE + 12,
            INSTRUCTION_ADDS_X22_SP_W23_SXTW_2);
    write_instruction(&c_fixture.tlb, CODE_PAGE,
            INSTRUCTION_LDR_X2);

    struct aarch64_runner first_runner;
    struct aarch64_runner second_runner;
    struct aarch64_runner c_runner;
    assert(aarch64_runner_init_backend(&first_runner,
            &first_fixture.tlb, AARCH64_BACKEND_THREADED));
    assert(aarch64_runner_init_backend(&second_runner,
            &second_fixture.tlb, AARCH64_BACKEND_THREADED));
    assert(aarch64_runner_init_backend(
            &c_runner, &c_fixture.tlb, AARCH64_BACKEND_C));
    struct cpu_state first_cpu = {0};
    struct cpu_state second_cpu = {0};
    struct cpu_state c_cpu = {.x[1] = DATA_PAGE};

    first_cpu.sp = UINT64_C(0x1000);
    first_cpu.x[2] = UINT64_C(0xa5a5a5a500800000);
    first_cpu.nzcv = UINT32_C(0x30000000);
    assert(run_at(&first_runner, &first_cpu, CODE_PAGE).stop ==
            AARCH64_STEP_RETIRED);
    assert(first_cpu.x[2] == 8);
    assert(first_cpu.sp == UINT64_C(0x1000));
    assert(first_cpu.nzcv == UINT32_C(0x30000000));

    first_cpu.sp = UINT64_C(0x2000);
    first_cpu.x[2] = UINT64_C(0xa5a5a5a500000001);
    first_cpu.nzcv = UINT32_C(0xa0000000);
    assert(run_at(&first_runner, &first_cpu, CODE_PAGE).stop ==
            AARCH64_STEP_RETIRED);
    assert(first_cpu.x[2] == 31);
    assert(first_cpu.sp == UINT64_C(0x2000));
    assert(first_cpu.nzcv == UINT32_C(0xa0000000));

    first_cpu.sp = UINT64_C(0x1000);
    first_cpu.x[22] = UINT64_MAX;
    first_cpu.x[23] = 1;
    first_cpu.nzcv = UINT32_C(0x30000000);
    assert(run_at(&first_runner, &first_cpu, CODE_PAGE + 4).stop ==
            AARCH64_STEP_RETIRED);
    assert(first_cpu.x[22] == UINT64_C(0x1004));
    assert(first_cpu.sp == UINT64_C(0x1000));
    assert(first_cpu.x[23] == 1);
    assert(first_cpu.nzcv == 0);
    assert_stats(&first_runner, 1, 2, 2, 1);

    for (unsigned iteration = 0; iteration < 3; iteration++) {
        assert(run_at(&second_runner, &second_cpu, CODE_PAGE).stop ==
                AARCH64_STEP_RETIRED);
    }
    for (unsigned iteration = 0; iteration < 2; iteration++) {
        assert(run_at(&second_runner, &second_cpu, CODE_PAGE + 4).stop ==
                AARCH64_STEP_UNDEFINED);
    }
    second_cpu.sp = UINT64_C(0x2800);
    second_cpu.x[2] = UINT64_C(0xa5a5a5a500000000);
    second_cpu.nzcv = UINT32_C(0x90000000);
    assert(run_at(&second_runner, &second_cpu, CODE_PAGE + 8).stop ==
            AARCH64_STEP_RETIRED);
    assert(second_cpu.x[2] == 32);
    assert(second_cpu.sp == UINT64_C(0x2800));
    assert(second_cpu.nzcv == UINT32_C(0x90000000));

    second_cpu.sp = UINT64_C(0x1000);
    second_cpu.x[22] = UINT64_MAX;
    second_cpu.x[23] = 1;
    second_cpu.nzcv = UINT32_C(0x30000000);
    assert(run_at(&second_runner, &second_cpu, CODE_PAGE + 12).stop ==
            AARCH64_STEP_RETIRED);
    assert(second_cpu.x[22] == UINT64_C(0x1004));
    assert(second_cpu.sp == UINT64_C(0x1000));
    assert(second_cpu.x[23] == 1);
    assert(second_cpu.nzcv == 0);
    assert_stats(&second_runner, 3, 4, 1, 4);
    assert(run_at(&c_runner, &c_cpu, CODE_PAGE).stop ==
            AARCH64_STEP_RETIRED);

    aarch64_threaded_profile_reset_for_test();
    aarch64_threaded_profile_merge(&c_runner.threaded_cache);
    struct aarch64_threaded_profile_snapshot snapshot;
    aarch64_threaded_profile_snapshot(&snapshot);
    assert(snapshot.cache_hits == 0);
    assert(snapshot.cache_misses == 0);
    assert(snapshot.fast_dispatches == 0);
    assert(snapshot.c_fallbacks == 0);
    assert(snapshot.undefined_dispatches == 0);

    aarch64_threaded_profile_reset_for_test();
    aarch64_threaded_profile_merge(&first_runner.threaded_cache);
    aarch64_threaded_profile_merge(&second_runner.threaded_cache);
    aarch64_threaded_profile_merge(&c_runner.threaded_cache);
    aarch64_threaded_profile_snapshot(&snapshot);
    assert(snapshot.cache_hits == 4);
    assert(snapshot.cache_misses == 6);
    assert(snapshot.fast_dispatches == 3);
    assert(snapshot.c_fallbacks == 5);
    assert(snapshot.undefined_dispatches == 2);
    assert(snapshot.cache_hits + snapshot.cache_misses ==
            snapshot.fast_dispatches + snapshot.c_fallbacks +
            snapshot.undefined_dispatches);
    assert(snapshot.fallback_by_opcode[
            AARCH64_OP_STORE_REGISTER_OFFSET] == 0);
    assert(snapshot.fallback_by_opcode[
            AARCH64_OP_ANDS_SHIFTED_REGISTER] == 0);
    assert(snapshot.fallback_by_opcode[AARCH64_OP_STORE_IMM9] == 0);
    assert(snapshot.fallback_by_opcode[AARCH64_OP_ORR_IMMEDIATE] == 0);
    assert(snapshot.fallback_by_opcode[AARCH64_OP_EOR_IMMEDIATE] == 0);
    assert(snapshot.fallback_by_opcode[AARCH64_OP_SMADDL] == 0);
    assert(snapshot.fallback_by_opcode[AARCH64_OP_UMADDL] == 0);
    assert(snapshot.fallback_by_opcode[AARCH64_OP_CSINC] == 0);
    assert(snapshot.fallback_by_opcode[AARCH64_OP_REV32] == 0);
    assert(snapshot.fallback_by_opcode[AARCH64_OP_CLZ] == 0);
    assert(snapshot.fallback_by_opcode[
            AARCH64_OP_ADD_EXTENDED_REGISTER] == 0);
    assert(snapshot.fallback_by_opcode[
            AARCH64_OP_SUB_EXTENDED_REGISTER] == 0);
    assert(snapshot.fallback_by_opcode[
            AARCH64_OP_ADDS_EXTENDED_REGISTER] == 2);
    assert(snapshot.fallback_by_opcode[AARCH64_OP_LOAD_IMM9] == 0);
    assert(snapshot.fallback_by_opcode[AARCH64_OP_CSEL] == 0);
    assert(snapshot.fallback_by_opcode[
            AARCH64_OP_ANDS_IMMEDIATE] == 0);
    assert(snapshot.fallback_by_opcode[
            AARCH64_OP_STORE_SIMD_PAIR] == 0);
    assert(snapshot.fallback_by_opcode[
            AARCH64_OP_LOAD_SIMD_IMM12] == 0);
    assert(snapshot.fallback_by_opcode[
            AARCH64_OP_STORE_SIMD_IMM12] == 0);
    assert(snapshot.fallback_by_opcode[
            AARCH64_OP_STORE_SIMD_IMM9] == 0);
    assert(snapshot.fallback_by_opcode[AARCH64_OP_SBFM] == 0);
    assert(snapshot.fallback_by_opcode[AARCH64_OP_BFM] == 0);
    assert(snapshot.fallback_by_opcode[AARCH64_OP_LSRV] == 0);
    assert(snapshot.fallback_by_opcode[AARCH64_OP_ASRV] == 0);
    assert(snapshot.fallback_by_opcode[AARCH64_OP_MADD] == 0);
    assert(snapshot.fallback_by_opcode[AARCH64_OP_MSUB] == 0);
    assert(snapshot.fallback_by_opcode[AARCH64_OP_ADVSIMD_MOVI] == 0);
    assert(snapshot.fallback_by_opcode[AARCH64_OP_LOAD_SIMD_PAIR] == 0);
    assert(snapshot.fallback_by_opcode[AARCH64_OP_LSLV] == 0);
    assert(snapshot.fallback_by_opcode[AARCH64_OP_STORE_IMM12] == 0);
    assert(snapshot.fallback_by_opcode[AARCH64_OP_LOAD_PAIR] == 0);
    assert(snapshot.fallback_by_opcode[AARCH64_OP_UBFM] == 0);
    assert(snapshot.fallback_by_opcode[AARCH64_OP_STORE_PAIR] == 0);
    assert(snapshot.fallback_by_opcode[
            AARCH64_OP_SUBS_SHIFTED_REGISTER] == 0);
    assert(snapshot.fallback_by_opcode[
            AARCH64_OP_SUBS_EXTENDED_REGISTER] == 0);
    assert(snapshot.fallback_by_opcode[AARCH64_OP_ADR] == 0);
    assert(snapshot.fallback_by_opcode[AARCH64_OP_ADRP] == 0);
    assert(snapshot.fallback_by_opcode[
            AARCH64_OP_AND_IMMEDIATE] == 0);
    assert(snapshot.fallback_by_opcode[
            AARCH64_OP_SUB_SHIFTED_REGISTER] == 0);
    assert(snapshot.fallback_by_opcode[AARCH64_OP_CCMP] == 0);
    assert(snapshot.fallback_by_opcode[
            AARCH64_OP_ADDS_SHIFTED_REGISTER] == 3);
    assert(snapshot.representative_word_by_opcode[
            AARCH64_OP_STORE_REGISTER_OFFSET] == 0);
    assert(snapshot.representative_word_by_opcode[
            AARCH64_OP_ANDS_SHIFTED_REGISTER] == 0);
    assert(snapshot.representative_word_by_opcode[
            AARCH64_OP_STORE_IMM9] == 0);
    assert(snapshot.representative_word_by_opcode[
            AARCH64_OP_ORR_IMMEDIATE] == 0);
    assert(snapshot.representative_word_by_opcode[
            AARCH64_OP_EOR_IMMEDIATE] == 0);
    assert(snapshot.representative_word_by_opcode[AARCH64_OP_SMADDL] == 0);
    assert(snapshot.representative_word_by_opcode[
            AARCH64_OP_UMADDL] == 0);
    assert(snapshot.representative_word_by_opcode[
            AARCH64_OP_CSINC] == 0);
    assert(snapshot.representative_word_by_opcode[
            AARCH64_OP_REV32] == 0);
    assert(snapshot.representative_word_by_opcode[AARCH64_OP_CLZ] == 0);
    assert(snapshot.representative_word_by_opcode[
            AARCH64_OP_ADD_EXTENDED_REGISTER] == 0);
    assert(snapshot.representative_word_by_opcode[
            AARCH64_OP_SUB_EXTENDED_REGISTER] == 0);
    assert(snapshot.representative_word_by_opcode[
            AARCH64_OP_ADDS_EXTENDED_REGISTER] ==
            INSTRUCTION_ADDS_X22_SP_W23_SXTW_2);
    assert(snapshot.representative_word_by_opcode[
            AARCH64_OP_LOAD_IMM9] == 0);
    assert(snapshot.representative_word_by_opcode[
            AARCH64_OP_CSEL] == 0);
    assert(snapshot.representative_word_by_opcode[
            AARCH64_OP_ANDS_IMMEDIATE] == 0);
    assert(snapshot.representative_word_by_opcode[
            AARCH64_OP_STORE_SIMD_PAIR] == 0);
    assert(snapshot.representative_word_by_opcode[
            AARCH64_OP_LOAD_SIMD_IMM12] == 0);
    assert(snapshot.representative_word_by_opcode[
            AARCH64_OP_STORE_SIMD_IMM12] == 0);
    assert(snapshot.representative_word_by_opcode[
            AARCH64_OP_STORE_SIMD_IMM9] == 0);
    assert(snapshot.representative_word_by_opcode[AARCH64_OP_SBFM] == 0);
    assert(snapshot.representative_word_by_opcode[AARCH64_OP_BFM] == 0);
    assert(snapshot.representative_word_by_opcode[AARCH64_OP_LSRV] == 0);
    assert(snapshot.representative_word_by_opcode[AARCH64_OP_ASRV] == 0);
    assert(snapshot.representative_word_by_opcode[AARCH64_OP_MADD] == 0);
    assert(snapshot.representative_word_by_opcode[AARCH64_OP_MSUB] == 0);
    assert(snapshot.representative_word_by_opcode[
            AARCH64_OP_ADVSIMD_MOVI] == 0);
    assert(snapshot.representative_word_by_opcode[
            AARCH64_OP_LOAD_SIMD_PAIR] == 0);
    assert(snapshot.representative_word_by_opcode[AARCH64_OP_LSLV] == 0);
    assert(snapshot.representative_word_by_opcode[
            AARCH64_OP_STORE_IMM12] == 0);
    assert(snapshot.representative_word_by_opcode[
            AARCH64_OP_LOAD_PAIR] == 0);
    assert(snapshot.representative_word_by_opcode[AARCH64_OP_UBFM] == 0);
    assert(snapshot.representative_word_by_opcode[
            AARCH64_OP_STORE_PAIR] == 0);
    assert(snapshot.representative_word_by_opcode[
            AARCH64_OP_SUBS_SHIFTED_REGISTER] == 0);
    assert(snapshot.representative_word_by_opcode[
            AARCH64_OP_SUBS_EXTENDED_REGISTER] == 0);
    assert(snapshot.representative_word_by_opcode[AARCH64_OP_ADR] == 0);
    assert(snapshot.representative_word_by_opcode[
            AARCH64_OP_ADRP] == 0);
    assert(snapshot.representative_word_by_opcode[
            AARCH64_OP_AND_IMMEDIATE] == 0);
    assert(snapshot.representative_word_by_opcode[
            AARCH64_OP_SUB_SHIFTED_REGISTER] == 0);
    assert(snapshot.representative_word_by_opcode[
            AARCH64_OP_CCMP] == 0);
    assert(snapshot.representative_word_by_opcode[
            AARCH64_OP_ADDS_SHIFTED_REGISTER] == INSTRUCTION_ADDS_X3);

    qword_t fallback_sum = 0;
    for (enum aarch64_opcode opcode = 0;
            opcode < AARCH64_OP_COUNT; opcode++) {
        fallback_sum += snapshot.fallback_by_opcode[opcode];
    }
    assert(fallback_sum == snapshot.c_fallbacks);

    int profile_pipe[2];
    assert(pipe(profile_pipe) == 0);
    aarch64_threaded_profile_write_fd(profile_pipe[1]);
    assert(close(profile_pipe[1]) == 0);
    char output[2048];
    size_t output_length = 0;
    ssize_t bytes_read;
    while ((bytes_read = read(profile_pipe[0],
            output + output_length,
            sizeof(output) - output_length - 1)) > 0) {
        output_length += (size_t) bytes_read;
    }
    assert(bytes_read == 0);
    assert(close(profile_pipe[0]) == 0);
    output[output_length] = '\0';

    const char *version_line = strstr(output,
            "AARCH64_THREADED_PROFILE\tversion\t1\n");
    const char *backend_line = strstr(output,
            "AARCH64_THREADED_PROFILE\tbackend\tthreaded\n");
    const char *totals_line = strstr(output,
            "AARCH64_THREADED_PROFILE\ttotals"
            "\tcache_hits\t4\tcache_misses\t6"
            "\tfast_dispatches\t3\tc_fallbacks\t5"
            "\tundefined_dispatches\t2\n");
    char adds_line[160];
    char adds_extended_line[160];
    assert(snprintf(adds_line, sizeof(adds_line),
            "AARCH64_THREADED_PROFILE\topcode\t%u"
            "\tcount\t3\trepresentative_word\t0xab000043\n",
            (unsigned) AARCH64_OP_ADDS_SHIFTED_REGISTER) > 0);
    assert(snprintf(adds_extended_line, sizeof(adds_extended_line),
            "AARCH64_THREADED_PROFILE\topcode\t%u"
            "\tcount\t2\trepresentative_word\t0xab37cbf6\n",
            (unsigned) AARCH64_OP_ADDS_EXTENDED_REGISTER) > 0);
    const char *adds_output = strstr(output, adds_line);
    const char *adds_extended_output =
            strstr(output, adds_extended_line);
    assert(version_line == output);
    assert(backend_line != NULL);
    assert(totals_line != NULL);
    assert(adds_output != NULL);
    assert(adds_extended_output != NULL);
    assert(backend_line > version_line);
    assert(totals_line > backend_line);
    assert(adds_output > totals_line);
    assert(adds_extended_output > adds_output);
    unsigned output_lines = 0;
    for (const char *cursor = output; *cursor != '\0'; cursor++) {
        if (*cursor == '\n')
            output_lines++;
    }
    assert(output_lines == 5);

    aarch64_threaded_profile_reset_for_test();
    aarch64_threaded_profile_snapshot(&snapshot);
    assert(snapshot.cache_hits == 0);
    assert(snapshot.cache_misses == 0);
    assert(snapshot.fast_dispatches == 0);
    assert(snapshot.c_fallbacks == 0);
    assert(snapshot.undefined_dispatches == 0);
    for (enum aarch64_opcode opcode = 0;
            opcode < AARCH64_OP_COUNT; opcode++) {
        assert(snapshot.fallback_by_opcode[opcode] == 0);
        assert(snapshot.representative_word_by_opcode[opcode] == 0);
    }
}
#endif
#endif

int main(void) {
    test_backend_selection();
#if defined(__aarch64__)
    test_fast_data_processing_differential();
    test_fast_add_shifted_differential();
    test_fast_add_extended_differential();
    test_fast_sub_extended_differential();
    test_fast_subs_extended_differential();
    test_sub_extended_invalid_shifts();
    test_extended_sibling_fallback();
    test_fast_sub_shifted_differential();
    test_fast_ccmp_differential();
    test_ccmn_sibling_fallback();
    test_fast_csel_differential();
    test_fast_csinc_differential();
    test_conditional_select_sibling_fallback();
    test_fast_rev32_differential();
    test_fast_clz_differential();
    test_data_processing_1source_sibling_fallback();
    test_fast_subs_shifted_differential();
    test_add_sub_shifted_sibling_fallback();
    test_fast_adrp_differential();
    test_fast_adr_differential();
    test_fast_and_immediate_differential();
    test_fast_ands_immediate_differential();
    test_fast_orr_immediate_differential();
    test_fast_eor_immediate_differential();
    test_fast_and_shifted_differential();
    test_fast_ubfm_differential();
    test_fast_sbfm_differential();
    test_fast_bfm_differential();
    test_fast_lsrv_differential();
    test_fast_asrv_differential();
    test_fast_lslv_differential();
    test_fast_madd_differential();
    test_fast_msub_differential();
    test_fast_umaddl_differential();
    test_fast_smaddl_differential();
    test_multiply_add_sibling_fallback();
    test_data_processing_2source_sibling_fallback();
    test_fast_extract_differential();
    test_extract_invalid_encodings();
    test_fast_orr_shifted_differential();
    test_fast_eor_shifted_differential();
    test_fast_ands_shifted_differential();
    test_fast_load_imm12_differential();
    test_fast_load_imm12_faults();
    test_fast_load_imm9_differential();
    test_fast_load_imm9_aliases();
    test_fast_load_imm9_faults();
    test_fast_store_imm9_differential();
    test_fast_store_imm9_aliases();
    test_fast_store_imm9_faults();
    test_fast_store_imm9_exclusive_invalidation();
    test_fast_store_simd_imm9_differential();
    test_fast_store_simd_imm9_faults();
    test_fast_store_simd_imm9_exclusive_invalidation();
    test_imm9_sibling_fallback();
    test_fast_store_imm12_differential();
    test_fast_store_imm12_faults();
    test_fast_store_imm12_exclusive_invalidation();
    test_fast_store_simd_imm12_differential();
    test_fast_store_simd_imm12_faults();
    test_fast_store_simd_imm12_exclusive_invalidation();
    test_fast_load_simd_imm12_differential();
    test_fast_load_simd_imm12_faults();
    test_fast_load_simd_imm12_preserves_exclusive();
    test_fast_store_pair_differential();
    test_fast_store_pair_faults();
    test_fast_store_pair_exclusive_invalidation();
    test_fast_store_simd_pair_differential();
    test_fast_store_simd_pair_faults();
    test_fast_store_simd_pair_exclusive_invalidation();
    test_fast_advsimd_movi_differential();
    test_advsimd_movi_sibling_fallback();
    test_fast_load_simd_pair_differential();
    test_fast_load_simd_pair_faults();
    test_fast_load_simd_pair_preserves_exclusive();
    test_fast_load_pair_differential();
    test_fast_load_pair_faults();
    test_fast_load_pair_preserves_exclusive();
    test_load_pair_sibling_fallback();
    test_fast_load_register_offset_differential();
    test_fast_load_register_offset_aliases();
    test_fast_load_register_offset_faults();
    test_fast_store_register_offset_differential();
    test_fast_store_register_offset_boundaries();
    test_fast_store_register_offset_faults();
    test_fast_store_register_offset_exclusive_invalidation();
    test_simd_register_offset_sibling_fallback();
    test_fast_branch_differential();
    test_fast_svc_differential();
    test_fast_dispatch_structure();
    test_product_c_fallback();
    test_c_and_threaded_differential();
    test_cache_keys_and_collision();
    test_rwx_self_modifying_code();
    test_mapping_invalidation();
    test_shared_space_invalidation();
#if ISH_AARCH64_THREADED_PROFILE
    test_profile_aggregation();
#endif
#endif
    return 0;
}
