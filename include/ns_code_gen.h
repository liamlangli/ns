#pragma once

#include "ns_type.h"
#include "ns_path.h"
#include "ns_ast.h"

bool ns_code_gen_llvm_bc(ns_ast_ctx *ctx);
bool ns_code_gen_arm64(ns_ast_ctx *ctx);
bool ns_code_gen_x86_64(ns_ast_ctx *ctx);