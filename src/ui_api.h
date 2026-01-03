#ifndef HARMONY_UI_API_H
#define HARMONY_UI_API_H

#include "memory_arena.h"
#include <stdbool.h>

void UI_Init(MemoryArena *arena);
void UI_BeginFrame(int window_width, int window_height, int mouse_x, int mouse_y, bool mouse_down, int mouse_scroll, char input_char, bool paste_requested, bool ctrl_held, bool shift_held);
void UI_EndFrame();

void UI_CenterNext(int width);

bool UI_Button(const char *label, int x, int y, int w, int h);
void UI_Label(const char *text, int x, int y, float scale);
typedef enum {
    UI_INPUT_NONE     = 0,
    UI_INPUT_PASSWORD = (1 << 0),
    UI_INPUT_NUMERIC  = (1 << 1),
} UI_TextInputFlags;

bool UI_TextInput(const char *id, char *buffer, int buffer_size, int x, int y, int w, int h, UI_TextInputFlags flags);
#include "audio_api.h" // For AudioNodeInfo
bool UI_List(const char *id, AudioNodeInfo *items, int count, uint32_t *selected_id, int x, int y, int w, int h);
bool UI_Dropdown(const char *id, AudioNodeInfo *items, int count, uint32_t *selected_id, int x, int y, int w, int h);

// Streaming status UI
void UI_DrawRecordingIndicator(int x, int y, float time);
void UI_DrawStreamStatus(int w, int h, float time, int frames_encoded, 
                         const char *target_ip, int res_w, int res_h, 
                         bool is_capturing);

#endif // HARMONY_UI_API_H
