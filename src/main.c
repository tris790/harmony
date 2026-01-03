#include "os_api.h"
#include "codec_api.h"
#include "network_api.h"
#include "capture_api.h"
#include "audio_api.h"
#include "ui/render_api.h"
#include "ui_api.h"
#include "config_api.h"
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

// Calculate target bitrate based on resolution and framerate.
// Uses industry-standard recommendations (YouTube/Twitch/OBS 2024):
// 720p30: 5 Mbps, 720p60: 7.5 Mbps
// 1080p30: 8 Mbps, 1080p60: 12 Mbps
// 1440p60: 24 Mbps
// 4K30: 35 Mbps, 4K60: 50 Mbps
static int CalculateTargetBitrate(int width, int height, int fps) {
    int pixels = width * height;
    bool high_fps = (fps >= 50);
    
    // 4K (3840x2160 = 8.3M pixels)
    if (pixels >= 8000000) {
        return high_fps ? 50000000 : 35000000; // 50 or 35 Mbps
    }
    // 1440p (2560x1440 = 3.7M pixels)
    if (pixels >= 3500000) {
        return high_fps ? 24000000 : 16000000; // 24 or 16 Mbps
    }
    // 1080p (1920x1080 = 2M pixels)
    if (pixels >= 2000000) {
        return high_fps ? 12000000 : 8000000; // 12 or 8 Mbps
    }
    // 720p (1280x720 = 921k pixels)
    if (pixels >= 900000) {
        return high_fps ? 7500000 : 5000000; // 7.5 or 5 Mbps
    }
    // Lower resolutions - fallback to formula
    return (int)(width * height * fps * 0.15f);
}

int RunHost(MemoryArena *arena, WindowContext *window, const char *target_ip, bool verbose) {
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

    // Audio capture and encoding
    AudioCaptureContext *audio_capture = Audio_InitCapture(arena);
    AudioEncoder *audio_encoder = Audio_InitEncoder(arena);
    if (!audio_capture || !audio_encoder) {
        printf("Host: Audio initialization failed (continuing without audio)\n");
    }

    // Use dynamic bitrate calculation
    int initial_bitrate = CalculateTargetBitrate(1280, 720, 30);
    VideoFormat vfmt = { .width = 1280, .height = 720, .fps = 30, .bitrate = initial_bitrate };
    EncoderContext *encoder = Codec_InitEncoder(arena, vfmt);
    if (!encoder) return 1;

    // Network Setup - Single socket on port 9999 for both sending and receiving
    // This enables symmetric UDP hole punching through firewalls
    NetworkContext *net = Net_Init(arena, 9999, true);
    if (!net) {
        printf("Host: Failed to bind port 9999.\n");
        return 1;
    }
    
    // Viewer address tracking - will be updated when we receive punch packets
    char viewer_ip[16] = {0};
    int viewer_port = 9999;  // Viewer listens on 9999
    bool has_viewer = false;
    
    // Send to target_ip:9999 (updated when we receive punch from viewer)
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
    
    // Host punch - send punch to viewer to open our firewall for their punch  
    float time_since_host_punch = 0.0f;
    const float HOST_PUNCH_INTERVAL = 0.5f;
    
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
        
        // Send punch to viewer to open our firewall for their punch (same socket)
        time_since_host_punch += 1.0f / 30.0f;
        if (time_since_host_punch >= HOST_PUNCH_INTERVAL) {
            Protocol_SendPunch(&packetizer, Net_SendPacketCallback, &net_cb);
            time_since_host_punch = 0.0f;
        }
        
        // Check for punch packets from viewers (receive on same socket)
        {
            uint8_t punch_buf[64];
            char incoming_ip[16];
            int incoming_port;
            int n;
            while ((n = Net_Recv(net, punch_buf, sizeof(punch_buf), incoming_ip, &incoming_port)) > 0) {
                if (n >= (int)sizeof(PacketHeader)) {
                    PacketHeader *hdr = (PacketHeader *)punch_buf;
                    if (hdr->packet_type == PACKET_TYPE_PUNCH) {
                        // Viewer is sending punch packets - update our target to their source address
                        if (!has_viewer || strcmp(viewer_ip, incoming_ip) != 0) {
                            bool is_new_viewer = !has_viewer;
                            strncpy(viewer_ip, incoming_ip, sizeof(viewer_ip) - 1);
                            has_viewer = true;
                            net_cb.dest_ip = viewer_ip;
                            printf("Host: Viewer connected from %s:%d - starting stream\n", incoming_ip, incoming_port);
                            
                            // Force keyframe for new viewer by re-initializing encoder
                            // This ensures the first frame they receive has SPS/PPS
                            if (is_new_viewer && encoder) {
                                printf("Host: Re-initializing encoder to send fresh keyframe\n");
                                Codec_CloseEncoder(encoder);
                                encoder = Codec_InitEncoder(arena, vfmt);
                            }
                        }
                    }
                }
            }
        }
        
        // Only send data when we have a confirmed viewer (after UDP punch)
        if (!has_viewer) {
            // Still waiting for viewer - just poll capture to keep it active
            Capture_Poll(capture);
            Capture_GetFrame(capture); // Discard frames while waiting
            
            // Draw waiting UI
            Render_DrawRect(0, 0, w, h, 0.1f, 0.1f, 0.15f, 1.0f);
            char wait_msg[128];
            snprintf(wait_msg, sizeof(wait_msg), "Waiting for viewer at %s:9999...", target_ip);
            Render_DrawText(wait_msg, w/2 - 200, h/2, 2.0f, 0.8f, 0.8f, 0.8f, 1.0f);
            OS_SwapBuffers(window);
            continue;
        }
        
        // Send Metadata periodically (every ~1 sec)
        if (frame_count % 30 == 0) {
             Protocol_SendMetadata(&packetizer, &metadata, Net_SendPacketCallback, &net_cb);
        }

        Capture_Poll(capture);
        
        // Poll and send ALL available audio frames
        // Audio runs at 48kHz with 20ms (960 sample) frames = 50 frames/sec
        // We may have multiple buffered, send them all
        if (audio_capture && audio_encoder) {
            // Poll multiple times to ensure we get all buffered audio
            for (int poll = 0; poll < 5; poll++) {
                Audio_PollCapture(audio_capture);
            }
            
            // Send all available audio frames
            AudioFrame *aframe;
            while ((aframe = Audio_GetCapturedFrame(audio_capture)) != NULL) {
                EncodedAudio encoded_audio = {0};
                Audio_Encode(audio_encoder, aframe, &encoded_audio);
                if (encoded_audio.size > 0) {
                    Protocol_SendAudio(&packetizer, encoded_audio.data, encoded_audio.size, Net_SendPacketCallback, &net_cb);
                }
            }
        }
        
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
                vfmt.bitrate = CalculateTargetBitrate(safe_width, safe_height, vfmt.fps);
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
                    if (frame_count++ % 30 == 0 && verbose) printf("Host: Sent Frame %d (%zu bytes)\n", frames_encoded, pkt.size);
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
int RunViewer(MemoryArena *arena, WindowContext *window, const char *host_ip, bool verbose) {
    printf("Starting VIEWER Mode...\n");
    printf("Viewer: Will send punch packets to host at %s:9999\n", host_ip);
    
    // Single socket on Port 9999 for both receiving data and sending punch
    // This enables symmetric UDP hole punching through firewalls
    NetworkContext *net = Net_Init(arena, 9999, true);
    if (!net) {
        printf("Failed to bind port 9999.\n");
        return 1;
    }
    
    // Punch setup - use same socket, send to host port 9999
    Packetizer punch_packetizer = {0};
    NetCallbackData punch_cb = { .net = net, .dest_ip = host_ip, .dest_port = 9999 };
    
    DecoderContext *decoder = Codec_InitDecoder(arena);
    if (!decoder) return 1;
    
    // Audio decoding and playback
    AudioDecoder *audio_decoder = Audio_InitDecoder(arena);
    AudioPlaybackContext *audio_playback = Audio_InitPlayback(arena);
    if (!audio_decoder || !audio_playback) {
        printf("Viewer: Audio initialization failed (continuing without audio)\n");
    }
    
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
    
    // Punch packet timing
    float time_since_last_punch = 0.0f;
    const float PUNCH_INTERVAL = 0.5f; // Send punch every 500ms
    
    // Bandwidth measurement
    size_t bytes_received_window = 0;
    float bandwidth_window_time = 0.0f;
    const float BANDWIDTH_WINDOW = 1.0f; // Measure over 1 second
    float current_mbps = 0.0f;
    
    while (OS_ProcessEvents(window)) {
        // Check for ESC to return to menu
        if (OS_IsEscapePressed()) {
            printf("Viewer: ESC pressed, returning to menu.\n");
            return 2; // Return to menu
        }
        
        // Send punch packet periodically (UDP hole punching)
        time_since_last_punch += 1.0f / 30.0f;
        if (time_since_last_punch >= PUNCH_INTERVAL) {
            Protocol_SendPunch(&punch_packetizer, Net_SendPacketCallback, &punch_cb);
            time_since_last_punch = 0.0f;
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
            bytes_received_window += n; // Track bandwidth
            
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
                } else if (packet_type == PACKET_TYPE_AUDIO) {
                    // Decode and play audio
                    if (audio_decoder && audio_playback) {
                        AudioFrame audio_frame = {0};
                        Audio_Decode(audio_decoder, frame_data, frame_size, &audio_frame);
                        if (audio_frame.sample_count > 0) {
                            Audio_WritePlayback(audio_playback, &audio_frame);
                        }
                    }
                    received_any_packet = true;
                } else if (packet_type == PACKET_TYPE_VIDEO) {
                    // 2. Decode
                    EncodedPacket pkt = { .data = frame_data, .size = frame_size };
                    
                    // Decode directly
                    Codec_DecodePacket(decoder, &pkt, &decoded_frame);
                    received_frame_this_tick = true;
                    received_any_packet = true;
                    
                    // Log frame info periodically
                    if (decoded_frame.width > 0 && decoded_frame.height > 0) {
                        if (frames_decoded++ % 30 == 0 && verbose) {
                            printf("Viewer: Decoded Frame %d (%dx%d) from %s:%d\n", 
                                frames_decoded, decoded_frame.width, decoded_frame.height, sender_ip, sender_port);
                        }
                    }
                }
            }
        }
        
        // Poll audio playback to push buffered audio to output
        if (audio_playback) {
            Audio_PollPlayback(audio_playback);
        }
        
        // Update bandwidth measurement
        bandwidth_window_time += 1.0f / 30.0f;
        if (bandwidth_window_time >= BANDWIDTH_WINDOW) {
            current_mbps = (bytes_received_window * 8.0f) / (bandwidth_window_time * 1000000.0f);
            bytes_received_window = 0;
            bandwidth_window_time = 0.0f;
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
                snprintf(meta_text2, sizeof(meta_text2), "RES: %dx%d | FMT: %s | RX: %.1f Mbps", 
                    stream_meta.screen_width, stream_meta.screen_height, stream_meta.format_name, current_mbps);

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
    bool verbose;
    char target_ip[64];
    bool start_app;
} AppConfig;

void RunMenu(MemoryArena *arena, WindowContext *window, AppConfig *config, const PersistentConfig *saved_config) {
    UI_Init(arena); // Init UI Shader
    
    // Initialize from saved config (or defaults if first run)
    strcpy(config->target_ip, saved_config->target_ip);
    config->is_host = saved_config->is_host;
    config->verbose = saved_config->verbose;
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
    
    // Load saved configuration
    PersistentConfig saved_config;
    Config_Load(&saved_config);
    
    // Check CLI overrides
    AppConfig config = {0};
    if (argc > 1 && strcmp(argv[1], "viewer") == 0) {
        config.is_host = false;
        config.start_app = true; // Auto-start if CLI arg provided
        strcpy(config.target_ip, saved_config.target_ip); // Use saved IP
    } else if (argc > 1 && strcmp(argv[1], "host") == 0) {
        config.is_host = true;
        config.start_app = true;
        strcpy(config.target_ip, saved_config.target_ip); // Use saved IP
    }
    
    config.verbose = saved_config.verbose;
    
    // Main application loop - allows returning to menu
    while (1) {
        if (!config.start_app) {
            // Show Menu
            RunMenu(&main_arena, window, &config, &saved_config);
        }
        
        if (config.start_app) {
            // Save config before starting (in case app crashes)
            saved_config.is_host = config.is_host;
            saved_config.verbose = config.verbose;
            strcpy(saved_config.target_ip, config.target_ip);
            Config_Save(&saved_config);
            
            int result;
            if (config.is_host) {
                result = RunHost(&main_arena, window, config.target_ip, config.verbose);
            } else {
                result = RunViewer(&main_arena, window, config.target_ip, config.verbose);
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
