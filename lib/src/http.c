// http.c
// ---------------------------------------------------------------------------
// Minimal HTTP/1.1 helpers declared in lib/include/http.h, layered on the
// socket primitives in net.h. Request-line parsing results are staged in
// file-scope statics so they can be read back through the scalar FFI; responses
// are written straight to a socket fd.
#include "http.h"
#include "net.h"

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#ifdef NS_WIN
    #include <winsock2.h>
#else
    #include <sys/socket.h>
    #include <sys/time.h>
#endif

// ---- parsed-request state -------------------------------------------------

#define NS_HTTP_PATH_CAP 2048
static int g_method = -1;
static char g_path[NS_HTTP_PATH_CAP];
static int g_path_len = 0;
static char g_websocket_key[256];
static int g_websocket_upgrade = 0;

static int ascii_equal_ci(const char *a, const char *b, int len) {
    for (int i = 0; i < len; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + ('a' - 'A'));
        if (ca != cb) return 0;
    }
    return 1;
}

static void http_parse_websocket_headers(const char *buf, int n) {
    g_websocket_key[0] = '\0';
    g_websocket_upgrade = 0;
    for (int i = 0; i + 2 < n;) {
        while (i < n && buf[i] != '\n') i++;
        if (i < n) i++;
        if (i >= n || buf[i] == '\r' || buf[i] == '\n') break;
        int name_start = i;
        while (i < n && buf[i] != ':' && buf[i] != '\r' && buf[i] != '\n') i++;
        if (i >= n || buf[i] != ':') continue;
        int name_len = i - name_start;
        i++;
        while (i < n && (buf[i] == ' ' || buf[i] == '\t')) i++;
        int value_start = i;
        while (i < n && buf[i] != '\r' && buf[i] != '\n') i++;
        int value_len = i - value_start;
        if (name_len == 7 && ascii_equal_ci(buf + name_start, "upgrade", 7) &&
            value_len == 9 && ascii_equal_ci(buf + value_start, "websocket", 9)) {
            g_websocket_upgrade = 1;
        }
        if (name_len == 17 && ascii_equal_ci(buf + name_start, "sec-websocket-key", 17)) {
            if (value_len >= (int)sizeof(g_websocket_key)) value_len = (int)sizeof(g_websocket_key) - 1;
            memcpy(g_websocket_key, buf + value_start, (size_t)value_len);
            g_websocket_key[value_len] = '\0';
        }
    }
    if (!g_websocket_key[0]) g_websocket_upgrade = 0;
}

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
    int n = 0;
#ifdef NS_WIN
    DWORD timeout = 2000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
#else
    struct timeval timeout = {.tv_sec = 2};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif
    while (n < (int)sizeof(buf) - 1) {
        int received = (int)recv(fd, buf + n, sizeof(buf) - 1 - (size_t)n, 0);
        if (received <= 0) {
            g_method = -1;
            g_path_len = 0;
            return -1;
        }
        n += received;
        buf[n] = '\0';
        if (strstr(buf, "\r\n\r\n")) break;
    }
    if (!strstr(buf, "\r\n\r\n")) return -1;
    http_parse_websocket_headers(buf, n);

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

int http_websocket_is_upgrade(void) { return g_websocket_upgrade; }

static uint32_t sha1_rotl(uint32_t value, int bits) {
    return (value << bits) | (value >> (32 - bits));
}

static void sha1_digest(const unsigned char *data, size_t len, unsigned char out[20]) {
    uint64_t bit_len = (uint64_t)len * 8;
    size_t padded = len + 1;
    while ((padded % 64) != 56) padded++;
    unsigned char *message = (unsigned char *)calloc(padded + 8, 1);
    if (!message) { memset(out, 0, 20); return; }
    memcpy(message, data, len);
    message[len] = 0x80;
    for (int i = 0; i < 8; i++) message[padded + i] = (unsigned char)(bit_len >> (56 - i * 8));

    uint32_t h0 = 0x67452301u, h1 = 0xefcdab89u, h2 = 0x98badcfeu;
    uint32_t h3 = 0x10325476u, h4 = 0xc3d2e1f0u;
    for (size_t offset = 0; offset < padded + 8; offset += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) {
            size_t p = offset + (size_t)i * 4;
            w[i] = ((uint32_t)message[p] << 24) | ((uint32_t)message[p + 1] << 16) |
                   ((uint32_t)message[p + 2] << 8) | (uint32_t)message[p + 3];
        }
        for (int i = 16; i < 80; i++) w[i] = sha1_rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | ((~b) & d); k = 0x5a827999u; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ed9eba1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdcu; }
            else { f = b ^ c ^ d; k = 0xca62c1d6u; }
            uint32_t temp = sha1_rotl(a, 5) + f + e + k + w[i];
            e = d; d = c; c = sha1_rotl(b, 30); b = a; a = temp;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }
    free(message);
    uint32_t words[5] = {h0, h1, h2, h3, h4};
    for (int i = 0; i < 5; i++) {
        out[i * 4] = (unsigned char)(words[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(words[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(words[i] >> 8);
        out[i * 4 + 3] = (unsigned char)words[i];
    }
}

static void base64_encode(const unsigned char *data, int len, char *out) {
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int p = 0;
    for (int i = 0; i < len; i += 3) {
        int remain = len - i;
        uint32_t value = (uint32_t)data[i] << 16;
        if (remain > 1) value |= (uint32_t)data[i + 1] << 8;
        if (remain > 2) value |= data[i + 2];
        out[p++] = table[(value >> 18) & 63];
        out[p++] = table[(value >> 12) & 63];
        out[p++] = remain > 1 ? table[(value >> 6) & 63] : '=';
        out[p++] = remain > 2 ? table[value & 63] : '=';
    }
    out[p] = '\0';
}

int http_websocket_accept(int fd) {
    if (!g_websocket_upgrade || !g_websocket_key[0]) return -1;
    char source[320];
    int n = snprintf(source, sizeof(source), "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", g_websocket_key);
    unsigned char digest[20];
    char accept[32];
    sha1_digest((const unsigned char *)source, (size_t)n, digest);
    base64_encode(digest, 20, accept);
    char response[512];
    int response_len = snprintf(response, sizeof(response),
                                "HTTP/1.1 101 Switching Protocols\r\n"
                                "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                                "Sec-WebSocket-Accept: %s\r\n\r\n", accept);
    return net_send(fd, response, response_len) < 0 ? -1 : 0;
}

int http_websocket_send_text(int fd, const char *text, int len) {
    if (!text || len < 0 || len > 125) return -1;
    unsigned char frame[127];
    frame[0] = 0x81;
    frame[1] = (unsigned char)len;
    if (len > 0) memcpy(frame + 2, text, (size_t)len);
    return net_send(fd, (const char *)frame, len + 2);
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
                        "Cache-Control: no-store\r\n"
                        "Connection: close\r\n"
                        "\r\n",
                        status, http_status_text(status), content_type, body_len);
    } else {
        hlen = snprintf(header, sizeof(header),
                        "HTTP/1.1 %d %s\r\n"
                        "Content-Length: %d\r\n"
                        "Cache-Control: no-store\r\n"
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
                        "Cache-Control: no-store\r\n"
                        "Connection: close\r\n"
                        "\r\n",
                        http_content_type(path), size);
    if (net_send(fd, header, hlen) < 0) return -1;
    if (g_method == NS_HTTP_HEAD) return 0;
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
