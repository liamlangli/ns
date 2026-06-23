// net.c
// ---------------------------------------------------------------------------
// Native TCP / UDP socket primitives declared in lib/include/net.h. The whole
// interface is scalar (plus `str` arguments) so it marshals through the ns
// interpreter's FFI; see net.h for the rationale. All cross-FFI state -- the
// shared receive buffer and the remembered UDP sender -- lives in file-scope
// statics.
//
// One file serves both POSIX (Linux + macOS) and Windows; the platform-specific
// socket calls are bridged with a few macros so the bodies below stay shared.
#include "net.h"

#include <stdio.h>
#include <string.h>

#ifdef NS_WIN
    #include <winsock2.h>
    #include <ws2tcpip.h>
    typedef int socklen_t;
    #define NS_SOCK_ERR    SOCKET_ERROR
    #define ns_close_sock(fd) closesocket(fd)
#else
    #include <unistd.h>
    #include <arpa/inet.h>
    #include <netdb.h>
    #include <sys/socket.h>
    #include <sys/stat.h>
    #include <sys/types.h>
    #include <netinet/in.h>
    #define NS_SOCK_ERR    (-1)
    #define ns_close_sock(fd) close(fd)
#endif

// Shared staging buffer for received bytes. A blocking, single-threaded server
// fully consumes one datagram / read before issuing the next, so a single
// buffer is sufficient and keeps the interface scalar (see net.h).
#define NS_NET_BUF_CAP 65536
static char g_buf[NS_NET_BUF_CAP];
static int g_buf_len = 0;

// Sender of the most recent net_udp_recv(), so net_udp_reply() can answer it.
#ifndef NS_WIN
static struct sockaddr_in g_udp_from;
static socklen_t g_udp_from_len = 0;
#else
static struct sockaddr_in g_udp_from;
static int g_udp_from_len = 0;
#endif

// One-time Winsock initialisation (no-op elsewhere).
static int net_startup(void) {
#ifdef NS_WIN
    static int started = 0;
    if (!started) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return -1;
        started = 1;
    }
#endif
    return 0;
}

// ---- TCP ------------------------------------------------------------------

int net_tcp_listen(int port) {
    if (net_startup() != 0) return -1;

    int fd = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((unsigned short)port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == NS_SOCK_ERR) {
        ns_close_sock(fd);
        return -1;
    }
    if (listen(fd, 128) == NS_SOCK_ERR) {
        ns_close_sock(fd);
        return -1;
    }
    return fd;
}

int net_tcp_accept(int server_fd) {
    struct sockaddr_in client;
    socklen_t len = sizeof(client);
    int fd = (int)accept(server_fd, (struct sockaddr *)&client, &len);
    if (fd < 0) return -1;
    return fd;
}

int net_tcp_connect(const char *host, int port) {
    if (net_startup() != 0) return -1;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    struct addrinfo hints;
    struct addrinfo *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) return -1;

    int fd = (int)socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        return -1;
    }
    if (connect(fd, res->ai_addr, (socklen_t)res->ai_addrlen) == NS_SOCK_ERR) {
        ns_close_sock(fd);
        freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);
    return fd;
}

// ---- UDP ------------------------------------------------------------------

int net_udp_bind(int port) {
    if (net_startup() != 0) return -1;

    int fd = (int)socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((unsigned short)port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == NS_SOCK_ERR) {
        ns_close_sock(fd);
        return -1;
    }
    return fd;
}

int net_udp_socket(void) {
    if (net_startup() != 0) return -1;
    int fd = (int)socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    return fd;
}

// ---- receive --------------------------------------------------------------

int net_recv(int fd) {
    int n = (int)recv(fd, g_buf, NS_NET_BUF_CAP, 0);
    if (n < 0) {
        g_buf_len = 0;
        return -1;
    }
    g_buf_len = n;
    return n;
}

int net_udp_recv(int fd) {
    g_udp_from_len = sizeof(g_udp_from);
    int n = (int)recvfrom(fd, g_buf, NS_NET_BUF_CAP, 0,
                          (struct sockaddr *)&g_udp_from, (socklen_t *)&g_udp_from_len);
    if (n < 0) {
        g_buf_len = 0;
        return -1;
    }
    g_buf_len = n;
    return n;
}

int net_buf_len(void) { return g_buf_len; }

int net_buf_byte(int i) {
    if (i < 0 || i >= g_buf_len) return -1;
    return (int)(unsigned char)g_buf[i];
}

// ---- send -----------------------------------------------------------------

// Send the whole buffer, looping over partial writes.
static int net_send_all(int fd, const char *data, int len) {
    int sent = 0;
    while (sent < len) {
        int n = (int)send(fd, data + sent, (size_t)(len - sent), 0);
        if (n <= 0) return sent > 0 ? sent : -1;
        sent += n;
    }
    return sent;
}

int net_send(int fd, const char *data, int len) {
    if (len < 0) return -1;
    return net_send_all(fd, data, len);
}

int net_send_str(int fd, const char *s) {
    if (!s) return -1;
    return net_send_all(fd, s, (int)strlen(s));
}

int net_send_buf(int fd, int len) {
    if (len < 0 || len > g_buf_len) len = g_buf_len;
    return net_send_all(fd, g_buf, len);
}

int net_udp_reply(int fd, const char *data, int len) {
    if (g_udp_from_len == 0 || len < 0) return -1;
    return (int)sendto(fd, data, (size_t)len, 0,
                       (struct sockaddr *)&g_udp_from, (socklen_t)g_udp_from_len);
}

int net_udp_send(int fd, const char *host, int port, const char *data, int len) {
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    struct addrinfo hints;
    struct addrinfo *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) return -1;
    int n = (int)sendto(fd, data, (size_t)len, 0, res->ai_addr, (socklen_t)res->ai_addrlen);
    freeaddrinfo(res);
    return n;
}

// ---- file helpers ---------------------------------------------------------

int net_file_size(const char *path) {
#ifdef NS_WIN
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long n = ftell(f);
    fclose(f);
    return (n < 0) ? -1 : (int)n;
#else
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    if (!S_ISREG(st.st_mode)) return -1;
    return (int)st.st_size;
#endif
}

int net_send_file(int fd, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    char chunk[8192];
    int total = 0;
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        if (net_send_all(fd, chunk, (int)n) < 0) {
            fclose(f);
            return -1;
        }
        total += (int)n;
    }
    fclose(f);
    return total;
}

// ---- lifecycle ------------------------------------------------------------

int net_close(int fd) {
    return ns_close_sock(fd) == 0 ? 0 : -1;
}
