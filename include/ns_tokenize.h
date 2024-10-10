#pragma once

#include "ns_type.h"

int ns_next_token(ns_token_t *token, ns_str src, ns_str filename, int from);
void ns_tokenize(ns_str source, ns_str filename);
