#ifndef HARMONY_RENDER_API_H
#define HARMONY_RENDER_API_H

#include "../codec_api.h"
#include "../memory_arena.h"

// Initialize Renderer (Compile Shaders, etc.)
// Assumes OpenGL context is already active.
void Render_Init(MemoryArena *arena);

void Render_SetScreenSize(int width, int height);
void Render_Clear(float r, float g, float b, float a);

// Draw a Video Frame (YUV) to the screen (fills viewport)
void Render_DrawFrame(VideoFrame *frame, int target_width, int target_height);

// Draw a colored rectangle
void Render_DrawRect(float x, float y, float w, float h, float r, float g, float b, float a);

// Draw a rounded rectangle
void Render_DrawRoundedRect(float x, float y, float w, float h, float rad, float r, float g, float b, float a);

// Draw text (using internal font)
void Render_DrawText(const char *text, float x, float y, float scale, float r, float g, float b, float a);

#endif // HARMONY_RENDER_API_H
