#ifndef HARMONY_AUDIO_API_H
#define HARMONY_AUDIO_API_H

#include "memory_arena.h"
#include <stdint.h>
#include <stdbool.h>

// Audio format constants
#define AUDIO_SAMPLE_RATE 48000
#define AUDIO_CHANNELS 2
#define AUDIO_FRAME_SIZE 960  // 20ms at 48kHz (Opus standard frame size)

// Raw PCM audio frame (before encoding)
typedef struct AudioFrame {
    int16_t *samples;       // Interleaved S16LE samples
    int sample_count;       // Number of samples per channel
    int channels;
} AudioFrame;

// Encoded audio packet
typedef struct EncodedAudio {
    uint8_t *data;
    size_t size;
} EncodedAudio;

// --- Audio Capture (Host) ---
typedef struct AudioCaptureContext AudioCaptureContext;

AudioCaptureContext* Audio_InitCapture(MemoryArena *arena);
void Audio_PollCapture(AudioCaptureContext *ctx);
AudioFrame* Audio_GetCapturedFrame(AudioCaptureContext *ctx);
void Audio_CloseCapture(AudioCaptureContext *ctx);

// --- Audio Encoder (Opus) ---
typedef struct AudioEncoder AudioEncoder;

AudioEncoder* Audio_InitEncoder(MemoryArena *arena);
void Audio_Encode(AudioEncoder *enc, AudioFrame *frame, EncodedAudio *out);
void Audio_CloseEncoder(AudioEncoder *enc);

// --- Audio Decoder (Opus) ---
typedef struct AudioDecoder AudioDecoder;

AudioDecoder* Audio_InitDecoder(MemoryArena *arena);
void Audio_Decode(AudioDecoder *dec, void *data, size_t size, AudioFrame *out);
void Audio_CloseDecoder(AudioDecoder *dec);

// --- Audio Playback (Viewer) ---
typedef struct AudioPlaybackContext AudioPlaybackContext;

AudioPlaybackContext* Audio_InitPlayback(MemoryArena *arena);
void Audio_PollPlayback(AudioPlaybackContext *ctx);
void Audio_WritePlayback(AudioPlaybackContext *ctx, AudioFrame *frame);
void Audio_ClosePlayback(AudioPlaybackContext *ctx);

#endif // HARMONY_AUDIO_API_H
