#include "codec_api.h"
#include <libavcodec/avcodec.h>

struct DecoderContext {
    AVCodecContext *codec_ctx;
    AVFrame *frame_yuv;
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

void Codec_DecodePacket(DecoderContext *ctx, EncodedPacket *packet, VideoFrame *out_frame) {
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
