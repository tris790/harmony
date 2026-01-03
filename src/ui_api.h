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
// bool UI_Slider(const char *label, float *value, float min, float max, ...);

#endif // HARMONY_UI_API_H
