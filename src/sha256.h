#ifndef ASTOOLS_SHA256_H
#define ASTOOLS_SHA256_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
  uint32_t state[8];
  uint64_t bitlen;
  uint8_t buffer[64];
  size_t buflen;
} astools_sha256_ctx;

void astools_sha256_init(astools_sha256_ctx *ctx);
void astools_sha256_update(astools_sha256_ctx *ctx, const void *data,
                           size_t len);
void astools_sha256_final(astools_sha256_ctx *ctx, uint8_t out[32]);
void astools_sha256(const void *data, size_t len, uint8_t out[32]);
#endif
