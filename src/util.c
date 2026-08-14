/*
 * util.c — buffers, strings, FNV-1a, Base64, slug checks, token heuristic.
 *
 * astools_buf: data stays NULL until the first append/printf; every mutating
 * call leaves data NUL-terminated on success.
 */

#include "astools_internal.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* ---- growable buffer ---------------------------------------------------- */

void astools_buf_init(astools_buf *b) {
  if (!b) return;
  b->data = NULL;
  b->len = 0;
  b->cap = 0;
}

void astools_buf_free(astools_buf *b) {
  if (!b) return;
  free(b->data);
  b->data = NULL;
  b->len = 0;
  b->cap = 0;
}

static astools_err buf_reserve(astools_buf *b, size_t extra) {
  size_t need, cap;
  char *p;
  if (extra > SIZE_MAX - b->len - 1) return ASTOOLS_ERR_NOMEM;
  need = b->len + extra + 1;
  if (need <= b->cap) return ASTOOLS_OK;
  cap = b->cap ? b->cap : 64;
  while (cap < need) {
    if (cap > SIZE_MAX / 2) {
      cap = need;
      break;
    }
    cap *= 2;
  }
  p = realloc(b->data, cap);
  if (!p) return ASTOOLS_ERR_NOMEM;
  b->data = p;
  b->cap = cap;
  return ASTOOLS_OK;
}

astools_err astools_buf_append(astools_buf *b, const char *data, size_t len) {
  astools_err e;
  if (!b || (!data && len > 0)) return ASTOOLS_ERR_INVALID;
  e = buf_reserve(b, len);
  if (e != ASTOOLS_OK) return e;
  if (len > 0) memcpy(b->data + b->len, data, len);
  b->len += len;
  b->data[b->len] = '\0';
  return ASTOOLS_OK;
}

astools_err astools_buf_appends(astools_buf *b, const char *s) {
  if (!s) return ASTOOLS_ERR_INVALID;
  return astools_buf_append(b, s, strlen(s));
}

astools_err astools_buf_appendc(astools_buf *b, char ch) {
  return astools_buf_append(b, &ch, 1);
}

astools_err astools_buf_printf(astools_buf *b, const char *fmt, ...) {
  va_list ap;
  int need;
  astools_err e;
  if (!b || !fmt) return ASTOOLS_ERR_INVALID;
  va_start(ap, fmt);
  need = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (need < 0) return ASTOOLS_ERR_INVALID;
  e = buf_reserve(b, (size_t)need);
  if (e != ASTOOLS_OK) return e;
  va_start(ap, fmt);
  vsnprintf(b->data + b->len, (size_t)need + 1, fmt, ap);
  va_end(ap);
  b->len += (size_t)need;
  return ASTOOLS_OK;
}

char *astools_buf_detach(astools_buf *b) {
  char *p;
  if (!b) return NULL;
  p = b->data;
  if (!p) {
    p = malloc(1);
    if (p) p[0] = '\0';
  }
  b->data = NULL;
  b->len = 0;
  b->cap = 0;
  return p;
}

/* ---- strings ------------------------------------------------------------ */

char *astools_strdup(const char *s) {
  size_t n;
  char *p;
  if (!s) return NULL;
  n = strlen(s);
  p = malloc(n + 1);
  if (!p) return NULL;
  memcpy(p, s, n + 1);
  return p;
}

char *astools_strndup(const char *s, size_t n) {
  size_t len = 0;
  char *p;
  if (!s) return NULL;
  while (len < n && s[len] != '\0') len++;
  p = malloc(len + 1);
  if (!p) return NULL;
  memcpy(p, s, len);
  p[len] = '\0';
  return p;
}

static bool is_ascii_space(char ch) {
  return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\v' ||
         ch == '\f';
}

char *astools_str_trim(char *s) {
  size_t len, start = 0;
  if (!s) return NULL;
  len = strlen(s);
  while (len > 0 && is_ascii_space(s[len - 1])) len--;
  while (start < len && is_ascii_space(s[start])) start++;
  if (start > 0) memmove(s, s + start, len - start);
  s[len - start] = '\0';
  return s;
}

bool astools_str_blank(const char *s) {
  if (!s) return true;
  for (; *s; s++)
    if (!is_ascii_space(*s)) return false;
  return true;
}

/* ---- FNV-1a 64 ---------------------------------------------------------- */

uint64_t astools_fnv1a64(const void *data, size_t len) {
  const uint8_t *p = (const uint8_t *)data;
  uint64_t h = UINT64_C(0xcbf29ce484222325);
  size_t i;
  if (!p) return h;
  for (i = 0; i < len; i++) {
    h ^= p[i];
    h *= UINT64_C(0x100000001b3);
  }
  return h;
}

/* ---- Base64 (RFC 4648, strict) ------------------------------------------ */

static const char b64_alpha[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

char *astools_base64_encode(const uint8_t *data, size_t len) {
  size_t olen, i, o = 0;
  char *out;
  if (!data && len > 0) return NULL;
  if (len > (SIZE_MAX / 4) * 3 - 2) return NULL;
  olen = ((len + 2) / 3) * 4;
  out = malloc(olen + 1);
  if (!out) return NULL;
  for (i = 0; i + 3 <= len; i += 3) {
    uint32_t v = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8) |
                 (uint32_t)data[i + 2];
    out[o++] = b64_alpha[(v >> 18) & 63];
    out[o++] = b64_alpha[(v >> 12) & 63];
    out[o++] = b64_alpha[(v >> 6) & 63];
    out[o++] = b64_alpha[v & 63];
  }
  if (len - i == 1) {
    uint32_t v = (uint32_t)data[i] << 16;
    out[o++] = b64_alpha[(v >> 18) & 63];
    out[o++] = b64_alpha[(v >> 12) & 63];
    out[o++] = '=';
    out[o++] = '=';
  } else if (len - i == 2) {
    uint32_t v = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8);
    out[o++] = b64_alpha[(v >> 18) & 63];
    out[o++] = b64_alpha[(v >> 12) & 63];
    out[o++] = b64_alpha[(v >> 6) & 63];
    out[o++] = '=';
  }
  out[o] = '\0';
  return out;
}

static int b64_val(char ch) {
  if (ch >= 'A' && ch <= 'Z') return ch - 'A';
  if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
  if (ch >= '0' && ch <= '9') return ch - '0' + 52;
  if (ch == '+') return 62;
  if (ch == '/') return 63;
  return -1;
}

bool astools_base64_decode(const char *s, uint8_t **out, size_t *out_len) {
  size_t n, pad = 0, i, o = 0;
  uint8_t *buf;
  if (!s || !out || !out_len) return false;
  n = strlen(s);
  if (n % 4 != 0) return false;
  if (n == 0) {
    buf = malloc(1);
    if (!buf) return false;
    *out = buf;
    *out_len = 0;
    return true;
  }
  if (s[n - 1] == '=') {
    pad = 1;
    if (s[n - 2] == '=') pad = 2;
  }
  for (i = 0; i < n - pad; i++)
    if (b64_val(s[i]) < 0) return false; /* also rejects interior '=' */
  buf = malloc((n / 4) * 3);
  if (!buf) return false;
  for (i = 0; i < n; i += 4) {
    int a = b64_val(s[i]), b = b64_val(s[i + 1]);
    if (i + 4 < n || pad == 0) {
      int c3 = b64_val(s[i + 2]), c4 = b64_val(s[i + 3]);
      uint32_t v = ((uint32_t)a << 18) | ((uint32_t)b << 12) |
                   ((uint32_t)c3 << 6) | (uint32_t)c4;
      buf[o++] = (uint8_t)(v >> 16);
      buf[o++] = (uint8_t)(v >> 8);
      buf[o++] = (uint8_t)v;
    } else if (pad == 1) {
      int c3 = b64_val(s[i + 2]);
      uint32_t v;
      if (c3 & 0x03) { /* non-canonical trailing bits */
        free(buf);
        return false;
      }
      v = ((uint32_t)a << 18) | ((uint32_t)b << 12) | ((uint32_t)c3 << 6);
      buf[o++] = (uint8_t)(v >> 16);
      buf[o++] = (uint8_t)(v >> 8);
    } else { /* pad == 2 */
      if (b & 0x0f) {
        free(buf);
        return false;
      }
      buf[o++] = (uint8_t)((((uint32_t)a << 18) | ((uint32_t)b << 12)) >> 16);
    }
  }
  *out = buf;
  *out_len = o;
  return true;
}

/* ---- project slug ------------------------------------------------------- */

bool astools_slug_valid(const char *s) {
  size_t i;
  if (!s) return false;
  if (!((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= '0' && s[0] <= '9')))
    return false;
  for (i = 1; s[i] != '\0'; i++) {
    char ch = s[i];
    if (i > 63) return false;
    if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-'))
      return false;
  }
  return true;
}

/* ---- token heuristic ---------------------------------------------------- */

int astools_token_heuristic(const char *text) {
  size_t len, t;
  if (!text || text[0] == '\0') return 0;
  len = strlen(text);
  t = len / 4;
  if (t == 0) t = 1;
  if (t > (size_t)INT_MAX) t = (size_t)INT_MAX;
  return (int)t;
}
