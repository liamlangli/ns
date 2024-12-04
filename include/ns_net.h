#pragma once

#include "ns_type.h"

ns_bool ns_udp_serve(u16 port, ns_str (*on_data)(ns_str));
ns_bool ns_tcp_serve(u16 port, ns_str (*on_data)(ns_str));