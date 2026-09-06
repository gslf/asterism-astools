/* Preserve every captured byte; strings cannot silently hide a NUL or invalid UTF-8. */
#include "json.h"
#include "sdk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int stream(xcdn_value_t *v, const char *name, const char *data, size_t n, int truncated) {
  int binary = memchr(data, 0, n) != NULL || !jx_utf8_valid(data, n);
  char *encoded = binary ? astd_base64_encode((const uint8_t *)data, n) : NULL;
  if (binary && !encoded)
    return -1;
  int e = astd_set_str(v, name, binary ? encoded : data);
  char key[32];
  snprintf(key, sizeof key, "%s_encoding", name);
  e |= astd_set_str(v, key, binary ? "base64" : "utf8");
  snprintf(key, sizeof key, "%s_bytes", name);
  e |= astd_set_int(v, key, (int64_t)n);
  snprintf(key, sizeof key, "%s_truncated", name);
  e |= astd_set_bool(v, key, truncated);
  free(encoded);
  return e;
}
int astd_run_output(xcdn_value_t *v, const astd_run_res *r) {
  return stream(v, "stdout", r->out ? r->out : "", r->out_n, r->out_trunc) |
         stream(v, "stderr", r->err ? r->err : "", r->err_n, r->err_trunc);
}
