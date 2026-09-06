/* Parse hostile manifests without registering packages or executing commands. */
#include "astools_internal.h"
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

static void duration(const uint8_t *data, size_t size) {
  char *text = malloc(size + 1);
  if (!text)
    return;
  memcpy(text, data, size);
  text[size] = 0;
  int64_t ms = -1, again = -1;
  if (astools_duration_parse_ms(text, &ms)) {
    char canonical[64];
    if (ms < 0)
      abort();
    snprintf(canonical, sizeof canonical, "PT%" PRId64 ".%03" PRId64 "S", ms / 1000, ms % 1000);
    if (!astools_duration_parse_ms(canonical, &again) || ms != again)
      abort();
  }
  free(text);
}
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size > 16384)
    return 0;
  duration(data, size);
  char *error = NULL, *wire = NULL, *again = NULL;
  astools_manifest *m = astools_manifest_parse((const char *)data, size, NULL, &error);
  free(error);
  if (!m)
    return 0;
  if (astools_manifest_render(m, &wire) == ASTOOLS_OK) {
    astools_manifest *copy = astools_manifest_parse(wire, strlen(wire), NULL, &error);
    if (!copy)
      abort();
    if (astools_manifest_render(copy, &again) == ASTOOLS_OK && strcmp(wire, again))
      abort();
    free(error);
    free(again);
    free(wire);
    astools_manifest_free(copy);
  }
  astools_manifest_free(m);
  return 0;
}
