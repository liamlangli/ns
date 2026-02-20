#include "ns_debug.h"
#include <unistd.h>

#define ns_debug_HEADER_SEP "\r\n\r\n"
#define ns_debug_CONTENT_LENGTH "Content-Length: "

static ns_str _in;
static ns_str _out;
static i8 _chunk[512];

static ns_debug_options _options = {0};
static i32 _dap_out_fd = -1;

// stdio mode

ns_str ns_debug_read();
void ns_debug_write(ns_str data);
ns_bool ns_debug_read_frame(ns_str *body);

// socket mode
void ns_debug_on_request(ns_conn *conn);
void ns_debug_send_response(ns_conn *conn, ns_str res);

// repl mode
i32 ns_debug_parse(ns_str s);

static i32 ns_debug_readline_stdin(i8 *buf, i32 cap) {
    if (cap <= 1) return 0;
    i32 i = 0;
    i32 c = 0;
    while ((c = fgetc(stdin)) != EOF) {
        if (c == '\r') continue;
        if (c == '\n') break;
        if (i < cap - 1) buf[i++] = (i8)c;
    }
    if (c == EOF && i == 0) return -1;
    buf[i] = '\0';
    return i;
}

ns_bool ns_debug_read_frame(ns_str *body) {
    i8 line[256] = {0};
    i32 content_length = -1;

    while (1) {
        i32 n = ns_debug_readline_stdin(line, (i32)sizeof(line));
        if (n < 0) return false;
        if (n == 0 || line[0] == '\0') break;

        if (strncmp((char*)line, ns_debug_CONTENT_LENGTH, strlen(ns_debug_CONTENT_LENGTH)) == 0) {
            content_length = atoi((char*)line + strlen(ns_debug_CONTENT_LENGTH));
        }
    }

    if (content_length <= 0) return false;

    ns_array_set_length(body->data, content_length + 1);
    i32 read_n = (i32)fread(body->data, 1, content_length, stdin);
    if (read_n != content_length) return false;
    body->data[content_length] = '\0';
    body->len = content_length;
    body->dynamic = 1;
    return true;
}

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
    if (_dap_out_fd < 0) return;

    i8 header[64] = {0};
    i32 hlen = snprintf((char *)header, sizeof(header), "Content-Length: %d\r\n\r\n", data.len);
    if (hlen <= 0) return;

    i32 off = 0;
    while (off < hlen) {
        i32 n = (i32)write(_dap_out_fd, header + off, hlen - off);
        if (n <= 0) return;
        off += n;
    }

    off = 0;
    while (off < data.len) {
        i32 n = (i32)write(_dap_out_fd, data.data + off, data.len - off);
        if (n <= 0) return;
        off += n;
    }
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
    ns_debug_session sess = (ns_debug_session){.options = _options, .conn = conn}; // TODO: support multiple sessions
    while(1) {
        ns_data data = ns_tcp_read(conn);
        ns_str s = (ns_str){data.data, data.len, 0};
        ns_json_ref req = ns_debug_parse(s);
        ns_debug_handle(&sess, req);
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
    ns_debug_session sess = (ns_debug_session){.options = _options, .state = NS_DEBUG_STATE_INIT};
    _dap_out_fd = dup(STDOUT_FILENO);
    if (_dap_out_fd < 0) return 1;
    fflush(stdout);
    dup2(STDERR_FILENO, STDOUT_FILENO);

    ns_str body = ns_str_null;
    while (1)
    {
        if (!ns_debug_read_frame(&body)) {
            break;
        }
        ns_json_ref req = ns_json_parse(body);
        ns_debug_handle(&sess, req);
    }
    ns_str_free(body);
    close(_dap_out_fd);
    _dap_out_fd = -1;
    return 0;
}

i32 ns_debug_socket(ns_debug_options options) {
    ns_info("ns_debug", "socket mode at port %d.\n", options.port);
    ns_tcp_serve(options.port, ns_debug_on_request);
    return 0;
}

ns_debug_options ns_debug_parse_args(i32 argc, i8 **argv) {
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
        } else {
            _options.filename = ns_str_cstr(argv[i]);
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
    default: break;
    }
    return ns_debug_repl(options);
}
