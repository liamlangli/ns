#include "ns_lsp.h"
#include "ns_json.h"
#include "ns_net.h"

static ns_str _in = (ns_str){0, 0, 1};
ns_str ns_lsp_read() {
    size_t s;
    i8* b = 0;
    ssize_t n = getline(&b, &s, stdin);
    if (n == -1) {
        return ns_str_null;
    }
    
    size_t len = ns_str_to_i32(ns_str_range(b, n - 1));

    ns_array_set_length(_in.data, len);
    if (fread(_in.data, 1, len, stdin) != len) {
        return ns_str_null;
    }
    _in.len = len;
    _in.data[len] = '\0';
    return _in;
}

void send_response(i32 result) {
    ns_str str = ns_json_stringify(ns_json_get(result));
    fprintf(stdout, "%zu\n", (size_t)str.len);
    fprintf(stdout, "%.*s\n", str.len, str.data);
}

void handle_init(i32 req) {
    i32 r = ns_json_make_object();

    ns_json_set(r, ns_str_cstr("jsonrpc"), ns_json_make_string(ns_str_cstr("2.0")));
    ns_json_set(r, ns_str_cstr("id"), ns_json_get_prop(req, ns_str_cstr("id")));
    i32 result = ns_json_make_object();
    ns_json_set(result, ns_str_cstr("capabilities"), ns_json_make_object());
    ns_json_set(r, ns_str_cstr("result"), result);
    send_response(r);
}

int main() {
    while (1)
    {
        ns_str line = ns_lsp_read();
        i32 r = ns_json_parse(line);
        handle_init(r);
    }

    return 0;
}