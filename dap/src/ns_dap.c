#include "ns_dap.h"
#include "ns_json.h"
#include "ns_net.h"

static i32 _last_seq = 0;

#define NS_DAP_HEADER_SEP "\r\n\r\n"
#define NS_DAP_CONTENT_LENGTH "Content-Length: "

i32 ns_dap_parse(ns_str s) {
    i32 i = ns_str_index_of(s, ns_str_cstr("\r\n\r\n"));
    ns_str header = (ns_str){s.data, i, 0};
    i32 l = ns_str_index_of(header, ns_str_cstr("Content-Length: "));
    if (l == -1) {
        return 0;
    }
    i32 len = ns_str_to_i32(ns_str_slice(header, l + 16, header.len));
    ns_str body = (ns_str){s.data + i + 4, len, 0};
    return ns_json_parse(body);
}

void ns_dap_response(ns_str data) {
    fprintf(stdout, "Content-Length: %d\r\n\r\n%s", data.len, data.data);
}

void ns_dap_response_ack(ns_str type, i32 seq, ns_str cmd,ns_bool suc) {
    i32 res = ns_json_make_object();
    ns_json_set(res, ns_str_cstr("type"), ns_json_make_string(type));
    ns_json_set(res, ns_str_cstr("seq"), ns_json_make_number(++_last_seq));
    ns_json_set(res, ns_str_cstr("request_seq"), ns_json_make_number(seq));
    ns_json_set(res, ns_str_cstr("command"), ns_json_make_string(cmd));
    ns_json_set(res, ns_str_cstr("success"), ns_json_make_bool(suc));
    ns_str res_str = ns_json_to_string(ns_json_get(res));
    ns_dap_response(res_str);
}

void ns_dap_step(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    if (vm->step_hook) {
        vm->step_hook(vm, ctx, i);
    }
}

ns_str ns_dap_on_data(ns_str data) {
    ns_info("ns_dap", "received data: %.*s", data.len, data.data);
    return data;
}

i32 main() {
    u16 port = 5001;
    ns_info("ns_dap", "starting dap server at port %d.\n", port);
    ns_tcp_serve(port, ns_dap_on_data);
    return 0;
}