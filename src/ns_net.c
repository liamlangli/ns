#include "ns_net.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#define BUFFER_SIZE 1024

typedef struct ns_conn {
    ns_conn_type type;
    int sockfd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t addr_len;
} ns_conn;

ns_bool ns_udp_serve(u16 port, ns_on_data on_data) {
    int sockfd;
    char buffer[BUFFER_SIZE];
    struct sockaddr_in server_addr, client_addr;
    socklen_t addr_len = sizeof(client_addr);

    // create a UDP socket
    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        ns_exit(EXIT_FAILURE, "ns_net", "socket creation failed");
    }

    // set server address
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;         // IPv4
    server_addr.sin_addr.s_addr = INADDR_ANY; // Listen on all network interfaces
    server_addr.sin_port = htons(port);       // Port

    // bind the socket to the address
    if (bind(sockfd, (const struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        close(sockfd);
        ns_exit(EXIT_FAILURE, "ns_net", "bind failed");
    }

    // main loop to receive and respond to messages
    while (1) {
        memset(buffer, 0, BUFFER_SIZE);

        ssize_t n = recvfrom(sockfd, buffer, BUFFER_SIZE, 0, (struct sockaddr *)&client_addr, &addr_len);
        if (n < 0) {
            ns_warn("ns_net", "receive failed");
            continue;
        }

        ns_conn *conn = (ns_conn *)malloc(sizeof(ns_conn));
        conn->type = NS_CONN_UDP;
        conn->sockfd = sockfd;
        conn->server_addr = server_addr;
        conn->client_addr = client_addr;

        on_data(conn, ns_str_cstr(buffer));
    }

    close(sockfd);
    return true;
}

ns_bool ns_tcp_serve(u16 port, ns_on_data on_data) {
    int sockfd, conn_fd;
    char buffer[BUFFER_SIZE];
    struct sockaddr_in server_addr, client_addr;
    socklen_t addr_len = sizeof(client_addr);

    // create a TCP socket
    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        ns_exit(EXIT_FAILURE, "ns_net", "socket creation failed");
    }

    // set server address
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;         // IPv4
    server_addr.sin_addr.s_addr = INADDR_ANY; // Listen on all network interfaces
    server_addr.sin_port = htons(port);       // Port

    // bind the socket to the address
    if (bind(sockfd, (const struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        close(sockfd);
        ns_exit(EXIT_FAILURE, "ns_net", "bind failed");
    }

    // listen for incoming connections
    if (listen(sockfd, 5) < 0) {
        close(sockfd);
        ns_exit(EXIT_FAILURE, "ns_net", "listen failed");
    }

    // main loop to accept and respond to connections
    while (1) {
        conn_fd = accept(sockfd, (struct sockaddr *)&client_addr, &addr_len);
        if (conn_fd < 0) {
            ns_warn("ns_net", "accept failed");
            continue;
        }

        ns_conn *conn = (ns_conn *)malloc(sizeof(ns_conn));
        conn->type = NS_CONN_TCP;
        conn->sockfd = conn_fd;
        conn->server_addr = server_addr;
        conn->client_addr = client_addr;
        conn->addr_len = addr_len;

        memset(buffer, 0, BUFFER_SIZE);

        ssize_t n = read(conn_fd, buffer, BUFFER_SIZE);
        if (n < 0) {
            ns_warn("ns_net", "read failed");
            continue;
        }

        on_data(conn, ns_str_cstr(buffer));
    }

    close(sockfd);
    return true;
}

void ns_conn_send(ns_conn *conn, ns_data data) {
    if (conn->type == NS_CONN_UDP) {
        sendto(conn->sockfd, data.data, data.len, 0, (const struct sockaddr *)&conn->client_addr, conn->addr_len);
    } else {
        write(conn->sockfd, data.data, data.len);
    }
}

void ns_conn_close(ns_conn *conn) {
    if (conn->type == NS_CONN_TCP) {
        close(conn->sockfd);
    }
}