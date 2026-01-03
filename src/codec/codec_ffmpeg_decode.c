#include "codec_api.h"
#include "os_api.h"
#include <libavcodec/avcodec.h>

struct DecoderContext {
    AVCodecContext *codec_ctx;
    AVFrame *frame_yuv;
    bool has_received_keyframe;  // Track if we've seen a keyframe with SPS/PPS
};

DecoderContext* Codec_InitDecoder(MemoryArena *arena) {
    DecoderContext *ctx = PushStructZero(arena, DecoderContext);

    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        fprintf(stderr, "Codec_InitDecoder: H.264 codec not found\n");
        return NULL;
    }

    ctx->codec_ctx = avcodec_alloc_context3(codec);
    if (!ctx->codec_ctx) {
        return NULL;
    }

    if (avcodec_open2(ctx->codec_ctx, codec, NULL) < 0) {
        fprintf(stderr, "Codec_InitDecoder: Could not open codec\n");
        return NULL;
    }

    ctx->frame_yuv = av_frame_alloc();

    return ctx;
}

// Helper function to check if H.264 packet contains a keyframe (SPS/PPS/IDR)
// This scans for NAL unit types in the bitstream
static bool Codec_IsKeyframe(uint8_t *data, size_t size) {
    if (size < 4) return false;
    
    // Look for NAL start codes (0x00 0x00 0x01 or 0x00 0x00 0x00 0x01)
    // and check NAL unit types
    // If we are looking for a keyframe, let's print what NALs we find to debug
    bool found_any_nal = false;
    
    for (size_t i = 0; i + 4 < size; i++) {
        bool start_code_3 = (data[i] == 0 && data[i+1] == 0 && data[i+2] == 1);
        bool start_code_4 = (i + 5 < size && data[i] == 0 && data[i+1] == 0 && data[i+2] == 0 && data[i+3] == 1);
        
        if (start_code_3 || start_code_4) {
            found_any_nal = true;
            size_t nal_offset = start_code_4 ? i + 4 : i + 3;
            if (nal_offset < size) {
                uint8_t nal_type = data[nal_offset] & 0x1F;
                
                // Debug print for non-keyframe packets if we haven't seen a keyframe yet
                // (This helps identify if we are receiving P-frames but missed I-frame)
                // printf("Decoder: Found NAL Type %d\n", nal_type);
                
                // NAL types: 5=IDR, 7=SPS, 8=PPS
                if (nal_type == 5 || nal_type == 7 || nal_type == 8) {
                    return true;
                }
            }
        }
    }
    
    if (!found_any_nal && size > 16) {
        static double last_nal_log = 0;
        double now = OS_GetTime();
        if (now - last_nal_log >= 5.0) {
            printf("Decoder: No NAL start codes found in packet of size %zu! (Header: %02x %02x %02x %02x)\n", 
                   size, data[0], data[1], data[2], data[3]);
            last_nal_log = now;
        }
    }
    
    return false;
}

void Codec_DecodePacket(DecoderContext *ctx, EncodedPacket *packet, VideoFrame *out_frame) {
    // Check if this packet contains a keyframe (SPS/PPS/IDR)
    bool is_keyframe = Codec_IsKeyframe(packet->data, packet->size);
    
    if (is_keyframe) {
        if (!ctx->has_received_keyframe) {
            printf("Decoder: First keyframe received! Enabling decoding.\n");
        }
        ctx->has_received_keyframe = true;
    }
    
// Don't decode until we've received a keyframe - prevents "non-existing PPS" errors
    if (!ctx->has_received_keyframe) {
        static double last_log_time = 0;
        double current_time = OS_GetTime();
        if (current_time - last_log_time >= 5.0) {
            printf("Decoder: Skipping packet - waiting for keyframe (has_kf=%d, is_kf=%d)\n", 
                   ctx->has_received_keyframe, is_keyframe);
            last_log_time = current_time;
        }
        return;
    }
    
    AVPacket *av_pkt = av_packet_alloc();
    // Wrap our data in an AVPacket
    // Warning: av_packet_from_data might take ownership? cleaner to just manually set
    // Note: Since 'packet->data' is in an arena, we don't want FFmpeg to free it.
    // We just reference it.
    av_pkt->data = packet->data;
    av_pkt->size = packet->size;
    av_pkt->pts = packet->pts;
    av_pkt->dts = packet->dts;

    int ret = avcodec_send_packet(ctx->codec_ctx, av_pkt);
    if (ret < 0) {
        fprintf(stderr, "Codec_DecodePacket: Error sending packet for decoding\n");
        av_packet_free(&av_pkt);
        return;
    }

    ret = avcodec_receive_frame(ctx->codec_ctx, ctx->frame_yuv);
    if (ret == 0) {
        // Success
        // For the output, we just point to the internal FFmpeg frame data for now
        // If we want to render it, we might need to convert YUV->RGB or upload YUV textures.
        // For Verification: we leave it as YUV420P
        out_frame->width = ctx->frame_yuv->width;
        out_frame->height = ctx->frame_yuv->height;
        for (int i = 0; i < 3; ++i) { // Y, U, V
            out_frame->data[i] = ctx->frame_yuv->data[i];
            out_frame->linesize[i] = ctx->frame_yuv->linesize[i];
        }
    } else if (ret != AVERROR(EAGAIN)) {
        fprintf(stderr, "Codec_DecodePacket: Error during decoding\n");
    }

    // Do not free packet data since it belongs to us/arena, but free the wrapper structure
    // Since we didn't use any referencing buffering functions, av_packet_free handles the struct.
    // Ensure we reset pointers so it doesn't try to free our buffer if it thinks it owns it.
    av_pkt->data = NULL;
    av_packet_free(&av_pkt);
}

