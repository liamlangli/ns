#include "ns_debug.h"

#define ns_debug_HEADER_SEP "\r\n\r\n"
#define ns_debug_CONTENT_LENGTH "Content-Length: "

static ns_str _in;
static ns_str _out;
static i8 _chunk[512];

static ns_debug_options _options;

// stdio mode
i32 ns_debug_stdio(ns_debug_options options);
ns_str ns_debug_read();
void ns_debug_write(ns_str data);

// socket mode
i32 ns_debug_socket(ns_debug_options options);
void ns_debug_on_request(ns_conn *conn);
void ns_debug_send_response(ns_conn *conn, ns_str res);

// repl mode
i32 ns_debug_repl(ns_debug_options options);

i32 ns_debug_parse(ns_str s);
ns_str ns_debug_response_ack(ns_str type, i32 seq, ns_str cmd, ns_bool suc); 

ns_str ns_debug_read() {
    i32 size = 0;
    do {
        size = fread(_chunk, 1, sizeof(_chunk), stdin);
        if (size == 0) {
            break;
        }

        i32 len = ns_array_length(_in.data);
        ns_array_set_length(_in.data, size + len + 1);
        memcpy(_in.data + len, _chunk, size);
        _in.data[len + size] = '\0';
    } while (size != 0);
    return _in;
}

void ns_debug_write(ns_str data) {
    fprintf(stdout, "Content-Length: %d\r\n\r\n%s", data.len, data.data);
}

ns_json_ref ns_debug_parse(ns_str s) {
    i32 i = ns_str_index_of(s, ns_str_cstr("\r\n\r\n"));
    ns_str header = (ns_str){s.data, i, 0};
    i32 l = ns_str_index_of(header, ns_str_cstr("Content-Length: "));
    if (l == -1) {
        return 0;
    }
    i32 len = ns_str_to_i32(ns_str_slice(header, l + 16, header.len));
    ns_str body = (ns_str){s.data + i + 4, len, 0};
    ns_info("ns_debug", "request: %.*s\n", body.len, body.data);
    return ns_json_parse(body);
}

void ns_debug_on_request(ns_conn *conn) {
    ns_debug_session sess = (ns_debug_session){_options, conn, 0, 0};
    while(1) {
        ns_data data = ns_tcp_read(conn);
        ns_str s = (ns_str){data.data, data.len, 0};
        ns_json_ref req = ns_debug_parse(s);
        ns_debug_handle(&sess, req);
        if (sess.terminated) break;
    }
    ns_info("ns_debug", "connection closed\n");
    ns_conn_close(conn);
}

void ns_debug_session_response(ns_debug_session *session, ns_json_ref res) {
    ns_str s = ns_json_stringify(ns_json_get(res));
    if (session->options.mode == NS_DEBUG_STDIO) {
        ns_debug_write(s);
    } else if (session->options.mode == NS_DEBUG_SOCKET) {
        ns_debug_send_response(session->conn, s);
    } else {
        ns_error("ns_debug", "invalid mode\n");
    }
}

void ns_debug_send_response(ns_conn *conn, ns_str res) {
    _out.len = 0;
    ns_array_set_length(_out.data, 0);
    ns_str_append(&_out, ns_str_cstr("Content-Length: "));
    ns_str_append(&_out, ns_str_from_i32(res.len));
    ns_str_append(&_out, ns_str_cstr(ns_debug_HEADER_SEP));
    ns_str_append(&_out, res);
    ns_info("ns_debug", "response: %.*s\n", res.len, res.data);
    ns_conn_send(conn, (ns_data){_out.data, _out.len});
}

void ns_debug_help() {
    ns_info("ns_debug", "usage: ns_debug [options] [/path/to/file.ns]\n");
    printf("options:\n");
    printf("  -h --help   show help\n");
    printf("  --stdio     stdio mode\n");
    printf("  --socket    socket mode\n");
    printf("  --port      port number, for socket mode\n");
    printf("  --repl      repl mode\n");
    exit(0);
}

i32 ns_debug_stdio(ns_debug_options _) {
    ns_info("ns_debug", "stdio mode\n");
    while (1)
    {
        ns_str line = ns_debug_read();
        if (line.len == 0) {
            break;
        }
    }
    return 0;
}

i32 ns_debug_socket(ns_debug_options options) {
    ns_info("ns_debug", "socket mode at port %d.\n", options.port);
    ns_tcp_serve(options.port, ns_debug_on_request);
    return 0;
}

i32 ns_debug_repl(ns_debug_options _) {
    ns_info("ns_debug", "repl mode\n");
    return 0;
}

ns_debug_options ns_debug_parse_args(i32 argc, i8 **argv) {
    if (argc == 1) {
        ns_debug_help();
    }

    for (i32 i = 1; i < argc; i++) {
        if (ns_str_equals(ns_str_cstr("-h"), ns_str_cstr(argv[i])) || ns_str_equals(ns_str_cstr("--help"), ns_str_cstr(argv[i]))) {
            ns_debug_help();
        } else if (ns_str_equals(ns_str_cstr("--stdio"), ns_str_cstr(argv[i]))) {
            _options.mode = NS_DEBUG_STDIO;
        } else if (ns_str_equals(ns_str_cstr("--socket"), ns_str_cstr(argv[i]))) {
            _options.mode = NS_DEBUG_SOCKET;
        } else if (ns_str_equals(ns_str_cstr("--repl"), ns_str_cstr(argv[i]))) {
            _options.mode = NS_DEBUG_REPL;
        } else if (ns_str_equals(ns_str_cstr("--port"), ns_str_cstr(argv[i]))) {
            i8* arg = argv[++i];
            _options.port = ns_str_to_i32(ns_str_cstr(arg));
        }
    }
    return _options;
}

i32 main(i32 argc, i8** argv) {
    ns_debug_options options = ns_debug_parse_args(argc, argv);
    switch (options.mode)
    {
    case NS_DEBUG_STDIO: return ns_debug_stdio(options);
    case NS_DEBUG_SOCKET: return ns_debug_socket(options);
    case NS_DEBUG_REPL: return ns_debug_repl(options);
    default:
        break;
    }
    return 0;
}