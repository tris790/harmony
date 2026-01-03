#include <wayland-client.h>
#include <wayland-egl.h>
#include <EGL/egl.h>
#include <GL/gl.h>
#include <wayland-cursor.h>
#include <unistd.h>
#include <linux/input.h>
#include "../ui_api.h"
#include "../os_api.h"
#include "../audio_api.h"
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <time.h>

#include "generated/xdg-shell-client-protocol.h"
#include "generated/xdg-decoration-client-protocol.h"
#include "../os_api.h"

double OS_GetTime() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

// --- Global Wayland State ---
static struct wl_display *display;
static struct wl_registry *registry;
static struct wl_compositor *compositor;
static struct xdg_wm_base *wm_base;
static struct zxdg_decoration_manager_v1 *decoration_manager;
static struct wl_seat *seat;
static struct wl_shm *shm;
static struct wl_data_device_manager *data_device_manager;
static struct wl_data_device *data_device;

// --- Input Globals ---
static struct wl_pointer *g_pointer = NULL;
static double g_mouse_x = 0;
static double g_mouse_y = 0;
static bool g_mouse_left_down = false;
static double g_mouse_scroll_delta = 0;

static struct wl_cursor_theme *g_cursor_theme = NULL;
static struct wl_cursor *g_cursors[OS_CURSOR_COUNT] = {0};
static struct wl_surface *g_cursor_surface = NULL;
static uint32_t g_last_pointer_serial = 0;
static OS_CursorType g_current_cursor_type = OS_CURSOR_ARROW;

static struct wl_data_offer *g_active_offer = NULL;
static bool g_ctrl_down = false;
static bool g_paste_requested = false;

// --- Clipboard Copy State ---
static struct wl_data_source *g_data_source = NULL;
static char *g_clipboard_content = NULL;
static MemoryArena *g_wayland_arena = NULL;


static int32_t g_repeat_rate = 0;
static int32_t g_repeat_delay = 0;
static uint32_t g_repeat_key = 0;
static double g_next_repeat_time = 0;

// --- Data Device ---
static void data_offer_offer(void *data, struct wl_data_offer *wl_data_offer, const char *mime_type) {
    (void)data;
    (void)wl_data_offer;
    (void)mime_type;
    // We only care about text
}

static const struct wl_data_offer_listener data_offer_listener = { .offer = data_offer_offer };

// --- Data Source (Copy) ---
static void data_source_target(void *data, struct wl_data_source *wl_data_source, const char *mime_type) {
    (void)data;
    (void)wl_data_source;
    (void)mime_type;
}

static void data_source_send(void *data, struct wl_data_source *wl_data_source, const char *mime_type, int32_t fd) {
    (void)data;
    (void)wl_data_source;
    if (g_clipboard_content && (strcmp(mime_type, "text/plain") == 0 || strcmp(mime_type, "text/plain;charset=utf-8") == 0)) {
        write(fd, g_clipboard_content, strlen(g_clipboard_content));
    }
    close(fd);
}

static void data_source_cancelled(void *data, struct wl_data_source *wl_data_source) {
    (void)data;
    if (g_data_source == wl_data_source) {
        wl_data_source_destroy(g_data_source);
        g_data_source = NULL;
        // g_clipboard_content belongs to arena, no need to free
        g_clipboard_content = NULL;
    }
}

static const struct wl_data_source_listener data_source_listener = {
    .target = data_source_target,
    .send = data_source_send,
    .cancelled = data_source_cancelled,
    .dnd_drop_performed = NULL, // Since v3
    .dnd_finished = NULL,       // Since v3
    .action = NULL,             // Since v3
};


static void data_device_data_offer(void *data, struct wl_data_device *wl_data_device, struct wl_data_offer *offer) {
    (void)data;
    (void)wl_data_device;
    wl_data_offer_add_listener(offer, &data_offer_listener, NULL);
}
static void data_device_selection(void *data, struct wl_data_device *wl_data_device, struct wl_data_offer *offer) {
    (void)data;
    (void)wl_data_device;
    g_active_offer = offer;
}
static const struct wl_data_device_listener data_device_listener = {
    .data_offer = data_device_data_offer,
    .selection = data_device_selection,
};

// --- Window Context ---
struct WindowContext {
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *xdg_toplevel;
    struct wl_egl_window *egl_window;
    EGLDisplay egl_display;
    EGLContext egl_context;
    EGLSurface egl_surface;
    struct zxdg_toplevel_decoration_v1 *decoration;
    int width, height;
    bool configured;
    bool should_close;
};

// --- XDG Shell Listeners ---
static void xdg_wm_base_ping(void *data, struct xdg_wm_base *xdg_wm_base, uint32_t serial) {
    (void)data;
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
    (void)xdg_toplevel;
    (void)states;
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
    (void)xdg_toplevel;
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
static bool g_escape_pressed = false;

static void keyboard_keymap(void *data, struct wl_keyboard *wl_keyboard, uint32_t format, int32_t fd, uint32_t size) {
    (void)data;
    (void)wl_keyboard;
    (void)format;
    (void)size;
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
    (void)data;
    (void)wl_keyboard;
    (void)serial;
    (void)surface;
    (void)keys;
}

static void keyboard_leave(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial, struct wl_surface *surface) {
    (void)data;
    (void)wl_keyboard;
    (void)serial;
    (void)surface;
}

static void keyboard_modifiers(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial, uint32_t mods_depressed, uint32_t mods_latched, uint32_t mods_locked, uint32_t group) {
    (void)data;
    (void)wl_keyboard;
    (void)serial;
    (void)mods_depressed;
    (void)mods_latched;
    (void)mods_locked;
    (void)group;
    // We'll use keyboard_key for modifier state as xkbcommon isn't used for raw scancodes here.
}

static void keyboard_key(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
    (void)data;
    (void)wl_keyboard;
    (void)serial;
    (void)time;
    bool pressed = (state == WL_KEYBOARD_KEY_STATE_PRESSED);
    if (key == 29 || key == 97) g_ctrl_down = pressed; // Left Ctrl=29, Right Ctrl=97

    if (pressed) {
        if (key == 1) {
            g_escape_pressed = true;
            return;
        }
        
        if (key == 47 && g_ctrl_down) { // 'V' is 47
            g_paste_requested = true;
            return;
        }

        // Simple QWERTY scancode map
        char c = 0;
        if (key >= 2 && key <= 10) c = '1' + (key - 2);
        if (key == 11) c = '0';
        if (key == 52) c = '.';
        if (key == 14) c = '\b'; // Backspace
        if (key == 111) c = '\x7f'; // Delete
        
        if (key == 105) c = '\x11'; // Left
        if (key == 106) c = '\x12'; // Right
        if (key == 102) c = '\x13'; // Home
        if (key == 107) c = '\x14'; // End
        
        if (g_ctrl_down) {
            if (key == 44) c = '\x1a'; // Ctrl+Z (Undo)
            if (key == 21) c = '\x19'; // Ctrl+Y (Redo)
        }
        
        last_char = c;

        // Start repeat
        if (g_repeat_rate > 0) {
            g_repeat_key = key;
            g_next_repeat_time = OS_GetTime() + (double)g_repeat_delay / 1000.0;
        }
    } else {
        // Stop repeat if this key was repeating
        if (key == g_repeat_key) {
            g_repeat_key = 0;
            g_next_repeat_time = 0;
        }
    }
}

static void keyboard_repeat_info(void *data, struct wl_keyboard *wl_keyboard, int32_t rate, int32_t delay) {
    (void)data;
    (void)wl_keyboard;
    g_repeat_rate = rate;
    g_repeat_delay = delay;
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
    (void)data;
    (void)wl_pointer;
    (void)surface;
    g_last_pointer_serial = serial;
    g_mouse_x = wl_fixed_to_double(surface_x);
    g_mouse_y = wl_fixed_to_double(surface_y);
    
    // Set initial cursor
    OS_SetCursor(NULL, g_current_cursor_type);
}

static void pointer_leave(void *data, struct wl_pointer *wl_pointer, uint32_t serial, struct wl_surface *surface) {
    (void)data;
    (void)wl_pointer;
    (void)serial;
    (void)surface;
}

static void pointer_motion(void *data, struct wl_pointer *wl_pointer, uint32_t time, wl_fixed_t surface_x, wl_fixed_t surface_y) {
    (void)data;
    (void)wl_pointer;
    (void)time;
    g_mouse_x = wl_fixed_to_double(surface_x);
    g_mouse_y = wl_fixed_to_double(surface_y);
}

static void pointer_button(void *data, struct wl_pointer *wl_pointer, uint32_t serial, uint32_t time, uint32_t button, uint32_t state) {
    (void)data;
    (void)wl_pointer;
    (void)time;
    g_last_pointer_serial = serial;
    if (button == BTN_LEFT) {
        g_mouse_left_down = (state == WL_POINTER_BUTTON_STATE_PRESSED);
    }
}

static void pointer_axis(void *data, struct wl_pointer *wl_pointer, uint32_t time, uint32_t axis, wl_fixed_t value) {
    (void)data;
    (void)wl_pointer;
    (void)time;
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
        g_mouse_scroll_delta -= wl_fixed_to_double(value);
    }
}

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis,
};

static void seat_capabilities(void *data, struct wl_seat *wl_seat, uint32_t capabilities) {
    (void)data;
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

    if (data_device_manager && !data_device) {
        data_device = wl_data_device_manager_get_data_device(data_device_manager, wl_seat);
        wl_data_device_add_listener(data_device, &data_device_listener, NULL);
    }
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
};

// ... 

char OS_GetLastChar(WindowContext *window) {
    (void)window;
    char c = last_char;
    last_char = 0; // Consume
    return c;
}

bool OS_IsEscapePressed(void) {
    bool pressed = g_escape_pressed;
    g_escape_pressed = false; // Consume
    return pressed;
}

// --- Registry Listener ---
static void registry_handle_global(void *data, struct wl_registry *registry,
                                   uint32_t name, const char *interface, uint32_t version) {
    (void)data;
    (void)version;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(wm_base, &xdg_wm_base_listener, NULL);
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
        wl_seat_add_listener(seat, &seat_listener, NULL);
    } else if (strcmp(interface, zxdg_decoration_manager_v1_interface.name) == 0) {
        decoration_manager = wl_registry_bind(registry, name, &zxdg_decoration_manager_v1_interface, 1);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, wl_data_device_manager_interface.name) == 0) {
        data_device_manager = wl_registry_bind(registry, name, &wl_data_device_manager_interface, 3);
    }
}

static void registry_handle_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
    (void)data;
    (void)registry;
    (void)name;
    // Handle removal
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_handle_global,
    .global_remove = registry_handle_global_remove,
};

// --- OS API Implementation ---

WindowContext* OS_CreateWindow(MemoryArena *arena, int width, int height, const char *title) {
    // 1. Connect Display
    g_wayland_arena = arena;
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
    
    if (decoration_manager) {
        win->decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(decoration_manager, win->xdg_toplevel);
        zxdg_toplevel_decoration_v1_set_mode(win->decoration, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    }

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
        EGL_ALPHA_SIZE, 0,
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

    if (!g_cursor_theme) {
        g_cursor_theme = wl_cursor_theme_load(NULL, 24, shm);
        if (g_cursor_theme) {
            g_cursors[OS_CURSOR_ARROW] = wl_cursor_theme_get_cursor(g_cursor_theme, "left_ptr");
            g_cursors[OS_CURSOR_HAND] = wl_cursor_theme_get_cursor(g_cursor_theme, "hand2");
            if (!g_cursors[OS_CURSOR_HAND]) g_cursors[OS_CURSOR_HAND] = wl_cursor_theme_get_cursor(g_cursor_theme, "pointer");
            g_cursors[OS_CURSOR_TEXT] = wl_cursor_theme_get_cursor(g_cursor_theme, "xterm");
            if (!g_cursors[OS_CURSOR_TEXT]) g_cursors[OS_CURSOR_TEXT] = wl_cursor_theme_get_cursor(g_cursor_theme, "ibeam");
        }
        g_cursor_surface = wl_compositor_create_surface(compositor);
    }

    return win;
}

void OS_SetCursor(WindowContext *window, OS_CursorType type) {
    (void)window;
    g_current_cursor_type = type;
    if (!g_pointer || !g_cursor_surface || type >= OS_CURSOR_COUNT) return;

    struct wl_cursor *cursor = g_cursors[type];
    if (!cursor) cursor = g_cursors[OS_CURSOR_ARROW];
    if (!cursor) return;

    struct wl_cursor_image *image = cursor->images[0];
    struct wl_buffer *buffer = wl_cursor_image_get_buffer(image);
    if (!buffer) return;

    wl_pointer_set_cursor(g_pointer, g_last_pointer_serial, g_cursor_surface, image->hotspot_x, image->hotspot_y);
    wl_surface_attach(g_cursor_surface, buffer, 0, 0);
    wl_surface_damage(g_cursor_surface, 0, 0, image->width, image->height);
    wl_surface_commit(g_cursor_surface);
}

bool OS_ProcessEvents(WindowContext *window) {
    while (wl_display_prepare_read(display) != 0) {
        wl_display_dispatch_pending(display);
    }
    wl_display_flush(display);
    wl_display_read_events(display);
    wl_display_dispatch_pending(display);

    // Handle repeat
    if (g_repeat_key != 0 && g_repeat_rate > 0) {
        double now = OS_GetTime();
        if (now >= g_next_repeat_time) {
            // Trigger repeat
            uint32_t key = g_repeat_key;
            char c = 0;
            if (key >= 2 && key <= 10) c = '1' + (key - 2);
            if (key == 11) c = '0';
            if (key == 52) c = '.';
            if (key == 14) c = '\b'; // Backspace
            if (key == 111) c = '\x7f'; // Delete
            if (key == 105) c = '\x11'; // Left
            if (key == 106) c = '\x12'; // Right
            if (key == 102) c = '\x13'; // Home
            if (key == 107) c = '\x14'; // End
            
            if (g_ctrl_down) {
                if (key == 44) c = '\x1a'; // Ctrl+Z
                if (key == 21) c = '\x19'; // Ctrl+Y
            }
            
            if (c != 0) {
                last_char = c;
            }
            
            g_next_repeat_time = now + 1.0 / (double)g_repeat_rate;
        }
    }

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
    (void)window;
    if (x) *x = (int)g_mouse_x;
    if (y) *y = (int)g_mouse_y;
    if (left_down) *left_down = g_mouse_left_down;
}

int OS_GetMouseScroll(WindowContext *window) {
    (void)window;
    int scroll = 0;
    // Standard Wayland axis values are usually ~10 per "click"
    // We'll normalize this a bit for the UI.
    if (g_mouse_scroll_delta >= 10.0) {
        scroll = 1;
        g_mouse_scroll_delta -= 10.0;
    } else if (g_mouse_scroll_delta <= -10.0) {
        scroll = -1;
        g_mouse_scroll_delta += 10.0;
    }
    return scroll;
}

const char* OS_GetClipboardText(MemoryArena *arena) {
    if (!g_active_offer) return "";

    int fds[2];
    if (pipe(fds) == -1) return "";

    wl_data_offer_receive(g_active_offer, "text/plain", fds[1]);
    close(fds[1]);
    wl_display_flush(display);
    wl_display_roundtrip(display);

    char temp[1024];
    int n = read(fds[0], temp, sizeof(temp) - 1);
    close(fds[0]);

    if (n > 0) {
        temp[n] = 0;
        char *result = PushArray(arena, n + 1, char);
        memcpy(result, temp, n + 1);
        return result;
    }

    return "";
}

void OS_SetClipboardText(const char *text) {
    if (!data_device_manager || !seat) return;

    if (text) {
        size_t len = strlen(text);
        g_clipboard_content = ArenaPush(g_wayland_arena, len + 1);
        strcpy(g_clipboard_content, text);
        
        g_data_source = wl_data_device_manager_create_data_source(data_device_manager);
        wl_data_source_add_listener(g_data_source, &data_source_listener, NULL);
        wl_data_source_offer(g_data_source, "text/plain");
        wl_data_source_offer(g_data_source, "text/plain;charset=utf-8");
        
        wl_data_device_set_selection(data_device, g_data_source, g_last_pointer_serial);
    }
}


bool OS_IsPastePressed(void) {
    bool p = g_paste_requested;
    g_paste_requested = false;
    return p;
}

bool OS_IsCtrlDown(void) {
    return g_ctrl_down;
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

