#include "codec_api.h"
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

struct EncoderContext {
    AVCodecContext *codec_ctx;
    AVFrame *frame_yuv;
    struct SwsContext *sws_ctx;
    int pts_counter;
};

EncoderContext* Codec_InitEncoder(MemoryArena *arena, VideoFormat format) {
    // Note: We use the arena for our context, but FFmpeg manages its own memory internally.
    EncoderContext *ctx = PushStructZero(arena, EncoderContext);

    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!codec) {
        fprintf(stderr, "Codec_InitEncoder: H.264 codec not found\n");
        return NULL;
    }

    ctx->codec_ctx = avcodec_alloc_context3(codec);
    if (!ctx->codec_ctx) {
        fprintf(stderr, "Codec_InitEncoder: Could not allocate video codec context\n");
        return NULL;
    }

    // Set Codec Parameters
    ctx->codec_ctx->bit_rate = format.bitrate;
    ctx->codec_ctx->width = format.width;
    ctx->codec_ctx->height = format.height;
    ctx->codec_ctx->time_base = (AVRational){1, format.fps};
    ctx->codec_ctx->framerate = (AVRational){format.fps, 1};
    ctx->codec_ctx->gop_size = 10; // Frequent keyframes for low latency recovery
    ctx->codec_ctx->max_b_frames = 0; // No B-frames for low latency
    ctx->codec_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    
    // VBR Rate Control: Allow short bursts for high-motion scenes
    // rc_max_rate caps instantaneous bitrate to respect network limits
    // rc_buffer_size (0.5s buffer) allows encoder to "borrow" bits for complex frames
    ctx->codec_ctx->rc_max_rate = format.bitrate;
    ctx->codec_ctx->rc_buffer_size = format.bitrate / 2;

    // Optional: Tune for low latency
    av_opt_set(ctx->codec_ctx->priv_data, "preset", "ultrafast", 0);
    av_opt_set(ctx->codec_ctx->priv_data, "tune", "zerolatency", 0);
    
    // CRITICAL for network streaming: Insert SPS/PPS headers with every keyframe
    // This ensures the decoder can recover if it misses the initial keyframe
    // (common when viewer connects mid-stream or packets are lost over network)
    av_opt_set_int(ctx->codec_ctx->priv_data, "repeat_headers", 1, 0);

    if (avcodec_open2(ctx->codec_ctx, codec, NULL) < 0) {
        fprintf(stderr, "Codec_InitEncoder: Could not open codec\n");
        return NULL;
    }

    // Allocate Reusable YUV Frame
    ctx->frame_yuv = av_frame_alloc();
    ctx->frame_yuv->format = ctx->codec_ctx->pix_fmt;
    ctx->frame_yuv->width = ctx->codec_ctx->width;
    ctx->frame_yuv->height = ctx->codec_ctx->height;
    
    if (av_frame_get_buffer(ctx->frame_yuv, 32) < 0) {
        fprintf(stderr, "Codec_InitEncoder: Could not allocate frame data\n");
        return NULL;
    }

    // Initialize SWS Context for RGB -> YUV conversion
    // Assuming input is RGB24 or RGBA. We'll decide standard input format: likely RGBA from OpenGL/Wayland
    // UPDATE: Capture provides BGRx (BGRA), so we must tell SWS to expect BGRA.
    ctx->sws_ctx = sws_getContext(
        format.width, format.height, AV_PIX_FMT_BGRA, // Changed from RGBA
        format.width, format.height, AV_PIX_FMT_YUV420P,
        SWS_BILINEAR, NULL, NULL, NULL
    );

    return ctx;
}

void Codec_EncodeFrame(EncoderContext *ctx, VideoFrame *frame, MemoryArena *packet_arena, EncodedPacket *out_packet) {
    // 1. Convert Input Frame (RGBA) to YUV
    // Note: frame->data and frame->linesize match the signature expected by sws_scale
    sws_scale(ctx->sws_ctx, 
              (const uint8_t * const *)frame->data, frame->linesize, 
              0, ctx->codec_ctx->height,
              ctx->frame_yuv->data, ctx->frame_yuv->linesize);

    ctx->frame_yuv->pts = ctx->pts_counter++;

    // 2. Send Frame to Encoder
    int ret = avcodec_send_frame(ctx->codec_ctx, ctx->frame_yuv);
    if (ret < 0) {
        fprintf(stderr, "Codec_EncodeFrame: Error sending frame for encoding\n");
        return;
    }

    // 3. Receive Packets
    // In low latency mode, we expect 1 packet out per frame usually.
    // However, the API allows multiple packets. For simplicity in this loop, we just grab one.
    // TODO: Handle multiple packets if needed (rare with 0 B-frames and consistent size)
    AVPacket *av_pkt = av_packet_alloc();
    ret = avcodec_receive_packet(ctx->codec_ctx, av_pkt);
    if (ret == 0) {
        // Successful encode
        // Copy to Arena to own the memory (Handmade: we manage lifetime)
        out_packet->size = av_pkt->size;
        out_packet->data = ArenaPush(packet_arena, av_pkt->size);
        memcpy(out_packet->data, av_pkt->data, av_pkt->size);

        out_packet->pts = av_pkt->pts;
        out_packet->dts = av_pkt->dts;
        out_packet->keyframe = (av_pkt->flags & AV_PKT_FLAG_KEY);
        
        av_packet_unref(av_pkt);
    } else if (ret == AVERROR(EAGAIN)) {
        // Need more input
        out_packet->size = 0;
    } else {
        fprintf(stderr, "Codec_EncodeFrame: Error during encoding\n");
        out_packet->size = 0;
    }
    av_packet_free(&av_pkt);
}

void Codec_CloseEncoder(EncoderContext *ctx) {
    if (!ctx) return;
    
    if (ctx->codec_ctx) {
        avcodec_free_context(&ctx->codec_ctx);
    }
    if (ctx->frame_yuv) {
        av_frame_free(&ctx->frame_yuv);
    }
    if (ctx->sws_ctx) {
        sws_freeContext(ctx->sws_ctx);
        ctx->sws_ctx = NULL;
    }
    // Arena-allocated struct remains "allocated" until arena reset, but we zero it to define it as closed.
    memset(ctx, 0, sizeof(EncoderContext));
}
