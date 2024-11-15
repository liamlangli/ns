#include "ns_lsp.h"

bool ns_lsp_serve(u16 port) {
    ns_info("ns_lsp", "serve lsp on port %d\n", port);
    return true;
}

int main() {
    u16 port = 9000;
    // todo: parse port from command line

    ns_lsp_serve(port);
    return 0;
}