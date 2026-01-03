#include "websocket.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <errno.h>

#define MAX_CLIENTS 10
#define WS_BUFFER_SIZE 4096

typedef struct WSClient {
    int sockfd;
    bool active;
    bool handshake_complete;
} WSClient;

struct WebSocketContext {
    int server_fd;
    WSClient clients[MAX_CLIENTS];
};

// --- Minimal SHA1 Implementation (Public Domain style) ---
#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void sha1_transform(uint32_t state[5], const uint8_t buffer[64]) {
    uint32_t w[80], a, b, c, d, e, i;
    for (i = 0; i < 16; i++) {
        w[i] = (buffer[i*4] << 24) | (buffer[i*4+1] << 16) | (buffer[i*4+2] << 8) | buffer[i*4+3];
    }
    for (i = 16; i < 80; i++) {
        w[i] = ROL(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
    }
    a = state[0]; b = state[1]; c = state[2]; d = state[3]; e = state[4];
    for (i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) { f = (b & c) | ((~b) & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6; }
        uint32_t temp = ROL(a, 5) + f + e + k + w[i];
        e = d; d = c; c = ROL(b, 30); b = a; a = temp;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e;
}

static void sha1(const uint8_t *data, size_t len, uint8_t hash[20]) {
    uint32_t state[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    uint8_t buffer[64];
    uint64_t bitlen = (uint64_t)len * 8;
    size_t i;

    for (i = 0; i < len / 64; i++) {
        sha1_transform(state, data + i * 64);
    }
    size_t remaining = len % 64;
    memcpy(buffer, data + i * 64, remaining);
    buffer[remaining++] = 0x80;
    if (remaining > 56) {
        memset(buffer + remaining, 0, 64 - remaining);
        sha1_transform(state, buffer);
        remaining = 0;
    }
    memset(buffer + remaining, 0, 56 - remaining);
    for (i = 0; i < 8; i++) buffer[56 + i] = (bitlen >> ((7 - i) * 8)) & 0xFF;
    sha1_transform(state, buffer);

    for (i = 0; i < 5; i++) {
        hash[i*4] = (state[i] >> 24) & 0xFF;
        hash[i*4+1] = (state[i] >> 16) & 0xFF;
        hash[i*4+2] = (state[i] >> 8) & 0xFF;
        hash[i*4+3] = state[i] & 0xFF;
    }
}

// --- Minimal Base64 Implementation ---
static void base64_encode(const uint8_t *data, size_t len, char *out) {
    const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i, j = 0;
    for (i = 0; i < len; i += 3) {
        uint32_t v = data[i] << 16;
        if (i + 1 < len) v |= data[i + 1] << 8;
        if (i + 2 < len) v |= data[i + 2];
        out[j++] = b64[(v >> 18) & 0x3F];
        out[j++] = b64[(v >> 12) & 0x3F];
        out[j++] = (i + 1 < len) ? b64[(v >> 6) & 0x3F] : '=';
        out[j++] = (i + 2 < len) ? b64[v & 0x3F] : '=';
    }
    out[j] = '\0';
}

// --- WebSocket Logic ---

WebSocketContext* WS_Init(MemoryArena *arena, int port) {
    WebSocketContext *ctx = PushStructZero(arena, WebSocketContext);
    
    ctx->server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (ctx->server_fd < 0) {
        perror("WS_Init: socket");
        return NULL;
    }

    int opt = 1;
    setsockopt(ctx->server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(ctx->server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("WS_Init: bind");
        close(ctx->server_fd);
        return NULL;
    }

    if (listen(ctx->server_fd, 5) < 0) {
        perror("WS_Init: listen");
        close(ctx->server_fd);
        return NULL;
    }

    // Set non-blocking
    int flags = fcntl(ctx->server_fd, F_GETFL, 0);
    fcntl(ctx->server_fd, F_SETFL, flags | O_NONBLOCK);

    return ctx;
}

static void perform_handshake(int fd, const char *key) {
    char combined[128];
    snprintf(combined, sizeof(combined), "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key);
    
    uint8_t hash[20];
    sha1((uint8_t*)combined, strlen(combined), hash);
    
    char encoded[32];
    base64_encode(hash, 20, encoded);
    
    char response[512];
    snprintf(response, sizeof(response),
             "HTTP/1.1 101 Switching Protocols\r\n"
             "Upgrade: websocket\r\n"
             "Connection: Upgrade\r\n"
             "Sec-WebSocket-Accept: %s\r\n\r\n",
             encoded);
             
    send(fd, response, strlen(response), 0);
}

void WS_Poll(WebSocketContext *ctx) {
    if (!ctx) return;

    // Accept new connections
    int new_fd = accept(ctx->server_fd, NULL, NULL);
    if (new_fd >= 0) {
        int flags = fcntl(new_fd, F_GETFL, 0);
        fcntl(new_fd, F_SETFL, flags | O_NONBLOCK);
        
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (!ctx->clients[i].active) {
                ctx->clients[i].sockfd = new_fd;
                ctx->clients[i].active = true;
                ctx->clients[i].handshake_complete = false;
                
                // Increase send buffer for video frames (e.g. 1MB)
                int sndbuf = 1024 * 1024;
                setsockopt(new_fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
                
                printf("WS: New connection [%d]\n", i);
                break;
            }
        }
    }

    // Process clients
    uint8_t buffer[WS_BUFFER_SIZE];
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!ctx->clients[i].active) continue;
        
        ssize_t n = recv(ctx->clients[i].sockfd, buffer, sizeof(buffer) - 1, 0);
        if (n > 0) {
            if (!ctx->clients[i].handshake_complete) {
                buffer[n] = '\0';
                char *key_start = strstr((char*)buffer, "Sec-WebSocket-Key: ");
                if (key_start) {
                    key_start += 19;
                    char *key_end = strstr(key_start, "\r\n");
                    if (key_end) {
                        *key_end = '\0';
                        perform_handshake(ctx->clients[i].sockfd, key_start);
                        ctx->clients[i].handshake_complete = true;
                        printf("WS: Handshake complete [%d]\n", i);
                    }
                }
            } else {
                // Handle control frames if needed (e.g. close)
                // Minimal: ignore incoming data for now
            }
        } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
            close(ctx->clients[i].sockfd);
            ctx->clients[i].active = false;
            printf("WS: Client disconnected [%d]\n", i);
        }
    }
}

void WS_Broadcast(WebSocketContext *ctx, const void *data, size_t size) {
    if (!ctx) return;
    
    // Construct WebSocket Frame
    // FIN=1, Opcode=2 (Binary)
    uint8_t header[10];
    int header_len = 2;
    header[0] = 0x82; 
    
    if (size < 126) {
        header[1] = (uint8_t)size;
    } else if (size < 65536) {
        header[1] = 126;
        header[2] = (size >> 8) & 0xFF;
        header[3] = size & 0xFF;
        header_len = 4;
    } else {
        header[1] = 127;
        // 64-bit size (only using lower 32 bits effectively)
        header[2] = 0; header[3] = 0; header[4] = 0; header[5] = 0;
        header[6] = (size >> 24) & 0xFF;
        header[7] = (size >> 16) & 0xFF;
        header[8] = (size >> 8) & 0xFF;
        header[9] = size & 0xFF;
        header_len = 10;
    }

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (ctx->clients[i].active && ctx->clients[i].handshake_complete) {
            struct iovec iov[2];
            iov[0].iov_base = header;
            iov[0].iov_len = header_len;
            iov[1].iov_base = (void*)data;
            iov[1].iov_len = size;

            ssize_t total_expected = header_len + size;
            ssize_t n = writev(ctx->clients[i].sockfd, iov, 2);
            
            if (n < total_expected) {
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    // Buffer full - Real-time stream: skip frame or disconnect? 
                    // Better to disconnect as protocol state is now unknown if partial 
                    // (although writev is more 'atomic' at the system call level, 
                    // it doesn't guarantee the whole amount is sent if the pipe is full).
                    printf("WS: Send buffer full, disconnecting client [%d]\n", i);
                } else {
                    printf("WS: Write error or partial write (%zd/%zd), disconnecting [%d]\n", n, total_expected, i);
                }
                close(ctx->clients[i].sockfd);
                ctx->clients[i].active = false;
            }
        }
    }
}

void WS_Shutdown(WebSocketContext *ctx) {
    if (!ctx) return;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (ctx->clients[i].active) {
            close(ctx->clients[i].sockfd);
        }
    }
    if (ctx->server_fd >= 0) close(ctx->server_fd);
}
