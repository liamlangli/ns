#pragma once

#include "ns_type.h"
#include "ns_vm.h"

typedef struct ns_debug_request_init {
    ns_str type;
    i32 seq;
    ns_str command;
    ns_str arguments;
} ns_debug_request_init;