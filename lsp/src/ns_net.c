#include "ns_net.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#define BUFFER_SIZE 1024

bool ns_udp_serve(u16 port, ns_str (*on_data)(ns_str)) {
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

    ns_info("ns_net", "udp server is running and listening on port %d...\n", port);

    // main loop to receive and respond to messages
    while (1) {
        memset(buffer, 0, BUFFER_SIZE);

        ssize_t n = recvfrom(sockfd, buffer, BUFFER_SIZE, 0, (struct sockaddr *)&client_addr, &addr_len);
        if (n < 0) {
            ns_warn("ns_net", "receive failed");
            continue;
        }

        ns_str res = on_data(ns_str_cstr(buffer));
        memcpy(buffer, res.data, res.len);

        if (sendto(sockfd, buffer, (u64)res.len, 0, (struct sockaddr *)&client_addr, addr_len) < 0) {
            ns_warn("ns_net", "send failed");
        }
    }

    close(sockfd);
    return true;
}