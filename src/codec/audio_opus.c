// Opus Audio Encoder/Decoder
// Uses libopus for efficient audio compression

#include "../audio_api.h"
#include <opus/opus.h>
#include <stdio.h>
#include <string.h>

// --- Opus Encoder ---
struct AudioEncoder {
  OpusEncoder *encoder;
  MemoryArena *arena;
  uint8_t *encode_buffer;
  int encode_buffer_size;
};

AudioEncoder *Audio_InitEncoder(MemoryArena *arena) {
  AudioEncoder *enc = PushStructZero(arena, AudioEncoder);
  enc->arena = arena;

  int error;
  enc->encoder =
      opus_encoder_create(AUDIO_SAMPLE_RATE, AUDIO_CHANNELS,
                          OPUS_APPLICATION_AUDIO, // Optimized for music/audio
                          &error);

  if (error != OPUS_OK) {
    fprintf(stderr, "Audio: Opus encoder create failed: %s\n",
            opus_strerror(error));
    return NULL;
  }

  // Configure encoder
  opus_encoder_ctl(enc->encoder, OPUS_SET_BITRATE(128000)); // 128 kbps
  opus_encoder_ctl(enc->encoder, OPUS_SET_COMPLEXITY(5)); // Balance quality/CPU

  // Max encoded size (Opus recommends 4000 bytes for safety)
  enc->encode_buffer_size = 4000;
  enc->encode_buffer = ArenaPush(arena, enc->encode_buffer_size);

  printf("Audio: Opus encoder initialized (128kbps stereo)\n");
  return enc;
}

void Audio_Encode(AudioEncoder *enc, AudioFrame *frame, EncodedAudio *out) {
  if (!enc || !frame || !out)
    return;

  int encoded_bytes =
      opus_encode(enc->encoder, frame->samples, frame->sample_count,
                  enc->encode_buffer, enc->encode_buffer_size);

  if (encoded_bytes < 0) {
    fprintf(stderr, "Audio: Opus encode error: %s\n",
            opus_strerror(encoded_bytes));
    out->data = NULL;
    out->size = 0;
    return;
  }

  out->data = enc->encode_buffer;
  out->size = encoded_bytes;
}

void Audio_CloseEncoder(AudioEncoder *enc) {
  if (enc && enc->encoder) {
    opus_encoder_destroy(enc->encoder);
  }
}

// --- Opus Decoder ---
struct AudioDecoder {
  OpusDecoder *decoder;
  MemoryArena *arena;
  int16_t *decode_buffer;
  int decode_buffer_size;
};

AudioDecoder *Audio_InitDecoder(MemoryArena *arena) {
  AudioDecoder *dec = PushStructZero(arena, AudioDecoder);
  dec->arena = arena;

  int error;
  dec->decoder = opus_decoder_create(AUDIO_SAMPLE_RATE, AUDIO_CHANNELS, &error);

  if (error != OPUS_OK) {
    fprintf(stderr, "Audio: Opus decoder create failed: %s\n",
            opus_strerror(error));
    return NULL;
  }

  // Max decoded samples (120ms at 48kHz stereo = 5760 * 2)
  dec->decode_buffer_size = 5760 * AUDIO_CHANNELS;
  dec->decode_buffer =
      ArenaPush(arena, dec->decode_buffer_size * sizeof(int16_t));

  printf("Audio: Opus decoder initialized\n");
  return dec;
}

void Audio_Decode(AudioDecoder *dec, void *data, size_t size, AudioFrame *out) {
  if (!dec || !data || !out)
    return;

  int decoded_samples = opus_decode(dec->decoder, (const unsigned char *)data,
                                    (opus_int32)size, dec->decode_buffer,
                                    5760, // Max samples per channel
                                    0     // No FEC
  );

  if (decoded_samples < 0) {
    // fprintf(stderr, "Audio: Opus decode error: %s\n",
    // opus_strerror(decoded_samples));
    out->samples = NULL;
    out->sample_count = 0;
    return;
  }

  out->samples = dec->decode_buffer;
  out->sample_count = decoded_samples;
  out->channels = AUDIO_CHANNELS;
}

void Audio_CloseDecoder(AudioDecoder *dec) {
  if (dec && dec->decoder) {
    opus_decoder_destroy(dec->decoder);
  }
}
