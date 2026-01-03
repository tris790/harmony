#ifndef HARMONY_CODEC_API_H
#define HARMONY_CODEC_API_H

#include "memory_arena.h"
#include <stdint.h>

// Video Format
typedef struct VideoFormat {
    int width;
    int height;
    int fps;
    int bitrate;
} VideoFormat;

// Raw Video Frame (RGB/YUV)
typedef struct VideoFrame {
    uint8_t *data[4]; // Plane pointers
    int linesize[4];  // Plane strides
    int width;
    int height;
    // timestamp?
} VideoFrame;

// Encoded Packet
typedef struct EncodedPacket {
    uint8_t *data;
    size_t size;
    int64_t pts;
    int64_t dts;
    bool keyframe;
} EncodedPacket;

// Encoder
typedef struct EncoderContext EncoderContext;

EncoderContext* Codec_InitEncoder(MemoryArena *arena, VideoFormat format);
void Codec_EncodeFrame(EncoderContext *ctx, VideoFrame *frame, MemoryArena *packet_arena, EncodedPacket *out_packet);
void Codec_CloseEncoder(EncoderContext *ctx);

// Decoder
typedef struct DecoderContext DecoderContext;

DecoderContext* Codec_InitDecoder(MemoryArena *arena);
void Codec_DecodePacket(DecoderContext *ctx, EncodedPacket *packet, VideoFrame *out_frame);

#endif // HARMONY_CODEC_API_H
