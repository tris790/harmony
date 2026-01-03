#ifndef HARMONY_CAPTURE_API_H
#define HARMONY_CAPTURE_API_H

#include "memory_arena.h"
#include <stdint.h>

typedef struct CaptureContext CaptureContext;

// Initialize capture from a specific PipeWire Node ID
CaptureContext* Capture_Init(MemoryArena *arena, uint32_t node_id);

// Poll for events (blocking/non-blocking depending on implementation, here non-blocking iteration)
void Capture_Poll(CaptureContext *ctx);

// Check if a new frame is ready
// Returns pointer to VideoFrame if ready, NULL otherwise.
// Note: This frame might be internal to the context and valid only until next Poll.
struct VideoFrame* Capture_GetFrame(CaptureContext *ctx);

#endif // HARMONY_CAPTURE_API_H
