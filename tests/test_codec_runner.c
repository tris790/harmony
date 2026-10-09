#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <time.h>
#include "../src/memory_arena.h"
#include "../src/codec_api.h"
#include "../src/codec/codec_ffmpeg.c"
#include "../src/codec/codec_ffmpeg_decode.c"

// The standalone codec test does not link the Wayland time implementation.
double OS_GetTime(void) {
    struct timespec ts;
    assert(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

// Helper: Fill frame with dummy RGBA data (moving box)
void FillTestFrame(VideoFrame *frame, int frame_idx) {
    uint8_t *pixels = frame->data[0];
    int stride = frame->linesize[0];
    
    // Clear background (black)
    for (int y = 0; y < frame->height; ++y) {
        for (int x = 0; x < frame->width; ++x) {
            int offset = y * stride + x * 4;
            pixels[offset + 0] = 0;
            pixels[offset + 1] = 0;
            pixels[offset + 2] = 0;
            pixels[offset + 3] = 255;
        }
    }

    // Draw white box
    int box_size = 50;
    int pos_x = (frame_idx * 5) % (frame->width - box_size);
    int pos_y = (frame_idx * 5) % (frame->height - box_size);

    for (int y = 0; y < box_size; ++y) {
        for (int x = 0; x < box_size; ++x) {
            int offset = (pos_y + y) * stride + (pos_x + x) * 4;
            pixels[offset + 0] = 255;
            pixels[offset + 1] = 255;
            pixels[offset + 2] = 255;
        }
    }
}

int main() {
    printf("Starting Codec Test...\n");

    VideoFormat format = {
        .width = 1280,
        .height = 720,
        .fps = 60,
        .bitrate = 4000000 // 4 Mbps
    };

    // Arenas
    MemoryArena main_arena;
    ArenaInit(&main_arena, 128 * 1024 * 1024);
    
    MemoryArena packet_arena;
    ArenaInit(&packet_arena, 16 * 1024 * 1024); // Reused for packets

    // 1. Init Encoder
    EncoderContext *encoder = Codec_InitEncoder(&main_arena, format);
    assert(encoder != NULL);
    printf("Encoder Initialized.\n");

    // 2. Init Decoder
    DecoderContext *decoder = Codec_InitDecoder(&main_arena);
    assert(decoder != NULL);
    printf("Decoder Initialized.\n");

    // Create Input Frame Buffer (RGBA)
    VideoFrame input_frame = {0};
    input_frame.width = format.width;
    input_frame.height = format.height;
    input_frame.linesize[0] = format.width * 4;
    input_frame.data[0] = ArenaPush(&main_arena, format.height * input_frame.linesize[0]);

    // Test Loop
    int frame_count = 60;
    int success_count = 0;
    
    for (int i = 0; i < frame_count; ++i) {
        ArenaClear(&packet_arena); // Reset packet memory each frame

        FillTestFrame(&input_frame, i);

        // Encode
        EncodedPacket pkt = {0};
        Codec_EncodeFrame(encoder, &input_frame, &packet_arena, &pkt);

        if (pkt.size > 0) {
            // Decode
            VideoFrame decoded_frame = {0};
            Codec_DecodePacket(decoder, &pkt, &decoded_frame);

            if (decoded_frame.data[0] != NULL) {
                // Decoder output is YUV420P. Scan the luma plane using its stride,
                // rather than reading width * height * 4 bytes as if it were RGBA.
                int non_zero = 0;
                uint8_t *p = decoded_frame.data[0];
                assert(decoded_frame.width == format.width);
                assert(decoded_frame.height == format.height);
                for (int y = 0; y < decoded_frame.height && !non_zero; ++y) {
                    for (int x = 0; x < decoded_frame.width; ++x) {
                        if (p[y * decoded_frame.linesize[0] + x] > 128) {
                            non_zero = 1;
                            break;
                        }
                    }
                }
                
                if (non_zero) {
                    success_count++;
                    if (i % 10 == 0) printf("Frame %d: Encoded %zu bytes -> Decoded OK (Content Verified).\n", i, pkt.size);
                } else {
                    printf("Frame %d: Decoded frame is ALL BLACK! Failure.\n", i);
                }
            }
        }
    }

    printf("Test Finished. %d/%d frames successfully round-tripped.\n", success_count, frame_count);

    Codec_CloseDecoder(decoder);
    Codec_CloseEncoder(encoder);
    munmap(packet_arena.base, packet_arena.size);
    munmap(main_arena.base, main_arena.size);
    return success_count == frame_count ? 0 : 1;
}
