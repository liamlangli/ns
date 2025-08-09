#pragma once

#include "ns_type.h"

typedef u8 ns_reg;

typedef struct {
    ns_reg in[32];
    ns_reg out[8];
    ns_reg clobbered[8];
    ns_reg preserved[8];
} ns_abi;

typedef struct {
    ns_reg gpr[32];
    ns_abi call;
    ns_abi extern_call;
    ns_abi syscall;
    ns_reg division_remainder[2];
    ns_reg divisor_restricted[2];
    ns_reg shift_clobbered[2];
    ns_reg shift_restricted[2];
} ns_cpu;

// == instructions ==
typedef struct {
    ns_reg dst;
    ns_reg src;
} ns_inst_rr;

typedef struct {
    ns_reg dst;
    i32 imm;
} ns_inst_rn;

typedef struct {
    ns_reg dst;
    ns_reg src;
    ns_reg opnd;
} ns_inst_rrr;

typedef struct {
    ns_reg dst;
    ns_reg src;
    i32 imm;
} ns_inst_rrn;

typedef ns_inst_rrr ns_inst_add;
typedef ns_inst_rrr ns_inst_and;
typedef ns_inst_rrn ns_inst_add_n;
// call
// call_extern
// call_extern_start
// call_extern_end
typedef ns_inst_rr  ns_inst_cmp;
typedef ns_inst_rn  ns_inst_cmp_n;
typedef ns_inst_rrr ns_inst_div;
// jump
// label
typedef ns_inst_rrr ns_inst_mod;
typedef ns_inst_rr  ns_inst_mv;
// mv label
typedef ns_inst_rn  ns_inst_mv_n;
typedef ns_inst_rrr ns_inst_mul;
typedef ns_inst_rr  ns_inst_neg;
typedef ns_inst_rrr ns_inst_or;
// pop
// push
// return
typedef ns_inst_rrr ns_inst_shl;
typedef ns_inst_rrr ns_inst_shr;
typedef ns_inst_rr  ns_inst_sub;
typedef ns_inst_rrn ns_inst_sub_n;
// stack frame start
// stack frame end
// syscall
typedef ns_inst_rrr ns_inst_xor;

typedef union {
    ns_inst_rr  rr;
    ns_inst_rn  rn;
    ns_inst_rrr rrr;
    ns_inst_rrn rrn;
} ns_inst;

// == assembler ==
typedef struct  {
    ns_str name;
    ns_str *fns;
} ns_asm_lib;

typedef enum {
    NS_ARCH_UNKNOWN = 0,
    NS_ARCH_X64,
    NS_ARCH_X86,
    NS_ARCH_ARM,
    NS_ARCH_AARCH64
} ns_arch;

typedef enum {
    NS_OS_UNKNOWN = 0,
    NS_OS_LINUX,
    NS_OS_DARWIN,
    NS_OS_WINDOWS
} ns_os;

typedef struct {
    ns_arch arch;
    ns_os os;
} ns_asm_target;

typedef enum {
    NS_EXE_FMT_UNKNOWN = 0,
    NS_EXE_FMT_PE,
    NS_EXE_FMT_ELF,
    NS_EXE_FMT_MACHO
} ns_exe_fmt;

typedef struct {
    ns_asm_target target;
    ns_str *files;
    ns_inst *insts;
} ns_assembler;

void ns_asm_get_current_target(ns_asm_target *target);
ns_str ns_os_str(ns_os os);
ns_str ns_arch_str(ns_arch arch);