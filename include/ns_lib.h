#pragma once

#include "ns_type.h"

typedef struct  {
    ns_str name;
} ns_fn;

typedef struct {
    ns_str name;
    ns_fn *fns;
} ns_lib;