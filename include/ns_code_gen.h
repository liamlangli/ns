#pragma once

#include "ns_type.h"
#include "ns_path.h"
#include "ns_ast.h"
#include "ns_vm.h"

bool ns_code_gen_llvm_bc(ns_vm *vm, ns_ast_ctx *ctx);