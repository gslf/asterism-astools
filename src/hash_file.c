/* Platform-aware file wrapper around the shared hash primitive. */
#include "astools_internal.h"
#include <stdlib.h>

astools_err astools_sha256_file(const char *path, uint8_t out[32]) {
  FILE *f;
  uint8_t *buf;
  astools_sha256_ctx ctx;
  size_t got;
  astools_err err = ASTOOLS_OK;
  if (!path || !out) return ASTOOLS_ERR_INVALID;
  f = os_fopen(path, "rb");
  if (!f) return os_file_exists(path) ? ASTOOLS_ERR_IO : ASTOOLS_ERR_NOT_FOUND;
  buf = malloc(65536);
  if (!buf) {
    fclose(f);
    return ASTOOLS_ERR_NOMEM;
  }
  astools_sha256_init(&ctx);
  while ((got = fread(buf, 1, 65536, f)) > 0)
    astools_sha256_update(&ctx, buf, got);
  if (ferror(f)) err = ASTOOLS_ERR_IO;
  free(buf);
  fclose(f);
  if (err == ASTOOLS_OK) astools_sha256_final(&ctx, out);
  return err;
}
