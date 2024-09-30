#pragma once

#include "ns_type.h"
#include "ns_path.h"
#include "ns_parse.h"

bool ns_code_gen_llvm_bc(ns_parse_context_t *ctx);

bool ns_code_gen_arm64(ns_parse_context_t *ctx);

bool ns_code_gen_x86_64(ns_parse_context_t *ctx);