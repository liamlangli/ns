// net.h
// ---------------------------------------------------------------------------
// Native TCP / UDP socket primitives exposed to ns programs via `ref fn`
// declarations in lib/net.ns and the static symbol table in src/ns_vm_lib.c.
//
// Like the terminal primitives in term.h, the interface is deliberately kept to
// the FFI subset that marshals reliably from inside an ns function body:
//   * scalar `i32` arguments and return values, and
//   * `str` arguments (passed to C as a `const char*`).
// It never returns a `str` or a `ref struct` (those do not round-trip through
// the interpreter's FFI). Received bytes are therefore staged in a shared
// file-scope buffer and read back one byte at a time with net_buf_byte(), the
// same approach term.posix.c uses for files.
//
// Sockets are identified by their integer file descriptor. The model is
// blocking and single-threaded, matching the terminal editor's event loop: a
// server calls net_tcp_listen() once, then loops over net_tcp_accept().
//
// Two implementations provide the same symbols:
//   * lib/src/net.c        -- Linux + macOS (BSD sockets)
//   * (Windows uses the same file, guarded with Winsock)
#ifndef NS_NET_H
#define NS_NET_H

// ---- TCP ------------------------------------------------------------------

// Create a TCP socket bound to `port` on all interfaces and start listening.
// Sets SO_REUSEADDR so a restarted server can re-bind immediately. Returns the
// listening socket fd, or -1 on error.
int net_tcp_listen(int port);
int net_tcp_listen_local(int port);

// Block until a client connects to the listening socket `server_fd`. Returns
// the connected client fd, or -1 on error.
int net_tcp_accept(int server_fd);
int net_set_nonblocking(int fd, int enabled);

// Open a TCP connection to `host` (a hostname or dotted-quad) on `port`.
// Returns the connected socket fd, or -1 on error.
int net_tcp_connect(const char *host, int port);

// ---- UDP ------------------------------------------------------------------

// Create a UDP socket bound to `port` on all interfaces (for receiving).
// Returns the socket fd, or -1 on error.
int net_udp_bind(int port);

// Create an unbound UDP socket (for sending with net_udp_send). Returns the
// socket fd, or -1 on error.
int net_udp_socket(void);

// ---- receive (into the shared buffer) -------------------------------------

// Receive up to the buffer capacity from a connected TCP socket `fd` into the
// shared receive buffer. Returns the number of bytes read (0 when the peer has
// closed the connection), or -1 on error.
int net_recv(int fd);

// Receive one UDP datagram on `fd` into the shared receive buffer, remembering
// the sender so net_udp_reply() can answer it. Returns the byte count, or -1.
int net_udp_recv(int fd);

// Number of bytes currently held in the shared receive buffer.
int net_buf_len(void);

// Byte `i` of the shared receive buffer as 0..255, or -1 when out of range.
int net_buf_byte(int i);

// ---- send -----------------------------------------------------------------

// Send `len` bytes of `data` over the connected socket `fd`. The length is
// explicit so binary payloads (with embedded NULs) are sent correctly. Returns
// the number of bytes sent, or -1 on error.
int net_send(int fd, const char *data, int len);

// Send the NUL-terminated text `s` over `fd` (convenience for header/text
// lines). Returns the number of bytes sent, or -1.
int net_send_str(int fd, const char *s);

// Send the first `len` bytes of the shared receive buffer over `fd`. This lets
// a server forward exactly what net_recv() read without copying the bytes back
// out through the FFI (e.g. an echo server). Returns bytes sent, or -1.
int net_send_buf(int fd, int len);

// Reply to the sender of the most recent net_udp_recv() on `fd`. Returns the
// number of bytes sent, or -1.
int net_udp_reply(int fd, const char *data, int len);

// Send a UDP datagram from `fd` to `host`:`port`. Returns bytes sent, or -1.
int net_udp_send(int fd, const char *host, int port, const char *data, int len);

// ---- file helpers ---------------------------------------------------------

// Size of the file at `path` in bytes, or -1 when it does not exist / is not a
// regular file.
int net_file_size(const char *path);

// Stream the contents of the file at `path` over the socket `fd`. Returns the
// number of body bytes sent, or -1 when the file cannot be opened.
int net_send_file(int fd, const char *path);

// ---- lifecycle ------------------------------------------------------------

// Close the socket `fd`. Returns 0 on success, -1 on error.
int net_close(int fd);

#endif // NS_NET_H
