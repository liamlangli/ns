#pragma once

/*
 * ns_cpu instruction set, shared by the code generator (src/ns_cpu_gen.c) and
 * the loader/interpreter (src/ns_cpu.c). See doc/cpu.md.
 *
 * Code is a stream of 16-bit units. An instruction is its opcode followed by
 * the operands its format string lists, one character each:
 *
 *   d  destination register        r  source register
 *   t  branch target, a code offset in units (u32, two units, low first)
 *   w  32-bit immediate (two units, low first; signed where it is an offset)
 *   f  function index              g  global index
 *   x  runtime helper index        y  foreign call site index
 *   k  indirect call cache slot    l  source location (u32, two units)
 *   n  count, then that many source registers
 *
 * `n` is always the last operand. Every register is 64 bits wide; an f32 lives
 * in the low half of one. Registers are numbered per frame: the parameters
 * first, then values, then constants the loader writes on entry.
 */

#define NS_CPU_OPS(X) \
    X(NOP, "") \
    X(MOV, "dr") \
    X(ZERO, "d") \
    X(ADD, "drr") X(SUB, "drr") X(MUL, "drr") \
    X(DIVS, "drr") X(DIVU, "drr") X(MODS, "drr") X(MODU, "drr") \
    X(AND, "drr") X(OR, "drr") X(XOR, "drr") \
    X(SHL, "drr") X(SHRS, "drr") X(SHRU, "drr") \
    X(NEG, "dr") X(NOT, "dr") \
    X(ADDI, "drw") \
    X(FADD64, "drr") X(FSUB64, "drr") X(FMUL64, "drr") X(FDIV64, "drr") \
    X(FADD32, "drr") X(FSUB32, "drr") X(FMUL32, "drr") X(FDIV32, "drr") \
    X(FNEG64, "dr") X(FNEG32, "dr") \
    X(EQ, "drr") X(NE, "drr") X(LTS, "drr") X(LES, "drr") X(LTU, "drr") X(LEU, "drr") \
    X(FEQ64, "drr") X(FNE64, "drr") X(FLT64, "drr") X(FLE64, "drr") \
    X(FEQ32, "drr") X(FNE32, "drr") X(FLT32, "drr") X(FLE32, "drr") \
    X(JMP, "t") \
    X(BNZ, "rtt") \
    X(BEQ, "rrtt") X(BNE, "rrtt") X(BLTS, "rrtt") X(BLES, "rrtt") X(BLTU, "rrtt") X(BLEU, "rrtt") \
    X(BFEQ64, "rrtt") X(BFNE64, "rrtt") X(BFLT64, "rrtt") X(BFLE64, "rrtt") \
    X(BFEQ32, "rrtt") X(BFNE32, "rrtt") X(BFLT32, "rrtt") X(BFLE32, "rrtt") \
    X(SEXT8, "dr") X(ZEXT8, "dr") X(SEXT16, "dr") X(ZEXT16, "dr") X(SEXT32, "dr") X(ZEXT32, "dr") \
    X(I2F64, "dr") X(I2F32, "dr") X(F64TOI, "dr") X(F32TOI, "dr") X(F32TO64, "dr") X(F64TO32, "dr") \
    X(LD8S, "drw") X(LD8U, "drw") X(LD16S, "drw") X(LD16U, "drw") X(LD32, "drw") X(LD64, "drw") \
    X(LDX64, "drr") \
    X(ST8, "rwr") X(ST16, "rwr") X(ST32, "rwr") X(ST64, "rwr") \
    X(STCOPY, "rwrw") \
    X(AIDX8S, "drr") X(AIDX8U, "drr") X(AIDX16S, "drr") X(AIDX16U, "drr") X(AIDX32, "drr") X(AIDX64, "drr") \
    X(ASLOT, "drrw") \
    X(AST8, "rrr") X(AST16, "rrr") X(AST32, "rrr") X(AST64, "rrr") \
    X(ASTCOPY, "rrrw") \
    X(GGET, "dg") X(GSET, "gr") \
    X(CALL, "dfn") \
    X(CALLI, "drkn") \
    X(RT, "dxn") \
    X(FFI, "dyn") \
    X(FNADDR, "df") \
    X(RET, "r") \
    X(ASSERT, "rl") \
    X(TRAP, "l")

typedef enum ns_cpu_op {
#define NS_CPU_OP_ENUM(name, fmt) NS_CPU_##name,
    NS_CPU_OPS(NS_CPU_OP_ENUM)
#undef NS_CPU_OP_ENUM
    NS_CPU_OP_COUNT
} ns_cpu_op;

// How a constant register is filled when a frame is entered.
typedef enum ns_cpu_const_kind {
    NS_CPU_CONST_BITS = 0,   // the 64-bit value itself
    NS_CPU_CONST_STRING = 1, // the address of string literal `value` of the pool
} ns_cpu_const_kind;

// Argument and result classes of a foreign call site: how a register crosses
// into the host ABI and back.
typedef enum ns_cpu_ffi_kind {
    NS_CPU_FFI_VOID = 0,
    NS_CPU_FFI_I8,
    NS_CPU_FFI_U8,
    NS_CPU_FFI_I16,
    NS_CPU_FFI_U16,
    NS_CPU_FFI_I32,
    NS_CPU_FFI_U32,
    NS_CPU_FFI_I64,
    NS_CPU_FFI_F32,
    NS_CPU_FFI_F64,
    NS_CPU_FFI_STR,   // str: char * on the host side
    NS_CPU_FFI_REF,   // ref T: host pointer to the referent
    NS_CPU_FFI_ARRAY, // [T]: host pointer to the payload
    NS_CPU_FFI_KIND_COUNT
} ns_cpu_ffi_kind;

#define NS_CPU_MAX_REGS 0xfffe
#define NS_CPU_NO_FN 0xffffffffu
