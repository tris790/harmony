#ifndef HARMONY_UI_API_H
#define HARMONY_UI_API_H

#include "memory_arena.h"
#include <stdbool.h>

void UI_Init(MemoryArena *arena);
void UI_BeginFrame(int window_width, int window_height, int mouse_x, int mouse_y, bool mouse_down, char input_char);
void UI_EndFrame();

bool UI_Button(const char *label, int x, int y, int w, int h);
void UI_Label(const char *text, int x, int y);
bool UI_TextInput(const char *id, char *buffer, int buffer_size, int x, int y, int w, int h);

// Streaming status UI
void UI_DrawRecordingIndicator(int x, int y, float time);
void UI_DrawStreamStatus(int w, int h, float time, int frames_encoded, 
                         const char *target_ip, int res_w, int res_h, 
                         bool is_capturing);

#endif // HARMONY_UI_API_H
