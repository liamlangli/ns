#include "ns_lsp.h"
#include "ns_json.h"

bool ns_lsp_serve(u16 port) {
    ns_info("ns_lsp", "serve lsp on port %d\n", port);
    return true;
}

int main() {
    // u16 port = 9000;
    // todo: parse port from command line
    // ns_lsp_serve(port);

    i8* json = "{\"name\": \"ns\", \"age\": 18}";
    ns_json *root = ns_json_parse(ns_str_cstr(json));
    ns_json_print(root);

    return 0;
}