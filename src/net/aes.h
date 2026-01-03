#ifndef HARMONY_AES_H
#define HARMONY_AES_H

#include <stdint.h>
#include <stddef.h>

typedef struct AES_Ctx {
    uint32_t round_keys[44];
} AES_Ctx;

// Initialize AES context with 128-bit key
void AES_Init(AES_Ctx *ctx, const uint8_t key[16]);

// Derive a 128-bit key from a password string using SHA1
void AES_DeriveKey(const char *password, uint8_t key[16]);

// Encrypt/Decrypt data using AES-128-CTR mode.
// Note: CTR is symmetric, so the same function is used for both.
// iv must be 16 bytes.
void AES_CTR_Xcrypt(AES_Ctx *ctx, const uint8_t iv[16], uint8_t *data, size_t size);

#endif // HARMONY_AES_H
