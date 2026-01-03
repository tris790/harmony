#include "../network_api.h"
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <string.h>
#include <errno.h>

struct NetworkContext {
    int sockfd;
    struct sockaddr_in final_dest; // For "connect" style convenience (optional)
};

NetworkContext* Net_Init(MemoryArena *arena, int port, bool is_server) {
    NetworkContext *ctx = PushStructZero(arena, NetworkContext);

    ctx->sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (ctx->sockfd < 0) {
        perror("Net_Init: socket");
        return NULL;
    }

    // Non-blocking mode
    int flags = fcntl(ctx->sockfd, F_GETFL, 0);
    fcntl(ctx->sockfd, F_SETFL, flags | O_NONBLOCK);

    // Allow port reuse for faster recovery on restart
    int reuse = 1;
    if (setsockopt(ctx->sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        perror("Net_Init: setsockopt SO_REUSEADDR");
    }

    // Increase Socket Buffers to prevent drops during large keyframe bursts
    int rcvbuf = 4 * 1024 * 1024; // 4MB
    int sndbuf = 4 * 1024 * 1024; // 4MB
    
    if (setsockopt(ctx->sockfd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) < 0) {
        perror("Net_Init: setsockopt SO_RCVBUF");
    }
    if (setsockopt(ctx->sockfd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)) < 0) {
        perror("Net_Init: setsockopt SO_SNDBUF");
    }

    if (is_server) {
        struct sockaddr_in addr = {0};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = INADDR_ANY;

        if (bind(ctx->sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            perror("Net_Init: bind");
            close(ctx->sockfd);
            return NULL;
        }
        printf("Net: Bound to port %d (RCVBUF: %d bytes)\n", port, rcvbuf);
    }

    return ctx;
}

void Net_Send(NetworkContext *ctx, const char *ip, int port, void *data, size_t size) {
    struct sockaddr_in dest = {0};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(port);
    inet_pton(AF_INET, ip, &dest.sin_addr);

    ssize_t sent = sendto(ctx->sockfd, data, size, 0, (struct sockaddr*)&dest, sizeof(dest));
    if (sent < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            perror("Net_Send: sendto");
        }
    }
}

int Net_Recv(NetworkContext *ctx, void *buffer, size_t buffer_size, char *out_sender_ip, int *out_sender_port) {
    struct sockaddr_in src_addr = {0};
    socklen_t addr_len = sizeof(src_addr);

    ssize_t len = recvfrom(ctx->sockfd, buffer, buffer_size, 0, (struct sockaddr*)&src_addr, &addr_len);
    
    if (len > 0) {
        if (out_sender_ip) {
            inet_ntop(AF_INET, &src_addr.sin_addr, out_sender_ip, 16); // INET_ADDRSTRLEN is 16
        }
        if (out_sender_port) {
            *out_sender_port = ntohs(src_addr.sin_port);
        }
        return (int)len;
    }

    return 0;
}

void Net_Close(NetworkContext *ctx) {
    if (ctx && ctx->sockfd >= 0) {
        close(ctx->sockfd);
        ctx->sockfd = -1;
    }
}
