// http.c
// ---------------------------------------------------------------------------
// Minimal HTTP/1.1 helpers declared in lib/include/http.h, layered on the
// socket primitives in net.h. Request-line parsing results are staged in
// file-scope statics so they can be read back through the scalar FFI; responses
// are written straight to a socket fd.
#include "http.h"
#include "net.h"

#include <stdio.h>
#include <string.h>

#ifdef NS_WIN
    #include <winsock2.h>
#else
    #include <sys/socket.h>
#endif

// ---- parsed-request state -------------------------------------------------

#define NS_HTTP_PATH_CAP 2048
static int g_method = -1;
static char g_path[NS_HTTP_PATH_CAP];
static int g_path_len = 0;

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Decode percent-escapes in [src, src+len) into g_path, stopping at the first
// '?' (query string) and clamping to the buffer capacity.
static void http_decode_path(const char *src, int len) {
    int o = 0;
    for (int i = 0; i < len && o < NS_HTTP_PATH_CAP - 1; i++) {
        char c = src[i];
        if (c == '?') break;
        if (c == '%' && i + 2 < len) {
            int hi = hex_val(src[i + 1]);
            int lo = hex_val(src[i + 2]);
            if (hi >= 0 && lo >= 0) {
                g_path[o++] = (char)((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        g_path[o++] = c;
    }
    g_path[o] = '\0';
    g_path_len = o;
}

static int http_method_code(const char *m, int len) {
    if (len == 3 && memcmp(m, "GET", 3) == 0) return NS_HTTP_GET;
    if (len == 4 && memcmp(m, "HEAD", 4) == 0) return NS_HTTP_HEAD;
    if (len == 4 && memcmp(m, "POST", 4) == 0) return NS_HTTP_POST;
    if (len == 3 && memcmp(m, "PUT", 3) == 0) return NS_HTTP_PUT;
    if (len == 6 && memcmp(m, "DELETE", 6) == 0) return NS_HTTP_DELETE;
    return NS_HTTP_OTHER;
}

int http_recv_request(int fd) {
    char buf[8192];
    int n = (int)recv(fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) {
        g_method = -1;
        g_path_len = 0;
        return -1;
    }
    buf[n] = '\0';

    // Request line: METHOD SP TARGET SP VERSION CRLF
    int i = 0;
    int m_start = 0;
    while (i < n && buf[i] != ' ') i++;
    if (i >= n) { g_method = -1; return -1; }
    g_method = http_method_code(buf + m_start, i - m_start);

    i++; // skip space
    int t_start = i;
    while (i < n && buf[i] != ' ' && buf[i] != '\r' && buf[i] != '\n') i++;
    http_decode_path(buf + t_start, i - t_start);

    return g_method;
}

int http_method(void) { return g_method; }
int http_path_len(void) { return g_path_len; }

int http_path_byte(int i) {
    if (i < 0 || i >= g_path_len) return -1;
    return (int)(unsigned char)g_path[i];
}

// ---- status / content-type tables -----------------------------------------

static const char *http_status_text(int status) {
    switch (status) {
        case 200: return "OK";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 500: return "Internal Server Error";
        default:  return "OK";
    }
}

// Infer a Content-Type from a file path's extension.
static const char *http_content_type(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    dot++;
    if (strcmp(dot, "html") == 0 || strcmp(dot, "htm") == 0) return "text/html; charset=utf-8";
    if (strcmp(dot, "css") == 0)  return "text/css; charset=utf-8";
    if (strcmp(dot, "js") == 0 || strcmp(dot, "mjs") == 0) return "text/javascript; charset=utf-8";
    if (strcmp(dot, "json") == 0) return "application/json; charset=utf-8";
    if (strcmp(dot, "txt") == 0 || strcmp(dot, "md") == 0) return "text/plain; charset=utf-8";
    if (strcmp(dot, "svg") == 0)  return "image/svg+xml";
    if (strcmp(dot, "png") == 0)  return "image/png";
    if (strcmp(dot, "jpg") == 0 || strcmp(dot, "jpeg") == 0) return "image/jpeg";
    if (strcmp(dot, "gif") == 0)  return "image/gif";
    if (strcmp(dot, "ico") == 0)  return "image/x-icon";
    if (strcmp(dot, "wasm") == 0) return "application/wasm";
    if (strcmp(dot, "woff2") == 0) return "font/woff2";
    if (strcmp(dot, "woff") == 0) return "font/woff";
    return "application/octet-stream";
}

// ---- response building ----------------------------------------------------

int http_send_response(int fd, int status, const char *content_type,
                       const char *body, int body_len) {
    char header[512];
    int hlen;
    if (content_type && content_type[0]) {
        hlen = snprintf(header, sizeof(header),
                        "HTTP/1.1 %d %s\r\n"
                        "Content-Type: %s\r\n"
                        "Content-Length: %d\r\n"
                        "Connection: close\r\n"
                        "\r\n",
                        status, http_status_text(status), content_type, body_len);
    } else {
        hlen = snprintf(header, sizeof(header),
                        "HTTP/1.1 %d %s\r\n"
                        "Content-Length: %d\r\n"
                        "Connection: close\r\n"
                        "\r\n",
                        status, http_status_text(status), body_len);
    }
    if (net_send(fd, header, hlen) < 0) return -1;
    if (body && body_len > 0) {
        if (net_send(fd, body, body_len) < 0) return -1;
    }
    return 0;
}

int http_send_file(int fd, const char *path) {
    int size = net_file_size(path);
    if (size < 0) return -1;

    char header[512];
    int hlen = snprintf(header, sizeof(header),
                        "HTTP/1.1 200 OK\r\n"
                        "Content-Type: %s\r\n"
                        "Content-Length: %d\r\n"
                        "Connection: close\r\n"
                        "\r\n",
                        http_content_type(path), size);
    if (net_send(fd, header, hlen) < 0) return -1;
    return net_send_file(fd, path);
}

int http_send_status(int fd, int status) {
    const char *text = http_status_text(status);
    char body[256];
    int blen = snprintf(body, sizeof(body),
                        "<!doctype html><html><body><h1>%d %s</h1></body></html>\n",
                        status, text);
    return http_send_response(fd, status, "text/html; charset=utf-8", body, blen);
}

// ---- static file server ---------------------------------------------------

// Build "<root><path>" into `out`, rejecting any path that tries to escape the
// root with a "../" segment. Returns 0 on success, -1 when rejected.
static int http_resolve_path(const char *root, const char *path, int path_len,
                             char *out, int out_cap) {
    // Reject parent-directory traversal anywhere in the (already decoded) path.
    for (int i = 0; i + 1 < path_len; i++) {
        if (path[i] == '.' && path[i + 1] == '.') return -1;
    }

    int rlen = (int)strlen(root);
    // Strip a trailing slash from root so we do not double up on the joiner.
    while (rlen > 0 && root[rlen - 1] == '/') rlen--;

    int o = 0;
    for (int i = 0; i < rlen && o < out_cap - 1; i++) out[o++] = root[i];

    if (path_len == 0 || path[0] != '/') {
        if (o < out_cap - 1) out[o++] = '/';
    }
    for (int i = 0; i < path_len && o < out_cap - 1; i++) out[o++] = path[i];

    // Map a directory request ("/" or trailing slash) to its index.html.
    if (o == 0 || out[o - 1] == '/') {
        const char *idx = "index.html";
        for (int i = 0; idx[i] && o < out_cap - 1; i++) out[o++] = idx[i];
    }
    out[o] = '\0';
    return 0;
}

int http_serve_static(int port, const char *root) {
    int server = net_tcp_listen(port);
    if (server < 0) return -1;

    for (;;) {
        int client = net_tcp_accept(server);
        if (client < 0) continue;

        int method = http_recv_request(client);
        if (method < 0) {
            net_close(client);
            continue;
        }
        if (method != NS_HTTP_GET && method != NS_HTTP_HEAD) {
            http_send_status(client, 405);
            net_close(client);
            continue;
        }

        char file_path[NS_HTTP_PATH_CAP + 256];
        if (http_resolve_path(root, g_path, g_path_len, file_path, sizeof(file_path)) != 0) {
            http_send_status(client, 403);
            net_close(client);
            continue;
        }

        if (http_send_file(client, file_path) < 0) {
            http_send_status(client, 404);
        }
        net_close(client);
    }
    // unreachable
}

// ---- minimal client ------------------------------------------------------

int http_get(const char *host, int port, const char *path) {
    int fd = net_tcp_connect(host, port);
    if (fd < 0) return -1;

    char req[1024];
    int rlen = snprintf(req, sizeof(req),
                        "GET %s HTTP/1.1\r\n"
                        "Host: %s\r\n"
                        "User-Agent: ns-http\r\n"
                        "Connection: close\r\n"
                        "\r\n",
                        path, host);
    if (net_send(fd, req, rlen) < 0) {
        net_close(fd);
        return -1;
    }
    return fd;
}
