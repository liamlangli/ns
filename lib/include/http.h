// http.h
// ---------------------------------------------------------------------------
// Minimal HTTP/1.1 helpers built on the socket primitives in net.h, exposed to
// ns programs via lib/http.ns and the static symbol table in src/ns_vm_lib.c.
//
// The interface follows the same scalar-plus-`str` FFI discipline as net.h: the
// parsed request method and path are staged in file-scope statics and read back
// scalar-by-scalar (http_method() / http_path_byte()), while responses are
// emitted directly to a socket fd.
//
// The model is blocking and single-threaded -- http_serve_static() is a
// complete, self-contained static file server, and http_recv_request() plus the
// response builders let ns code implement custom request handling on top of the
// net primitives.
#ifndef NS_HTTP_H
#define NS_HTTP_H

// Request method codes returned by http_recv_request() / http_method().
#define NS_HTTP_GET     0
#define NS_HTTP_HEAD    1
#define NS_HTTP_POST    2
#define NS_HTTP_PUT     3
#define NS_HTTP_DELETE  4
#define NS_HTTP_OTHER   5

// ---- request parsing ------------------------------------------------------

// Read one HTTP request from the connected socket `fd` and parse its request
// line into the shared statics. Returns one of the NS_HTTP_* method codes, or
// -1 when the connection closed or the request was malformed.
int http_recv_request(int fd);

// Method code (NS_HTTP_*) of the most recently parsed request.
int http_method(void);

// Length of the most recently parsed request path (percent-decoded, query
// string stripped), and access to its bytes.
int http_path_len(void);
int http_path_byte(int i);  // 0..255, or -1 when out of range
int http_websocket_is_upgrade(void);
int http_websocket_accept(int fd);
int http_websocket_send_text(int fd, const char *text, int len);

// ---- response building ----------------------------------------------------

// Send a complete response: status line for `status`, the given `content_type`
// header (skipped when NULL/empty), Content-Length, a blank line, then
// `body_len` bytes of `body`. Returns 0 on success, -1 on error.
int http_send_response(int fd, int status, const char *content_type,
                       const char *body, int body_len);

// Send `path` as a 200 response with Content-Type inferred from its extension.
// Returns the number of body bytes sent, or -1 when the file cannot be opened
// (the caller should then send a 404 with http_send_status).
int http_send_file(int fd, const char *path);

// Send a status-only response (e.g. 404, 500) with a small HTML body describing
// the status. Returns 0 on success, -1 on error.
int http_send_status(int fd, int status);
int http_send_static_request(int fd, const char *root);

// ---- complete static file server ------------------------------------------

// Serve files under directory `root` over HTTP on `port`, forever. Maps the
// request path to a file beneath `root` (rejecting "../" traversal), serves it
// with an inferred Content-Type, falls back to "<root>/index.html" for "/", and
// answers 404 for anything missing. Blocks; returns -1 only on fatal setup
// failure (e.g. the port could not be bound).
int http_serve_static(int port, const char *root);

// ---- minimal client ------------------------------------------------------

// Connect to `host`:`port` and send "GET `path`" with a Host header. Returns a
// connected socket fd whose response can be read with net_recv(), or -1.
int http_get(const char *host, int port, const char *path);

#endif // NS_HTTP_H
