#include "../ui_api.h"
#include "../os_api.h"
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
    
    OS_CursorType next_cursor;
    bool paste_requested;
    bool ctrl_held;
    bool shift_held;

    // Advanced Input State
    int cursor_pos;
    int selection_pos; // -1 if no selection, otherwise marks the other end of selection (virtual anchor)
    char undo_buffer[256];
    char redo_buffer[256];
    bool has_undo;
    const char *last_active_id;
    
    MemoryArena *arena;
} UIContext;

static UIContext ui;

// --- Style Constants ---
static const float UI_COLOR_BG_NORMAL[4]   = {0.19f, 0.20f, 0.27f, 1.0f};
static const float UI_COLOR_BG_HOVER[4]    = {0.27f, 0.28f, 0.35f, 1.0f};
static const float UI_COLOR_BG_ACTIVE[4]   = {0.35f, 0.36f, 0.44f, 1.0f};

static const float UI_COLOR_TEXT_NORMAL[4] = {0.80f, 0.84f, 0.96f, 1.0f};

static const float UI_CORNER_RADIUS        = 12.0f;
static const int   UI_ITEM_HEIGHT          = 30;
static const int   UI_SCROLLBAR_WIDTH      = 6;
static const float UI_INPUT_CORNER_RADIUS  = 8.0f;

// New Style Constants
static const float UI_COLOR_SELECTION[4] = {0.26f, 0.42f, 0.60f, 0.7f}; // Selection blue

// --- Helper Functions ---
static const float UI_COLOR_INPUT_BG_NORMAL[4]   = {0.09f, 0.09f, 0.15f, 1.0f}; // Dark background for inputs
static const float UI_COLOR_INPUT_BORDER_NORMAL[4] = {0.27f, 0.28f, 0.35f, 1.0f};
static const float UI_COLOR_INPUT_BORDER_ACTIVE[4] = {0.54f, 0.71f, 0.98f, 1.0f};

static const float UI_COLOR_SCROLLBAR_TRACK[4]   = {0.1f, 0.1f, 0.12f, 1.0f};
static const float UI_COLOR_SCROLLBAR_HANDLE[4]  = {0.4f, 0.45f, 0.6f, 1.0f};

static const float UI_DROPDOWN_BG_BACK[4]        = {0.09f, 0.09f, 0.15f, 1.0f};
static const float UI_DROPDOWN_BG_FRONT[4]       = {0.19f, 0.20f, 0.27f, 0.4f};

// --- Helper Functions ---

static void UI_GetRect(int *x, int *y, int w, int h) {
    (void)y; // Unused
    (void)h; // Unused
    if (ui.next_centered) {
        int width_to_use = (ui.next_centered_w > 0) ? ui.next_centered_w : w;
        *x = (ui.window_width - width_to_use) / 2;
        ui.next_centered = false;
        ui.next_centered_w = 0;
    }
}

static bool UI_IsHovered(int x, int y, int w, int h) {
    return (ui.mouse_x >= x && ui.mouse_x <= x + w &&
            ui.mouse_y >= y && ui.mouse_y <= y + h);
}

typedef enum {
    UI_STATE_NORMAL,
    UI_STATE_HOVER,
    UI_STATE_ACTIVE
} UI_WidgetState;

static void UI_DrawWidgetBackground(int x, int y, int w, int h, UI_WidgetState state, float corner_radius) {
    const float *col;
    switch (state) {
        case UI_STATE_HOVER:  col = UI_COLOR_BG_HOVER; break;
        case UI_STATE_ACTIVE: col = UI_COLOR_BG_ACTIVE; break;
        default:              col = UI_COLOR_BG_NORMAL; break;
    }
    Render_DrawRoundedRect(x, y, w, h, corner_radius, col[0], col[1], col[2], col[3]);
}

static void UI_DrawTruncatedText(const char *text, int x, int y, int w, int h, bool center) {
    (void)center; // Unused for now
    char display_text[256];
    strncpy(display_text, text, sizeof(display_text) - 1);
    display_text[sizeof(display_text) - 1] = '\0';

    // Check if full text fits
    float full_width = Render_GetTextWidth(display_text, 1.8f);
    int available_width = w - 20; // Padding

    if ((int)full_width > available_width) {
        int len = strlen(display_text);
        if (len > 3) {
             while (len > 3) {
                 display_text[len] = '\0';
                 char temp[300];
                 snprintf(temp, sizeof(temp), "%s...", display_text);
                 if ((int)Render_GetTextWidth(temp, 1.8f) <= available_width) {
                     strcpy(display_text, temp);
                     break;
                 }
                 len--;
                 display_text[len] = '\0'; // shorten for next iteration checks
             }
        }
    }

    // Default to ~vertical center
    int text_y = y + (h / 2) - 8; 
    // If exact centering logic differs per widget, we might need to parameterize y-offset or alignment
    
    // Using standard colors
    Render_DrawText(display_text, x + 10, text_y, 1.8f, UI_COLOR_TEXT_NORMAL[0], UI_COLOR_TEXT_NORMAL[1], UI_COLOR_TEXT_NORMAL[2], UI_COLOR_TEXT_NORMAL[3]);
}

// Internal helper to get/compute widget rect (Deprecated/Legacy wrapper if needed, but we typically use UI_GetRect inline)
static void UI_GetWidgetRect(int *x, int *y, int w, int h) {
    UI_GetRect(x, y, w, h);
}

// API Expansion
void UI_CenterNext(int w) {
    ui.next_centered = true;
    ui.next_centered_w = w;
}

static void GetDropdownRect(int *x, int *y, int *w, int *h) {
    *x = ui.dropdown_x;
    *w = ui.dropdown_w;
    
    int item_h = UI_ITEM_HEIGHT;
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

static void UI_DrawScrollbar(int x, int y, int h, int content_h, int view_h, int scroll_offset) {
    if (content_h <= view_h) return;

    int sb_w = UI_SCROLLBAR_WIDTH;
    int sb_x = x - sb_w - 2; // Position relative to right edge of container given as x
    int sb_track_h = h;
    
    Render_DrawRect(sb_x, y, sb_w, sb_track_h, UI_COLOR_SCROLLBAR_TRACK[0], UI_COLOR_SCROLLBAR_TRACK[1], UI_COLOR_SCROLLBAR_TRACK[2], UI_COLOR_SCROLLBAR_TRACK[3]);
    
    float handle_h_ratio = (float)view_h / (float)content_h;
    int handle_h = (int)(sb_track_h * handle_h_ratio);
    if (handle_h < 20) handle_h = 20;
    
    (void)scroll_offset;
    // Assuming scroll_offset is in "items" units not pixels in the current codebase logic...
    // Wait, the current logic calculates handle position based on item index offset.
    // The previous code was:
    // float scroll_ratio = (float)ui.dropdown_scroll_offset / (float)(ui.dropdown_count - display_count);
    // Let's adapt the helper to take normalized ratio or sufficient info.
    // Actually, passing ratio might be cleaner.
}
// Redefining to match existing logic better
static void UI_DrawScrollbarV(int x, int y, int h, int total_items, int visible_items, int scroll_offset) {
    if (total_items <= visible_items) return;

    int sb_w = UI_SCROLLBAR_WIDTH;
    int sb_x = x - sb_w - 2;
    
    Render_DrawRect(sb_x, y, sb_w, h, UI_COLOR_SCROLLBAR_TRACK[0], UI_COLOR_SCROLLBAR_TRACK[1], UI_COLOR_SCROLLBAR_TRACK[2], UI_COLOR_SCROLLBAR_TRACK[3]);
    
    float handle_h_ratio = (float)visible_items / (float)total_items;
    int handle_h = (int)(h * handle_h_ratio);
    if (handle_h < 20) handle_h = 20;
    
    float scroll_ratio = (float)scroll_offset / (float)(total_items - visible_items);
    int handle_y = y + (int)((h - handle_h) * scroll_ratio);
    
    Render_DrawRoundedRect(sb_x, handle_y, sb_w, handle_h, 3.0f, UI_COLOR_SCROLLBAR_HANDLE[0], UI_COLOR_SCROLLBAR_HANDLE[1], UI_COLOR_SCROLLBAR_HANDLE[2], UI_COLOR_SCROLLBAR_HANDLE[3]);
}

void UI_Init(MemoryArena *arena) {
    ui.arena = arena;
}

void UI_BeginFrame(int window_width, int window_height, int mouse_x, int mouse_y, bool mouse_down, int mouse_scroll, char input_char, bool paste_requested, bool ctrl_held, bool shift_held) {
    ui.window_width = window_width;
    ui.window_height = window_height;
    Render_SetScreenSize(window_width, window_height);
    ui.mouse_x = mouse_x;
    ui.mouse_y = mouse_y;
    ui.mouse_pressed = mouse_down && !ui.mouse_down;
    ui.mouse_down = mouse_down;
    ui.mouse_scroll = mouse_scroll;
    ui.last_char = input_char;
    ui.paste_requested = paste_requested;
    ui.ctrl_held = ctrl_held;
    ui.shift_held = shift_held;
    
    ui.next_centered = false;
    ui.next_cursor = OS_CURSOR_ARROW;

    if (ui.mouse_pressed) {
        ui.active_id = NULL; 
        
        if (strlen(ui.open_dropdown_id) > 0) {
            int ox, oy, ow, oh;
            GetDropdownRect(&ox, &oy, &ow, &oh);
            
            bool inside_overlay = UI_IsHovered(ox, oy, ow, oh);
                                   
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
        int item_h = UI_ITEM_HEIGHT;
        int max_view = 8;
        int display_count = (ui.dropdown_count < max_view) ? ui.dropdown_count : max_view;
        int list_h = display_count * item_h;
        
        int x, y_start, w, h_dummy;
        GetDropdownRect(&x, &y_start, &w, &h_dummy);

        bool click_action = ui.mouse_pressed || ui.overlay_consumed_click;
        
        if (click_action) {
             int x_rect, y_rect, w_rect, h_rect;
             GetDropdownRect(&x_rect, &y_rect, &w_rect, &h_rect);
             
             bool inside_list = UI_IsHovered(x_rect, y_rect, w_rect, h_rect);
             
             if (!inside_list) {
                 if (ui.mouse_pressed) { 
                    ui.open_dropdown_id[0] = '\0'; 
                 }
             }
        }
        
        if (strlen(ui.open_dropdown_id) > 0) {
            Render_DrawRoundedRect(x, y_start, ui.dropdown_w, list_h, UI_CORNER_RADIUS, UI_DROPDOWN_BG_BACK[0], UI_DROPDOWN_BG_BACK[1], UI_DROPDOWN_BG_BACK[2], UI_DROPDOWN_BG_BACK[3]);
            Render_DrawRoundedRect(x, y_start, ui.dropdown_w, list_h, UI_CORNER_RADIUS, UI_DROPDOWN_BG_FRONT[0], UI_DROPDOWN_BG_FRONT[1], UI_DROPDOWN_BG_FRONT[2], UI_DROPDOWN_BG_FRONT[3]);
            
            bool mouse_over_overlay = UI_IsHovered(x, y_start, ui.dropdown_w, list_h);
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
                
                bool hover = UI_IsHovered(x, item_y, item_active_w, item_h);
                
                if (hover) ui.next_cursor = OS_CURSOR_HAND;
                
                bool is_selected = (*ui.dropdown_selected_id == ui.dropdown_items[idx].id);
                
                if (hover) {
                    Render_DrawRect(x + 5, item_y, item_active_w - 10, item_h, UI_COLOR_BG_HOVER[0], UI_COLOR_BG_HOVER[1], UI_COLOR_BG_HOVER[2], UI_COLOR_BG_HOVER[3]);
                    if (ui.mouse_pressed || ui.overlay_consumed_click) {
                        *ui.dropdown_selected_id = ui.dropdown_items[idx].id;
                        ui.open_dropdown_id[0] = '\0'; 
                        ui.mouse_pressed = false; 
                        ui.overlay_consumed_click = false; 
                    }
                } else if (is_selected) {
                    Render_DrawRect(x + 5, item_y, item_active_w - 10, item_h, UI_COLOR_BG_ACTIVE[0], UI_COLOR_BG_ACTIVE[1], UI_COLOR_BG_ACTIVE[2], UI_COLOR_BG_ACTIVE[3]);
                }
                
                UI_DrawTruncatedText(ui.dropdown_items[idx].name, x + 5, item_y, item_active_w - 10, item_h, false); // Adjusted rect for padding
            }
            
            if (ui.dropdown_count > display_count) {
                int sb_x = x + ui.dropdown_w; 
                UI_DrawScrollbarV(sb_x, y_start, list_h, ui.dropdown_count, display_count, ui.dropdown_scroll_offset);
            }
        }
    }
    OS_SetCursor(NULL, ui.next_cursor);
}

bool UI_Button(const char *text, int x, int y, int w, int h) {
    float tw = Render_GetTextWidth(text, 2.0f);
    int min_w = (int)tw + 40; // Padding
    if (w < min_w) w = min_w;

    UI_GetRect(&x, &y, w, h);

    bool hover = UI_IsHovered(x, y, w, h);
    
    if (hover) ui.next_cursor = OS_CURSOR_HAND;
    
    UI_WidgetState state = UI_STATE_NORMAL;
    if (hover) {
        state = ui.mouse_down ? UI_STATE_ACTIVE : UI_STATE_HOVER;
    }
    
    UI_DrawWidgetBackground(x, y, w, h, state, UI_CORNER_RADIUS);
    
    int char_h = 16;
    int text_x = x + (w - (int)tw) / 2;
    int text_y = y + (h - char_h) / 2;
    
    Render_DrawText(text, text_x, text_y, 2.0f, UI_COLOR_TEXT_NORMAL[0], UI_COLOR_TEXT_NORMAL[1], UI_COLOR_TEXT_NORMAL[2], UI_COLOR_TEXT_NORMAL[3]);
    
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

bool UI_TextInput(const char *id, char *buffer, int buffer_size, int x, int y, int w, int h, UI_TextInputFlags flags) {
    UI_GetRect(&x, &y, w, h);
    bool hover = UI_IsHovered(x, y, w, h);
    
    if (hover) {
        ui.next_cursor = OS_CURSOR_TEXT;
        if (ui.mouse_pressed) {
            ui.active_id = id;
            // Calculate cursor position from mouse X
            // Simple approximation: check width of substrings until > mouse_local_x
            // Ideally we'd do a binary search or precise glyph measure logic
            // For now, linear scan is fine for short strings
            int len = strlen(buffer);
            int mouse_local_x = ui.mouse_x - (x + 10);
            int best_i = 0;
            float best_dist = 10000.0f;
            
            // Password masking for measurement?
            char measure_buf[256];
            if (flags & UI_INPUT_PASSWORD) {
                for(int i=0; i<len; ++i) measure_buf[i] = '*';
                measure_buf[len] = 0;
            } else {
                strcpy(measure_buf, buffer);
            }

            for (int i = 0; i <= len; i++) {
                char temp[256];
                strncpy(temp, measure_buf, i);
                temp[i] = 0;
                float tw = Render_GetTextWidth(temp, 2.25f);
                float dist = fabsf(tw - (float)mouse_local_x);
                if (dist < best_dist) {
                    best_dist = dist;
                    best_i = i;
                }
            }
            ui.cursor_pos = best_i;
            if (!ui.shift_held) {
                ui.selection_pos = best_i; // Collapse selection
            }
        } else if (ui.mouse_down && ui.active_id == id) {
            // Drag selection
            int len = strlen(buffer);
            int mouse_local_x = ui.mouse_x - (x + 10);
            int best_i = 0;
            float best_dist = 10000.0f;
            
            // Re-measure like above
            char measure_buf[256];
            if (flags & UI_INPUT_PASSWORD) {
                for(int i=0; i<len; ++i) measure_buf[i] = '*';
                measure_buf[len] = 0;
            } else {
                strcpy(measure_buf, buffer);
            }

            for (int i = 0; i <= len; i++) {
                char temp[256];
                strncpy(temp, measure_buf, i);
                temp[i] = 0;
                float tw = Render_GetTextWidth(temp, 2.25f);
                float dist = fabsf(tw - (float)mouse_local_x);
                if (dist < best_dist) {
                    best_dist = dist;
                    best_i = i;
                }
            }
            ui.cursor_pos = best_i;
            // selection_pos remains as the anchor
        }
    }
    
    bool is_active = (ui.active_id == id);
    
    if (is_active) {
        Render_DrawRoundedRect(x-2, y-2, w+4, h+4, UI_INPUT_CORNER_RADIUS+2.0f, UI_COLOR_INPUT_BORDER_ACTIVE[0], UI_COLOR_INPUT_BORDER_ACTIVE[1], UI_COLOR_INPUT_BORDER_ACTIVE[2], UI_COLOR_INPUT_BORDER_ACTIVE[3]);
        Render_DrawRoundedRect(x, y, w, h, UI_INPUT_CORNER_RADIUS, UI_COLOR_INPUT_BG_NORMAL[0], UI_COLOR_INPUT_BG_NORMAL[1], UI_COLOR_INPUT_BG_NORMAL[2], UI_COLOR_INPUT_BG_NORMAL[3]);
    } else {
        Render_DrawRoundedRect(x-1, y-1, w+2, h+2, UI_INPUT_CORNER_RADIUS+1.0f, UI_COLOR_INPUT_BORDER_NORMAL[0], UI_COLOR_INPUT_BORDER_NORMAL[1], UI_COLOR_INPUT_BORDER_NORMAL[2], UI_COLOR_INPUT_BORDER_NORMAL[3]);
        Render_DrawRoundedRect(x, y, w, h, UI_INPUT_CORNER_RADIUS, UI_COLOR_INPUT_BG_NORMAL[0], UI_COLOR_INPUT_BG_NORMAL[1], UI_COLOR_INPUT_BG_NORMAL[2], UI_COLOR_INPUT_BG_NORMAL[3]);
    }

    bool changed = false;
    if (is_active) {
        int len = strlen(buffer);
        if (id != ui.last_active_id) {
            ui.last_active_id = id;
            ui.cursor_pos = len;
            ui.selection_pos = len;
            ui.has_undo = false;
            ui.undo_buffer[0] = 0;
            ui.redo_buffer[0] = 0;
        }

        if (ui.cursor_pos < 0) ui.cursor_pos = 0;
        if (ui.cursor_pos > len) ui.cursor_pos = len;
        
        // Handle Select All (Ctrl+A)
        if (ui.ctrl_held && (ui.last_char == 'a' || ui.last_char == '\x01')) { // check for 'a' or Ctrl-A code if any
             ui.selection_pos = 0;
             ui.cursor_pos = len;
             ui.last_char = 0; // Consume
        }

        if (ui.last_char) {
            bool typing = (ui.last_char >= 32 && ui.last_char <= 126);
            if (typing && (flags & UI_INPUT_NUMERIC)) {
                if (!((ui.last_char >= '0' && ui.last_char <= '9') || ui.last_char == '.')) {
                    typing = false;
                }
            }

            bool backspacing = (ui.last_char == '\b' || ui.last_char == '\x7f' || ui.last_char == '\x11' || ui.last_char == '\x12' || ui.last_char == '\x13' || ui.last_char == '\x14');
            bool editing = (ui.last_char == '\x1a' || ui.last_char == '\x19');

            // Handling Selection Delete/Overwrite
            bool has_selection = (ui.cursor_pos != ui.selection_pos);
            if (has_selection && (typing || ui.last_char == '\b' || ui.last_char == '\x7f')) {
                 int start = (ui.cursor_pos < ui.selection_pos) ? ui.cursor_pos : ui.selection_pos;
                 int end = (ui.cursor_pos > ui.selection_pos) ? ui.cursor_pos : ui.selection_pos;
                 
                 // Delete selection
                 memmove(buffer + start, buffer + end, len - end + 1);
                 len = strlen(buffer);
                 ui.cursor_pos = start;
                 ui.selection_pos = start;
                 changed = true;
                 
                 // If it was just backspace/delete, we are done (selection is gone)
                 if (ui.last_char == '\b' || ui.last_char == '\x7f') {
                     typing = false; 
                     backspacing = false; // Consumed
                 }
            }

            if (typing || backspacing) {
                if (!editing) {
                    if (!ui.has_undo) {
                        strncpy(ui.undo_buffer, buffer, sizeof(ui.undo_buffer)-1);
                        ui.has_undo = true;
                    }
                    if (ui.last_char == ' ' || ui.last_char == '.') {
                        ui.has_undo = false; // Commit current session
                    }
                    ui.redo_buffer[0] = 0;
                }
            }

            if (ui.last_char == '\x11') { // Left
                if (ui.cursor_pos > 0) ui.cursor_pos--;
                if (!ui.shift_held) ui.selection_pos = ui.cursor_pos;
            } else if (ui.last_char == '\x12') { // Right
                if (ui.cursor_pos < len) ui.cursor_pos++;
                if (!ui.shift_held) ui.selection_pos = ui.cursor_pos;
            } else if (ui.last_char == '\x13') { // Home
                ui.cursor_pos = 0;
                if (!ui.shift_held) ui.selection_pos = ui.cursor_pos;
            } else if (ui.last_char == '\x14') { // End
                ui.cursor_pos = len;
                if (!ui.shift_held) ui.selection_pos = ui.cursor_pos;
            } else if (ui.last_char == '\x1a') { // Undo
                if (ui.has_undo || (ui.undo_buffer[0] != 0)) {
                    char tmp[256];
                    strncpy(tmp, buffer, 255);
                    tmp[255] = 0;
                    strncpy(buffer, ui.undo_buffer, buffer_size - 1);
                    buffer[buffer_size - 1] = 0;
                    strncpy(ui.redo_buffer, tmp, 255);
                    ui.redo_buffer[255] = 0;
                    ui.cursor_pos = strlen(buffer);
                    ui.selection_pos = ui.cursor_pos; 
                    ui.has_undo = false;
                    changed = true;
                }
            } else if (ui.last_char == '\x19') { // Redo
                if (ui.redo_buffer[0] != 0) {
                    char tmp[256];
                    strncpy(tmp, buffer, 255);
                    tmp[255] = 0;
                    strncpy(buffer, ui.redo_buffer, buffer_size - 1);
                    buffer[buffer_size - 1] = 0;
                    strncpy(ui.undo_buffer, tmp, 255);
                    ui.undo_buffer[255] = 0;
                    ui.cursor_pos = strlen(buffer);
                    ui.selection_pos = ui.cursor_pos;
                    changed = true;
                }
            } else if (ui.last_char == '\b' || ui.last_char == '\x7f') {
                if (ui.last_char == '\b') {
                    if (ui.cursor_pos > 0) {
                        if (ui.ctrl_held) {
                            int start = ui.cursor_pos;
                            while (start > 0 && (buffer[start-1] == '.' || buffer[start-1] == ' ')) start--;
                            while (start > 0 && (buffer[start-1] != '.' && buffer[start-1] != ' ')) start--;
                            memmove(buffer + start, buffer + ui.cursor_pos, len - ui.cursor_pos + 1);
                            ui.cursor_pos = start;
                        } else {
                            memmove(buffer + ui.cursor_pos - 1, buffer + ui.cursor_pos, len - ui.cursor_pos + 1);
                            ui.cursor_pos--;
                        }
                        ui.selection_pos = ui.cursor_pos;
                        changed = true;
                    }
                } else { // Delete
                    if (ui.cursor_pos < len) {
                        if (ui.ctrl_held) {
                            int end = ui.cursor_pos;
                            while (end < len && (buffer[end] == '.' || buffer[end] == ' ')) end++;
                            while (end < len && (buffer[end] != '.' && buffer[end] != ' ')) end++;
                            memmove(buffer + ui.cursor_pos, buffer + end, len - end + 1);
                        } else {
                            memmove(buffer + ui.cursor_pos, buffer + ui.cursor_pos + 1, len - ui.cursor_pos);
                        }
                        ui.selection_pos = ui.cursor_pos; // Reset selection ? Or keep it? Standard behavior is reset.
                        changed = true;
                    }
                }
            } else if (typing && len < buffer_size - 1) {
                memmove(buffer + ui.cursor_pos + 1, buffer + ui.cursor_pos, len - ui.cursor_pos + 1);
                buffer[ui.cursor_pos] = ui.last_char;
                ui.cursor_pos++;
                ui.selection_pos = ui.cursor_pos;
                changed = true;
            }
        }
        
        if (ui.paste_requested) {
            // Remove selection first if any
            if (ui.cursor_pos != ui.selection_pos) {
                 int start = (ui.cursor_pos < ui.selection_pos) ? ui.cursor_pos : ui.selection_pos;
                 int end = (ui.cursor_pos > ui.selection_pos) ? ui.cursor_pos : ui.selection_pos;
                 memmove(buffer + start, buffer + end, len - end + 1);
                 len = strlen(buffer);
                 ui.cursor_pos = start;
                 ui.selection_pos = start;
                 changed = true;
            }

            TemporaryMemory temp = BeginTemporaryMemory(ui.arena);
            const char *clipboard = OS_GetClipboardText(ui.arena);
            if (clipboard && clipboard[0]) {
                int len = strlen(buffer);
                int clip_len = strlen(clipboard);
                int remaining_space = buffer_size - 1 - len;
                int to_copy = (clip_len < remaining_space) ? clip_len : remaining_space;
                
                if (to_copy > 0) {
                    // Make space for the pasted text
                    memmove(buffer + ui.cursor_pos + to_copy, buffer + ui.cursor_pos, len - ui.cursor_pos + 1);
                    // Insert the pasted text
                    memcpy(buffer + ui.cursor_pos, clipboard, to_copy);
                    buffer[len + to_copy] = 0; // Null-terminate the new string
                    ui.cursor_pos += to_copy;
                    ui.selection_pos = ui.cursor_pos;
                    changed = true;
                }
            }
            EndTemporaryMemory(temp);
            ui.paste_requested = false;
        }
    }

    // Prepare display text (handle password masking)
    char display_buf[256];
    if (flags & UI_INPUT_PASSWORD) {
        int len = strlen(buffer);
        for (int i = 0; i < len && i < 255; i++) display_buf[i] = '*';
        display_buf[len < 255 ? len : 255] = 0;
    } else {
        strncpy(display_buf, buffer, sizeof(display_buf)-1);
        display_buf[sizeof(display_buf)-1] = 0;
    }
    
    // Draw Selection
    // If active and selection exists
    if (is_active && ui.cursor_pos != ui.selection_pos) {
        int start = (ui.cursor_pos < ui.selection_pos) ? ui.cursor_pos : ui.selection_pos;
        int end = (ui.cursor_pos > ui.selection_pos) ? ui.cursor_pos : ui.selection_pos;
        
        char sub_start[256];
        char sub_end[256];
        memset(sub_start, 0, 256);
        memset(sub_end, 0, 256);
        
        // We need to measure partial strings of display_buf to get X positions
        strncpy(sub_start, display_buf, start);
        strncpy(sub_end, display_buf, end);
        
        float x_start = Render_GetTextWidth(sub_start, 2.25f);
        float x_end = Render_GetTextWidth(sub_end, 2.25f);
        
        float sel_x = x + 10 + x_start;
        float sel_w = x_end - x_start;
        float sel_h = 28.0f;
        
        Render_DrawRect(sel_x, y + (h - sel_h) / 2, sel_w, sel_h, UI_COLOR_SELECTION[0], UI_COLOR_SELECTION[1], UI_COLOR_SELECTION[2], UI_COLOR_SELECTION[3]);
    }

    // Center text vertically
    Render_DrawText(display_buf, x + 10, y + (h - 18) / 2, 2.25f, UI_COLOR_TEXT_NORMAL[0], UI_COLOR_TEXT_NORMAL[1], UI_COLOR_TEXT_NORMAL[2], UI_COLOR_TEXT_NORMAL[3]);

    // Draw cursor
    if (is_active && (int)(OS_GetTime() * 2) % 2 == 0) {
        char sub[256];
        int sub_len = (ui.cursor_pos < 255) ? ui.cursor_pos : 255;
        // Use display_buf logic
        for(int i=0; i<sub_len; ++i) sub[i] = display_buf[i];
        sub[sub_len] = 0;

        float tw = Render_GetTextWidth(sub, 2.25f);
        float th = 28.0f; // Cursor height
        // Vertically center cursor
        Render_DrawRect(x + 10 + tw, y + (h - th) / 2, 2, th, 1.0f, 1.0f, 1.0f, 1.0f);
    }

    return changed;
}

bool UI_List(const char *id, AudioNodeInfo *items, int count, uint32_t *selected_id, int x, int y, int w, int h) {
    (void)id;
    
    // Draw Border and Background using standard styles
    Render_DrawRoundedRect(x-1, y-1, w+2, h+2, UI_INPUT_CORNER_RADIUS + 1.0f, UI_COLOR_INPUT_BORDER_NORMAL[0], UI_COLOR_INPUT_BORDER_NORMAL[1], UI_COLOR_INPUT_BORDER_NORMAL[2], UI_COLOR_INPUT_BORDER_NORMAL[3]);
    Render_DrawRoundedRect(x, y, w, h, UI_INPUT_CORNER_RADIUS, UI_COLOR_INPUT_BG_NORMAL[0], UI_COLOR_INPUT_BG_NORMAL[1], UI_COLOR_INPUT_BG_NORMAL[2], UI_COLOR_INPUT_BG_NORMAL[3]);

    int item_h = UI_ITEM_HEIGHT;
    int visible_items = h / item_h;
    
    int start_y = y + 5;
    bool changed = false;
    
    for (int i = 0; i < count; i++) {
        if (i >= visible_items) break;
        
        int item_y = start_y + (i * item_h);
        
        bool hover = UI_IsHovered(x, item_y, w, item_h);
        
        if (hover) ui.next_cursor = OS_CURSOR_HAND;
        
        bool is_selected = (*selected_id == items[i].id);
        
        if (hover && ui.mouse_pressed) {
            *selected_id = items[i].id;
            changed = true;
        }
        
        if (is_selected) {
             Render_DrawRect(x + 2, item_y, w - 4, item_h - 2, UI_COLOR_BG_ACTIVE[0], UI_COLOR_BG_ACTIVE[1], UI_COLOR_BG_ACTIVE[2], UI_COLOR_BG_ACTIVE[3]);
        } else if (hover) {
             Render_DrawRect(x + 2, item_y, w - 4, item_h - 2, UI_COLOR_BG_HOVER[0], UI_COLOR_BG_HOVER[1], UI_COLOR_BG_HOVER[2], UI_COLOR_BG_HOVER[3]);
        }
        
        UI_DrawTruncatedText(items[i].name, x, item_y, w, item_h, false);
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
    UI_GetRect(&x, &y, w, h);

    bool is_open = (strcmp(ui.open_dropdown_id, id) == 0);
    bool just_opened = false;
    
    bool hover = UI_IsHovered(x, y, w, h);
    
    if (hover) {
        ui.next_cursor = OS_CURSOR_HAND;
        if (ui.mouse_pressed) {
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
    }
    
    UI_WidgetState state = UI_STATE_NORMAL;
    if (hover) {
        state = ui.mouse_down ? UI_STATE_ACTIVE : UI_STATE_HOVER;
    }
    
    UI_DrawWidgetBackground(x, y, w, h, state, UI_CORNER_RADIUS);
    // Draw dropdown arrow background/overlay if needed, existing code had a secondary rect?
    // Render_DrawRoundedRect(x, y, w, h, 12.0f, 0.45f, 0.47f, 0.58f, 0.15f); 
    // This was an overlay. I will keep it or skip it? It looks like a "glass" headers effect.
    // I'll keep it manual for now as it's specific.
    Render_DrawRoundedRect(x, y, w, h, UI_CORNER_RADIUS, 0.45f, 0.47f, 0.58f, 0.15f); 
    
    const char *current_name = "Select Audio Source...";
    for (int i=0; i<count; ++i) {
        if (items[i].id == *selected_id) {
            current_name = items[i].name;
            break;
        }
    }
    
    // Draw text with truncation
    // UI_DrawTruncatedText(current_name, x, y, w - 20, /*h?*/); 
    // Pass w-30 to account for arrow
    UI_DrawTruncatedText(current_name, x, y, w - 30, h, false);

    Render_DrawText("v", x + w - 30, y + (h/2) - 10, 1.5f, 0.71f, 0.75f, 1.0f, 1.0f);

    return just_opened; 
}
