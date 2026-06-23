# Networking, HTTP and static file serving

The standard library ships two modules for network programming:

- **`net`** — TCP / UDP socket primitives.
- **`http`** — minimal HTTP/1.1 helpers built on `net`, including a complete
  static file server.

Both are implemented in C (`lib/src/net.c`, `lib/src/http.c`) and compiled
position-independent directly into the `ns` binary, resolved at runtime through
the static symbol table in `src/ns_vm_lib.c` (no shared object to load). The ns
bindings live in `lib/net.ns` and `lib/http.ns`.

## FFI shape

Like the terminal primitives in `term`, the interface is restricted to the FFI
subset that marshals reliably from inside an ns function body:

- scalar `i32` arguments and return values, and
- `str` arguments (passed to C as a `const char*`).

It never returns a `str` or a `ref struct`. Received bytes are staged in a
shared file-scope buffer and read back one byte at a time with `net_buf_byte()`;
payloads are sent with an explicit length (pass `s.len`) so binary data is
handled correctly. The model is blocking and single-threaded: a server calls
`net_tcp_listen()` once and then loops over `net_tcp_accept()`.

## Static file server

The headline feature is a one-call static file server:

```ns
use http

fn main() {
    let r = http_serve_static(8080, ".")   // serve "." on :8080, forever
}
```

`http_serve_static` binds the port, accepts connections, maps each request path
to a file beneath the root (rejecting `../` traversal), serves it with a
`Content-Type` inferred from the extension, maps directory requests to
`index.html`, and answers 404 for anything missing. See
`sample/ns/http_server.ns`.

## TCP echo server

A custom server runs the accept loop in ns on top of the primitives:

```ns
use net

fn main() {
    let server = net_tcp_listen(7000)
    if server >= 0 {
        for i in 0 to 16 {
            let client = net_tcp_accept(server)
            if client >= 0 {
                let n = net_recv(client)          // read into the shared buffer
                if n > 0 {
                    let w = net_send_buf(client, n) // echo it straight back
                }
                let d = net_close(client)
            }
        }
        let d = net_close(server)
    }
}
```

See `sample/ns/tcp_echo.ns`.

## Custom HTTP handling

For dynamic responses, run the loop yourself, parse each request with
`http_recv_request()`, route on the parsed path (`http_path_len()` /
`http_path_byte()`), and reply with `http_send_response()`,
`http_send_file()`, or `http_send_status()`:

```ns
use net
use http

fn path_is(s: str) bool {
    if http_path_len() != s.len { return false }
    for i in 0 to s.len {
        if http_path_byte(i) != (s[i] as i32) { return false }
    }
    return true
}

fn main() {
    let server = net_tcp_listen(8081)
    if server >= 0 {
        for i in 0 to 16 {
            let client = net_tcp_accept(server)
            if client >= 0 {
                let method = http_recv_request(client)
                if path_is("/hello") {
                    let body = "hello\n"
                    let r = http_send_response(client, 200, "text/plain", body, body.len)
                }
                if path_is("/hello") == false {
                    let r = http_send_status(client, 404)
                }
                let d = net_close(client)
            }
        }
        let d = net_close(server)
    }
}
```

## HTTP client

`http_get` connects, sends the request, and hands back a socket fd whose
response is read with `net_recv`:

```ns
use net
use http

let fd = http_get("example.com", 80, "/")
if fd >= 0 {
    let n = net_recv(fd)            // first chunk of the response
    let first = net_buf_byte(0)     // 'H'
    let d = net_close(fd)
}
```

## UDP

```ns
use net

let fd = net_udp_bind(9000)
let n = net_udp_recv(fd)            // receive one datagram (sender remembered)
let r = net_udp_reply(fd, "pong", 4) // answer that sender
let d = net_close(fd)
```

## Reference

### `net`

| Function | Description |
| --- | --- |
| `net_tcp_listen(port) i32` | Bind+listen on all interfaces; listening fd or -1. |
| `net_tcp_accept(server_fd) i32` | Block for a client; client fd or -1. |
| `net_tcp_connect(host, port) i32` | Connect; socket fd or -1. |
| `net_udp_bind(port) i32` | UDP socket bound for receiving; fd or -1. |
| `net_udp_socket() i32` | Unbound UDP socket for sending; fd or -1. |
| `net_recv(fd) i32` | TCP read into shared buffer; byte count (0=closed) or -1. |
| `net_udp_recv(fd) i32` | Receive one datagram (remembers sender); count or -1. |
| `net_buf_len() i32` | Bytes in the shared receive buffer. |
| `net_buf_byte(i) i32` | Byte `i` (0..255), or -1 out of range. |
| `net_send(fd, data, len) i32` | Send `len` bytes; pass `s.len`. Bytes sent or -1. |
| `net_send_str(fd, s) i32` | Send NUL-terminated text; bytes sent or -1. |
| `net_send_buf(fd, len) i32` | Forward the first `len` bytes of the shared buffer. |
| `net_udp_reply(fd, data, len) i32` | Reply to the last datagram's sender. |
| `net_udp_send(fd, host, port, data, len) i32` | Send a datagram to host:port. |
| `net_file_size(path) i32` | File size, or -1 if missing / not a regular file. |
| `net_send_file(fd, path) i32` | Stream a file over `fd`; bytes sent or -1. |
| `net_close(fd) i32` | Close the socket; 0 or -1. |

### `http`

| Function | Description |
| --- | --- |
| `http_recv_request(fd) i32` | Read+parse one request; method code (`HTTP_*`) or -1. |
| `http_method() i32` | Method code of the last parsed request. |
| `http_path_len() i32` | Length of the decoded request path. |
| `http_path_byte(i) i32` | Byte `i` of the decoded path, or -1. |
| `http_send_response(fd, status, content_type, body, body_len) i32` | Full response; 0 or -1. |
| `http_send_file(fd, path) i32` | 200 with file body + inferred type; body bytes or -1. |
| `http_send_status(fd, status) i32` | Status-only response with a small HTML body. |
| `http_serve_static(port, root) i32` | Complete blocking static file server. |
| `http_get(host, port, path) i32` | Connect + send GET; fd to read with `net_recv`, or -1. |

Method codes: `HTTP_GET=0`, `HTTP_HEAD=1`, `HTTP_POST=2`, `HTTP_PUT=3`,
`HTTP_DELETE=4`, `HTTP_OTHER=5`.
