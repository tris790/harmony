#include "../ui_api.h"
#include "render_api.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

typedef struct UIContext {
    int mouse_x;
    int mouse_y;
    bool mouse_down;
    bool mouse_pressed; // Single frame click
    int window_width;
    int window_height;
    
    // Focus
    const char *active_id;
    char last_char;
} UIContext;

static UIContext ui;

void UI_Init(MemoryArena *arena) {
    (void)arena;
    // Renderer is initialized by main (Render_Init) or implicitly if needed.
}

// OS_GetLastChar is external
// extern char OS_GetLastChar(WindowContext *window); 
// We are passing char via UI_BeginFrame now.

void UI_BeginFrame(int window_width, int window_height, int mouse_x, int mouse_y, bool mouse_down, char input_char) {
    ui.window_width = window_width;
    ui.window_height = window_height;
    Render_SetScreenSize(window_width, window_height);
    ui.mouse_x = mouse_x;
    ui.mouse_y = mouse_y;
    ui.mouse_pressed = mouse_down && !ui.mouse_down;
    ui.mouse_down = mouse_down;
    ui.last_char = input_char;
    
    if (ui.mouse_pressed) {
        ui.active_id = NULL; // Clear focus on click (widgets will claim if clicked)
    }
}

void UI_EndFrame() {
}

bool UI_Button(const char *text, int x, int y, int w, int h) {
    // Check Hover
    bool hover = (ui.mouse_x >= x && ui.mouse_x <= x + w &&
                  ui.mouse_y >= y && ui.mouse_y <= y + h);
    
    // Modern Palette
    // Normal: #313244 (0.19, 0.20, 0.27)
    // Hover: #45475a (0.27, 0.28, 0.35)
    // Active/Click: #585b70 (0.35, 0.36, 0.44)
    
    float r, g, b;
    if (hover && ui.mouse_down) {
         r = 0.35f; g = 0.36f; b = 0.44f;
    } else if (hover) {
         r = 0.27f; g = 0.28f; b = 0.35f;
    } else {
         r = 0.19f; g = 0.20f; b = 0.27f;
    }
    
    Render_DrawRoundedRect(x, y, w, h, 12.0f, r, g, b, 1.0f);
    
    // Draw Text
    int char_w = 16; 
    int char_h = 16;
    int text_len = strlen(text);
    int text_w = text_len * char_w;
    int text_x = x + (w - text_w) / 2;
    int text_y = y + (h - char_h) / 2;
    
    // Text Color: #cdd6f4 (0.80, 0.84, 0.96)
    Render_DrawText(text, text_x, text_y, 2.0f, 0.80f, 0.84f, 0.96f, 1.0f);
    
    return hover && ui.mouse_pressed;
}

void UI_Label(const char *text, int x, int y) {
    Render_DrawText(text, x, y, 2.0f, 0.90f, 0.90f, 0.95f, 1.0f);
}

bool UI_TextInput(const char *id, char *buffer, int buffer_size, int x, int y, int w, int h) {
    bool hover = (ui.mouse_x >= x && ui.mouse_x <= x + w &&
                  ui.mouse_y >= y && ui.mouse_y <= y + h);
    
    if (hover && ui.mouse_pressed) {
        ui.active_id = id;
    }
    
    bool is_active = (ui.active_id == id);
    
    // Background: #181825 (0.09, 0.09, 0.15)
    Render_DrawRoundedRect(x, y, w, h, 8.0f, 0.09f, 0.09f, 0.15f, 1.0f);
    
    // Border
    if (is_active) {
        // Active Border (Blue): #89b4fa (0.54, 0.71, 0.98)
        // Draw 2px border by drawing larger rect behind? Or drawing 4 lines?
        // Or since we have rounded rects, allow border in shader?
        // For now, let's just draw an outline via multiple rounded rects or just a colored rect behind.
        // Actually, drawing a slightly larger rect BEHIND is easiest for rounded border.
        // BUT we already drew background. Let's redraw 'border' first if I can change loop order? No.
        
        // Let's just draw a thin frame using lines for now or accept no rounded border?
        // Let's try drawing a slightly larger rect behind inside this function?
        // No, we are painter's algorithm.
        // Let's Draw the border first, then the background on top.
    }
    
    // Actually, let's do the Border by drawing a larger rect first.
    if (is_active) {
        // 2px border
        Render_DrawRoundedRect(x-2, y-2, w+4, h+4, 10.0f, 0.54f, 0.71f, 0.98f, 1.0f);
        // Then Redraw Background
        Render_DrawRoundedRect(x, y, w, h, 8.0f, 0.09f, 0.09f, 0.15f, 1.0f);
    } else {
        // Inactive Border: #45475a
        Render_DrawRoundedRect(x-1, y-1, w+2, h+2, 9.0f, 0.27f, 0.28f, 0.35f, 1.0f);
        Render_DrawRoundedRect(x, y, w, h, 8.0f, 0.09f, 0.09f, 0.15f, 1.0f);
    }

    // Input Handling
    bool changed = false;
    if (is_active && ui.last_char) {
        int len = strlen(buffer);
        if (ui.last_char == '\b') { // Backspace
            if (len > 0) {
                buffer[len - 1] = 0;
                changed = true;
            }
        } else if (len < buffer_size - 1) {
            // Append
            // Filter non-printable if needed, but for now allow basic
             if (ui.last_char >= 32 && ui.last_char <= 126) {
                buffer[len] = ui.last_char;
                buffer[len + 1] = 0;
                changed = true;
            }
        }
    }
    
    // Draw Text
    Render_DrawText(buffer, x + 10, y + (h/2) - 8, 2.0f, 0.9f, 0.9f, 0.9f, 1.0f);
    
    // Cursor
    if (is_active) {
         int cursor_x = x + 10 + (strlen(buffer) * 16);
         // Blinking?
         Render_DrawRect(cursor_x, y + (h/2) - 8, 2, 16, 0.8f, 0.8f, 0.9f, 1.0f);
    }
    
    return changed;
}

// Draw a pulsing recording indicator (red circle that pulses)
void UI_DrawRecordingIndicator(int x, int y, float time) {
    // Pulse effect: sin wave oscillates alpha between 0.6 and 1.0
    float pulse = 0.7f + 0.3f * sinf(time * 4.0f); // ~4 pulses per second
    
    // Draw outer glow (subtle)
    float glow_size = 16.0f + 4.0f * sinf(time * 4.0f);
    Render_DrawRoundedRect(x - glow_size/2, y - glow_size/2, glow_size, glow_size, glow_size/2, 
                           0.9f, 0.2f, 0.2f, pulse * 0.3f);
    
    // Draw main red circle
    Render_DrawRoundedRect(x - 8, y - 8, 16, 16, 8.0f, 
                           0.9f, 0.2f, 0.2f, pulse);
}

// Draw complete stream status UI
void UI_DrawStreamStatus(int w, int h, float time, int frames_encoded, 
                         const char *target_ip, int res_w, int res_h, 
                         bool is_capturing) {
    // Draw dark background - prevents flickering
    // Mocha Base: #1e1e2e
    Render_DrawRect(0, 0, w, h, 0.12f, 0.12f, 0.18f, 1.0f);
    
    int cx = w / 2;
    int cy = h / 2;
    
    // Status Container (centered card)
    int card_w = 500;
    int card_h = 300;
    int card_x = cx - card_w / 2;
    int card_y = cy - card_h / 2;
    
    // Card background (slightly lighter)
    Render_DrawRoundedRect(card_x, card_y, card_w, card_h, 16.0f, 
                           0.16f, 0.16f, 0.22f, 1.0f);
    
    // Recording indicator + Status text
    int indicator_x = card_x + 60;
    int status_y = card_y + 50;
    
    UI_DrawRecordingIndicator(indicator_x, status_y + 8, time);
    
    // Status text
    if (is_capturing) {
        // Greenish: STREAMING LIVE
        Render_DrawText("STREAMING LIVE", indicator_x + 30, status_y, 2.5f, 
                        0.65f, 0.89f, 0.63f, 1.0f);
    } else {
        // Yellow: Waiting for capture
        Render_DrawText("Waiting for capture...", indicator_x + 30, status_y, 2.0f, 
                        0.98f, 0.84f, 0.48f, 1.0f);
    }
    
    // Details section
    int details_y = card_y + 110;
    char buf[128];
    
    // Target IP
    snprintf(buf, sizeof(buf), "Target: %s", target_ip);
    Render_DrawText(buf, card_x + 40, details_y, 2.0f, 0.8f, 0.84f, 0.96f, 1.0f);
    
    // Frames encoded
    snprintf(buf, sizeof(buf), "Frames: %d", frames_encoded);
    Render_DrawText(buf, card_x + 40, details_y + 40, 2.0f, 0.8f, 0.84f, 0.96f, 1.0f);
    
    // Resolution
    if (res_w > 0 && res_h > 0) {
        snprintf(buf, sizeof(buf), "Resolution: %dx%d", res_w, res_h);
        Render_DrawText(buf, card_x + 40, details_y + 80, 2.0f, 0.8f, 0.84f, 0.96f, 1.0f);
    }
    
    // Help text at bottom
    Render_DrawText("Close window to stop streaming", card_x + 70, card_y + card_h - 50, 
                    1.5f, 0.5f, 0.52f, 0.6f, 1.0f);
}
