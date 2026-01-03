#include <wayland-client.h>
#include <wayland-egl.h>
#include <EGL/egl.h>
#include <GL/gl.h>
#include <unistd.h>
#include <linux/input.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

#include "generated/xdg-shell-client-protocol.h"
#include "../os_api.h"

// --- Global Wayland State ---
static struct wl_display *display;
static struct wl_registry *registry;
static struct wl_compositor *compositor;
static struct xdg_wm_base *wm_base;
static struct wl_seat *seat;

// --- Input Globals ---
static struct wl_pointer *g_pointer = NULL;
static double g_mouse_x = 0;
static double g_mouse_y = 0;
static bool g_mouse_left_down = false;

// --- Window Context ---
struct WindowContext {
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *xdg_toplevel;
    struct wl_egl_window *egl_window;
    EGLDisplay egl_display;
    EGLContext egl_context;
    EGLSurface egl_surface;
    int width, height;
    bool configured;
    bool should_close;
};

// --- XDG Shell Listeners ---
static void xdg_wm_base_ping(void *data, struct xdg_wm_base *xdg_wm_base, uint32_t serial) {
    xdg_wm_base_pong(xdg_wm_base, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
    .ping = xdg_wm_base_ping,
};

static void xdg_surface_configure(void *data, struct xdg_surface *xdg_surface, uint32_t serial) {
    xdg_surface_ack_configure(xdg_surface, serial);
    WindowContext *win = (WindowContext*)data;
    win->configured = true; // Primitive check
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static void xdg_toplevel_configure(void *data, struct xdg_toplevel *xdg_toplevel,
                                   int32_t width, int32_t height, struct wl_array *states) {
    WindowContext *win = (WindowContext*)data;
    if (width > 0 && height > 0) {
        if (width != win->width || height != win->height) {
            win->width = width;
            win->height = height;
            wl_egl_window_resize(win->egl_window, width, height, 0, 0);
        }
    }
}

static void xdg_toplevel_close(void *data, struct xdg_toplevel *xdg_toplevel) {
    WindowContext *win = (WindowContext*)data;
    win->should_close = true;
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
    .configure = xdg_toplevel_configure,
    .close = xdg_toplevel_close,
};

// --- Seat & Input ---
// Forward decl
static const struct wl_pointer_listener pointer_listener;

// --- Keyboard ---
struct wl_keyboard *keyboard;
static char last_char = 0;

static void keyboard_keymap(void *data, struct wl_keyboard *wl_keyboard, uint32_t format, int32_t fd, uint32_t size) {
    // We strictly need XKB common to map keys...
    // For "Handmade" MVP without xkbcommon dependency, we can't easily map raw scancodes to ASCII.
    // However, including xkbcommon is standard even for minimal Wayland.
    // If we want ZERO dependencies, we are stuck with raw scancodes (driver dependent).
    // Let's assume we map only numeric keys and dots for IP?
    // Or we use a tiny scancode-to-ascii table for US QWERTY.
    // Let's implement a MICRO scancode table for 0-9, dot.
    close(fd);
}

static void keyboard_enter(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial, struct wl_surface *surface, struct wl_array *keys) {
}

static void keyboard_leave(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial, struct wl_surface *surface) {
}

static void keyboard_key(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
    if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        // Simple QWERTY scancode map (offset by 8 in Linux/X11 usually, but Wayland gives evdev codes)
        // Evdev codes:
        // 2-11: 1234567890
        // 12: -
        // 13: =
        // 14: BS
        // 16-25: QWERTYUIOP
        // 52: .
        
        // This is extremely hacky but fits "Handmade" minimal scope for just typing an IP.
        char c = 0;
        if (key >= 2 && key <= 10) c = '1' + (key - 2);
        if (key == 11) c = '0';
        if (key == 52) c = '.';
        if (key == 14) c = '\b'; // Backspace
        
        last_char = c;
    }
}

static void keyboard_modifiers(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial, uint32_t mods_depressed, uint32_t mods_latched, uint32_t mods_locked, uint32_t group) {
}

static void keyboard_repeat_info(void *data, struct wl_keyboard *wl_keyboard, int32_t rate, int32_t delay) {
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_keymap,
    .enter = keyboard_enter,
    .leave = keyboard_leave,
    .key = keyboard_key,
    .modifiers = keyboard_modifiers,
    .repeat_info = keyboard_repeat_info,
};

// --- Pointer Listeners ---
static void pointer_enter(void *data, struct wl_pointer *wl_pointer, uint32_t serial, struct wl_surface *surface, wl_fixed_t surface_x, wl_fixed_t surface_y) {
    g_mouse_x = wl_fixed_to_double(surface_x);
    g_mouse_y = wl_fixed_to_double(surface_y);
}

static void pointer_leave(void *data, struct wl_pointer *wl_pointer, uint32_t serial, struct wl_surface *surface) {
}

static void pointer_motion(void *data, struct wl_pointer *wl_pointer, uint32_t time, wl_fixed_t surface_x, wl_fixed_t surface_y) {
    g_mouse_x = wl_fixed_to_double(surface_x);
    g_mouse_y = wl_fixed_to_double(surface_y);
}

static void pointer_button(void *data, struct wl_pointer *wl_pointer, uint32_t serial, uint32_t time, uint32_t button, uint32_t state) {
    if (button == BTN_LEFT) {
        g_mouse_left_down = (state == WL_POINTER_BUTTON_STATE_PRESSED);
    }
}

static void pointer_axis(void *data, struct wl_pointer *wl_pointer, uint32_t time, uint32_t axis, wl_fixed_t value) {
}

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis,
};

static void seat_capabilities(void *data, struct wl_seat *wl_seat, uint32_t capabilities) {
    bool has_pointer = capabilities & WL_SEAT_CAPABILITY_POINTER;
    bool has_keyboard = capabilities & WL_SEAT_CAPABILITY_KEYBOARD;
    
    if (has_pointer && !g_pointer) {
        g_pointer = wl_seat_get_pointer(wl_seat);
        wl_pointer_add_listener(g_pointer, &pointer_listener, NULL);
    }
    
    if (has_keyboard && !keyboard) {
        keyboard = wl_seat_get_keyboard(wl_seat);
        wl_keyboard_add_listener(keyboard, &keyboard_listener, NULL); 
    }
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
};

// ... 

char OS_GetLastChar(WindowContext *window) {
    char c = last_char;
    last_char = 0; // Consume
    return c;
}

// --- Registry Listener ---
static void registry_handle_global(void *data, struct wl_registry *registry,
                                   uint32_t name, const char *interface, uint32_t version) {
    (void)data;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(wm_base, &xdg_wm_base_listener, NULL);
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
        wl_seat_add_listener(seat, &seat_listener, NULL);
    }
}

static void registry_handle_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
    // Handle removal
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_handle_global,
    .global_remove = registry_handle_global_remove,
};

// --- OS API Implementation ---

WindowContext* OS_CreateWindow(MemoryArena *arena, int width, int height, const char *title) {
    // 1. Connect Display
    if (!display) {
        display = wl_display_connect(NULL);
        if (!display) {
            fprintf(stderr, "Failed to connect to Wayland display\n");
            return NULL;
        }
        registry = wl_display_get_registry(display);
        wl_registry_add_listener(registry, &registry_listener, NULL);
        wl_display_roundtrip(display); // Wait for globals
        wl_display_roundtrip(display); // Wait for extras
    }

    if (!compositor || !wm_base) {
        fprintf(stderr, "Missing Wayland globals (Compositor or XDG Shell)\n");
        return NULL;
    }

    WindowContext *win = PushStructZero(arena, WindowContext);
    win->width = width;
    win->height = height;

    // 2. Create Surface
    win->surface = wl_compositor_create_surface(compositor);
    win->xdg_surface = xdg_wm_base_get_xdg_surface(wm_base, win->surface);
    xdg_surface_add_listener(win->xdg_surface, &xdg_surface_listener, win);
    
    win->xdg_toplevel = xdg_surface_get_toplevel(win->xdg_surface);
    xdg_toplevel_add_listener(win->xdg_toplevel, &xdg_toplevel_listener, win);
    
    xdg_toplevel_set_title(win->xdg_toplevel, title);
    xdg_toplevel_set_app_id(win->xdg_toplevel, "harmony");

    win->surface = win->surface; // Keep ref
    
    // 3. Init EGL
    win->egl_display = eglGetDisplay((EGLNativeDisplayType)display);
    if (win->egl_display == EGL_NO_DISPLAY) {
        fprintf(stderr, "Failed to get EGL display\n");
        return NULL;
    }

    // Input Setup (Moved to Registry)

    EGLint major, minor;
    if (!eglInitialize(win->egl_display, &major, &minor)) {
        fprintf(stderr, "Failed to initialize EGL\n");
        return NULL;
    }

    EGLint config_attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NONE
    };

    EGLConfig egl_config;
    EGLint num_configs;
    eglChooseConfig(win->egl_display, config_attribs, &egl_config, 1, &num_configs);

    EGLint context_attribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };

    win->egl_context = eglCreateContext(win->egl_display, egl_config, EGL_NO_CONTEXT, context_attribs);
    if (win->egl_context == EGL_NO_CONTEXT) {
        fprintf(stderr, "Failed to create EGL context\n");
        return NULL;
    }

    win->egl_window = wl_egl_window_create(win->surface, width, height);
    win->egl_surface = eglCreateWindowSurface(win->egl_display, egl_config, (EGLNativeWindowType)win->egl_window, NULL);
    
    if (win->egl_surface == EGL_NO_SURFACE) {
        fprintf(stderr, "Failed to create EGL surface\n");
        return NULL;
    }

    eglMakeCurrent(win->egl_display, win->egl_surface, win->egl_surface, win->egl_context);

    // Initial commit to get configured event
    wl_surface_commit(win->surface);
    wl_display_roundtrip(display);
    
    printf("Wayland Window Created: %dx%d\n", width, height);
    return win;
}

bool OS_ProcessEvents(WindowContext *window) {
    while (wl_display_prepare_read(display) != 0) {
        wl_display_dispatch_pending(display);
    }
    wl_display_flush(display);
    wl_display_read_events(display);
    wl_display_dispatch_pending(display);

    return !window->should_close;
}

void OS_SwapBuffers(WindowContext *window) {
    eglSwapBuffers(window->egl_display, window->egl_surface);
}

void OS_GetWindowSize(WindowContext *window, int *width, int *height) {
    if (window) {
        if (width) *width = window->width;
        if (height) *height = window->height;
    }
}

// --- Input ---

void OS_GetMouseState(WindowContext *window, int *x, int *y, bool *left_down) {
    if (x) *x = (int)g_mouse_x;
    if (y) *y = (int)g_mouse_y;
    if (left_down) *left_down = g_mouse_left_down;
}

// Listeners moved above seat_capabilities

// (Removed duplicate seat_capabilities)

// We need to properly hook up input.
// Making seat handling part of WindowContext or Global?
// The current `registry_handle_global` binds seat to a static variable.
// Ideally, we move `seat` and `registry` handling to be context-aware or use the static global for the single window app.

// Let's implement a helper to bind input for the window.
void Input_Init(WindowContext *win) {
    if (seat) {
        wl_seat_add_listener(seat, (const struct wl_seat_listener *)&seat_listener, win);
    }
}

// Wait, undefined `seat_listener` in this scope if I defined it inside.
// I will define it above.

