/*
 * ed25519_verify.h  SHA-256, SHA-512, and RFC 8032 Ed25519 signature verification (pure C)
 */

#ifndef ED25519_VERIFY_H
#define ED25519_VERIFY_H

#include <stddef.h>
#include <stdint.h>

typedef struct SHA256_CTX {
    uint64_t bits;
    uint32_t state[8];
    uint8_t buf[64];
    size_t len;
} SHA256_CTX;

void sha256_init(SHA256_CTX* ctx);
void sha256_update(SHA256_CTX* ctx, const void* data, size_t len);
void sha256_final(SHA256_CTX* ctx, uint8_t out[32]);
void sha256_bytes(const void* data, size_t len, uint8_t out[32]);
int  sha256_file(const char* path, uint8_t out[32]);
void sha256_hex(const uint8_t hash[32], char out_hex[65]);

void sha512_bytes(const void* data, size_t len, uint8_t out[64]);

/* Returns 1 if RFC 8032 Ed25519 signature over msg[0..msg_len) verifies under pk[32], else 0. */
int ed25519_verify(const uint8_t sig[64], const void* msg, size_t msg_len, const uint8_t pk[32]);

/* Returns 1 if embedded RFC 8032 test vectors pass, else 0. */
int ed25519_self_test(void);

#endif
