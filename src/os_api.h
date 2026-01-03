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

// Returns the last ASCII character pressed, or 0 if none.
// Very basic input handling for MVP.
char OS_GetLastChar(WindowContext *window);

#endif // HARMONY_OS_API_H
