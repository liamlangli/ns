#include "ns_lsp.h"
#include "ns_json.h"
#include "ns_net.h"

typedef enum {
    NS_LSP_INVALID = 0,
    NS_LSP_STDIO,
    NS_LSP_SOCKET
} ns_lsp_mode;

typedef struct {
    ns_lsp_mode mode;
    union {
        struct {
            ns_conn *conn;
            u16 port;
        } socket; // For socket mode
    };

    struct {
        ns_bool connected;
        i32 process_id;
        ns_str name;
    } client;

    struct {
        ns_str root_path;
        ns_str root_uri;
    } workspace;
} ns_lsp_client;

typedef enum {
    NS_LSP_METHOD_UNKNOWN = 0,
    NS_LSP_METHOD_INITIALIZE,
    NS_LSP_METHOD_INITIALIZED,
    NS_LSP_METHOD_SHUTDOWN,
    NS_LSP_METHOD_EXIT,
    NS_LSP_METHOD_TEXT_DOCUMENT_DID_OPEN,
    NS_LSP_METHOD_TEXT_DOCUMENT_DID_CLOSE,
    NS_LSP_METHOD_TEXT_DOCUMENT_DID_SAVE,
    NS_LSP_METHOD_TEXT_DOCUMENT_DID_CHANGE,
    NS_LSP_METHOD_COMPLETION,
    NS_LSP_METHOD_HOVER,
    NS_LSP_METHOD_DEFINITION,
    NS_LSP_METHOD_REFERENCES,
    NS_LSP_METHOD_DOCUMENT_SYMBOL,
    NS_LSP_METHOD_WORKSPACE_SYMBOL,
} ns_lsp_method;

static ns_lsp_client _client = {0};
static ns_str _in = (ns_str){0, 0, 1};
static ns_str _out = (ns_str){0, 0, 1};


szt ns_getline(char **lineptr, szt *n, FILE *stream) {
    if (*lineptr == NULL || *n == 0) {
        *n = 128; // Start with an initial buffer size
        *lineptr = (char *)ns_malloc(*n);
        if (*lineptr == NULL) {
            return -1; // Allocation failure
        }
    }

    szt len = 0;
    while (fgets(*lineptr + len, *n - len, stream)) {
        len += strlen(*lineptr + len);
        if ((*lineptr)[len - 1] == '\n') {
            break;
        }

        // Resize the buffer if needed
        if (len + 1 >= *n) {
            *n *= 2;
            *lineptr = (char *)realloc(*lineptr, *n);
            if (*lineptr == NULL) {
                return -1; // Allocation failure
            }
        }
    }

    if (len == 0) {
        return -1; // End of input or error
    }

    return len;
}

ns_str ns_lsp_read() {
    szt s;
    i8* b = 0;
    i32 n = (i32)ns_getline(&b, &s, stdin);
    if (n == -1) {
        return ns_str_null;
    }
    
    szt len = ns_str_to_i32(ns_str_range(b, n - 1));

    ns_array_set_length(_in.data, len);
    if (fread(_in.data, 1, len, stdin) != len) {
        return ns_str_null;
    }
    _in.len = len;
    _in.data[len] = '\0';
    return _in;
}

void ns_lsp_response(i32 r) {
    ns_str str = ns_json_stringify(ns_json_get(r));

    ns_str_append(&_out, ns_str_cstr("Content-Length: "));
    ns_str_append_i32(&_out, (i32)str.len);
    ns_str_append(&_out, ns_str_cstr("\r\n\r\n"));
    ns_str_append(&_out, str);

    if (_client.mode == NS_LSP_STDIO) {
        fprintf(stdout, "%.*s", _out.len, _out.data);
        fflush(stdout);
    } else if (_client.mode == NS_LSP_SOCKET) {
        ns_tcp_write(_client.socket.conn, (ns_data){_out.data, _out.len});
    } else {
        ns_warn("lsp", "Invalid LSP mode.\n");
        return;
    }
    ns_str_clear(&_out);
}

ns_lsp_method ns_lsp_parse_method(ns_str str) {
    if (ns_str_equals_STR(str, "initialized")) {
        return NS_LSP_METHOD_INITIALIZED;
    } else if (ns_str_equals_STR(str, "initialize")) {
        return NS_LSP_METHOD_INITIALIZE;
    } else if (ns_str_equals_STR(str, "shutdown")) {
        return NS_LSP_METHOD_SHUTDOWN;
    } else if (ns_str_equals_STR(str, "exit")) {
        return NS_LSP_METHOD_EXIT;
    } else if (ns_str_equals_STR(str, "textDocument/didOpen")) {
        return NS_LSP_METHOD_TEXT_DOCUMENT_DID_OPEN;
    } else if (ns_str_equals_STR(str, "textDocument/didClose")) {
        return NS_LSP_METHOD_TEXT_DOCUMENT_DID_CLOSE;
    } else if (ns_str_equals_STR(str, "textDocument/didSave")) {
        return NS_LSP_METHOD_TEXT_DOCUMENT_DID_SAVE;
    } else if (ns_str_equals_STR(str, "textDocument/didChange")) {
        return NS_LSP_METHOD_TEXT_DOCUMENT_DID_CHANGE;
    } else if (ns_str_equals_STR(str, "textDocument/completion")) {
        return NS_LSP_METHOD_COMPLETION;
    } else if (ns_str_equals_STR(str, "textDocument/hover")) {
        return NS_LSP_METHOD_HOVER;
    } else if (ns_str_equals_STR(str, "textDocument/definition")) {
        return NS_LSP_METHOD_DEFINITION;
    } else if (ns_str_equals_STR(str, "textDocument/references")) {
        return NS_LSP_METHOD_REFERENCES;
    } else if (ns_str_equals_STR(str, "textDocument/documentSymbol")) {
        return NS_LSP_METHOD_DOCUMENT_SYMBOL;
    } else if (ns_str_equals_STR(str, "workspace/symbol")) {
        return NS_LSP_METHOD_WORKSPACE_SYMBOL;
    } else {
        return NS_LSP_METHOD_UNKNOWN;
    }
}

void ns_lsp_handle_init(i32 req) {
    i32 params = ns_json_get_prop(req, ns_str_cstr("params"));
    if (params == 0) {
        ns_warn("lsp", "No params in request.\n");
    }

    i32 r = ns_json_make_object();
    ns_json_set(r, ns_str_cstr("jsonrpc"), ns_json_make_string(ns_str_cstr("2.0")));

    // set id
    i32 id = ns_json_to_i32(ns_json_get_prop(req, ns_str_cstr("id")));
    ns_json_set(r, ns_str_cstr("id"), ns_json_make_number(id));

    // set result
    i32 result = ns_json_make_object();
    ns_json_set(result, ns_str_cstr("capabilities"), ns_json_make_object());

    ns_json_set(r, ns_str_cstr("result"), result);

    _client.client.process_id = ns_json_to_i32(ns_json_get_prop(params, ns_str_cstr("processId")));
    i32 client_info = ns_json_get_prop(params, ns_str_cstr("clientInfo"));
    if (client_info != 0) {
        ns_str name = ns_json_to_string(ns_json_get_prop(client_info, ns_str_cstr("name")));
        if (!ns_str_is_empty(name)) {
            _client.client.name = name;
        }
    }
    i32 root_path = ns_json_get_prop(params, ns_str_cstr("rootPath"));
    if (root_path != 0) {
        _client.workspace.root_path = ns_json_to_string(root_path);
    }
    i32 root_uri = ns_json_get_prop(params, ns_str_cstr("rootUri"));
    if (root_uri != 0) {
        _client.workspace.root_uri = ns_json_to_string(root_uri);
    }
    ns_lsp_response(r);
    ns_info("lsp", "Initialized LSP server with process ID: %d, client name: %.*s\n", _client.client.process_id, _client.client.name.len, _client.client.name.data);
}

void ns_lsp_handle_inited(i32 req) {
    i32 params = ns_json_get_prop(req, ns_str_cstr("params"));
    if (params == 0) {
        ns_warn("lsp", "No params in initialized request.\n");
    }

    i32 r = ns_json_make_object();
    ns_json_set(r, ns_str_cstr("jsonrpc"), ns_json_make_string(ns_str_cstr("2.0")));

    // set id
    i32 id = ns_json_to_i32(ns_json_get_prop(req, ns_str_cstr("id")));
    ns_json_set(r, ns_str_cstr("id"), ns_json_make_number(id));

    // set result
    i32 result = ns_json_make_object();
    ns_json_set(result, ns_str_cstr("capabilities"), ns_json_make_object());

    ns_json_set(r, ns_str_cstr("result"), result);

    _client.client.connected = true;
    ns_lsp_response(r);
}

void ns_lsp_handle_request(i32 req) {
    i32 method = ns_json_get_prop(req, ns_str_cstr("method"));
    if (method == 0) {
        // dump input json
        ns_str input_json = ns_json_stringify(ns_json_get(req));
        ns_exit(1, "lsp", "Received request without method: %.*s\n", input_json.len, input_json.data);
    }
    ns_str method_str = ns_json_to_string(method);
    ns_lsp_method lsp_method = ns_lsp_parse_method(method_str);
    switch (lsp_method) {
        case NS_LSP_METHOD_INITIALIZE:
            ns_lsp_handle_init(req);
            break;
        case NS_LSP_METHOD_INITIALIZED:
            ns_lsp_handle_inited(req);
            ns_info("lsp", "Initialized method called.\n");
            break;
        case NS_LSP_METHOD_SHUTDOWN:
            // Handle shutdown method
            ns_info("lsp", "Shutdown method called.\n");
            break;
        case NS_LSP_METHOD_EXIT:
            // Handle exit method
            ns_info("lsp", "Exit method called.\n");
            exit(0);
            break;
        default:
            ns_exit(1, "lsp", "Unknown method: %.*s\n", method_str.len, method_str.data);
            // You can handle other methods here as needed
            break;
    }
}

ns_bool ns_lsp_parse_header(ns_str s, i32 *head_len, i32 *body_len) {
    i32 i = ns_str_index_of(s, ns_str_cstr("\r\n\r\n"));
    if (i == -1) {
        return false;
    }
    ns_str header = (ns_str){s.data, i, 0};
    i32 l = ns_str_index_of(header, ns_str_cstr("Content-Length: "));
    if (l == -1) {
        return false;
    }
    i32 len = ns_str_to_i32(ns_str_slice(header, l + 16, header.len));
    if (len < 0) {
        return false; // Invalid content length
    }
    *head_len = i + 4; // Length of header including "\r\n\r\n"
    *body_len = len;
    return true;
}

void ns_lsp_on_connect(ns_conn *conn) {
    ns_info("lsp", "New connection established.\n");
    _client.socket.conn = conn;

    i32 head, body, recv;
    while(1) {
        ns_data data = ns_tcp_read(conn);
        if (data.len == 0) {
            continue;
        }

        if (ns_lsp_parse_header(ns_str_range(data.data, data.len), &head, &body)) {
            if ((i32)data.len == head + body) {
                ns_str_append(&_in, ns_str_range(data.data + head, body));
            } else {
                recv = data.len - head;
                while (recv < body) {
                    ns_data more_data = ns_tcp_read(conn);
                    if (more_data.len == 0) continue;
                    ns_str_append(&_in, ns_str_range(more_data.data, more_data.len));
                    recv += more_data.len;
                }
            }
        } else {
            ns_exit(1, "lsp", "Invalid request header.");
        }

        ns_lsp_handle_request(ns_json_parse(_in));
        ns_str_clear(&_in);
    }
}

void ns_lsp_parse_arg(i32 argc, i8 **argv) {
    _client.mode = NS_LSP_INVALID;
    for (i32 i = 1; i < argc; i++) {
        if (ns_str_equals_STR(ns_str_cstr("--socket"), argv[i])) {
            _client.mode = NS_LSP_SOCKET;
        } else if (ns_str_equals_STR(ns_str_cstr("--stdio"), argv[i])) {
            _client.mode = NS_LSP_STDIO;
        } else if (ns_str_equals_STR(ns_str_cstr("--port"), argv[i])) {
            if (i + 1 < argc) {
                i8 *port_str = argv[i + 1];
                szt len = strlen(port_str);
                ns_str port_ns_str = ns_str_range(port_str, len);
                _client.socket.port = (u16)ns_str_to_i32(port_ns_str);
                ++i;
            } else {
                ns_exit(1, "lsp", "Error: --port requires a value.");
            }
        } else {
            ns_exit(1, "lsp", "Unknown argument: %s.", argv[i]);
        }
    }
}

i32 main(i32 argc, i8 **argv) {
    if (argc < 2) {
        ns_exit(1, "lsp", "Usage: lsp --socket|--stdio [--port <port>]");
    } else {
        ns_lsp_parse_arg(argc, argv);
    }

    if (_client.mode == NS_LSP_SOCKET) {
        u16 port = _client.socket.port;
        ns_info("lsp", "Listening on port %d.\n", port);
        ns_tcp_serve(port, ns_lsp_on_connect);
    } else if (_client.mode == NS_LSP_STDIO) {
        while (1) {
            ns_str line = ns_lsp_read();
            if (ns_str_is_empty(line)) continue;
            ns_lsp_handle_request(ns_json_parse(line));
        }
    } else {
        ns_exit(1, "lsp", "Error: No valid mode specified. Use --socket or --stdio.");
    }
    return 0;
}