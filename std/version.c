/* Content versions use the original bytes, before line-ending normalization. */
#include "sdk.h"
#include "sha256.h"
#include <stdio.h>
#include <string.h>

void astd_version(const void *data, size_t n, char out[65]) {
  unsigned char digest[32];
  size_t i;
  astools_sha256(data, n, digest);
  for (i = 0; i < 32; i++) {
    out[i * 2] = "0123456789abcdef"[digest[i] >> 4];
    out[i * 2 + 1] = "0123456789abcdef"[digest[i] & 15];
  }
  out[64] = 0;
}

int astd_version_matches(const char *path, const char *expected) {
  FILE *f = fopen(path, "rb");
  astools_sha256_ctx hash;
  unsigned char buf[4096], digest[32];
  char actual[65];
  size_t n, i;
  if (!f) return 0;
  astools_sha256_init(&hash);
  while ((n = fread(buf, 1, sizeof buf, f)) > 0)
    astools_sha256_update(&hash, buf, n);
  if (ferror(f)) { fclose(f); return 0; }
  fclose(f);
  astools_sha256_final(&hash, digest);
  for (i = 0; i < 32; i++) {
    actual[i * 2] = "0123456789abcdef"[digest[i] >> 4];
    actual[i * 2 + 1] = "0123456789abcdef"[digest[i] & 15];
  }
  actual[64] = 0;
  return expected && strcmp(actual, expected) == 0;
}
