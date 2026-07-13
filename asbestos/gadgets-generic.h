#include "cpu-offsets.h"

.if TLB_ENTRY_size != 16
    .error "TLB 缓存项宽度与 gadget 步长不一致"
.endif
.if PAGE_BITS != 12
    .error "宿主 gadget 仅支持 4 KiB guest 页"
.endif
.if TLB_BITS != 10
    .error "宿主 gadget 要求 10 位 TLB 索引"
.endif

#define ifin(thing, ...) _ifin(thing, __COUNTER__, __VA_ARGS__)
#define _ifin(thing, line, ...) __ifin(thing, line, __VA_ARGS__)
#define __ifin(thing, line, ...) irp da_op##line, __VA_ARGS__ N .ifc thing,\da_op##line
#define endifin endif N .endr

# sync with enum reg
#define REG_LIST reg_a,reg_c,reg_d,reg_b,reg_sp,reg_bp,reg_si,reg_di
# sync with enum arg
#define GADGET_LIST REG_LIST,imm,mem,addr,gs
# sync with enum size
#define SIZE_LIST 8,16,32

# darwin/linux compatibility
.macro .pushsection_rodata
#if __APPLE__
    .pushsection __DATA,__const
#else
    .pushsection .data.rel.ro
#endif
.endm
.macro .pushsection_bullshit
#if __APPLE__
    .pushsection __TEXT,__text_bullshit,regular,pure_instructions
#else
    .pushsection .text.bullshit
#endif
.endm

// Gadget 表保存的是宿主函数指针，而不是固定宽度的 fiber_cell_t。
#if __SIZEOF_POINTER__ == 4
.macro .align_host_pointer
    .p2align 2
.endm
.macro .host_pointer value
    .long \value
.endm
#elif __SIZEOF_POINTER__ == 8
.macro .align_host_pointer
    .p2align 3
.endm
.macro .host_pointer value
    .quad \value
.endm
#else
#error "unsupported host pointer size"
#endif

#if __APPLE__
#define NAME(x) _##x
#else
#define NAME(x) x
#endif
.macro .global.name name
    .global NAME(\name)
    NAME(\name)\():
.endm

.macro .type_compat type:vararg
#if !__APPLE__
    .type \type
#endif
.endm

# an array of gadgets
.macro _gadget_array_start name
    .pushsection_rodata
    .align_host_pointer
    .type_compat \name\()_gadgets,@object
    .global.name \name\()_gadgets
.endm

.macro gadgets type, list:vararg
    .irp arg, \list
        .ifndef NAME(gadget_\type\()_\arg)
            .set NAME(gadget_\type\()_\arg), 0
        .endif
        .host_pointer NAME(gadget_\type\()_\arg)
    .endr
.endm

.macro .gadget_list type, list:vararg
    _gadget_array_start \type
        gadgets \type, \list
    .popsection
.endm

.macro .gadget_list_size type, list:vararg
    _gadget_array_start \type
        # sync with enum size
        gadgets \type\()8, \list
        gadgets \type\()16, \list
        gadgets \type\()32, \list
        gadgets \type\()64, \list
        gadgets \type\()80, \list
    .popsection
.endm

.macro .gadget_array type
    .gadget_list_size \type, GADGET_LIST
.endm

# jfc
# https://github.com/llvm-mirror/llvm/blob/release_80/lib/Target/AArch64/MCTargetDesc/AArch64MCAsmInfo.cpp#L41
# https://bugs.llvm.org/show_bug.cgi?id=39010#c4
#if defined(__APPLE__) && defined(__arm64__)
#define N %%
#else
#define N ;
#endif

# vim: ft=gas
