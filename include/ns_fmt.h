#pragma once

#include "ns_type.h"
#include "ns_vm.h"

ns_str ns_fmt_type_str(ns_type t);
ns_str ns_fmt_value(ns_vm *vm, ns_value n);
ns_str ns_fmt_eval(ns_vm *vm, ns_str fmt);