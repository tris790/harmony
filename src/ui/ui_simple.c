#include "../ui_api.h"
#include "../audio_api.h"
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

    // Layout
    bool next_centered;
    int next_centered_w;

    // Dropdown State
    char open_dropdown_id[64];
    AudioNodeInfo *dropdown_items;
    int dropdown_count;
    uint32_t *dropdown_selected_id;
    int dropdown_scroll_offset;
    int dropdown_x, dropdown_y, dropdown_w;
    int dropdown_header_h; // Store header height to offset list
    int mouse_scroll;
    bool overlay_consumed_click; 
} UIContext;

static UIContext ui;

// Internal helper to get/compute widget rect
static void UI_GetWidgetRect(int *x, int *y, int w, int h) {
    if (ui.next_centered) {
        *x = (ui.window_width - ui.next_centered_w) / 2;
        ui.next_centered = false;
    }
    // For now, we still allow explicit x/y if centered is not set, 
    // but the intention is to use helpers.
}

// API Expansion
void UI_CenterNext(int w) {
    ui.next_centered = true;
    ui.next_centered_w = w;
}

static void GetDropdownRect(int *x, int *y, int *w, int *h) {
    *x = ui.dropdown_x;
    *w = ui.dropdown_w;
    
    int item_h = 30;
    int max_view = 8;
    int display_count = (ui.dropdown_count < max_view) ? ui.dropdown_count : max_view;
    int list_h = display_count * item_h;
    
    int y_start = ui.dropdown_y + ui.dropdown_header_h; 
    
    if (y_start + list_h > ui.window_height && ui.dropdown_y - list_h > 0) {
         y_start = ui.dropdown_y - list_h;
    }
    
    *y = y_start;
    *h = list_h;
}

void UI_Init(MemoryArena *arena) {
    (void)arena;
}

void UI_BeginFrame(int window_width, int window_height, int mouse_x, int mouse_y, bool mouse_down, int mouse_scroll, char input_char) {
    ui.window_width = window_width;
    ui.window_height = window_height;
    Render_SetScreenSize(window_width, window_height);
    ui.mouse_x = mouse_x;
    ui.mouse_y = mouse_y;
    ui.mouse_pressed = mouse_down && !ui.mouse_down;
    ui.mouse_down = mouse_down;
    ui.mouse_scroll = mouse_scroll;
    ui.last_char = input_char;
    
    ui.next_centered = false;

    if (ui.mouse_pressed) {
        ui.active_id = NULL; 
        
        if (strlen(ui.open_dropdown_id) > 0) {
            int ox, oy, ow, oh;
            GetDropdownRect(&ox, &oy, &ow, &oh);
            
            bool inside_overlay = (ui.mouse_x >= ox && ui.mouse_x <= ox + ow &&
                                   ui.mouse_y >= oy && ui.mouse_y <= oy + oh);
                                   
             if (inside_overlay) {
                 ui.overlay_consumed_click = true;
                 ui.mouse_pressed = false; 
             } else {
                 ui.overlay_consumed_click = false;
             }
        } else {
            ui.overlay_consumed_click = false;
        }
    } else {
        ui.overlay_consumed_click = false;
    }
}

void UI_EndFrame() {
    if (strlen(ui.open_dropdown_id) > 0) {
        int item_h = 30;
        int max_view = 8;
        int display_count = (ui.dropdown_count < max_view) ? ui.dropdown_count : max_view;
        int list_h = display_count * item_h;
        
        int x, y_start, w, h_dummy;
        GetDropdownRect(&x, &y_start, &w, &h_dummy);

        bool click_action = ui.mouse_pressed || ui.overlay_consumed_click;
        
        if (click_action) {
             int x_rect, y_rect, w_rect, h_rect;
             GetDropdownRect(&x_rect, &y_rect, &w_rect, &h_rect);
             
             bool inside_list = (ui.mouse_x >= x_rect && ui.mouse_x <= x_rect + w_rect &&
                                 ui.mouse_y >= y_rect && ui.mouse_y <= y_rect + h_rect);
             
             if (!inside_list) {
                 if (ui.mouse_pressed) { 
                    ui.open_dropdown_id[0] = '\0'; 
                 }
             }
        }
        
        if (strlen(ui.open_dropdown_id) > 0) {
            Render_DrawRoundedRect(x, y_start, ui.dropdown_w, list_h, 12.0f, 0.09f, 0.09f, 0.15f, 1.0f);
            Render_DrawRoundedRect(x, y_start, ui.dropdown_w, list_h, 12.0f, 0.19f, 0.20f, 0.27f, 0.4f); 
            
            bool mouse_over_overlay = (ui.mouse_x >= x && ui.mouse_x <= x + ui.dropdown_w &&
                                       ui.mouse_y >= y_start && ui.mouse_y <= y_start + list_h);
            if (mouse_over_overlay && ui.mouse_scroll != 0) {
                ui.dropdown_scroll_offset -= ui.mouse_scroll;
            }

            int max_scroll = ui.dropdown_count - display_count;
            if (max_scroll < 0) max_scroll = 0;
            if (ui.dropdown_scroll_offset > max_scroll) ui.dropdown_scroll_offset = max_scroll;
            if (ui.dropdown_scroll_offset < 0) ui.dropdown_scroll_offset = 0;
            
            int start_idx = ui.dropdown_scroll_offset;
            int scrollbar_w = (ui.dropdown_count > display_count) ? 12 : 0;
            int item_active_w = ui.dropdown_w - scrollbar_w;
             
            for (int i = 0; i < display_count; ++i) {
                int idx = start_idx + i;
                int item_y = y_start + (i * item_h);
                
                bool hover = (ui.mouse_x >= x && ui.mouse_x <= x + item_active_w &&
                              ui.mouse_y >= item_y && ui.mouse_y <= item_y + item_h);
                
                bool is_selected = (*ui.dropdown_selected_id == ui.dropdown_items[idx].id);
                
                if (hover) {
                    Render_DrawRect(x + 5, item_y, item_active_w - 10, item_h, 0.27f, 0.28f, 0.35f, 1.0f);
                    if (ui.mouse_pressed || ui.overlay_consumed_click) {
                        *ui.dropdown_selected_id = ui.dropdown_items[idx].id;
                        ui.open_dropdown_id[0] = '\0'; 
                        ui.mouse_pressed = false; 
                        ui.overlay_consumed_click = false; 
                    }
                } else if (is_selected) {
                    Render_DrawRect(x + 5, item_y, item_active_w - 10, item_h, 0.35f, 0.36f, 0.44f, 1.0f);
                }
                
                char display_text[128];
                strncpy(display_text, ui.dropdown_items[idx].name, sizeof(display_text) - 1);
                display_text[sizeof(display_text) - 1] = '\0';
                
                int max_chars = (item_active_w - 15) / 10; 
                if ((int)strlen(display_text) > max_chars && max_chars > 3) {
                    display_text[max_chars - 3] = '.';
                    display_text[max_chars - 2] = '.';
                    display_text[max_chars - 1] = '.';
                    display_text[max_chars] = '\0';
                }
                Render_DrawText(display_text, x + 15, item_y + 6, 1.8f, 0.80f, 0.84f, 0.96f, 1.0f);
            }
            
            if (ui.dropdown_count > display_count) {
                int sb_w = 6;
                int sb_x = x + ui.dropdown_w - sb_w - 2;
                int sb_track_h = list_h;
                
                Render_DrawRect(sb_x, y_start, sb_w, sb_track_h, 0.1f, 0.1f, 0.12f, 1.0f);
                
                float handle_h_ratio = (float)display_count / (float)ui.dropdown_count;
                int handle_h = (int)(sb_track_h * handle_h_ratio);
                if (handle_h < 20) handle_h = 20;
                
                float scroll_ratio = (float)ui.dropdown_scroll_offset / (float)(ui.dropdown_count - display_count);
                int handle_y = y_start + (int)((sb_track_h - handle_h) * scroll_ratio);
                
                Render_DrawRoundedRect(sb_x, handle_y, sb_w, handle_h, 3.0f, 0.4f, 0.45f, 0.6f, 1.0f);
            }
        }
    }
}

bool UI_Button(const char *text, int x, int y, int w, int h) {
    if (ui.next_centered) {
        x = (ui.window_width - ui.next_centered_w) / 2;
        ui.next_centered = false;
    }

    bool hover = (ui.mouse_x >= x && ui.mouse_x <= x + w &&
                  ui.mouse_y >= y && ui.mouse_y <= y + h);
    
    float r, g, b;
    if (hover && ui.mouse_down) {
         r = 0.35f; g = 0.36f; b = 0.44f;
    } else if (hover) {
         r = 0.27f; g = 0.28f; b = 0.35f;
    } else {
         r = 0.19f; g = 0.20f; b = 0.27f;
    }
    
    Render_DrawRoundedRect(x, y, w, h, 12.0f, r, g, b, 1.0f);
    
    int char_w = 16; 
    int char_h = 16;
    float tw = Render_GetTextWidth(text, 2.0f);
    int text_x = x + (w - (int)tw) / 2;
    int text_y = y + (h - char_h) / 2;
    
    Render_DrawText(text, text_x, text_y, 2.0f, 0.80f, 0.84f, 0.96f, 1.0f);
    
    return hover && ui.mouse_pressed;
}

void UI_Label(const char *text, int x, int y, float scale) {
    if (ui.next_centered) {
        float tw = Render_GetTextWidth(text, scale);
        x = (ui.window_width - (int)tw) / 2;
        ui.next_centered = false;
    }
    Render_DrawText(text, x, y, scale, 0.90f, 0.90f, 0.95f, 1.0f);
}

bool UI_TextInput(const char *id, char *buffer, int buffer_size, int x, int y, int w, int h) {
    if (ui.next_centered) {
        x = (ui.window_width - ui.next_centered_w) / 2;
        ui.next_centered = false;
    }

    bool hover = (ui.mouse_x >= x && ui.mouse_x <= x + w &&
                  ui.mouse_y >= y && ui.mouse_y <= y + h);
    
    if (hover && ui.mouse_pressed) {
        ui.active_id = id;
    }
    
    bool is_active = (ui.active_id == id);
    
    if (is_active) {
        Render_DrawRoundedRect(x-2, y-2, w+4, h+4, 10.0f, 0.54f, 0.71f, 0.98f, 1.0f);
        Render_DrawRoundedRect(x, y, w, h, 8.0f, 0.09f, 0.09f, 0.15f, 1.0f);
    } else {
        Render_DrawRoundedRect(x-1, y-1, w+2, h+2, 9.0f, 0.27f, 0.28f, 0.35f, 1.0f);
        Render_DrawRoundedRect(x, y, w, h, 8.0f, 0.09f, 0.09f, 0.15f, 1.0f);
    }

    bool changed = false;
    if (is_active && ui.last_char) {
        int len = strlen(buffer);
        if (ui.last_char == '\b') {
            if (len > 0) {
                buffer[len - 1] = 0;
                changed = true;
            }
        } else if (len < buffer_size - 1) {
             if (ui.last_char >= 32 && ui.last_char <= 126) {
                buffer[len] = ui.last_char;
                buffer[len + 1] = 0;
                changed = true;
            }
        }
    }
    
    Render_DrawText(buffer, x + 10, y + (h/2) - 8, 2.0f, 0.9f, 0.9f, 0.9f, 1.0f);
    
    if (is_active) {
         int cursor_x = x + 10 + (strlen(buffer) * 16);
         Render_DrawRect(cursor_x, y + (h/2) - 8, 2, 16, 0.8f, 0.8f, 0.9f, 1.0f);
    }
    
    return changed;
}

bool UI_List(const char *id, AudioNodeInfo *items, int count, uint32_t *selected_id, int x, int y, int w, int h) {
    (void)id;
    Render_DrawRoundedRect(x, y, w, h, 5.0f, 0.05f, 0.05f, 0.08f, 1.0f); 
    
    Render_DrawRect(x, y, w, 2, 0.3f, 0.3f, 0.3f, 1.0f); 
    Render_DrawRect(x, y + h - 2, w, 2, 0.3f, 0.3f, 0.3f, 1.0f); 
    Render_DrawRect(x, y, 2, h, 0.3f, 0.3f, 0.3f, 1.0f); 
    Render_DrawRect(x + w - 2, y, 2, h, 0.3f, 0.3f, 0.3f, 1.0f); 

    int item_h = 30;
    int visible_items = h / item_h;
    
    int start_y = y + 5;
    bool changed = false;
    
    for (int i = 0; i < count; i++) {
        if (i >= visible_items) break;
        
        int item_y = start_y + (i * item_h);
        
        bool hover = (ui.mouse_x >= x && ui.mouse_x <= x + w &&
                      ui.mouse_y >= item_y && ui.mouse_y <= item_y + item_h);
        
        bool is_selected = (*selected_id == items[i].id);
        
        if (hover && ui.mouse_pressed) {
            *selected_id = items[i].id;
            changed = true;
        }
        
        if (is_selected) {
             Render_DrawRect(x + 2, item_y, w - 4, item_h - 2, 0.3f, 0.4f, 0.6f, 1.0f);
        } else if (hover) {
             Render_DrawRect(x + 2, item_y, w - 4, item_h - 2, 0.25f, 0.25f, 0.3f, 1.0f);
        }
        
        char display_text[64];
        strncpy(display_text, items[i].name, sizeof(display_text) - 1);
        display_text[sizeof(display_text) - 1] = '\0';
        
        int max_chars = (w - 20) / 12; 
        if ((int)strlen(display_text) > max_chars && max_chars > 3) {
            display_text[max_chars - 3] = '.';
            display_text[max_chars - 2] = '.';
            display_text[max_chars - 1] = '.';
            display_text[max_chars] = '\0';
        }
        
        Render_DrawText(display_text, x + 10, item_y + 6, 1.8f, 0.9f, 0.9f, 0.9f, 1.0f);
    }
    
    return changed;
}

void UI_DrawRecordingIndicator(int x, int y, float time) {
    float pulse = 0.7f + 0.3f * sinf(time * 4.0f); 
    
    float glow_size = 16.0f + 4.0f * sinf(time * 4.0f);
    Render_DrawRoundedRect(x - glow_size/2, y - glow_size/2, glow_size, glow_size, glow_size/2, 
                           0.9f, 0.2f, 0.2f, pulse * 0.3f);
    
    Render_DrawRoundedRect(x - 8, y - 8, 16, 16, 8.0f, 
                           0.9f, 0.2f, 0.2f, pulse);
}

void UI_DrawStreamStatus(int w, int h, float time, int frames_encoded, 
                         const char *target_ip, int res_w, int res_h, 
                         bool is_capturing) {
    Render_DrawRect(0, 0, w, h, 0.12f, 0.12f, 0.18f, 1.0f);
    
    int cx = w / 2;
    int cy = h / 2;
    
    int card_w = 500;
    int card_h = 300;
    int card_x = cx - card_w / 2;
    int card_y = cy - card_h / 2;
    
    Render_DrawRoundedRect(card_x, card_y, card_w, card_h, 16.0f, 
                           0.16f, 0.16f, 0.22f, 1.0f);
    
    int indicator_x = card_x + 60;
    int status_y = card_y + 50;
    
    UI_DrawRecordingIndicator(indicator_x, status_y + 8, time);
    
    if (is_capturing) {
        Render_DrawText("STREAMING LIVE", indicator_x + 30, status_y, 2.5f, 
                        0.65f, 0.89f, 0.63f, 1.0f);
    } else {
        Render_DrawText("Waiting for capture...", indicator_x + 30, status_y, 2.0f, 
                        0.98f, 0.84f, 0.48f, 1.0f);
    }
    
    int details_y = card_y + 110;
    char buf[128];
    
    snprintf(buf, sizeof(buf), "Target: %s", target_ip);
    Render_DrawText(buf, card_x + 40, details_y, 2.0f, 0.8f, 0.84f, 0.96f, 1.0f);
    
    snprintf(buf, sizeof(buf), "Frames: %d", frames_encoded);
    Render_DrawText(buf, card_x + 40, details_y + 40, 2.0f, 0.8f, 0.84f, 0.96f, 1.0f);
    
    if (res_w > 0 && res_h > 0) {
        snprintf(buf, sizeof(buf), "Resolution: %dx%d", res_w, res_h);
        Render_DrawText(buf, card_x + 40, details_y + 80, 2.0f, 0.8f, 0.84f, 0.96f, 1.0f);
    }
    
    Render_DrawText("Close window to stop streaming", card_x + 70, card_y + card_h - 50, 
                    1.5f, 0.5f, 0.52f, 0.6f, 1.0f);
}

bool UI_Dropdown(const char *id, AudioNodeInfo *items, int count, uint32_t *selected_id, int x, int y, int w, int h) {
    if (ui.next_centered) {
        x = (ui.window_width - ui.next_centered_w) / 2;
        ui.next_centered = false;
    }

    bool is_open = (strcmp(ui.open_dropdown_id, id) == 0);
    bool just_opened = false;
    
    bool hover = (ui.mouse_x >= x && ui.mouse_x <= x + w &&
                  ui.mouse_y >= y && ui.mouse_y <= y + h);
    
    if (hover && ui.mouse_pressed) {
        if (is_open) {
            ui.open_dropdown_id[0] = '\0'; 
        } else {
            strncpy(ui.open_dropdown_id, id, sizeof(ui.open_dropdown_id)-1);
            ui.dropdown_items = items;
            ui.dropdown_count = count;
            ui.dropdown_selected_id = selected_id;
            ui.dropdown_x = x;
            ui.dropdown_y = y;
            ui.dropdown_w = w;
            ui.dropdown_header_h = h; 
            ui.dropdown_scroll_offset = 0;
            just_opened = true;
        }
        ui.mouse_pressed = false; 
    }
    
    float r = 0.19f; float g = 0.20f; float b = 0.27f; 
    if (hover && ui.mouse_down) {
        r = 0.35f; g = 0.36f; b = 0.44f; 
    } else if (hover) {
        r = 0.27f; g = 0.28f; b = 0.35f; 
    }
    
    Render_DrawRoundedRect(x, y, w, h, 12.0f, r, g, b, 1.0f);
    Render_DrawRoundedRect(x, y, w, h, 12.0f, 0.45f, 0.47f, 0.58f, 0.15f); 
    
    const char *current_name = "Select Audio Source...";
    for (int i=0; i<count; ++i) {
        if (items[i].id == *selected_id) {
            current_name = items[i].name;
            break;
        }
    }
    
    char display_text[128];
    strncpy(display_text, current_name, sizeof(display_text)-1);
    
    int max_chars = (w - 40) / 10; 
    if ((int)strlen(display_text) > max_chars && max_chars > 3) {
        display_text[max_chars - 3] = '.';
        display_text[max_chars - 2] = '.';
        display_text[max_chars - 1] = '.';
        display_text[max_chars] = '\0';
    }
    
    Render_DrawText(display_text, x + 15, y + (h/2) - 8, 1.8f, 0.80f, 0.84f, 0.96f, 1.0f);
    Render_DrawText("v", x + w - 30, y + (h/2) - 10, 1.5f, 0.71f, 0.75f, 1.0f, 1.0f);

    return just_opened; 
}
