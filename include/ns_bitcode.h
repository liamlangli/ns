#pragma once

#ifdef NS_BITCODE

#include "ns_type.h"
#include "ns_os.h"
#include "ns_ast.h"
#include "ns_vm.h"

ns_bool ns_bc_gen(ns_vm *vm, ns_ast_ctx *ctx);

#endif // NS_BITCODE