#include "ns_lsp.h"
#include "ns_json.h"
#include "ns_net.h"

ns_str ns_lsp_on_data(ns_str data) {
    ns_info("ns_lsp", "received data: %.*s", data.len, data.data);
    return data;
}

int main() {
    u16 port = 5000;
    ns_info("ns_dap", "starting lsp server at port %d.\n", port);
    ns_tcp_serve(port, ns_lsp_on_data);
    return 0;
}