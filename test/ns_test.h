#pragma once 

#include "ns_type.h"
#include "ns_json.h"

#define ns_expect(t, e) if (!(t)) ns_error("FAILED", e "\n"); else ns_info("PASS", e "\n")