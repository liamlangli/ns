#pragma once

#ifdef NS_BITCODE

#include "ns_type.h"
#include "ns_os.h"
#include "ns_ast.h"
#include "ns_vm.h"

ns_return_bool ns_bc_gen(ns_str input, ns_str output);

#endif // NS_BITCODE