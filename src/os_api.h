#ifndef HARMONY_OS_API_H
#define HARMONY_OS_API_H

#include "memory_arena.h"
#include <stdbool.h>

// Windowing
typedef struct WindowContext WindowContext; // Opaque handle

WindowContext* OS_CreateWindow(MemoryArena *arena, int width, int height, const char *title);
bool OS_ProcessEvents(WindowContext *window);
void OS_SwapBuffers(WindowContext *window);
void OS_GetWindowSize(WindowContext *window, int *width, int *height);

// Time
double OS_GetTime(); // Seconds since app start

// Input (Simple polling for now)
typedef struct InputState {
    bool quit_requested;
    int mouse_x, mouse_y;
    bool mouse_left_down;
    // Add keys as needed
} InputState;

InputState OS_GetInputState(WindowContext *window);

// Input
void OS_GetMouseState(WindowContext *window, int *x, int *y, bool *left_down);

// Returns the accumulated vertical mouse scroll delta since the last call.
// Positive = up/away, Negative = down/towards.
int OS_GetMouseScroll(WindowContext *window);

// Returns the last ASCII character pressed, or 0 if none.
// Very basic input handling for MVP.
char OS_GetLastChar(WindowContext *window);

// Returns true if ESC was pressed (consumes the press)
bool OS_IsEscapePressed(void);

// Returns true if F11 was pressed (consumes the press)
bool OS_IsF11Pressed(void);

// Returns true if Enter was pressed (consumes the press)
bool OS_IsEnterPressed(void);

void OS_SetFullscreen(WindowContext *window, bool fullscreen);

// Cursor
typedef enum {
    OS_CURSOR_ARROW,
    OS_CURSOR_HAND,
    OS_CURSOR_TEXT,
    OS_CURSOR_COUNT
} OS_CursorType;

void OS_SetCursor(WindowContext *window, OS_CursorType cursor);

// Clipboard & Shortcuts
const char *OS_GetClipboardText(MemoryArena *arena);
void OS_SetClipboardText(const char *text);
bool OS_IsPastePressed(void);
bool OS_IsCtrlDown(void);

#endif // HARMONY_OS_API_H
