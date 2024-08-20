#pragma once

#include "ns_parse.h"
#include "ns_type.h"

bool ns_code_gen_ir(ns_parse_context_t *ctx);

bool ns_code_gen_arm64(ns_parse_context_t *ctx);

bool ns_code_gen_x86_64(ns_parse_context_t *ctx);