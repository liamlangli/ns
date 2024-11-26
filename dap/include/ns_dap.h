#pragma once

#include "ns_type.h"
#include "ns_vm.h"

i32 ns_dap_set_breakpoint(ns_vm *vm, ns_str file, i32 line);
i32 ns_dap_del_breakpoint(ns_vm *vm, i32 id);
i32 ns_dap_step_into(ns_vm *vm);
i32 ns_dap_step_over(ns_vm *vm);
i32 ns_dap_step_out(ns_vm *vm);