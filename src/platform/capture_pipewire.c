#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "../memory_arena.h"
#include "../codec_api.h"
#include "../capture_api.h"

// Capture State
struct CaptureContext {
    struct pw_main_loop *loop;
    struct pw_context *context;
    struct pw_core *core;
    struct pw_stream *stream;
    
    struct spa_hook stream_listener;
    
    VideoFrame current_frame;
    bool frame_ready;
    
    MemoryArena *arena; 
    
    // Dynamic State
    int32_t current_width;
    int32_t current_height;
    int32_t current_stride;
    size_t data_capacity;
};

// ... on_process ... (keep existing)

// ... (Callback for Process)
// ... (Callback for Process)

// ... (Callback for Process)

static void on_param_changed(void *data, uint32_t id, const struct spa_pod *param) {
    CaptureContext *ctx = (CaptureContext *)data;
    
    if (param == NULL || id != SPA_PARAM_Format) return;
    
    struct spa_video_info_raw info;
    if (spa_format_video_raw_parse(param, &info) < 0) return;
    
    if (info.format != SPA_VIDEO_FORMAT_BGRx && info.format != SPA_VIDEO_FORMAT_BGRA) {
        fprintf(stderr, "Capture: Unsupported param format %d\n", info.format);
        return;
    }
    
    ctx->current_width = info.size.width;
    ctx->current_height = info.size.height;
    // Stride is not in format info usually, it's per buffer or guessed.
    // However, we just store W/H here.
    
    printf("Capture: Format Changed to %dx%d\n", ctx->current_width, ctx->current_height);
}

static void on_process(void *data) {
    CaptureContext *ctx = (CaptureContext *)data;
    struct pw_buffer *b;
    struct spa_buffer *buf;
    
    if ((b = pw_stream_dequeue_buffer(ctx->stream)) == NULL) {
        pw_log_warn("out of buffers: %m");
        return;
    }
    
    buf = b->buffer;
    if (buf->datas[0].data == NULL) return; // No data

    // Use current dimensions from param_changed
    int width = ctx->current_width;
    int height = ctx->current_height;
    
    if (width == 0 || height == 0) {
        // Fallback or wait for param_changed
        width = 1280; 
        height = 720;
    }

    int packed_stride = width * 4;
    
    // Allocate Destination (Packed BGRx)
    // Re-allocate if dimensions changed
    bool need_alloc = false;
    if (!ctx->current_frame.data[0]) need_alloc = true;
    else if (ctx->current_frame.width != width || ctx->current_frame.height != height) need_alloc = true;
    
    if (need_alloc) {
         size_t required_size = height * packed_stride;
         if (required_size > ctx->data_capacity) {
             ctx->current_frame.data[0] = ArenaPush(ctx->arena, required_size);
             ctx->data_capacity = required_size;
         }
         ctx->current_frame.width = width;
         ctx->current_frame.height = height;
         ctx->current_frame.linesize[0] = packed_stride;
         ctx->current_frame.data[1] = NULL;
         ctx->current_frame.data[2] = NULL;
    }
    
    // Copy Row-by-Row
    uint32_t src_stride = buf->datas[0].chunk->stride;
    uint32_t copy_width = ((uint32_t)packed_stride < src_stride) ? (uint32_t)packed_stride : src_stride;
    uint8_t *src = buf->datas[0].data;
    uint8_t *dst = ctx->current_frame.data[0];
    
    // Safety check
    if (src && dst) {
        for (int y = 0; y < height; ++y) {
            memcpy(dst + y * packed_stride, src + y * src_stride, copy_width);
        }
    }
    
    ctx->frame_ready = true;
    
    pw_stream_queue_buffer(ctx->stream, b);
}

// ... rest of Init ...

static const struct pw_stream_events stream_events = {
    .version = PW_VERSION_STREAM_EVENTS,
    .process = on_process,
    .param_changed = on_param_changed,
};

CaptureContext* Capture_Init(MemoryArena *arena, uint32_t node_id) {
    CaptureContext *ctx = PushStructZero(arena, CaptureContext);
    ctx->arena = arena;
    
    pw_init(NULL, NULL);
    
    ctx->loop = pw_main_loop_new(NULL);
    ctx->context = pw_context_new(pw_main_loop_get_loop(ctx->loop), NULL, 0);
    ctx->core = pw_context_connect(ctx->context, NULL, 0);
    
    if (!ctx->core) {
        fprintf(stderr, "Failed to connect to PipeWire\n");
        return NULL;
    }
    
    ctx->stream = pw_stream_new_simple(
        pw_main_loop_get_loop(ctx->loop),
        "harmony-capture",
        pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Video",
            PW_KEY_MEDIA_CATEGORY, "Capture",
            PW_KEY_MEDIA_ROLE, "Screen",
            NULL
        ),
        &stream_events,
        ctx 
    );
    
    const struct spa_pod *params[1];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(NULL, 0);
    uint8_t buffer[1024];
    spa_pod_builder_init(&b, buffer, sizeof(buffer));
    
    params[0] = spa_pod_builder_add_object(&b,
        SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
        SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        SPA_FORMAT_VIDEO_format, SPA_POD_Id(SPA_VIDEO_FORMAT_BGRx),
        SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(
            &SPA_RECTANGLE(1280, 720),
            &SPA_RECTANGLE(1, 1),
            &SPA_RECTANGLE(4096, 4096))
    );
    
    int ret = pw_stream_connect(
        ctx->stream,
        PW_DIRECTION_INPUT,
        node_id,
        PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS,
        params, 1
    );
    
    if (ret < 0) {
        fprintf(stderr, "PipeWire Connect failed: %s\n", strerror(-ret));
        return NULL;
    }
    
    return ctx;
}

void Capture_Poll(CaptureContext *ctx) {
    pw_loop_iterate(pw_main_loop_get_loop(ctx->loop), 0);
}

struct VideoFrame* Capture_GetFrame(CaptureContext *ctx) {
    if (ctx->frame_ready) {
        ctx->frame_ready = false;
        return &ctx->current_frame;
    }
    return NULL;
}

void Capture_Close(CaptureContext *ctx) {
    if (ctx) {
        if (ctx->stream) pw_stream_destroy(ctx->stream);
        if (ctx->core) pw_core_disconnect(ctx->core);
        if (ctx->context) pw_context_destroy(ctx->context);
        if (ctx->loop) pw_main_loop_destroy(ctx->loop);
        pw_deinit();
    }
}
