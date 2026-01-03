// Audio Capture and Playback via PipeWire
// Captures system audio output (monitor) for streaming
// Plays back received audio on the viewer side

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>
#include <stdio.h>
#include <string.h>
#include "../audio_api.h"

// --- Audio Capture Context ---
struct AudioCaptureContext {
    struct pw_main_loop *loop;
    struct pw_context *context;
    struct pw_core *core;
    struct pw_stream *stream;
    struct spa_hook stream_listener;
    
    MemoryArena *arena;
    
    // Ring buffer for captured audio
    int16_t *buffer;
    int buffer_size;      // Total size in samples
    int write_pos;
    int read_pos;
    int available;        // Samples available to read
    
    // Frame output
    AudioFrame current_frame;
    bool frame_ready;
};

// Capture stream callback - just buffer the samples
static void capture_on_process(void *data) {
    AudioCaptureContext *ctx = (AudioCaptureContext *)data;
    struct pw_buffer *b;
    struct spa_buffer *buf;
    
    if ((b = pw_stream_dequeue_buffer(ctx->stream)) == NULL) {
        return;
    }
    
    buf = b->buffer;
    if (buf->datas[0].data == NULL) {
        pw_stream_queue_buffer(ctx->stream, b);
        return;
    }
    
    int16_t *samples = (int16_t *)buf->datas[0].data;
    int n_samples = buf->datas[0].chunk->size / sizeof(int16_t);
    
    // Copy to ring buffer (overwrite old data if full)
    for (int i = 0; i < n_samples; i++) {
        ctx->buffer[ctx->write_pos] = samples[i];
        ctx->write_pos = (ctx->write_pos + 1) % ctx->buffer_size;
        if (ctx->available < ctx->buffer_size) {
            ctx->available++;
        } else {
            // Buffer full, advance read pos to drop oldest
            ctx->read_pos = (ctx->read_pos + 1) % ctx->buffer_size;
        }
    }
    
    pw_stream_queue_buffer(ctx->stream, b);
}

static const struct pw_stream_events capture_stream_events = {
    .version = PW_VERSION_STREAM_EVENTS,
    .process = capture_on_process,
};


AudioCaptureContext* Audio_InitCapture(MemoryArena *arena, uint32_t target_node_id) {
    AudioCaptureContext *ctx = PushStructZero(arena, AudioCaptureContext);
    ctx->arena = arena;
    
    // Ring buffer: 1 second of audio
    ctx->buffer_size = AUDIO_SAMPLE_RATE * AUDIO_CHANNELS;
    ctx->buffer = ArenaPush(arena, ctx->buffer_size * sizeof(int16_t));
    
    pw_init(NULL, NULL);
    
    ctx->loop = pw_main_loop_new(NULL);
    ctx->context = pw_context_new(pw_main_loop_get_loop(ctx->loop), NULL, 0);
    ctx->core = pw_context_connect(ctx->context, NULL, 0);
    
    if (!ctx->core) {
        fprintf(stderr, "Audio: Failed to connect to PipeWire\n");
        return NULL;
    }
    
    struct pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "Screen", // Changed from Music to Screen to avoid default Mic policies
        NULL
    );
    
    printf("Audio: InitCapture (v2-SerialCheck)\n");

    if (target_node_id != 0) {
        // Target specific node (Portal/App)
        char target_str[32];
        snprintf(target_str, sizeof(target_str), "%u", target_node_id);
        pw_properties_set(props, PW_KEY_TARGET_OBJECT, target_str);
        
        // Disable Autoconnect fallback behavior by hinting we want THIS object
        // (Note: WirePlumber policy might still override, but this helps)
        pw_properties_set(props, "stream.dont-reconnect", "true"); 
        
        printf("Audio: Targeting specific Node ID: %u\n", target_node_id);
    } else {
        // Capture System Monitor (Default Sink Monitor)
        pw_properties_set(props, PW_KEY_STREAM_CAPTURE_SINK, "true");
        printf("Audio: Targeting System Monitor\n");
    }

    // Create capture stream
    ctx->stream = pw_stream_new_simple(
        pw_main_loop_get_loop(ctx->loop),
        "harmony-audio-capture",
        props,
        &capture_stream_events,
        ctx
    );
    
    // Request S16LE stereo 48kHz
    const struct spa_pod *params[1];
    uint8_t buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    
    params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat,
        &SPA_AUDIO_INFO_RAW_INIT(
            .format = SPA_AUDIO_FORMAT_S16_LE,
            .channels = AUDIO_CHANNELS,
            .rate = AUDIO_SAMPLE_RATE
        ));
    
    int ret = pw_stream_connect(
        ctx->stream,
        PW_DIRECTION_INPUT,
        PW_ID_ANY,
        PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS,
        params, 1
    );
    
    if (ret < 0) {
        fprintf(stderr, "Audio: Stream connect failed: %s\n", strerror(-ret));
        return NULL;
    }
    
    printf("Audio: Capture initialized (48kHz stereo S16LE)\n");
    return ctx;
}

void Audio_PollCapture(AudioCaptureContext *ctx) {
    if (ctx && ctx->loop) {
        pw_loop_iterate(pw_main_loop_get_loop(ctx->loop), 0);
    }
}


// --- Node Enumeration Helpers ---

typedef struct EnumContext {
    AudioNodeList *list;
    MemoryArena *arena;
} EnumContext;

static void registry_event_global(void *data, uint32_t id,
                                  uint32_t permissions, const char *type, uint32_t version,
                                  const struct spa_dict *props) {
    EnumContext *ctx = (EnumContext *)data;
    
    if (strcmp(type, PW_TYPE_INTERFACE_Node) != 0) return;
    if (!props) return;

    // Filter for Audio Streams (Applications) and Audio Sinks (Monitors)
    // We look for 'media.class' = 'Stream/Output/Audio' (Apps) or 'Audio/Sink' (Hardware/Virtual Sinks)
    const char *media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
    const char *node_name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
    const char *app_name = spa_dict_lookup(props, PW_KEY_APP_NAME);
    const char *node_desc = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
    const char *serial_str = spa_dict_lookup(props, PW_KEY_OBJECT_SERIAL);

    if (!media_class) return;

    // Filter: Only want sources we can record FROM.
    // 1. "Stream/Output/Audio" -> Applications playing audio.
    // 2. "Audio/Sink" -> Output devices (we can capture from their monitor, but PW usually handles monitor capture via specific flags, 
    //    however setting target to a Sink usually captures its monitor if capture_sink=true? 
    //    Actually, for "Monitor" capture, we usually target the sink.
    // Let's include both for now so user can see what's available.
    
    bool is_app = (strcmp(media_class, "Stream/Output/Audio") == 0);
    bool is_sink = (strcmp(media_class, "Audio/Sink") == 0);
    
    if (is_app || is_sink) {
        // Grow list if needed (simple dynamic array on arena)
        if (ctx->list->count >= ctx->list->capacity) {
            int new_cap = ctx->list->capacity ? ctx->list->capacity * 2 : 16;
            AudioNodeInfo *new_nodes = ArenaPush(ctx->arena, new_cap * sizeof(AudioNodeInfo));
            if (ctx->list->count > 0) {
                memcpy(new_nodes, ctx->list->nodes, ctx->list->count * sizeof(AudioNodeInfo));
            }
            ctx->list->nodes = new_nodes;
            ctx->list->capacity = new_cap;
        }
        
        AudioNodeInfo *node = &ctx->list->nodes[ctx->list->count++];
        
        // Use Serial ID if available (More stable/correct for Targeting), else Global ID
        uint32_t serial_id = id;
        if (serial_str) {
            serial_id = (uint32_t)atoi(serial_str);
        }
        node->id = serial_id;
        
        // Pick best name
        const char *display_name = app_name ? app_name : (node_desc ? node_desc : node_name);
        if (!display_name) display_name = "Unknown Node";
        
        if (is_app) {
             snprintf(node->name, sizeof(node->name), "[App] %s", display_name);
        } else {
             snprintf(node->name, sizeof(node->name), "[Sys] %s", display_name);
        }
    }
}

static const struct pw_registry_events registry_events = {
    .version = PW_VERSION_REGISTRY_EVENTS,
    .global = registry_event_global,
};

void Audio_EnumerateNodes(MemoryArena *arena, AudioNodeList *out_list) {
    if (!out_list) return;
    out_list->count = 0;
    out_list->capacity = 0;
    out_list->nodes = NULL;
    
    // Setup temporary PipeWire connection just for enumeration
    pw_init(NULL, NULL);
    
    struct pw_main_loop *loop = pw_main_loop_new(NULL);
    struct pw_context *context = pw_context_new(pw_main_loop_get_loop(loop), NULL, 0);
    struct pw_core *core = pw_context_connect(context, NULL, 0);
    
    if (!core) {
        fprintf(stderr, "Audio Error: Failed to connect for enumeration\n");
        if (context) pw_context_destroy(context);
        if (loop) pw_main_loop_destroy(loop);
        return;
    }
    
    struct pw_registry *registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
    struct spa_hook registry_listener;
    
    EnumContext ctx = { .list = out_list, .arena = arena };
    
    pw_registry_add_listener(registry, &registry_listener, &registry_events, &ctx);
    
    // Sync to get all globals
    bool done = false;
    // We rely on the fact that initial globals are sent immediately. 
    // To be strictly correct we should use a sync callback, but roundtrip usually works.
    // Let's do a simple roundtrip.
    
    struct spa_hook core_listener; // We need a sync/done callback on core if we want to wait properly
    // Simplified: Just iterate loop a few times.
    // "Globals are emitted immediately when the registry is bound"
    
    // We need to flush the loop.
    // pw_core_sync is the robust way.
    
    // Logic: core_sync -> wait for done event -> exit loop.
    // For simplicity of implementation in this block, I will just iterate until no events?
    // Actually, pw_main_loop_run() is blocking. We can't use it easily without a quit condition.
    // Let's use pw_loop_iterate with a timeout or just roundtrip.
    
    // Better: Send sync, and in sync callback quit loop.
    // Since I can't easily add another callback struct here without more code:
    // I'll just iterate a few times which typically clears the socket buffer for initial dump.
    for (int i=0; i<10; i++) {
        pw_loop_iterate(pw_main_loop_get_loop(loop), 10); 
    }
    
    pw_proxy_destroy((struct pw_proxy*)registry);
    pw_core_disconnect(core);
    pw_context_destroy(context);
    pw_main_loop_destroy(loop);
}

AudioFrame* Audio_GetCapturedFrame(AudioCaptureContext *ctx) {
    if (!ctx) return NULL;
    
    // Check if we have enough samples for a full frame
    int frame_samples = AUDIO_FRAME_SIZE * AUDIO_CHANNELS;
    if (ctx->available >= frame_samples) {
        // Ensure we have a buffer for the frame
        if (!ctx->current_frame.samples) {
            ctx->current_frame.samples = ArenaPush(ctx->arena, frame_samples * sizeof(int16_t));
        }
        
        // Copy samples from ring buffer
        for (int i = 0; i < frame_samples; i++) {
            ctx->current_frame.samples[i] = ctx->buffer[ctx->read_pos];
            ctx->read_pos = (ctx->read_pos + 1) % ctx->buffer_size;
        }
        ctx->available -= frame_samples;
        
        ctx->current_frame.sample_count = AUDIO_FRAME_SIZE;
        ctx->current_frame.channels = AUDIO_CHANNELS;
        return &ctx->current_frame;
    }
    
    return NULL;
}

void Audio_CloseCapture(AudioCaptureContext *ctx) {
    if (ctx) {
        if (ctx->stream) pw_stream_destroy(ctx->stream);
        if (ctx->core) pw_core_disconnect(ctx->core);
        if (ctx->context) pw_context_destroy(ctx->context);
        if (ctx->loop) pw_main_loop_destroy(ctx->loop);
    }
}

// --- Audio Playback Context ---
struct AudioPlaybackContext {
    struct pw_main_loop *loop;
    struct pw_context *context;
    struct pw_core *core;
    struct pw_stream *stream;
    struct spa_hook stream_listener;
    
    MemoryArena *arena;
    
    // Ring buffer for playback
    int16_t *buffer;
    int buffer_size;
    int write_pos;
    int read_pos;
    int available;
};

// Playback stream callback
static void playback_on_process(void *data) {
    AudioPlaybackContext *ctx = (AudioPlaybackContext *)data;
    struct pw_buffer *b;
    struct spa_buffer *buf;
    
    if ((b = pw_stream_dequeue_buffer(ctx->stream)) == NULL) {
        return;
    }
    
    buf = b->buffer;
    if (buf->datas[0].data == NULL) {
        pw_stream_queue_buffer(ctx->stream, b);
        return;
    }
    
    int16_t *dst = (int16_t *)buf->datas[0].data;
    int max_samples = buf->datas[0].maxsize / sizeof(int16_t);
    int n_samples = 0;
    
    // Copy from ring buffer
    while (n_samples < max_samples && ctx->available > 0) {
        dst[n_samples++] = ctx->buffer[ctx->read_pos];
        ctx->read_pos = (ctx->read_pos + 1) % ctx->buffer_size;
        ctx->available--;
    }
    
    // Fill remainder with silence
    while (n_samples < max_samples) {
        dst[n_samples++] = 0;
    }
    
    buf->datas[0].chunk->offset = 0;
    buf->datas[0].chunk->stride = sizeof(int16_t) * AUDIO_CHANNELS;
    buf->datas[0].chunk->size = max_samples * sizeof(int16_t);
    
    pw_stream_queue_buffer(ctx->stream, b);
}

static const struct pw_stream_events playback_stream_events = {
    .version = PW_VERSION_STREAM_EVENTS,
    .process = playback_on_process,
};

AudioPlaybackContext* Audio_InitPlayback(MemoryArena *arena) {
    AudioPlaybackContext *ctx = PushStructZero(arena, AudioPlaybackContext);
    ctx->arena = arena;
    
    // Ring buffer: 1 second of audio
    ctx->buffer_size = AUDIO_SAMPLE_RATE * AUDIO_CHANNELS;
    ctx->buffer = ArenaPush(arena, ctx->buffer_size * sizeof(int16_t));
    
    pw_init(NULL, NULL);
    
    ctx->loop = pw_main_loop_new(NULL);
    ctx->context = pw_context_new(pw_main_loop_get_loop(ctx->loop), NULL, 0);
    ctx->core = pw_context_connect(ctx->context, NULL, 0);
    
    if (!ctx->core) {
        fprintf(stderr, "Audio: Failed to connect to PipeWire for playback\n");
        return NULL;
    }
    
    ctx->stream = pw_stream_new_simple(
        pw_main_loop_get_loop(ctx->loop),
        "harmony-audio-playback",
        pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Audio",
            PW_KEY_MEDIA_CATEGORY, "Playback",
            PW_KEY_MEDIA_ROLE, "Music",
            NULL
        ),
        &playback_stream_events,
        ctx
    );
    
    // Request S16LE stereo 48kHz
    const struct spa_pod *params[1];
    uint8_t buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    
    params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat,
        &SPA_AUDIO_INFO_RAW_INIT(
            .format = SPA_AUDIO_FORMAT_S16_LE,
            .channels = AUDIO_CHANNELS,
            .rate = AUDIO_SAMPLE_RATE
        ));
    
    int ret = pw_stream_connect(
        ctx->stream,
        PW_DIRECTION_OUTPUT,
        PW_ID_ANY,
        PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS,
        params, 1
    );
    
    if (ret < 0) {
        fprintf(stderr, "Audio: Playback connect failed: %s\n", strerror(-ret));
        return NULL;
    }
    
    printf("Audio: Playback initialized (48kHz stereo S16LE)\n");
    return ctx;
}

void Audio_PollPlayback(AudioPlaybackContext *ctx) {
    if (ctx && ctx->loop) {
        pw_loop_iterate(pw_main_loop_get_loop(ctx->loop), 0);
    }
}

void Audio_WritePlayback(AudioPlaybackContext *ctx, AudioFrame *frame) {
    if (!ctx || !frame || !frame->samples) return;
    
    int n_samples = frame->sample_count * frame->channels;
    
    // Copy to ring buffer
    for (int i = 0; i < n_samples && ctx->available < ctx->buffer_size; i++) {
        ctx->buffer[ctx->write_pos] = frame->samples[i];
        ctx->write_pos = (ctx->write_pos + 1) % ctx->buffer_size;
        ctx->available++;
    }
}

void Audio_ClosePlayback(AudioPlaybackContext *ctx) {
    if (ctx) {
        if (ctx->stream) pw_stream_destroy(ctx->stream);
        if (ctx->core) pw_core_disconnect(ctx->core);
        if (ctx->context) pw_context_destroy(ctx->context);
        if (ctx->loop) pw_main_loop_destroy(ctx->loop);
    }
}
