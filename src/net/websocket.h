#ifndef HARMONY_WEBSOCKET_H
#define HARMONY_WEBSOCKET_H

#include "../memory_arena.h"
#include <stdint.h>
#include <stdbool.h>

typedef struct WebSocketContext WebSocketContext;

// Initialize WebSocket server on specified port
WebSocketContext* WS_Init(MemoryArena *arena, int port);

// Broadcast binary data to all connected clients
void WS_Broadcast(WebSocketContext *ctx, uint32_t frame_id, const void *data, size_t size);

// Poll for new connections and handle incoming control frames (ping/close)
// Should be called every frame
void WS_Poll(WebSocketContext *ctx);

// Cleanup
void WS_Shutdown(WebSocketContext *ctx);

#endif // HARMONY_WEBSOCKET_H
