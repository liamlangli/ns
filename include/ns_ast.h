#pragma once

#include "ns_token.h"
#include "ns_type.h"

#define ns_ast_error(c, t, m, ...) ns_error(t, "\n[%s:%d:%d]: " m "\n", c->filename.data, c->token.line, c->f - c->token.line_start, ##__VA_ARGS__ );

typedef enum {
    NS_AST_UNKNOWN = 0,
    NS_AST_PROGRAM,

    NS_AST_TYPE_LABEL,

    NS_AST_ARG_DEF,
    NS_AST_FN_DEF,
    NS_AST_OP_FN_DEF,
    NS_AST_VAR_DEF,
    NS_AST_STRUCT_DEF,
    NS_AST_FIELD_DEF,
    NS_AST_TYPE_DEF,
    NS_AST_ENUM_DEF,
    NS_AST_ENUM_MEMBER,

    NS_AST_EXPR,
    NS_AST_STR_FMT_EXPR,
    NS_AST_PRIMARY_EXPR,
    NS_AST_BINARY_EXPR,
    NS_AST_MEMBER_EXPR,
    NS_AST_UNARY_EXPR,
    NS_AST_CALL_EXPR,
    NS_AST_INDEX_EXPR,
    NS_AST_CAST_EXPR,
    NS_AST_GEN_EXPR,
    NS_AST_DESIG_EXPR,
    NS_AST_ARRAY_EXPR,
    NS_AST_BLOCK_EXPR,

    NS_AST_ASSERT_STMT,
    NS_AST_IF_STMT,
    NS_AST_USE_STMT,
    NS_AST_MODULE_STMT,
    NS_AST_TYPE_ALIAS_STMT,
    NS_AST_FOR_STMT,
    NS_AST_LOOP_STMT,
    NS_AST_JUMP_STMT,
    NS_AST_LABELED_STMT,
    NS_AST_COMPOUND_STMT,
    NS_AST_TYPEDEF_STMT,

    NS_AST_COUNT,
} ns_ast_type;

typedef struct ns_ast_t ns_ast_t;

typedef struct ns_ast_state {
    i32 f, l, o; // buffer offset, line, line offset
} ns_ast_state;

typedef enum {
    NS_SYM_CACHE_NONE = 0,
    NS_SYM_CACHE_LOCAL,  // index is a frame-relative symbol_stack offset
    NS_SYM_CACHE_GLOBAL, // index is a vm->symbols index, valid while gen matches
} ns_sym_cache_kind;

typedef struct ns_sym_cache {
    i32 kind;  // ns_sym_cache_kind
    i32 index;
    u32 gen;   // vm->symbol_gen at fill time (GLOBAL only)
} ns_sym_cache;

// Maps one line of a merged translation unit back to its source file + line.
typedef struct ns_line_loc {
    ns_str f;
    i32 l;
} ns_line_loc;

typedef struct ns_ast_type_label {
    ns_token_t name;
    i32 arg_count;
    i32 ret;
    i32 elem;
    i32 key;
    i32 val;
    ns_bool is_array: 2;
    ns_bool is_dict: 2;
    ns_bool is_set: 2;
    ns_bool is_ref: 2;
    ns_bool is_mut: 2;
    ns_bool is_fn: 2;
} ns_ast_type_label;

typedef struct ns_ast_typedef_stmt {
    ns_token_t name;
    i32 type;
} ns_ast_typedef_stmt;

typedef struct ns_ast_str_fmt {
    ns_str fmt;
    i32 expr_count;
} ns_ast_str_fmt;

typedef struct ns_ast_arg {
    ns_token_t name;
    i32 type;
    i32 val; // default value expr
} ns_ast_arg;

typedef struct ns_ast_fn_def {
    i32 ret;
    i32 body;
    ns_token_t name;
    ns_bool is_ref;
    ns_fn_type type;
    i32 arg_count;
    i32 arg_required;
} ns_ast_fn_def;

typedef struct ns_ast_ops_fn_def {
    i32 ret;
    i32 body;
    ns_token_t ops;
    ns_bool is_ref;
    ns_fn_type type;
    i32 left;
    i32 right;
    i32 arg_required;
} ns_ast_ops_fn_def;

typedef struct ns_ast_var_def {
    ns_token_t name;
    i32 expr;
    i32 type;
    i32 type_size;
    ns_bool is_ref;
} ns_ast_var_def;

typedef struct ns_ast_struct_def {
    ns_token_t name;
    i32 count;
} ns_ast_struct_def;

typedef struct ns_ast_struct_field {
    ns_token_t name;
    i32 expr;
    struct {
        i32 index;
        u64 offset;
    } rt;
} ns_ast_struct_field;

typedef struct ns_ast_type_def {
    ns_token_t name;
    i32 type;       // first type label node
    i32 count;      // number of type labels; >1 means a union type (A | B | ...)
} ns_ast_type_def;

typedef struct ns_ast_enum_def {
    ns_token_t name;
    ns_token_t underlying;
    i32 count;
} ns_ast_enum_def;

typedef struct ns_ast_enum_member {
    ns_token_t name;
    i32 expr; // 0 means the value is inferred from the preceding member
} ns_ast_enum_member;

typedef struct ns_ast_binary_expr {
    i32 left;
    i32 right;
    ns_token_t op;
} ns_ast_binary_expr;

typedef struct ns_ast_cast_expr {
    i32 expr;
    ns_token_t type;
} ns_ast_cast_expr;

typedef struct ns_ast_primary_expr {
    ns_token_t token;
    ns_type t; // literal type resolved at vm-parse (suffix or expected-type adoption)
    struct {
        ns_sym_cache cache; // identifier lookup cache, filled at eval
    } rt;
} ns_ast_primary_expr;

typedef struct ns_ast_unary_expr {
    ns_token_t op;
    i32 expr;
} ns_ast_unary_expr;

typedef struct ns_ast_expr {
    i32 body;
    ns_type type;
    ns_bool atomic: 2; // ignore parenthesis
} ns_ast_expr;

typedef struct ns_ast_member_expr {
    i32 left;
    i32 right; // field/primary node (or chained member). Kept separate from
               // ns_ast_t.next so argument-list linking cannot clobber it.
} ns_ast_member_expr;

typedef struct ns_ast_call_expr {
    i32 callee;
    i32 arg_count;
} ns_ast_call_expr;

typedef struct ns_ast_index_expr {
    i32 table;
    i32 expr;
} ns_ast_index_expr;

typedef struct ns_ast_gen_expr {
    ns_token_t name;
    i32 from;
    i32 to;
    ns_bool range;
    struct {
        i32 next_fn;     // symbol index of the resolved `next(it): bool` fn, filled at vm-parse
        i32 value_field; // index of the subject struct's `value` field
    } rt; // iterator protocol resolution for a struct subject (non-range)
} ns_ast_gen_expr;

typedef struct ns_ast_use_stmt {
    ns_token_t lib;
} ns_ast_use_stmt;

typedef struct ns_ast_module_stmt {
    ns_token_t name;
} ns_ast_module_stmt;

typedef struct ns_ast_if_stmt {
    i32 condition;
    i32 body;
    i32 else_body;
} ns_ast_if_stmt;

typedef struct ns_ast_for_stmt {
    i32 generator;
    i32 body;
} ns_ast_for_stmt;

typedef struct ns_ast_loop_stmt {
    i32 condition;
    i32 body;
    ns_bool do_first;
} ns_ast_loop_stmt;

typedef struct ns_ast_jump_stmt {
    ns_token_t label;
    i32 expr;
} ns_ast_jump_stmt;

typedef struct ns_ast_labeled_stmt {
    ns_token_t label;
    i32 condition;
    i32 stmt;
} ns_ast_labeled_stmt;

typedef struct ns_ast_compound_stmt {
    i32 count;
} ns_ast_compound_stmt;

typedef struct ns_ast_desig_expr {
    ns_token_t name;
    i32 count;
    ns_bool positional;
    struct {
        ns_sym_cache cache; // struct symbol lookup cache, filled at eval
    } rt;
} ns_ast_desig_expr;

typedef struct ns_ast_array_expr {
    i32 type;
    i32 count_expr;
    i32 elem_count;
    ns_bool literal;
    ns_type rt;
} ns_ast_array_expr;

typedef struct ns_ast_block_expr {
    i32 ret;
    i32 body;
    i32 arg_count;
    struct {
        i32 index;
    } rt;
} ns_ast_block_expr;

typedef struct {
    i32 expr;
} ns_ast_assert_stmt;

typedef struct ns_ast_t {
    ns_ast_type type;
    i32 next; // -1 mean null ref
    ns_ast_state state;
    union {
        ns_ast_type_label type_label;
        ns_ast_str_fmt str_fmt;

        ns_ast_arg arg;
        ns_ast_fn_def fn_def;
        ns_ast_ops_fn_def ops_fn_def;
        ns_ast_var_def var_def;
        ns_ast_struct_def struct_def;
        ns_ast_type_def type_def;
        ns_ast_enum_def enum_def;
        ns_ast_enum_member enum_member;
        ns_ast_struct_field field_def;

        ns_ast_expr expr;
        ns_ast_binary_expr binary_expr;
        ns_ast_call_expr call_expr;
        ns_ast_index_expr index_expr;
        ns_ast_cast_expr cast_expr;
        ns_ast_member_expr member_expr;
        ns_ast_primary_expr primary_expr;
        ns_ast_unary_expr unary_expr;
        ns_ast_gen_expr gen_expr;
        ns_ast_desig_expr desig_expr;
        ns_ast_array_expr array_expr;
        ns_ast_block_expr block_expr;

        ns_ast_assert_stmt assert_stmt;
        ns_ast_use_stmt use_stmt;
        ns_ast_module_stmt module_stmt;
        ns_ast_if_stmt if_stmt;
        ns_ast_loop_stmt loop_stmt;
        ns_ast_for_stmt for_stmt;
        ns_ast_jump_stmt jump_stmt;
        ns_ast_labeled_stmt label_stmt;
        ns_ast_compound_stmt compound_stmt;
    };
} ns_ast_t;

#define ns_ast_nil (ns_ast_t){.type = NS_AST_UNKNOWN}

typedef struct ns_ast_expr_scope {
    i32 stack_top, op_top, pre;
} ns_ast_expr_scope;

typedef struct as_parse_context_t {
    i32 f, last_f;
    i32 *sections;
    i32 section_begin, section_end;
    i32 current;

    ns_ast_t *nodes;

    // stack based expr parsing
    i32 *stack;
    i32 *op_stack;
    i32 *expr_stack;
    ns_ast_expr_scope *scopes;
    // At this expression nesting depth, an opening brace begins a control-flow
    // body rather than a positional designated expression. Zero disables it.
    i32 block_expr_depth;

    ns_token_t token, last_token;
    ns_str source;
    ns_str filename;

    // Optional source map for linked/merged translation units (see
    // ns_project_link). When set, it maps each 1-based line of `source` back to
    // its originating file and line so diagnostics point at the real location
    // instead of the merged entry file. NULL for single-file parses.
    ns_line_loc *line_map;
} ns_ast_ctx;

// Resolve a merged-source (line, offset) back to its real file location via the
// context's line map. The column offset is preserved verbatim because the
// linker copies source lines unchanged. Falls back to the context filename and
// raw line when there is no map (single-file parse) or the line is out of range.
static inline ns_code_loc ns_ctx_loc(ns_ast_ctx *ctx, i32 line, i32 off) {
    if (ctx->line_map != ns_null && line >= 1 && line <= (i32)ns_array_length(ctx->line_map)) {
        ns_line_loc e = ctx->line_map[line - 1];
        return (ns_code_loc){.f = e.f, .l = e.l, .o = off};
    }
    return (ns_code_loc){.f = ctx->filename, .l = line, .o = off};
}

#define ns_ast_code_loc(ctx) ns_ctx_loc((ctx), (ctx)->token.line, (ctx)->f - (ctx)->token.line_start)
#define ns_ast_state_loc(ctx, s) ns_ctx_loc((ctx), (s).l, (s).o)

ns_str ns_ast_type_to_string(ns_ast_type type);

// token fn
ns_bool ns_parse_next_token(ns_ast_ctx *ctx); // skip space
ns_bool ns_token_can_be_type(ns_token_type t);
ns_bool ns_token_require(ns_ast_ctx *ctx, ns_token_type t);
ns_bool ns_token_require_type(ns_ast_ctx *ctx);
ns_bool ns_token_skip_eol(ns_ast_ctx *ctx);

// node fn
void ns_restore_state(ns_ast_ctx *ctx, ns_ast_state state);
ns_ast_state ns_save_state(ns_ast_ctx *ctx);
i32 ns_ast_push(ns_ast_ctx *ctx, ns_ast_t n);

// primary fn
ns_bool ns_parse_identifier(ns_ast_ctx *ctx);

// external fn
ns_return_bool ns_parse_fn_define(ns_ast_ctx *ctx);
ns_return_bool ns_parse_op_fn_define(ns_ast_ctx *ctx);
ns_return_bool ns_parse_var_define(ns_ast_ctx *ctx);
ns_return_bool ns_parse_struct_def(ns_ast_ctx *ctx);
ns_return_bool ns_parse_enum_def(ns_ast_ctx *ctx);
ns_return_bool ns_parse_type_label(ns_ast_ctx *ctx);

// stmt fn
ns_return_bool ns_parse_global_define(ns_ast_ctx *ctx);
ns_return_bool ns_parse_stmt(ns_ast_ctx *ctx);
ns_return_bool ns_parse_compound_stmt(ns_ast_ctx *ctx, ns_bool brace_required);

// arg fn
ns_return_bool ns_parse_arg(ns_ast_ctx *ctx, ns_bool type_required);

// expr fn
ns_return_bool ns_parse_designated_expr(ns_ast_ctx *ctx, i32 st);
ns_return_bool ns_parse_unary_expr(ns_ast_ctx *ctx);
ns_return_bool ns_parse_unary_operand(ns_ast_ctx *ctx);
ns_return_bool ns_parse_gen_expr(ns_ast_ctx *ctx);
ns_return_bool ns_parse_primary_expr(ns_ast_ctx *ctx);
ns_return_bool ns_parse_postfix_expr(ns_ast_ctx *ctx, i32 operand);
ns_return_bool ns_parse_block_expr(ns_ast_ctx *ctx);
ns_return_bool ns_parse_expr(ns_ast_ctx *ctx);

// dump fn
void ns_ast_ctx_print(ns_ast_ctx *ctx, ns_bool verbose);

// main parse fn
ns_return_bool ns_ast_parse(ns_ast_ctx *ctx, ns_str source, ns_str filename);
