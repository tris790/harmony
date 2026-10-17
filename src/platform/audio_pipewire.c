// Audio Capture and Playback via PipeWire
// Captures system audio output (monitor) for streaming
// Plays back received audio on the viewer side

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../audio_api.h"

#define MAX_CAPTURE_CANDIDATES 32

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

    // Target tracking: app streams get a new serial every time the app
    // restarts, so we follow the target by name and re-link to new instances.
    struct pw_registry *registry;
    struct spa_hook registry_listener;
    struct spa_hook core_listener;
    bool follow_target;
    bool target_is_app;
    char match_name[128];
    uint32_t target_serial;     // Serial the stream is currently targeting
    uint32_t target_global_id;  // Registry id of the target, PW_ID_ANY if gone
    int registry_sync_seq;
    bool registry_synced;
    struct {
        uint32_t id;
        uint32_t serial;
    } candidates[MAX_CAPTURE_CANDIDATES];
    int candidate_count;
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

// Name used to recognize the same source across restarts.
// application.name is stable for apps (process.binary is useless for Wine
// games, it's always wine64-preloader); sinks are matched by node.name.
static const char *audio_match_name(const struct spa_dict *props, bool is_app) {
    const char *name = NULL;
    if (is_app) name = spa_dict_lookup(props, PW_KEY_APP_NAME);
    if (!name) name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
    return name;
}

// (Re)create the capture stream linked to the node with the given serial.
// Serial 0 captures the default sink monitor.
static bool capture_create_stream(AudioCaptureContext *ctx, uint32_t serial) {
    if (ctx->stream) {
        pw_stream_destroy(ctx->stream);
        ctx->stream = NULL;
    }

    struct pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "Screen", // Changed from Music to Screen to avoid default Mic policies
        NULL
    );

    if (serial != 0) {
        // Target specific node (Portal/App)
        pw_properties_setf(props, PW_KEY_TARGET_OBJECT, "%u", serial);

        // Never let WirePlumber fall back to the default sink monitor when
        // the target is missing or goes away (that would leak all desktop
        // audio). linger keeps the stream alive and idle instead of having
        // it destroyed, we re-target it ourselves when the app comes back.
        pw_properties_set(props, "node.dont-fallback", "true");
        pw_properties_set(props, "node.linger", "true");
        if (!ctx->target_is_app) {
            pw_properties_set(props, PW_KEY_STREAM_CAPTURE_SINK, "true");
        }

        printf("Audio: Targeting Node serial %u (%s)\n", serial, ctx->match_name);
    } else {
        // Capture System Monitor (Default Sink Monitor)
        pw_properties_set(props, PW_KEY_STREAM_CAPTURE_SINK, "true");
        printf("Audio: Targeting System Monitor\n");
    }

    ctx->stream = pw_stream_new_simple(
        pw_main_loop_get_loop(ctx->loop),
        "harmony-audio-capture",
        props,
        &capture_stream_events,
        ctx
    );
    if (!ctx->stream) {
        fprintf(stderr, "Audio: Failed to create capture stream\n");
        return false;
    }

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
        return false;
    }

    ctx->target_serial = serial;
    return true;
}

static void capture_registry_global(void *data, uint32_t id,
                                    uint32_t permissions, const char *type, uint32_t version,
                                    const struct spa_dict *props) {
    AudioCaptureContext *ctx = (AudioCaptureContext *)data;
    (void)permissions;
    (void)version;

    if (strcmp(type, PW_TYPE_INTERFACE_Node) != 0 || !props) return;

    const char *media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
    if (!media_class) return;
    const char *wanted_class = ctx->target_is_app ? "Stream/Output/Audio" : "Audio/Sink";
    if (strcmp(media_class, wanted_class) != 0) return;

    const char *name = audio_match_name(props, ctx->target_is_app);
    if (!name || strcmp(name, ctx->match_name) != 0) return;

    const char *serial_str = spa_dict_lookup(props, PW_KEY_OBJECT_SERIAL);
    uint32_t serial = serial_str ? (uint32_t)strtoul(serial_str, NULL, 10) : id;

    if (ctx->candidate_count < MAX_CAPTURE_CANDIDATES) {
        ctx->candidates[ctx->candidate_count].id = id;
        ctx->candidates[ctx->candidate_count].serial = serial;
        ctx->candidate_count++;
    }

    if (serial == ctx->target_serial) {
        ctx->target_global_id = id;
    }
}

static void capture_registry_global_remove(void *data, uint32_t id) {
    AudioCaptureContext *ctx = (AudioCaptureContext *)data;

    for (int i = 0; i < ctx->candidate_count; i++) {
        if (ctx->candidates[i].id == id) {
            ctx->candidates[i] = ctx->candidates[--ctx->candidate_count];
            break;
        }
    }

    if (id == ctx->target_global_id) {
        ctx->target_global_id = PW_ID_ANY;
        printf("Audio: Source '%s' went away, waiting for it to come back\n", ctx->match_name);
    }
}

static const struct pw_registry_events capture_registry_events = {
    .version = PW_VERSION_REGISTRY_EVENTS,
    .global = capture_registry_global,
    .global_remove = capture_registry_global_remove,
};

static void capture_core_done(void *data, uint32_t id, int seq) {
    AudioCaptureContext *ctx = (AudioCaptureContext *)data;
    if (id == PW_ID_CORE && seq == ctx->registry_sync_seq) {
        ctx->registry_synced = true;
    }
}

static const struct pw_core_events capture_core_events = {
    .version = PW_VERSION_CORE_EVENTS,
    .done = capture_core_done,
};

AudioCaptureContext* Audio_InitCapture(MemoryArena *arena, const AudioNodeInfo *target) {
    uint32_t target_node_id = target ? target->id : 0;
    if (target_node_id == 0xFFFFFFFF) {
        printf("Audio: No Audio selected. Skipping capture initialization.\n");
        return NULL;
    }
    AudioCaptureContext *ctx = PushStructZero(arena, AudioCaptureContext);
    ctx->arena = arena;
    ctx->target_global_id = PW_ID_ANY;
    
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

    if (target_node_id != 0) {
        ctx->target_is_app = target->is_app;
        snprintf(ctx->match_name, sizeof(ctx->match_name), "%s", target->match_name);
        ctx->follow_target = ctx->match_name[0] != '\0';
    }

    if (!capture_create_stream(ctx, target_node_id)) {
        return NULL;
    }

    if (ctx->follow_target) {
        ctx->registry = pw_core_get_registry(ctx->core, PW_VERSION_REGISTRY, 0);
        pw_registry_add_listener(ctx->registry, &ctx->registry_listener,
                                 &capture_registry_events, ctx);
        pw_core_add_listener(ctx->core, &ctx->core_listener, &capture_core_events, ctx);
        ctx->registry_sync_seq = pw_core_sync(ctx->core, PW_ID_CORE, 0);
    }
    
    printf("Audio: Capture initialized (48kHz stereo S16LE)\n");
    return ctx;
}

void Audio_PollCapture(AudioCaptureContext *ctx) {
    if (ctx && ctx->loop) {
        // Polled from the audio worker thread: enter/leave so the loop lock is
        // released afterwards, otherwise pw_stream_destroy() on the main thread
        // deadlocks once the worker has exited while still owning it.
        struct pw_loop *loop = pw_main_loop_get_loop(ctx->loop);
        pw_loop_enter(loop);
        pw_loop_iterate(loop, 0);

        // Target is gone (app closed or restarted): switch to another
        // instance of the same app as soon as one shows up. Wait for the
        // initial registry dump so we don't jump away from a target that
        // simply hasn't been announced yet.
        if (ctx->follow_target && ctx->registry_synced &&
            ctx->target_global_id == PW_ID_ANY && ctx->candidate_count > 0) {
            printf("Audio: Source '%s' is back, re-linking\n", ctx->match_name);
            ctx->target_global_id = ctx->candidates[0].id;
            capture_create_stream(ctx, ctx->candidates[0].serial);
        }
        pw_loop_leave(loop);
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
        node->is_app = is_app;
        const char *match_name = audio_match_name(props, is_app);
        if (match_name) snprintf(node->match_name, sizeof(node->match_name), "%s", match_name);
        
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
        if (ctx->registry) pw_proxy_destroy((struct pw_proxy*)ctx->registry);
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
        // Stop joins the loop thread. Leave the mutex unlocked so an in-flight
        // playback callback can acquire it and finish before the join returns.
        if (ctx->thread_loop) pw_thread_loop_stop(ctx->thread_loop);
        
        if (ctx->stream) pw_stream_destroy(ctx->stream);
        if (ctx->core) pw_core_disconnect(ctx->core);
        if (ctx->context) pw_context_destroy(ctx->context);
        
        if (ctx->thread_loop) pw_thread_loop_destroy(ctx->thread_loop);
        
        pthread_mutex_destroy(&ctx->mutex);
    }
}
