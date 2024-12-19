#include "ns_net.h"

#define BUFFER_SIZE 1024
static i8 ns_net_buffer[BUFFER_SIZE];

#ifdef NS_WIN32
    #include <winsock2.h>

typedef struct ns_conn {
    ns_conn_type type;
    SOCKET socket_fd;
    struct sockaddr_in server_addr, client_addr;
    int addr_len;
} ns_conn;

ns_bool ns_udp_serve(u16 port, ns_on_data on_data) {
    WSADATA wsa_data;
    SOCKET server_socket;
    struct sockaddr_in server_addr, client_addr;
    int client_addr_size = sizeof(client_addr);

    // Initialize Winsock
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        ns_exit(EXIT_FAILURE, "ns_net", "WSAStartup failed: %d.", WSAGetLastError());
    }

    // Create a UDP socket
    server_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (server_socket == INVALID_SOCKET) {
        WSACleanup();
        ns_exit(EXIT_FAILURE, "ns_net", "socket creation failed with error: %d.", WSAGetLastError());
    }

    // Set up the sockaddr_in structure for the server (bind it to a port)
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY; // Bind to all available interfaces
    server_addr.sin_port = htons(port);

    // Bind the socket to the local address and port
    if (bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        closesocket(server_socket);
        WSACleanup();
        ns_exit(EXIT_FAILURE, "ns_net", "bind failed with error: %d.", WSAGetLastError());
    }

    // Receive data in a loop
    while (true) {
        int n = recvfrom(server_socket, ns_net_buffer, BUFFER_SIZE, 0, (struct sockaddr*)&client_addr, &client_addr_size);
        if (n == SOCKET_ERROR) {
            ns_warn("ns_net", "receive failed with error: %d.", WSAGetLastError());
            continue;
        }

        // Print received message and client info
        ns_net_buffer[n] = '\0'; // Null-terminate the received data
        ns_conn conn = (ns_conn){NS_CONN_UDP, server_socket, server_addr, client_addr, client_addr_size};
        on_data(&conn, (ns_data){ns_net_buffer, n});
    }

    // Cleanup
    closesocket(server_socket);
    WSACleanup();
    return true;
}

ns_bool ns_tcp_serve(u16 port, ns_on_connect on_connect) {
    WSADATA wsa_data;
    SOCKET server_socket, client_socket;
    struct sockaddr_in server_addr, client_addr;
    int client_addr_size = sizeof(client_addr);

    // Initialize Winsock
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        ns_exit(EXIT_FAILURE, "ns_net", "WSAStartup failed with error: %d.", WSAGetLastError());
        return 1;
    }

    // Create a TCP socket
    server_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server_socket == INVALID_SOCKET) {
        WSACleanup();
        ns_exit(EXIT_FAILURE, "ns_net", "Socket creation failed with error: %d.", WSAGetLastError());
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY; // Bind to all available interfaces
    server_addr.sin_port = htons(port);

    // Bind the socket to the local address and port
    if (bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        closesocket(server_socket);
        WSACleanup();
        ns_exit(EXIT_FAILURE, "ns_net", "Bind failed with error: %d.", WSAGetLastError());
    }

    // Listen for incoming connections
    if (listen(server_socket, SOMAXCONN) == SOCKET_ERROR) {
        closesocket(server_socket);
        WSACleanup();
        ns_exit(EXIT_FAILURE, "ns_net", "Listen failed with error: %d.", WSAGetLastError());
    }

    // Accept an incoming client connection
    while (1) {
        client_socket = accept(server_socket, (struct sockaddr*)&client_addr, &client_addr_size);
        if (client_socket == INVALID_SOCKET) {
            closesocket(server_socket);
            WSACleanup();
            ns_warn("ns_net", "Accept failed with error: %d.", WSAGetLastError());
            continue;
        }

        ns_conn conn = (ns_conn){NS_CONN_TCP, client_socket, server_addr, client_addr, client_addr_size };
        on_connect(&conn);
    }

    closesocket(server_socket);
    WSACleanup();
    return true;
}

ns_data ns_tcp_read(ns_conn *conn) {
    int n = recv(conn->socket_fd, ns_net_buffer, BUFFER_SIZE, 0);
    ns_net_buffer[n] = '\0';
    return (ns_data){ns_net_buffer, n};
}

void ns_conn_send(ns_conn *conn, ns_data data) {
    if (conn->type == NS_CONN_UDP) {
        sendto(conn->socket_fd, data.data, data.len, 0, (const struct sockaddr *)&conn->client_addr, conn->addr_len);
    } else {
        send(conn->socket_fd, data.data, data.len, 0);
    }
}

void ns_conn_close(ns_conn *conn) {
    if (conn->type == NS_CONN_TCP) {
        closesocket(conn->socket_fd);
    }
}

#else
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <sys/socket.h>

typedef struct ns_conn {
    ns_conn_type type;
    int socket_fd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t addr_len;
} ns_conn;

ns_bool ns_udp_serve(u16 port, ns_on_data on_data) {
    int socket_fd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t addr_len = sizeof(client_addr);

    // create a UDP socket
    if ((socket_fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        ns_exit(EXIT_FAILURE, "ns_net", "socket creation failed");
    }

    // set server address
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;         // IPv4
    server_addr.sin_addr.s_addr = INADDR_ANY; // Listen on all network interfaces
    server_addr.sin_port = htons(port);       // Port

    // bind the socket to the address
    if (bind(socket_fd, (const struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        close(socket_fd);
        ns_exit(EXIT_FAILURE, "ns_net", "bind failed");
    }

    // main loop to receive and respond to messages
    while (1) {
        ssize_t n = recvfrom(socket_fd, ns_net_buffer, BUFFER_SIZE, 0, (struct sockaddr *)&client_addr, &addr_len);
        if (n < 0 || n > BUFFER_SIZE) {
            ns_warn("ns_net", "receive failed");
            continue;
        }

        ns_conn *conn = (ns_conn *)malloc(sizeof(ns_conn));
        conn->type = NS_CONN_UDP;
        conn->socket_fd = socket_fd;
        conn->server_addr = server_addr;
        conn->client_addr = client_addr;

        on_data(conn, (ns_data){ns_net_buffer, n});
    }

    close(socket_fd);
    return true;
}

ns_bool ns_tcp_serve(u16 port, ns_on_connect on_connect) {
    int socket_fd, conn_fd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t addr_len = sizeof(client_addr);

    // create a TCP socket
    if ((socket_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        ns_exit(EXIT_FAILURE, "ns_net", "socket creation failed");
    }

    // set server address
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;         // IPv4
    server_addr.sin_addr.s_addr = INADDR_ANY; // Listen on all network interfaces
    server_addr.sin_port = htons(port);       // Port

    // bind the socket to the address
    if (bind(socket_fd, (const struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        close(socket_fd);
        ns_exit(EXIT_FAILURE, "ns_net", "bind failed");
    }

    // listen for incoming connections
    if (listen(socket_fd, 5) < 0) {
        close(socket_fd);
        ns_exit(EXIT_FAILURE, "ns_net", "listen failed");
    }

    // main loop to accept and respond to connections
    while (1) {
        conn_fd = accept(socket_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (conn_fd < 0) {
            ns_warn("ns_net", "accept failed");
            continue;
        }

        ns_conn conn = (ns_conn){NS_CONN_TCP, conn_fd, server_add, client_addr, addr_len };
        on_connect(conn);
    }

    close(socket_fd);
    return true;
}

ns_data ns_tcp_read(ns_conn *conn) {
    ssize_t n = read(conn->socket_fd, ns_net_buffer, BUFFER_SIZE);
    return (ns_data){ns_net_buffer, n};
}

void ns_conn_send(ns_conn *conn, ns_data data) {
    if (conn->type == NS_CONN_UDP) {
        sendto(conn->socket_fd, data.data, data.len, 0, (const struct sockaddr *)&conn->client_addr, conn->addr_len);
    } else {
        write(conn->socket_fd, data.data, data.len);
    }
}

void ns_conn_close(ns_conn *conn) {
    if (conn->type == NS_CONN_TCP) {
        close(conn->socket_fd);
    }
}

#endif