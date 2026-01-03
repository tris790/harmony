#include "audio_api.h"
#include "capture_api.h"
#include "codec_api.h"
#include "config_api.h"
#include "net/protocol.h" // For Packetizer
#include "network_api.h"
#include "os_api.h"
#include "ui/render_api.h"
#include "ui_api.h"
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h> // For getenv
#include <unistd.h>

#include "core/queue.h"
#include "net/aes.h"
#include "net/websocket.h"

// Forward Declaration (should be in a header)

// ... (Headers remain)

// Forward Declaration
void Portal_RequestScreenCast(uint32_t *out_video_node,
                              uint32_t *out_audio_node);

// --- THREADING CONTEXTS ---

typedef struct EncoderThreadContext {
  Queue *frame_queue; // Queue of VideoFrame* (must be copies)
  VideoFormat vfmt;
  MemoryArena *arena;

  // Communication with Network
  NetworkContext *net;
  char viewer_ip[16];
  int viewer_port;
  bool has_viewer;
  OS_Mutex *viewer_mutex;
  OS_Mutex *packetizer_mutex;

  // WebSocket
  WebSocketContext *ws;

  // Encryption
  AES_Ctx aes_ctx;
  bool encryption_enabled;

  Packetizer packetizer;

  bool running;
} EncoderThreadContext;

typedef struct AudioThreadContext {
  AudioCaptureContext *capture;
  AudioEncoder *encoder;

  // Communication with Network
  NetworkContext *net;
  char viewer_ip[16];
  int viewer_port;
  bool has_viewer;
  OS_Mutex *viewer_mutex;

  // WebSocket
  WebSocketContext *ws;

  // Encryption
  AES_Ctx aes_ctx;
  bool encryption_enabled;

  Packetizer *packetizer; // Shared with Video for ID sequence?
  // Actually, Protocol_SendAudio uses the packetizer's frame_id_counter.
  // If they share it, it must be protected.
  OS_Mutex *packetizer_mutex;

  bool running;
} AudioThreadContext;

typedef struct NetReceiverContext {
  NetworkContext *net;
  Queue *video_queue;
  Queue *audio_queue;

  // Shared State
  StreamMetadata *stream_meta;
  OS_Mutex *meta_mutex;

  size_t *bytes_received;
  OS_Mutex *stats_mutex;

  bool running;
} NetReceiverContext;

typedef struct DecoderThreadContext {
  Queue *video_queue;
  DecoderContext *decoder;
  VideoFrame *out_frame;
  OS_Mutex *frame_mutex;
  MemoryArena *arena;

  // Encryption
  AES_Ctx aes_ctx;
  bool encryption_enabled;

  bool running;
} DecoderThreadContext;

typedef struct AudioDecoderThreadContext {
  Queue *audio_queue;
  AudioDecoder *decoder;
  AudioPlaybackContext *playback;

  // Encryption
  AES_Ctx aes_ctx;
  bool encryption_enabled;

  bool running;
} AudioDecoderThreadContext;

// --- HELPER TYPES ---
typedef struct NetCallbackData {
  NetworkContext *net;
  const char *dest_ip;
  int dest_port;
} NetCallbackData;

static void Net_SendPacketCallback(void *user_data, void *packet_data,
                                   size_t packet_size) {
  NetCallbackData *d = (NetCallbackData *)user_data;
  Net_Send(d->net, d->dest_ip, d->dest_port, packet_data, packet_size);
}

// --- THREAD PROCEDURES ---

static void EncoderThreadProc(void *data) {
  EncoderThreadContext *ctx = (EncoderThreadContext *)data;
  printf("EncoderThread: Started\n");

  EncoderContext *encoder = Codec_InitEncoder(ctx->arena, ctx->vfmt);
  if (!encoder) {
    printf("EncoderThread: Failed to initialize encoder\n");
    return;
  }

  MemoryArena packet_arena;
  ArenaInit(&packet_arena, 16 * 1024 * 1024);

  while (ctx->running) {
    VideoFrame *frame = (VideoFrame *)Queue_Pop(ctx->frame_queue);
    if (!frame)
      break; // Shutdown signal

    // Handle resolution change?
    // For now we assume vfmt is constant or thread manages it.
    // Actually, if main thread changes vfmt, it should restart the thread.
    // But let's check if dimensions match.
    if (frame->width != ctx->vfmt.width || frame->height != ctx->vfmt.height) {
      printf("EncoderThread: Resolution change detected in queue! Restarting "
             "encoder.\n");
      Codec_CloseEncoder(encoder);
      ctx->vfmt.width = frame->width;
      ctx->vfmt.height = frame->height;
      encoder = Codec_InitEncoder(ctx->arena, ctx->vfmt);
    }

    if (encoder) {
      ArenaClear(&packet_arena);
      EncodedPacket pkt = {0};
      Codec_EncodeFrame(encoder, frame, &packet_arena, &pkt);

      if (pkt.size > 0) {
        OS_MutexLock(ctx->packetizer_mutex);
        uint32_t current_frame_id = ctx->packetizer.frame_id_counter + 1;

        // Encrypt if enabled
        if (ctx->encryption_enabled) {
          uint8_t iv[16] = {0};
          uint32_t net_id = htonl(current_frame_id);
          memcpy(iv, &net_id, 4);
          AES_CTR_Xcrypt(&ctx->aes_ctx, iv, pkt.data, pkt.size);
        }

        // Send UDP if viewer exists
        OS_MutexLock(ctx->viewer_mutex);
        if (ctx->has_viewer) {
          NetCallbackData net_cb = {.net = ctx->net,
                                    .dest_ip = ctx->viewer_ip,
                                    .dest_port = ctx->viewer_port};
          Protocol_SendFrame(&ctx->packetizer, pkt.data, pkt.size,
                             Net_SendPacketCallback, &net_cb);
        }
        OS_MutexUnlock(ctx->viewer_mutex);
        OS_MutexUnlock(ctx->packetizer_mutex);

        // Broadcast WebSocket
        WS_Broadcast(ctx->ws, PACKET_TYPE_VIDEO, current_frame_id, pkt.data,
                     pkt.size);
      }
    }

    // Free frame data (it was copied)
    if (frame->data[0])
      free(frame->data[0]);
    free(frame);
  }

  if (encoder)
    Codec_CloseEncoder(encoder);
  Queue_Destroy(ctx->frame_queue);
  printf("EncoderThread: Finished\n");
}

static void AudioThreadProc(void *data) {
  AudioThreadContext *ctx = (AudioThreadContext *)data;
  printf("AudioThread: Started\n");

  while (ctx->running) {
    // Poll multiple times to ensure we get all buffered audio
    for (int poll = 0; poll < 5; poll++) {
      Audio_PollCapture(ctx->capture);
    }

    AudioFrame *aframe;
    while ((aframe = Audio_GetCapturedFrame(ctx->capture)) != NULL) {
      EncodedAudio encoded_audio = {0};
      Audio_Encode(ctx->encoder, aframe, &encoded_audio);

      if (encoded_audio.size > 0) {
        OS_MutexLock(ctx->packetizer_mutex);
        uint32_t current_audio_id = ctx->packetizer->frame_id_counter + 1;

        // Encrypt audio if enabled
        if (ctx->encryption_enabled) {
          uint8_t iv[16] = {0};
          uint32_t net_id = htonl(current_audio_id);
          memcpy(iv, &net_id, 4);
          AES_CTR_Xcrypt(&ctx->aes_ctx, iv, encoded_audio.data,
                         encoded_audio.size);
        }

        OS_MutexLock(ctx->viewer_mutex);
        if (ctx->has_viewer) {
          NetCallbackData net_cb = {.net = ctx->net,
                                    .dest_ip = ctx->viewer_ip,
                                    .dest_port = ctx->viewer_port};
          Protocol_SendAudio(ctx->packetizer, encoded_audio.data,
                             encoded_audio.size, Net_SendPacketCallback,
                             &net_cb);
        }
        OS_MutexUnlock(ctx->viewer_mutex);

        WS_Broadcast(ctx->ws, PACKET_TYPE_AUDIO, current_audio_id,
                     encoded_audio.data, encoded_audio.size);
        OS_MutexUnlock(ctx->packetizer_mutex);
      }
    }

    usleep(5000); // 5ms sleep
  }

  printf("AudioThread: Finished\n");
}

static void NetReceiverProc(void *data) {
  NetReceiverContext *ctx = (NetReceiverContext *)data;
  printf("NetReceiverThread: Started\n");

  uint8_t buf[2048];
  char sender_ip[16];
  int sender_port;

  MemoryArena reasm_arena;
  ArenaInit(&reasm_arena, 8 * 1024 * 1024);
  Reassembler video_reassembler;
  Reassembler audio_reassembler;
  Reassembler_Init(&video_reassembler, &reasm_arena);
  Reassembler_Init(&audio_reassembler, &reasm_arena);

  while (ctx->running) {
    int n = Net_Recv(ctx->net, buf, sizeof(buf), sender_ip, &sender_port);
    if (n > 0) {
      OS_MutexLock(ctx->stats_mutex);
      *(ctx->bytes_received) += n;
      OS_MutexUnlock(ctx->stats_mutex);

      if (n < (int)sizeof(PacketHeader))
        continue;
      PacketHeader *peek_header = (PacketHeader *)buf;
      uint8_t ptype = peek_header->packet_type;

      if (ptype == PACKET_TYPE_KEEPALIVE)
        continue;

      if (ptype == PACKET_TYPE_METADATA) {
        uint8_t *payload = buf + sizeof(PacketHeader);
        size_t payload_size = peek_header->payload_size;
        if (payload_size >= sizeof(StreamMetadata) - sizeof(uint32_t) &&
            payload_size <= sizeof(StreamMetadata)) {
          OS_MutexLock(ctx->meta_mutex);
          memset(ctx->stream_meta, 0, sizeof(StreamMetadata));
          memcpy(ctx->stream_meta, payload, payload_size);
          OS_MutexUnlock(ctx->meta_mutex);
        }
        continue;
      }

      void *frame_data = NULL;
      size_t frame_size = 0;
      uint8_t packet_type = 0;
      ReassemblyResult res;

      if (ptype == PACKET_TYPE_VIDEO) {
        res = Protocol_HandlePacket(&video_reassembler, buf, n, &frame_data,
                                    &frame_size, &packet_type);
      } else if (ptype == PACKET_TYPE_AUDIO) {
        res = Protocol_HandlePacket(&audio_reassembler, buf, n, &frame_data,
                                    &frame_size, &packet_type);
      } else {
        continue;
      }

      if (res == RESULT_COMPLETE) {
        uint8_t *qdata = malloc(frame_size);
        memcpy(qdata, frame_data, frame_size);

        EncodedPacket *pkt = malloc(sizeof(EncodedPacket));
        pkt->data = qdata;
        pkt->size = frame_size;
        pkt->pts = (int64_t)peek_header->frame_id;

        if (packet_type == PACKET_TYPE_VIDEO) {
          Queue_Push(ctx->video_queue, pkt);
        } else if (packet_type == PACKET_TYPE_AUDIO) {
          Queue_Push(ctx->audio_queue, pkt);
        } else {
          free(qdata);
          free(pkt);
        }
      }
    } else {
      usleep(1000); // 1ms
    }
  }
  printf("NetReceiverThread: Finished\n");
}

static void DecoderThreadProc(void *data) {
  DecoderThreadContext *ctx = (DecoderThreadContext *)data;
  printf("DecoderThread: Started\n");

  while (ctx->running) {
    EncodedPacket *pkt = (EncodedPacket *)Queue_Pop(ctx->video_queue);
    if (!pkt)
      break;

    if (ctx->encryption_enabled) {
      uint8_t iv[16] = {0};
      uint32_t net_id = htonl((uint32_t)pkt->pts);
      memcpy(iv, &net_id, 4);
      AES_CTR_Xcrypt(&ctx->aes_ctx, iv, pkt->data, pkt->size);

      uint8_t *d = pkt->data;
      bool valid = false;
      if (pkt->size >= 3) {
        if (d[0] == 0 && d[1] == 0 && d[2] == 1)
          valid = true;
        else if (pkt->size >= 4 && d[0] == 0 && d[1] == 0 && d[2] == 0 &&
                 d[3] == 1)
          valid = true;
      }
      if (!valid) {
        static double last_warn_time = 0;
        double now = OS_GetTime();
        if (now - last_warn_time > 2.0) {
          printf("Viewer: Decryption failed (invalid start code) for Frame %u. "
                 "Wrong password? Data: %02X %02X %02X %02X\n",
                 (uint32_t)pkt->pts, d[0], d[1], d[2], d[3]);
          last_warn_time = now;
        }
        free(pkt->data);
        free(pkt);
        continue;
      }
    }

    OS_MutexLock(ctx->frame_mutex);
    Codec_DecodePacket(ctx->decoder, pkt, ctx->out_frame);
    OS_MutexUnlock(ctx->frame_mutex);

    free(pkt->data);
    free(pkt);
  }
  printf("DecoderThread: Finished\n");
}

static void AudioDecoderThreadProc(void *data) {
  AudioDecoderThreadContext *ctx = (AudioDecoderThreadContext *)data;
  printf("AudioDecoderThread: Started\n");

  while (ctx->running) {
    EncodedPacket *pkt = (EncodedPacket *)Queue_Pop(ctx->audio_queue);
    if (!pkt)
      break;

    if (ctx->encryption_enabled) {
      uint8_t iv[16] = {0};
      uint32_t net_id = htonl((uint32_t)pkt->pts);
      memcpy(iv, &net_id, 4);
      AES_CTR_Xcrypt(&ctx->aes_ctx, iv, pkt->data, pkt->size);
    }

    AudioFrame aframe = {0};
    Audio_Decode(ctx->decoder, pkt->data, pkt->size, &aframe);
    if (aframe.sample_count > 0) {
      Audio_WritePlayback(ctx->playback, &aframe);
    }

    free(pkt->data);
    free(pkt);
  }
  printf("AudioDecoderThread: Finished\n");
}

// --- HOST MODE ---

// Calculate target bitrate based on resolution and framerate.
// Uses industry-standard recommendations (YouTube/Twitch/OBS 2024):
// 720p30: 5 Mbps, 720p60: 7.5 Mbps
// 1080p30: 8 Mbps, 1080p60: 12 Mbps
// 1440p60: 24 Mbps
// 4K30: 35 Mbps, 4K60: 50 Mbps
static int CalculateTargetBitrate(int width, int height, int fps) {
  int pixels = width * height;
  bool high_fps = (fps >= 50);

  // Improved bitrates for better visual quality (closer to streaming platform
  // recommendations) 4K (3840x2160 = 8.3M pixels)
  if (pixels >= 8000000) {
    return high_fps ? 35000000 : 25000000; // 35 or 25 Mbps
  }
  // 1440p (2560x1440 = 3.7M pixels)
  if (pixels >= 3500000) {
    return high_fps ? 18000000 : 12000000; // 18 or 12 Mbps
  }
  // 1080p (1920x1080 = 2M pixels)
  if (pixels >= 2000000) {
    return high_fps ? 12000000 : 8000000; // 12 or 8 Mbps
  }
  // 720p (1280x720 = 921k pixels) - Most common
  if (pixels >= 900000) {
    return high_fps ? 7500000 : 5000000; // 7.5 or 5 Mbps
  }
  // Lower resolutions - fallback to formula
  return (int)(width * height * fps * 0.08f);
}

static void UI_DrawMetadataTooltip(WindowContext *window,
                                   const StreamMetadata *meta,
                                   float current_mbps, int frames_decoded) {
  int mx = 0, my = 0;
  OS_GetMouseState(window, &mx, &my, NULL);

  float icon_x = 10.0f;
  float icon_y = 10.0f;
  float icon_size = 24.0f;
  bool hovered = (mx >= icon_x && mx <= icon_x + icon_size && my >= icon_y &&
                  my <= icon_y + icon_size);

  // Draw Help Icon ('?' circle) - always visible
  float icon_alpha = hovered ? 1.0f : 0.6f;
  Render_DrawRoundedRect(icon_x, icon_y, icon_size, icon_size, icon_size * 0.5f,
                         0.0f, 0.0f, 0.0f, icon_alpha * 0.7f);
  Render_DrawText("?", icon_x + 6, icon_y + 4, 1.2f, 1.0f, 1.0f, 1.0f,
                  icon_alpha);

  if (hovered) {
    OS_SetCursor(window, OS_CURSOR_HAND);

    if (meta->screen_width > 0) {
      char meta_text[256];
      snprintf(meta_text, sizeof(meta_text), "HOST: %s | %s", meta->os_name,
               meta->de_name);
      char meta_text2[256];
      snprintf(meta_text2, sizeof(meta_text2),
               "RES: %dx%d | FPS: %u | FMT: %s | RX: %.1f Mbps | Frames: %d",
               meta->screen_width, meta->screen_height, meta->fps,
               meta->format_name, current_mbps, frames_decoded);

      float scale = 1.5f;
      float tw1 = Render_GetTextWidth(meta_text, scale);
      float tw2 = Render_GetTextWidth(meta_text2, scale);
      float max_tw = (tw1 > tw2) ? tw1 : tw2;
      float padding = 10.0f;
      float rect_w = max_tw + padding * 2.0f;
      float rect_h = 70.0f;

      // Draw tooltip next to the icon
      float tx = icon_x + icon_size + 5.0f;
      float ty = icon_y;
      Render_DrawRect(tx, ty, rect_w, rect_h, 0.0f, 0.0f, 0.0f, 0.8f);
      Render_DrawText(meta_text, tx + padding, ty + 20, scale, 1.0f, 1.0f, 1.0f,
                      1.0f);
      Render_DrawText(meta_text2, tx + padding, ty + 50, scale, 0.8f, 0.8f,
                      0.8f, 1.0f);
    }
  } else {
    OS_SetCursor(window, OS_CURSOR_ARROW);
  }
}

int RunHost(MemoryArena *arena, WindowContext *window, const char *target_ip,
            bool verbose, uint32_t audio_node_id, const char *encoder_preset,
            const char *password, const PersistentConfig *config) {
  (void)verbose;
  printf("Starting Multi-Threaded HOST Mode...\n");

  // Request Screen Share Permissions
  printf("Requesting Screen Share... Please acknowledge dialog.\n");
  uint32_t video_node_id = 0;
  uint32_t portal_audio_node = 0;
  Portal_RequestScreenCast(&video_node_id, &portal_audio_node);

  if (video_node_id == 0) {
    printf("FAILURE! Could not get screen stream. Exiting.\n");
    return 1;
  }
  printf("Got Video Node ID: %u, Audio Node ID: %u\n", video_node_id,
         audio_node_id);

  CaptureContext *capture = Capture_Init(arena, video_node_id);
  if (!capture)
    return 1;

  AudioCaptureContext *audio_capture = Audio_InitCapture(arena, audio_node_id);
  AudioEncoder *audio_encoder = Audio_InitEncoder(arena);

  // Main thread Network Setup (shared by worker threads)
  NetworkContext *net = Net_Init(arena, 9999, true);
  if (!net)
    return 1;

  WebSocketContext *ws = WS_Init(arena, 8080);

  // Video Format Setup
  int target_fps = (config && config->fps > 0) ? config->fps : 60;
  int initial_bitrate = CalculateTargetBitrate(1280, 720, target_fps);
  VideoFormat vfmt = {.width = 1280,
                      .height = 720,
                      .fps = target_fps,
                      .bitrate = initial_bitrate};
  strncpy(vfmt.preset, encoder_preset, sizeof(vfmt.preset) - 1);

  // Encryption Setup
  bool encryption_enabled = (password && password[0] != '\0');
  uint8_t master_key[16] = {0};
  if (encryption_enabled) {
    AES_DeriveKey(password, master_key);
  }

  // Threading Synchronization
  OS_Mutex *viewer_mutex = OS_MutexCreate();
  OS_Mutex *packetizer_mutex = OS_MutexCreate();

  // Start Encoder Thread
  EncoderThreadContext encoder_ctx = {0};
  encoder_ctx.frame_queue = Queue_Create();
  encoder_ctx.vfmt = vfmt;
  encoder_ctx.arena = PushStruct(arena, MemoryArena);
  ArenaInit(encoder_ctx.arena, 32 * 1024 * 1024);
  encoder_ctx.net = net;
  strncpy(encoder_ctx.viewer_ip, target_ip, 15);
  encoder_ctx.viewer_port = 9999;
  encoder_ctx.has_viewer = false;
  encoder_ctx.viewer_mutex = viewer_mutex;
  encoder_ctx.packetizer_mutex = packetizer_mutex;
  encoder_ctx.ws = ws;
  encoder_ctx.encryption_enabled = encryption_enabled;
  if (encryption_enabled)
    AES_Init(&encoder_ctx.aes_ctx, master_key);
  encoder_ctx.running = true;
  OS_Thread *encoder_thread = OS_ThreadCreate(EncoderThreadProc, &encoder_ctx);

  // Start Audio Thread
  AudioThreadContext audio_ctx = {0};
  audio_ctx.capture = audio_capture;
  audio_ctx.encoder = audio_encoder;
  audio_ctx.net = net;
  strncpy(audio_ctx.viewer_ip, target_ip, 15);
  audio_ctx.viewer_port = 9999;
  audio_ctx.has_viewer = false;
  audio_ctx.viewer_mutex = viewer_mutex;
  audio_ctx.ws = ws;
  audio_ctx.encryption_enabled = encryption_enabled;
  if (encryption_enabled)
    AES_Init(&audio_ctx.aes_ctx, master_key);
  audio_ctx.packetizer = &encoder_ctx.packetizer; // Interleave IDs
  audio_ctx.packetizer_mutex = packetizer_mutex;
  audio_ctx.running = true;
  OS_Thread *audio_thread = OS_ThreadCreate(AudioThreadProc, &audio_ctx);

  // Metadata & Packetizer for Main Thread (Metadata/Punches)
  StreamMetadata metadata = {0};
  strcpy(metadata.os_name, "Linux");
  const char *env_de = getenv("XDG_CURRENT_DESKTOP");
  strcpy(metadata.de_name, env_de ? env_de : "Unknown");
  strcpy(metadata.format_name, "BGRx");
  metadata.fps = vfmt.fps;

  int frame_count = 0;
  float elapsed_time = 0.0f;
  float time_since_host_punch = 0.0f;
  const float HOST_PUNCH_INTERVAL = 0.5f;

  int result = 0;
  while (OS_ProcessEvents(window)) {
    if (OS_IsEscapePressed()) {
      result = 2;
      break;
    }

    WS_Poll(ws);

    int w, h;
    OS_GetWindowSize(window, &w, &h);
    Render_SetScreenSize(w, h);

    elapsed_time += 1.0f / (float)vfmt.fps;
    time_since_host_punch += 1.0f / (float)vfmt.fps;

    // Periodic Punches (Main Thread)
    if (time_since_host_punch >= HOST_PUNCH_INTERVAL) {
      OS_MutexLock(packetizer_mutex);
      Protocol_SendPunch(&encoder_ctx.packetizer, Net_SendPacketCallback,
                         &(NetCallbackData){net, target_ip, 9999});
      OS_MutexUnlock(packetizer_mutex);
      time_since_host_punch = 0.0f;
    }

    // Receive Punch / Update Viewer (Main Thread)
    {
      uint8_t punch_buf[64];
      char incoming_ip[16];
      int incoming_port;
      int n;
      while ((n = Net_Recv(net, punch_buf, sizeof(punch_buf), incoming_ip,
                           &incoming_port)) > 0) {
        if (n >= (int)sizeof(PacketHeader)) {
          PacketHeader *hdr = (PacketHeader *)punch_buf;
          if (hdr->packet_type == PACKET_TYPE_PUNCH) {
            OS_MutexLock(viewer_mutex);
            if (!encoder_ctx.has_viewer ||
                strcmp(encoder_ctx.viewer_ip, incoming_ip) != 0) {
              strncpy(encoder_ctx.viewer_ip, incoming_ip, 15);
              strncpy(audio_ctx.viewer_ip, incoming_ip, 15);
              encoder_ctx.has_viewer = true;
              audio_ctx.has_viewer = true;
              printf("Host: Viewer connected from %s:%d\n", incoming_ip,
                     incoming_port);
            }
            OS_MutexUnlock(viewer_mutex);
          }
        }
      }
    }

    // Capture Loop
    Capture_Poll(capture);
    VideoFrame *frame = Capture_GetFrame(capture);
    if (frame) {
      frame_count++;

      // Send Metadata periodically
      if (frame_count % vfmt.fps == 0) {
        metadata.screen_width = frame->width;
        metadata.screen_height = frame->height;
        OS_MutexLock(packetizer_mutex);
        OS_MutexLock(viewer_mutex);
        if (encoder_ctx.has_viewer) {
          NetCallbackData net_cb = {
              .net = net, .dest_ip = encoder_ctx.viewer_ip, .dest_port = 9999};
          Protocol_SendMetadata(&encoder_ctx.packetizer, &metadata,
                                Net_SendPacketCallback, &net_cb);
        }
        OS_MutexUnlock(viewer_mutex);
        OS_MutexUnlock(packetizer_mutex);
      }

      // Copy frame and push to worker queue
      VideoFrame *qframe = malloc(sizeof(VideoFrame));
      *qframe = *frame;
      size_t data_size = frame->height * frame->linesize[0];
      qframe->data[0] = malloc(data_size);
      memcpy(qframe->data[0], frame->data[0], data_size);
      Queue_Push(encoder_ctx.frame_queue, qframe);
    }

    // Status UI
    UI_DrawStreamStatus(w, h, elapsed_time, frame_count, target_ip,
                        metadata.screen_width, metadata.screen_height,
                        frame_count > 0);

    OS_SwapBuffers(window);
  }

  // Stop Worker Threads
  encoder_ctx.running = false;
  audio_ctx.running = false;
  Queue_Push(encoder_ctx.frame_queue, NULL); // Unblock encoder thread

  OS_ThreadJoin(encoder_thread);
  OS_ThreadJoin(audio_thread);

  // Cleanup
  OS_MutexDestroy(viewer_mutex);
  OS_MutexDestroy(packetizer_mutex);
  if (audio_capture)
    Audio_CloseCapture(audio_capture);
  if (audio_encoder)
    Audio_CloseEncoder(audio_encoder);
  if (capture)
    Capture_Close(capture);
  if (net)
    Net_Close(net);
  if (ws)
    WS_Shutdown(ws);

  return result;
}

// --- VIEWER MODE ---
int RunViewer(MemoryArena *arena, WindowContext *window, const char *host_ip,
              bool verbose, const char *password) {
  (void)verbose;
  printf("Starting Multi-Threaded VIEWER Mode...\n");

  NetworkContext *net = Net_Init(arena, 9999, true);
  if (!net)
    return 1;

  Packetizer punch_packetizer = {0};
  NetCallbackData punch_cb = {
      .net = net, .dest_ip = host_ip, .dest_port = 9999};

  DecoderContext *decoder = Codec_InitDecoder(arena);
  AudioDecoder *audio_decoder = Audio_InitDecoder(arena);
  AudioPlaybackContext *audio_playback = Audio_InitPlayback(arena);

  bool encryption_enabled = (password && password[0] != '\0');
  uint8_t master_key[16] = {0};
  if (encryption_enabled) {
    AES_DeriveKey(password, master_key);
  }

  // Shared State
  VideoFrame decoded_frame = {0};
  StreamMetadata stream_meta = {0};
  size_t bytes_received_window = 0;
  float current_mbps = 0.0f;

  // Threading Synchronization
  Queue *video_queue = Queue_Create();
  Queue *audio_queue = Queue_Create();
  OS_Mutex *meta_mutex = OS_MutexCreate();
  OS_Mutex *stats_mutex = OS_MutexCreate();
  OS_Mutex *frame_mutex = OS_MutexCreate();

  // Worker Threads
  NetReceiverContext net_ctx = {0};
  net_ctx.net = net;
  net_ctx.video_queue = video_queue;
  net_ctx.audio_queue = audio_queue;
  net_ctx.stream_meta = &stream_meta;
  net_ctx.meta_mutex = meta_mutex;
  net_ctx.bytes_received = &bytes_received_window;
  net_ctx.stats_mutex = stats_mutex;
  net_ctx.running = true;
  OS_Thread *net_thread = OS_ThreadCreate(NetReceiverProc, &net_ctx);

  DecoderThreadContext decoder_ctx = {0};
  decoder_ctx.video_queue = video_queue;
  decoder_ctx.decoder = decoder;
  decoder_ctx.out_frame = &decoded_frame;
  decoder_ctx.frame_mutex = frame_mutex;
  decoder_ctx.arena = PushStruct(arena, MemoryArena);
  ArenaInit(decoder_ctx.arena, 32 * 1024 * 1024);
  decoder_ctx.encryption_enabled = encryption_enabled;
  if (encryption_enabled)
    AES_Init(&decoder_ctx.aes_ctx, master_key);
  decoder_ctx.running = true;
  OS_Thread *decoder_thread = OS_ThreadCreate(DecoderThreadProc, &decoder_ctx);

  AudioDecoderThreadContext audio_decoder_ctx = {0};
  audio_decoder_ctx.audio_queue = audio_queue;
  audio_decoder_ctx.decoder = audio_decoder;
  audio_decoder_ctx.playback = audio_playback;
  audio_decoder_ctx.encryption_enabled = encryption_enabled;
  if (encryption_enabled)
    AES_Init(&audio_decoder_ctx.aes_ctx, master_key);
  audio_decoder_ctx.running = true;
  OS_Thread *audio_decoder_thread =
      OS_ThreadCreate(AudioDecoderThreadProc, &audio_decoder_ctx);

  float time_since_last_punch = 0.0f;
  const float PUNCH_INTERVAL = 0.5f;
  float bandwidth_window_time = 0.0f;
  const float BANDWIDTH_WINDOW = 1.0f;

  int result = 0;
  while (OS_ProcessEvents(window)) {
    if (OS_IsEscapePressed()) {
      result = 2;
      break;
    }

    // Punch Loop (Main Thread)
    time_since_last_punch += 1.0f / 60.0f;
    if (time_since_last_punch >= PUNCH_INTERVAL) {
      Protocol_SendPunch(&punch_packetizer, Net_SendPacketCallback, &punch_cb);
      time_since_last_punch = 0.0f;
    }

    // Bandwidth Measurement (Main Thread)
    bandwidth_window_time += 1.0f / 60.0f;
    if (bandwidth_window_time >= BANDWIDTH_WINDOW) {
      OS_MutexLock(stats_mutex);
      current_mbps =
          (bytes_received_window * 8.0f) / (bandwidth_window_time * 1000000.0f);
      bytes_received_window = 0;
      OS_MutexUnlock(stats_mutex);
      bandwidth_window_time = 0.0f;
    }

    // Rendering Loop
    int win_w, win_h;
    OS_GetWindowSize(window, &win_w, &win_h);
    Render_SetScreenSize(win_w, win_h);

    OS_MutexLock(frame_mutex);
    if (decoded_frame.width > 0 && decoded_frame.height > 0) {
      Render_DrawFrame(&decoded_frame, win_w, win_h);
      OS_MutexUnlock(frame_mutex);

      OS_MutexLock(meta_mutex);
      UI_DrawMetadataTooltip(window, &stream_meta, current_mbps,
                             0); // frame count tracking?
      OS_MutexUnlock(meta_mutex);
    } else {
      OS_MutexUnlock(frame_mutex);
      Render_Clear(0.1f, 0.1f, 0.1f, 1.0f);
      const char *wait_msg = "Waiting for stream (Multi-Threaded)...";
      float tw = Render_GetTextWidth(wait_msg, 2.0f);
      Render_DrawText(wait_msg, (win_w - tw) / 2.0f, win_h / 2.0f, 2.0f, 0.8f,
                      0.8f, 0.8f, 1.0f);
    }

    OS_SwapBuffers(window);
  }

  // Stop Worker Threads
  net_ctx.running = false;
  decoder_ctx.running = false;
  audio_decoder_ctx.running = false;
  Queue_Push(video_queue, NULL);
  Queue_Push(audio_queue, NULL);

  OS_ThreadJoin(net_thread);
  OS_ThreadJoin(decoder_thread);
  OS_ThreadJoin(audio_decoder_thread);

  // Cleanup
  OS_MutexDestroy(meta_mutex);
  OS_MutexDestroy(stats_mutex);
  OS_MutexDestroy(frame_mutex);
  if (decoder)
    Codec_CloseDecoder(decoder);
  if (audio_decoder)
    Audio_CloseDecoder(audio_decoder);
  if (audio_playback)
    Audio_ClosePlayback(audio_playback);
  if (net)
    Net_Close(net);

  return result;
}

// --- MENU ---
typedef struct AppConfig {
  bool is_host;
  bool verbose;
  char target_ip[64];
  char stream_password[64];
  uint32_t fps;
  uint32_t selected_audio_node_id;
  bool start_app;
} AppConfig;

static void GetPublicIP(char *buffer, int size) {
  FILE *fp = popen("curl -s https://api.ipify.org", "r");
  if (fp) {
    if (fgets(buffer, size, fp) != NULL) {
      // Strip newline if present
      size_t len = strlen(buffer);
      if (len > 0 && buffer[len - 1] == '\n') {
        buffer[len - 1] = '\0';
      }
    }
    pclose(fp);
  }
}

void RunMenu(MemoryArena *arena, WindowContext *window, AppConfig *config,
             const PersistentConfig *saved_config) {
  UI_Init(arena); // Init UI Shader

  // Initialize from saved config (or defaults if first run)
  strcpy(config->target_ip, saved_config->target_ip);
  strcpy(config->stream_password, saved_config->stream_password);
  config->is_host = saved_config->is_host;
  config->verbose = saved_config->verbose;
  config->fps = saved_config->fps;
  config->selected_audio_node_id = 0; // Default System
  config->start_app = false;

  // Audio Node List
  MemoryArena temp_arena;
  ArenaInit(&temp_arena, 1024 * 1024); // 1MB temp
  AudioNodeList node_list = {0};
  AudioNodeList full_list = {0};

  // Initial Enumeration
  Audio_EnumerateNodes(&temp_arena, &node_list);
  full_list.nodes =
      ArenaPush(&temp_arena, (node_list.count + 1) * sizeof(AudioNodeInfo));
  full_list.count = node_list.count + 1;
  full_list.nodes[0].id = 0;
  strcpy(full_list.nodes[0].name, "[All] System Audio");
  for (int i = 0; i < node_list.count; i++) {
    full_list.nodes[i + 1] = node_list.nodes[i];
  }

  while (OS_ProcessEvents(window)) {
    if (OS_IsF11Pressed()) {
      static bool fs = false;
      fs = !fs;
      OS_SetFullscreen(window, fs);
    }
    int w = 1280, h = 720;
    OS_GetWindowSize(window, &w, &h);
    int mx = 0, my = 0;
    bool mdown = false;
    OS_GetMouseState(window, &mx, &my, &mdown);
    int mscroll = OS_GetMouseScroll(window);
    char c = OS_GetLastChar(window);
    bool paste = OS_IsPastePressed();
    bool ctrl = OS_IsCtrlDown();
    bool shift = OS_IsShiftDown();
    bool enter = OS_IsEnterPressed();

    UI_BeginFrame(w, h, mx, my, mdown, mscroll, c, paste, ctrl, shift);

    // Draw Background
    // Mocha Base: #1e1e2e -> 0.12, 0.12, 0.18
    Render_Clear(0.12f, 0.12f, 0.18f, 1.0f);

    // Draw Menu
    // Helper to center
    int cx = w / 2;
    int cy = h / 2;

    // Title
    // Lavender: #b4befe -> 0.71, 0.75, 1.00
    // Scale 4.0 for title
    UI_CenterNext(0); // 0 width means text width
    UI_Label("Harmony Screen Share", 0, cy - 250, 4.0f);

    // Mode Selection Labels (Subtitles)
    // Subtext1: #a6adc8
    UI_CenterNext(0);
    UI_Label("Select Mode:", 0, cy - 180, 2.0f);

    // Buttons
    // Host Button

    // Use different color/style for selected? ui_simple.c handles hover, but we
    // want 'Selected' state visualization We can just draw a selection
    // indicator or rely on the label below.

    if (UI_Button("HOST MODE", cx - 210, cy - 130, 200, 60)) {
      config->is_host = true;
    }

    if (UI_Button("VIEWER MODE", cx + 10, cy - 130, 200, 60)) {
      config->is_host = false;
    }

    if (config->is_host) {
      Render_DrawRect(cx - 210, cy - 65, 200, 4, 0.71f, 0.75f, 1.0f,
                      1.0f); // Underline Host
    } else {
      Render_DrawRect(cx + 10, cy - 65, 200, 4, 0.71f, 0.75f, 1.0f,
                      1.0f); // Underline Viewer
    }

    // --- Public IP Section (Shared) ---
    static char public_ip[64] = "";
    static bool ip_fetched = false;
    static bool ip_copied = false;

    int ip_y = cy - 40;
    if (!ip_fetched) {
      UI_CenterNext(0);
      if (UI_Button("Show Public IP", 0, ip_y, 160, 30)) {
        GetPublicIP(public_ip, sizeof(public_ip));
        if (strlen(public_ip) > 0) {
          ip_fetched = true;
          ip_copied = false;
        }
      }
    } else {
      char buf[128];
      snprintf(buf, sizeof(buf), "Public IP: %s", public_ip);
      float tw = Render_GetTextWidth(buf, 1.8f);
      int copy_btn_w = 80;
      int gap = 10;
      int total_w = (int)tw + gap + copy_btn_w;

      int start_x = cx - total_w / 2;

      Render_DrawText(buf, start_x, ip_y + 5, 1.8f, 0.9f, 0.9f, 0.95f, 1.0f);

      // Copy Button next to it
      if (UI_Button(ip_copied ? "Copied!" : "Copy", start_x + (int)tw + gap,
                    ip_y, copy_btn_w, 30)) {
        OS_SetClipboardText(public_ip);
        ip_copied = true;
      }
    }

    // Host Mode Specifics

    // IP Config
    UI_CenterNext(0);
    UI_Label("Target IP Address:", 0, cy + 20, 2.0f);

    int input_w = 350;
    UI_CenterNext(input_w);
    UI_TextInput("ip_input", config->target_ip, 64, 0, cy + 50, input_w, 50,
                 UI_INPUT_NUMERIC);

    // Password Config
    UI_CenterNext(0);
    UI_Label("Stream Password:", 0, cy + 105, 1.8f);

    UI_CenterNext(input_w);
    UI_TextInput("pass_input", config->stream_password, 64, 0, cy + 130,
                 input_w, 40, UI_INPUT_PASSWORD);

    // Audio Source List
    if (config->is_host) {
      int audio_y = cy + 175;
      UI_CenterNext(0);
      UI_Label("Audio Source:", 0, audio_y, 2.0f);

      // List Widget (Now Dropdown)
      int dropdown_w = 400;
      UI_CenterNext(dropdown_w);
      if (UI_Dropdown("audio_list", full_list.nodes, full_list.count,
                      &config->selected_audio_node_id, 0, audio_y + 30,
                      dropdown_w, 40)) {
        // Refresh on open!
        ArenaClear(&temp_arena);
        node_list.count = 0;
        Audio_EnumerateNodes(&temp_arena, &node_list);

        full_list.nodes = ArenaPush(&temp_arena, (node_list.count + 1) *
                                                     sizeof(AudioNodeInfo));
        full_list.count = node_list.count + 1;
        full_list.nodes[0].id = 0;
        strcpy(full_list.nodes[0].name, "[All] System Audio");
        for (int i = 0; i < node_list.count; i++) {
          full_list.nodes[i + 1] = node_list.nodes[i];
        }
      }
    }

    // Adjust Start Button Y position to be below list
    // List ends at audio_y + 30 + 40 = audio_y + 70 = cy + 250

    // Start Button
    UI_CenterNext(250);
    if (UI_Button("START HARMONY", 0, cy + 260, 250, 70) || enter) {
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

  WindowContext *window =
      OS_CreateWindow(&main_arena, 1280, 720, "Harmony Screen Share");
  if (!window)
    return 1;

  // Initialize the renderer once for the entire application session
  Render_Init(&main_arena);

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
      // saved_config.use_portal_audio = config.use_portal_audio; // Removed for
      // now
      strcpy(saved_config.target_ip, config.target_ip);
      strcpy(saved_config.stream_password, config.stream_password);
      saved_config.fps = config.fps;
      Config_Save(&saved_config);

      int result;
      if (config.is_host) {
        result =
            RunHost(&main_arena, window, config.target_ip, config.verbose,
                    config.selected_audio_node_id, saved_config.encoder_preset,
                    config.stream_password, &saved_config);
      } else {
        result = RunViewer(&main_arena, window, config.target_ip,
                           config.verbose, config.stream_password);
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
