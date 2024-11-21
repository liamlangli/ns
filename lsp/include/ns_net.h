#pragma once

#include "ns_type.h"

bool ns_udp_serve(u16 port, ns_str (*on_data)(ns_str));