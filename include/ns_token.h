#pragma once

#include "ns_type.h"

ns_str ns_token_type_to_string(NS_TOKEN type);
int ns_next_token(ns_token_t *token, ns_str src, ns_str filename, int from);
void ns_token(ns_str source, ns_str filename);
