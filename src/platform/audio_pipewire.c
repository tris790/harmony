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

// Capture stream callback
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
    
    // Copy to ring buffer
    for (int i = 0; i < n_samples && ctx->available < ctx->buffer_size; i++) {
        ctx->buffer[ctx->write_pos] = samples[i];
        ctx->write_pos = (ctx->write_pos + 1) % ctx->buffer_size;
        ctx->available++;
    }
    
    // Check if we have a full frame (AUDIO_FRAME_SIZE * AUDIO_CHANNELS samples)
    int frame_samples = AUDIO_FRAME_SIZE * AUDIO_CHANNELS;
    if (ctx->available >= frame_samples) {
        // Copy to current_frame
        if (!ctx->current_frame.samples) {
            ctx->current_frame.samples = ArenaPush(ctx->arena, frame_samples * sizeof(int16_t));
        }
        
        for (int i = 0; i < frame_samples; i++) {
            ctx->current_frame.samples[i] = ctx->buffer[ctx->read_pos];
            ctx->read_pos = (ctx->read_pos + 1) % ctx->buffer_size;
        }
        ctx->available -= frame_samples;
        
        ctx->current_frame.sample_count = AUDIO_FRAME_SIZE;
        ctx->current_frame.channels = AUDIO_CHANNELS;
        ctx->frame_ready = true;
    }
    
    pw_stream_queue_buffer(ctx->stream, b);
}

static const struct pw_stream_events capture_stream_events = {
    .version = PW_VERSION_STREAM_EVENTS,
    .process = capture_on_process,
};

AudioCaptureContext* Audio_InitCapture(MemoryArena *arena) {
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
    
    // Create capture stream targeting the monitor source (system audio output)
    ctx->stream = pw_stream_new_simple(
        pw_main_loop_get_loop(ctx->loop),
        "harmony-audio-capture",
        pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Audio",
            PW_KEY_MEDIA_CATEGORY, "Capture",
            PW_KEY_MEDIA_ROLE, "Music",
            PW_KEY_STREAM_CAPTURE_SINK, "true",  // Capture from sink (monitor)
            NULL
        ),
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

AudioFrame* Audio_GetCapturedFrame(AudioCaptureContext *ctx) {
    if (ctx && ctx->frame_ready) {
        ctx->frame_ready = false;
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
