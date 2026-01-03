#include "os_api.h"
#include "codec_api.h"
#include "network_api.h"
#include "capture_api.h"
#include "ui/render_api.h"
#include "ui_api.h"
#include <stdio.h>
#include <netinet/in.h>
#include "net/protocol.h" // For Packetizer
#include <stdlib.h> // For getenv

// Forward Declaration (should be in a header)
uint32_t Portal_RequestScreenCast();

// ... (Headers remain)

// Forward Declaration
uint32_t Portal_RequestScreenCast();

// --- HOST MODE ---
typedef struct NetCallbackData {
    NetworkContext *net;
    const char *dest_ip;
    int dest_port;
} NetCallbackData;

void Net_SendPacketCallback(void *user_data, void *packet_data, size_t packet_size) {
    NetCallbackData *d = (NetCallbackData *)user_data;
    Net_Send(d->net, d->dest_ip, d->dest_port, packet_data, packet_size);
}

int RunHost(MemoryArena *arena, WindowContext *window, const char *target_ip) {
    printf("Starting HOST Mode...\n");
    
    MemoryArena packet_arena;
    ArenaInit(&packet_arena, 32 * 1024 * 1024);

    // Initialize the renderer for status UI
    Render_Init(arena);

    // Request Screen Share Permissions
    printf("Requesting Screen Share... Please acknowledge dialog.\n");
    uint32_t node_id = Portal_RequestScreenCast();
    if (node_id == 0) {
        printf("FAILURE! Could not get screen stream. Exiting.\n");
        return 1;
    }
    printf("SUCCESS! Got Stream Node ID: %u\n", node_id);
    
    CaptureContext *capture = Capture_Init(arena, node_id);
    if (!capture) return 1;

    VideoFormat vfmt = { .width = 1280, .height = 720, .fps = 30, .bitrate = 2000000 };
    EncoderContext *encoder = Codec_InitEncoder(arena, vfmt);
    if (!encoder) return 1;

    // Send to Target IP
    NetworkContext *net = Net_Init(arena, 0, false); 
    NetCallbackData net_cb = { .net = net, .dest_ip = target_ip, .dest_port = 9999 };
    Packetizer packetizer = {0};

    // Stream Metadata
    StreamMetadata metadata = {0};
    strcpy(metadata.os_name, "Linux");
    const char *env_de = getenv("XDG_CURRENT_DESKTOP");
    strcpy(metadata.de_name, env_de ? env_de : "Unknown");
    // Width/Height set in loop
    strcpy(metadata.format_name, "BGRx");
    strcpy(metadata.color_space, "sRGB");

    int frame_count = 0;
    int frames_encoded = 0;
    float elapsed_time = 0.0f;
    bool has_started_capturing = false; // Persistent flag - once true, stays true
    
    // Keepalive tracking
    float time_since_last_send = 0.0f;
    const float KEEPALIVE_INTERVAL = 0.5f; // Send keepalive every 500ms if no frames
    
    while (OS_ProcessEvents(window)) {
        // Check for ESC to return to menu
        if (OS_IsEscapePressed()) {
            printf("Host: ESC pressed, returning to menu.\n");
            return 2; // Return to menu
        }
        
        // Get window size for UI
        int w = 1280, h = 720;
        OS_GetWindowSize(window, &w, &h);
        Render_SetScreenSize(w, h);
        
        // Update elapsed time for animations (~30fps assumed)
        elapsed_time += 1.0f / 30.0f;
        
        // Send Metadata periodically (every ~1 sec)
        if (frame_count % 30 == 0) {
             Protocol_SendMetadata(&packetizer, &metadata, Net_SendPacketCallback, &net_cb);
        }

        Capture_Poll(capture);
        
        VideoFrame *frame = Capture_GetFrame(capture);
        if (frame) {
            has_started_capturing = true; // Once we get a frame, we're capturing
            
            // Check for resolution change
            // Ensure dimensions are even for H.264
            int safe_width = frame->width & ~1;
            int safe_height = frame->height & ~1;

            if (safe_width != vfmt.width || safe_height != vfmt.height || !encoder) {
                printf("Host: Resolution changed to %dx%d (Safe: %dx%d). Re-initializing Encoder.\n", 
                    frame->width, frame->height, safe_width, safe_height);
                
                if (encoder) Codec_CloseEncoder(encoder);
                
                vfmt.width = safe_width;
                vfmt.height = safe_height;
                encoder = Codec_InitEncoder(arena, vfmt);
                
                if (encoder) {
                    // Update Metadata immediately
                    metadata.screen_width = safe_width;
                    metadata.screen_height = safe_height;
                    Protocol_SendMetadata(&packetizer, &metadata, Net_SendPacketCallback, &net_cb);
                } else {
                    printf("Host: Failed to re-init encoder!\n");
                }
            }

            // Periodically update metadata (store current dimensions)
            metadata.screen_width = vfmt.width;
            metadata.screen_height = vfmt.height;

            if (encoder) {
                ArenaClear(&packet_arena);
                EncodedPacket pkt = {0};
                Codec_EncodeFrame(encoder, frame, &packet_arena, &pkt);
            
                if (pkt.size > 0) {
                    Protocol_SendFrame(&packetizer, pkt.data, pkt.size, Net_SendPacketCallback, &net_cb);
                    frames_encoded++;
                    time_since_last_send = 0.0f; // Reset keepalive timer
                    if (frame_count++ % 30 == 0) printf("Host: Sent Frame %d (%zu bytes)\n", frames_encoded, pkt.size);
                }
            }
        } else {
            // No frame captured - track time for keepalive
            time_since_last_send += 1.0f / 30.0f;
            
            // Send keepalive if we've been idle for too long
            if (has_started_capturing && time_since_last_send > KEEPALIVE_INTERVAL) {
                Protocol_SendKeepalive(&packetizer, Net_SendPacketCallback, &net_cb);
                time_since_last_send = 0.0f;
            }
        }
        
        // Draw streaming status UI (prevents flickering, provides feedback)
        UI_DrawStreamStatus(w, h, elapsed_time, frames_encoded, 
                           target_ip, vfmt.width, vfmt.height, has_started_capturing);
        
        OS_SwapBuffers(window);
    }
    return 0;
}

// --- VIEWER MODE ---
int RunViewer(MemoryArena *arena, WindowContext *window) {
    printf("Starting VIEWER Mode...\n");
    
    // Listen on Port 9999
    NetworkContext *net = Net_Init(arena, 9999, true);
    if (!net) {
        printf("Failed to bind port 9999.\n");
        return 1;
    }
    
    DecoderContext *decoder = Codec_InitDecoder(arena);
    if (!decoder) return 1;
    
    Render_Init(arena);
    
    // Reassembler Setup
    Reassembler reassembler;
    Reassembler_Init(&reassembler, arena);
    
    // Video Frame for Decoding
    VideoFrame decoded_frame = {0}; 
    StreamMetadata stream_meta = {0};
    
    int frames_decoded = 0;
    float time_since_last_frame = 0.0f;
    const float STREAM_TIMEOUT = 2.0f; // Reset after 2 seconds of no frames
    
    while (OS_ProcessEvents(window)) {
        // Check for ESC to return to menu
        if (OS_IsEscapePressed()) {
            printf("Viewer: ESC pressed, returning to menu.\n");
            return 2; // Return to menu
        }
        
        // Track frame timing
        bool received_frame_this_tick = false;
        bool received_any_packet = false;  // For keepalive handling
        
        // 1. Receive Packets
        uint8_t buf[2048]; 
        char sender_ip[INET_ADDRSTRLEN];
        int sender_port;
        
        int n;
        while ((n = Net_Recv(net, buf, sizeof(buf), sender_ip, &sender_port)) > 0) {
            void *frame_data = NULL;
            size_t frame_size = 0;
            
            uint8_t packet_type = 0;
            ReassemblyResult res = Protocol_HandlePacket(&reassembler, buf, n, &frame_data, &frame_size, &packet_type);
            
            if (res == RESULT_COMPLETE) {
                if (packet_type == PACKET_TYPE_KEEPALIVE) {
                    // Keepalive received - just reset timeout, don't decode anything
                    received_any_packet = true;
                } else if (packet_type == PACKET_TYPE_METADATA) {
                     if (frame_size == sizeof(StreamMetadata)) {
                         memcpy(&stream_meta, frame_data, sizeof(StreamMetadata));
                     }
                     received_any_packet = true;
                } else {
                    // 2. Decode
                    EncodedPacket pkt = { .data = frame_data, .size = frame_size };
                    
                    // Decode directly
                    Codec_DecodePacket(decoder, &pkt, &decoded_frame);
                    received_frame_this_tick = true;
                    received_any_packet = true;
                    
                    // Log frame info periodically
                    if (decoded_frame.width > 0 && decoded_frame.height > 0) {
                        if (frames_decoded++ % 30 == 0) {
                            printf("Viewer: Decoded Frame %d (%dx%d) from %s:%d\n", 
                                frames_decoded, decoded_frame.width, decoded_frame.height, sender_ip, sender_port);
                        }
                    }
                }
            }
        }
        
        // Update timeout tracking - reset on ANY packet (including keepalive)
        if (received_any_packet) {
            time_since_last_frame = 0.0f;
        } else {
            time_since_last_frame += 1.0f / 30.0f; // Approximate frame time
        }
        
        // Reset to waiting state if stream timed out
        if (time_since_last_frame > STREAM_TIMEOUT && decoded_frame.width > 0) {
            printf("Viewer: Stream timeout, waiting for reconnection...\n");
            decoded_frame.width = 0;
            decoded_frame.height = 0;
            stream_meta.screen_width = 0; // Clear metadata too
            
            // Reset reassembler so it can accept new stream (which starts at frame_id 1)
            reassembler.active_buffer.frame_id = 0;
        }
        
        // Always draw the latest frame (or waiting screen)
        int win_w = 1280, win_h = 720;
        OS_GetWindowSize(window, &win_w, &win_h);
        Render_SetScreenSize(win_w, win_h); // Update UI projection

        if (decoded_frame.width > 0 && decoded_frame.height > 0) {
            Render_DrawFrame(&decoded_frame, win_w, win_h);

            // Draw Metadata Overlay
            if (stream_meta.screen_width > 0) {
                char meta_text[256];
                snprintf(meta_text, sizeof(meta_text), "HOST: %s | %s", stream_meta.os_name, stream_meta.de_name);
                char meta_text2[256];
                snprintf(meta_text2, sizeof(meta_text2), "RES: %dx%d | FMT: %s | COLOR: %s", 
                    stream_meta.screen_width, stream_meta.screen_height, stream_meta.format_name, stream_meta.color_space);

                Render_DrawRect(10, 10, 600, 80, 0.0f, 0.0f, 0.0f, 0.7f); // Transparent black box
                Render_DrawText(meta_text, 20, 30, 1.5f, 1.0f, 1.0f, 1.0f, 1.0f);
                Render_DrawText(meta_text2, 20, 60, 1.5f, 0.8f, 0.8f, 0.8f, 1.0f);
            }
        } else {
             Render_DrawRect(0, 0, win_w, win_h, 0.1f, 0.1f, 0.1f, 1.0f);
             Render_DrawText("Waiting for stream...", win_w/2 - 100, win_h/2, 2.0f, 0.8f, 0.8f, 0.8f, 1.0f);
        }
        
        // Don't clear if we drew frame? Or always clear background?
        // If we draw frame, it covers screen.
        OS_SwapBuffers(window);
    }
    return 0;
}

// --- MENU ---
typedef struct AppConfig {
    bool is_host;
    char target_ip[64];
    bool start_app;
} AppConfig;

void RunMenu(MemoryArena *arena, WindowContext *window, AppConfig *config) {
    UI_Init(arena); // Init UI Shader
    
    // Default config
    strcpy(config->target_ip, "127.0.0.1");
    config->is_host = true;
    config->start_app = false;
    
    while (OS_ProcessEvents(window)) {
        int w = 1280, h = 720; 
        OS_GetWindowSize(window, &w, &h);
        int mx = 0, my = 0;
        bool mdown = false;
        OS_GetMouseState(window, &mx, &my, &mdown);
        char c = OS_GetLastChar(window);
        
        UI_BeginFrame(w, h, mx, my, mdown, c);
        
        // Draw Background
        // Mocha Base: #1e1e2e -> 0.12, 0.12, 0.18
        Render_DrawRect(0, 0, w, h, 0.12f, 0.12f, 0.18f, 1.0f);
        
        // Draw Menu
        // Helper to center
        int cx = w / 2;
        int cy = h / 2;
        
        // Title
        // Lavender: #b4befe -> 0.71, 0.75, 1.00
        // Scale 4.0 for title
        const char *title = "Harmony Screen Share";
        int title_len = strlen(title);
        Render_DrawText(title, cx - (title_len * 16), cy - 250, 4.0f, 0.71f, 0.75f, 1.0f, 1.0f);
        
        // Mode Selection Labels (Subtitles)
        // Subtext1: #a6adc8 
        Render_DrawText("Select Mode:", cx - 200, cy - 180, 2.0f, 0.65f, 0.68f, 0.78f, 1.0f);
        
        // Buttons
        // Host Button
        bool is_host_selected = config->is_host;
        
        // Use different color/style for selected? ui_simple.c handles hover, but we want 'Selected' state visualization
        // We can just draw a selection indicator or rely on the label below.
        
        if (UI_Button("HOST MODE", cx - 210, cy - 130, 200, 60)) {
            config->is_host = true;
        }
        
        if (UI_Button("VIEWER MODE", cx + 10, cy - 130, 200, 60)) {
            config->is_host = false;
        }
        
        // Selection Indicator
        if (config->is_host) {
             Render_DrawRect(cx - 210, cy - 65, 200, 4, 0.71f, 0.75f, 1.0f, 1.0f); // Underline Host
        } else {
             Render_DrawRect(cx + 10, cy - 65, 200, 4, 0.71f, 0.75f, 1.0f, 1.0f); // Underline Viewer
        }
        
        // IP Config
        Render_DrawText("Target IP Address:", cx - 125, cy + 20, 2.0f, 0.8f, 0.8f, 0.9f, 1.0f);
        UI_TextInput("ip_input", config->target_ip, 64, cx - 125, cy + 50, 250, 50);
        
        // Start Button
        // Greenish variant for Start? #a6e3a1 (0.65, 0.89, 0.63)
        // We need to override color or just use default blue button.
        // Let's use default for consistency but maybe larger.
        
        if (UI_Button("START HARMONY", cx - 125, cy + 150, 250, 70)) {
            config->start_app = true;
            UI_EndFrame();
            OS_SwapBuffers(window);
            return; // Exit Menu
        }
        
        UI_EndFrame();
        OS_SwapBuffers(window);
    }
}

int main(int argc, char **argv) {
    MemoryArena main_arena;
    ArenaInit(&main_arena, 256 * 1024 * 1024);

    WindowContext *window = OS_CreateWindow(&main_arena, 1280, 720, "Harmony Screen Share");
    if (!window) return 1;
    
    // Check CLI overrides
    AppConfig config = {0};
    if (argc > 1 && strcmp(argv[1], "viewer") == 0) {
        config.is_host = false;
        config.start_app = true; // Auto-start if CLI arg provided
    } else if (argc > 1 && strcmp(argv[1], "host") == 0) {
        config.is_host = true;
        config.start_app = true;
    }
    
    // Main application loop - allows returning to menu
    while (1) {
        if (!config.start_app) {
            // Show Menu
            RunMenu(&main_arena, window, &config);
        }
        
        if (config.start_app) {
            int result;
            if (config.is_host) {
                result = RunHost(&main_arena, window, config.target_ip);
            } else {
                result = RunViewer(&main_arena, window);
            }
            
            // result == 2 means return to menu (ESC pressed)
            if (result == 2) {
                config.start_app = false; // Go back to menu
                continue;
            }
            return result;
        } else {
            break; // Menu closed without starting
        }
    }
    
    return 0;
}
