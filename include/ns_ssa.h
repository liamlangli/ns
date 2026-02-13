#pragma once

#include "ns_ast.h"
#include "ns_type.h"

typedef enum {
    NS_SSA_OP_UNKNOWN = 0,
    NS_SSA_OP_UNDEF,
    NS_SSA_OP_CONST,
    NS_SSA_OP_PARAM,
    NS_SSA_OP_COPY,
    NS_SSA_OP_PHI,
    NS_SSA_OP_CAST,
    NS_SSA_OP_CALL,
    NS_SSA_OP_ARG,
    NS_SSA_OP_MEMBER,
    NS_SSA_OP_INDEX,
    NS_SSA_OP_NEG,
    NS_SSA_OP_NOT,
    NS_SSA_OP_ADD,
    NS_SSA_OP_SUB,
    NS_SSA_OP_MUL,
    NS_SSA_OP_DIV,
    NS_SSA_OP_MOD,
    NS_SSA_OP_SHL,
    NS_SSA_OP_SHR,
    NS_SSA_OP_BAND,
    NS_SSA_OP_BOR,
    NS_SSA_OP_BXOR,
    NS_SSA_OP_AND,
    NS_SSA_OP_OR,
    NS_SSA_OP_EQ,
    NS_SSA_OP_NE,
    NS_SSA_OP_LT,
    NS_SSA_OP_LE,
    NS_SSA_OP_GT,
    NS_SSA_OP_GE,
    NS_SSA_OP_ASSERT,
    NS_SSA_OP_BR,
    NS_SSA_OP_JMP,
    NS_SSA_OP_RET,
    NS_SSA_OP_TRAP,
} ns_ssa_op;

typedef struct ns_ssa_inst {
    ns_ssa_op op;
    i32 dst;
    i32 a;
    i32 b;
    i32 c;
    i32 target0;
    i32 target1;
    i32 ast;
    ns_type type;
    ns_str name;
    ns_token_t token;
} ns_ssa_inst;

typedef struct ns_ssa_block {
    i32 id;
    i32 ast;
    i32 *insts;
    i32 *preds;
    i32 *succs;
    ns_bool terminated;
} ns_ssa_block;

typedef struct ns_ssa_fn {
    ns_str name;
    i32 ast;
    i32 entry;
    ns_ssa_block *blocks;
    ns_ssa_inst *insts;
} ns_ssa_fn;

typedef struct ns_ssa_module {
    ns_ssa_fn *fns;
} ns_ssa_module;

ns_str ns_ssa_op_to_string(ns_ssa_op op);
ns_return_ptr ns_ssa_build(ns_ast_ctx *ctx);
void ns_ssa_print(ns_ssa_module *m);
void ns_ssa_module_free(ns_ssa_module *m);
