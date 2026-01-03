#ifndef HARMONY_NETWORK_API_H
#define HARMONY_NETWORK_API_H

#include "memory_arena.h"
#include <stdbool.h>

typedef struct NetworkContext NetworkContext;

// Initialize UDP socket
NetworkContext* Net_Init(MemoryArena *arena, int port, bool is_server);

// Send data to a target
void Net_Send(NetworkContext *ctx, const char *ip, int port, void *data, size_t size);

// Receive data (non-blocking)
// Returns size of data read, or 0 if nothing
int Net_Recv(NetworkContext *ctx, void *buffer, size_t buffer_size, char *out_sender_ip, int *out_sender_port);

// Close socket and cleanup
void Net_Close(NetworkContext *ctx);

#endif // HARMONY_NETWORK_API_H
