#pragma once

#include "ns_type.h"

typedef enum {
    NS_CONN_UNKNOWN = 0,
    NS_CONN_UDP,
    NS_CONN_TCP
} ns_conn_type;

typedef struct ns_conn ns_conn;
typedef void(*ns_on_data)(ns_conn *, ns_str);

ns_bool ns_udp_serve(u16 port, ns_on_data on_data);
ns_bool ns_tcp_serve(u16 port, ns_on_data on_data);

void ns_conn_send(ns_conn *conn, ns_data data);
void ns_conn_close(ns_conn *conn);