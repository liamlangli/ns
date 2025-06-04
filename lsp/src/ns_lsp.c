#include "ns_lsp.h"
#include "ns_json.h"
#include "ns_net.h"

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

static ns_str _in = (ns_str){0, 0, 1};
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

void send_response(i32 result) {
    ns_str str = ns_json_stringify(ns_json_get(result));
    fprintf(stdout, "%zu\n", (szt)str.len);
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
    while (1) {
        ns_str line = ns_lsp_read();
        if (ns_str_empty(line)) continue;
        i32 r = ns_json_parse(line);
        handle_init(r);
    }

    return 0;
}