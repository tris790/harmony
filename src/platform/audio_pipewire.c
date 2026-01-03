// Audio Capture and Playback via PipeWire
// Captures system audio output (monitor) for streaming
// Plays back received audio on the viewer side

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>
#include <pthread.h>
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
    struct pw_core *core;
    struct pw_registry *registry;
    int sync_seq;
    bool done;
    
    // Temp storage for proxies to cleanup
    struct {
        struct pw_node *proxy;
        struct spa_hook listener;
    } proxies[128];
    int proxy_count;
} EnumContext;

static void node_event_info(void *data, const struct pw_node_info *info) {
    AudioNodeInfo *node = (AudioNodeInfo *)data;
    if (!info || !info->props) return;

    const char *media_class = spa_dict_lookup(info->props, PW_KEY_MEDIA_CLASS);
    const char *node_name = spa_dict_lookup(info->props, PW_KEY_NODE_NAME);
    const char *app_name = spa_dict_lookup(info->props, PW_KEY_APP_NAME);
    const char *node_desc = spa_dict_lookup(info->props, PW_KEY_NODE_DESCRIPTION);
    const char *media_name = spa_dict_lookup(info->props, PW_KEY_MEDIA_NAME);
    const char *media_title = spa_dict_lookup(info->props, PW_KEY_MEDIA_TITLE);

    if (!media_class) return;

    bool is_app = (strcmp(media_class, "Stream/Output/Audio") == 0);
    bool is_sink = (strcmp(media_class, "Audio/Sink") == 0);

    if (is_app || is_sink) {
        const char *detail = media_title ? media_title : (media_name ? media_name : (node_desc ? node_desc : ""));
        
        if (app_name && detail && strcasecmp(app_name, detail) == 0) {
            detail = (node_desc && strcasecmp(node_desc, app_name) != 0) ? node_desc : "";
        }

        const char *main_label = app_name ? app_name : (node_desc ? node_desc : (node_name ? node_name : "Unknown Node"));
        
        if (is_app) {
            if (detail && detail[0] != '\0' && strcasecmp(main_label, detail) != 0) {
                snprintf(node->name, sizeof(node->name), "%s (%s)", main_label, detail);
            } else {
                snprintf(node->name, sizeof(node->name), "%s", main_label);
            }
        } else {
            snprintf(node->name, sizeof(node->name), "%s", main_label);
        }
    }
}

static const struct pw_node_events node_events = {
    .version = PW_VERSION_NODE_EVENTS,
    .info = node_event_info,
};

static void registry_event_global(void *data, uint32_t id,
                                   uint32_t permissions, const char *type, uint32_t version,
                                   const struct spa_dict *props) {
    EnumContext *ctx = (EnumContext *)data;
    (void)permissions;
    (void)version;
    
    if (strcmp(type, PW_TYPE_INTERFACE_Node) != 0) return;
    if (!props) return;

    const char *media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
    if (!media_class) return;
    
    bool is_app = (strcmp(media_class, "Stream/Output/Audio") == 0);
    bool is_sink = (strcmp(media_class, "Audio/Sink") == 0);
    
    if (is_app || is_sink) {
        if (ctx->list->count >= ctx->list->capacity) return; // Safety check
        
        AudioNodeInfo *node = &ctx->list->nodes[ctx->list->count++];
        memset(node, 0, sizeof(AudioNodeInfo));
        
        const char *serial_str = spa_dict_lookup(props, PW_KEY_OBJECT_SERIAL);
        node->id = serial_str ? (uint32_t)atoi(serial_str) : id;
        
        // Set a basic fallback name immediately (will be improved by node_event_info)
        const char *app_name = spa_dict_lookup(props, PW_KEY_APP_NAME);
        const char *node_name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
        const char *node_desc = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
        const char *main_label = app_name ? app_name : (node_desc ? node_desc : (node_name ? node_name : "Unknown Node"));
        
        if (is_app) snprintf(node->name, sizeof(node->name), "%s", main_label);
        else snprintf(node->name, sizeof(node->name), "%s", main_label);

        if (ctx->proxy_count < 128) {
            struct pw_node *proxy = pw_registry_bind(ctx->registry, id, type, PW_VERSION_NODE, 0);
            if (proxy) {
                ctx->proxies[ctx->proxy_count].proxy = proxy;
                pw_node_add_listener(proxy, &ctx->proxies[ctx->proxy_count].listener, &node_events, node);
                ctx->proxy_count++;
            }
        }
    }
}

static const struct pw_registry_events registry_events = {
    .version = PW_VERSION_REGISTRY_EVENTS,
    .global = registry_event_global,
};

static void core_event_done(void *data, uint32_t id, int seq) {
    EnumContext *ctx = (EnumContext *)data;
    if (id == PW_ID_CORE && seq == ctx->sync_seq) {
        ctx->done = true;
    }
}

static const struct pw_core_events core_events = {
    .version = PW_VERSION_CORE_EVENTS,
    .done = core_event_done,
};

void Audio_EnumerateNodes(MemoryArena *arena, AudioNodeList *out_list) {
    if (!out_list) return;
    
    // Pre-allocate a fixed large array to avoid pointer invalidation when binding listeners
    const int MAX_NODES = 256;
    out_list->count = 0;
    out_list->capacity = MAX_NODES;
    out_list->nodes = ArenaPush(arena, MAX_NODES * sizeof(AudioNodeInfo));
    memset(out_list->nodes, 0, MAX_NODES * sizeof(AudioNodeInfo));
    
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
    
    struct spa_hook core_listener;
    struct spa_hook registry_listener;
    struct pw_registry *registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
    
    EnumContext ctx = { 
        .list = out_list, 
        .arena = arena, 
        .core = core, 
        .registry = registry,
        .proxy_count = 0,
        .done = false 
    };
    
    pw_core_add_listener(core, &core_listener, &core_events, &ctx);
    pw_registry_add_listener(registry, &registry_listener, &registry_events, &ctx);
    
    // Sync twice: first to get all registry globals, second to ensure all bound node info events are processed
    ctx.sync_seq = pw_core_sync(core, PW_ID_CORE, 0);
    while (!ctx.done) {
        if (pw_loop_iterate(pw_main_loop_get_loop(loop), -1) < 0) break;
    }
    
    // Second sync barrier for node info metadata
    ctx.done = false;
    ctx.sync_seq = pw_core_sync(core, PW_ID_CORE, 0);
    while (!ctx.done) {
        if (pw_loop_iterate(pw_main_loop_get_loop(loop), -1) < 0) break;
    }
    
    // Post-process to handle duplicates and ensure every entry has a name
    for (int i = 0; i < out_list->count; i++) {
        AudioNodeInfo *node = &out_list->nodes[i];
        
        // Final fallback if name is still empty (shouldn't happen with our logic)
        if (node->name[0] == '\0') {
            snprintf(node->name, sizeof(node->name), "[Node #%u]", node->id);
        }

        for (int j = 0; j < i; j++) {
            if (strcmp(node->name, out_list->nodes[j].name) == 0) {
                char temp[256];
                snprintf(temp, sizeof(temp), "%s #%u", node->name, node->id);
                strncpy(node->name, temp, sizeof(node->name) - 1);
                node->name[sizeof(node->name) - 1] = '\0';
                break;
            }
        }
    }

    // Cleanup
    for (int i = 0; i < ctx.proxy_count; i++) {
        pw_proxy_destroy((struct pw_proxy*)ctx.proxies[i].proxy);
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
    struct pw_thread_loop *thread_loop; // Dedicated audio thread
    struct pw_context *context;
    struct pw_core *core;
    struct pw_stream *stream;
    struct spa_hook stream_listener;
    
    MemoryArena *arena;
    
    // Thread safety
    pthread_mutex_t mutex;
    
    // Ring buffer for playback
    int16_t *buffer;
    int buffer_size;
    int write_pos;
    int read_pos;
    int available;
    
    // Jitter Buffer State
    bool buffering;
    int target_latency; // Samples to buffer before starting
};

// Playback stream callback (Runs on dedicated thread)
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
    int n_written = 0;
    
    pthread_mutex_lock(&ctx->mutex);
    
    // Jitter Buffer Logic
    // If we are in buffering state, we output silence until we have enough data
    if (ctx->buffering) {
        if (ctx->available >= ctx->target_latency) {
            ctx->buffering = false;
        //    printf("Audio: Buffering complete. Starting playback (Available: %d)\n", ctx->available);
        }
    } else {
        // If we run dry, go back to buffering state
        if (ctx->available == 0) {
            ctx->buffering = true;
        //    printf("Audio: Underrun! Re-buffering...\n");
        }
    }

    if (!ctx->buffering) {
        // Copy from ring buffer
        while (n_written < max_samples && ctx->available > 0) {
            dst[n_written++] = ctx->buffer[ctx->read_pos];
            ctx->read_pos = (ctx->read_pos + 1) % ctx->buffer_size;
            ctx->available--;
        }
    }
    
    pthread_mutex_unlock(&ctx->mutex);
    
    // Fill remainder with silence
    while (n_written < max_samples) {
        dst[n_written++] = 0;
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
    
    pthread_mutex_init(&ctx->mutex, NULL);
    
    // Ring buffer: 1 second of audio
    ctx->buffer_size = AUDIO_SAMPLE_RATE * AUDIO_CHANNELS;
    ctx->buffer = ArenaPush(arena, ctx->buffer_size * sizeof(int16_t));
    
    // Jitter Buffer Settings
    // 100ms buffering @ 48kHz = 4800 samples per channel * 2 channels = 9600 samples
    ctx->target_latency = (AUDIO_SAMPLE_RATE / 10) * AUDIO_CHANNELS; 
    ctx->buffering = true;
    
    pw_init(NULL, NULL);
    
    // Use Thread Loop instead of Main Loop for playback
    // This decouples audio from the main application frame rate/vsync
    ctx->thread_loop = pw_thread_loop_new("Audio Playback", NULL);
    ctx->context = pw_context_new(pw_thread_loop_get_loop(ctx->thread_loop), NULL, 0);
    ctx->core = pw_context_connect(ctx->context, NULL, 0);
    
    if (!ctx->core) {
        fprintf(stderr, "Audio: Failed to connect to PipeWire for playback\n");
        // Cleanup?
        return NULL;
    }
    
    ctx->stream = pw_stream_new_simple(
        pw_thread_loop_get_loop(ctx->thread_loop),
        "harmony-audio-playback",
        pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Audio",
            PW_KEY_MEDIA_CATEGORY, "Playback",
            PW_KEY_MEDIA_ROLE, "Music",
            // Low latency properties
            PW_KEY_NODE_LATENCY, "480/48000", // 10ms
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
        PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS,
        params, 1
    );
    
    if (ret < 0) {
        fprintf(stderr, "Audio: Playback connect failed: %s\n", strerror(-ret));
        return NULL;
    }
    
    // Start the thread loop
    pw_thread_loop_start(ctx->thread_loop);
    
    printf("Audio: Playback initialized (Threaded + JitterBuffer 100ms)\n");
    return ctx;
}

void Audio_PollPlayback(AudioPlaybackContext *ctx) {
    // No-op for threaded implementation
    // The thread loop handles processing internally
    (void)ctx;
}

void Audio_WritePlayback(AudioPlaybackContext *ctx, AudioFrame *frame) {
    if (!ctx || !frame || !frame->samples) return;
    
    int n_samples = frame->sample_count * frame->channels;
    
    pthread_mutex_lock(&ctx->mutex);
    
    // Copy to ring buffer
    for (int i = 0; i < n_samples && ctx->available < ctx->buffer_size; i++) {
        ctx->buffer[ctx->write_pos] = frame->samples[i];
        ctx->write_pos = (ctx->write_pos + 1) % ctx->buffer_size;
        ctx->available++;
    }
    
    pthread_mutex_unlock(&ctx->mutex);
}

void Audio_ClosePlayback(AudioPlaybackContext *ctx) {
    if (ctx) {
        if (ctx->thread_loop) pw_thread_loop_stop(ctx->thread_loop);
        
        if (ctx->stream) pw_stream_destroy(ctx->stream);
        if (ctx->core) pw_core_disconnect(ctx->core);
        if (ctx->context) pw_context_destroy(ctx->context);
        
        if (ctx->thread_loop) pw_thread_loop_destroy(ctx->thread_loop);
        
        pthread_mutex_destroy(&ctx->mutex);
    }
}
