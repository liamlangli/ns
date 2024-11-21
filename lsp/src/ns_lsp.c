#include "ns_lsp.h"
#include "ns_json.h"
#include "ns_net.h"

ns_str ns_lsp_on_data(ns_str data) {
    ns_info("ns_lsp", "received data: %.*s", data.len, data.data);
    return data;
}

bool ns_lsp_serve(u16 port) {
    ns_udp_serve(port, ns_lsp_on_data);
    return true;
}

int main() {
    u16 port = 9000;
    ns_lsp_serve(port);
    return 0;
}