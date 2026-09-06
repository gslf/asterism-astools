/* File URIs are decoded before containment checks. Remote authorities are refused. */
#include "lsp.h"

static int hex(unsigned char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
  return -1;
}
char *astools_lsp_uri(const char *path) {
  static const char digits[] = "0123456789ABCDEF";
  if (!path || path[0] != '/' || strlen(path) >= 4096 || !jx_utf8_valid(path, strlen(path)))
    return NULL;
  astools_buf b;
  astools_buf_init(&b);
  astools_err e = astools_buf_appends(&b, "file://");
  for (const unsigned char *p = (const unsigned char *)path; e == ASTOOLS_OK && *p; p++) {
    if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
        strchr("/-._~", *p))
      e = astools_buf_appendc(&b, (char)*p);
    else {
      char encoded[] = {'%', digits[*p >> 4], digits[*p & 15]};
      e = astools_buf_append(&b, encoded, sizeof encoded);
    }
  }
  char *out = e == ASTOOLS_OK ? astools_buf_detach(&b) : NULL;
  astools_buf_free(&b);
  return out;
}
char *astools_lsp_path(const char *uri) {
  if (!uri || strncmp(uri, "file:///", 8) || strlen(uri) > 3 * 4096 + 7) return NULL;
  char *out = malloc(strlen(uri) - 6);
  if (!out) return NULL;
  size_t n = 0;
  for (const unsigned char *p = (const unsigned char *)uri + 7; *p; p++) {
    unsigned char ch = *p;
    if (ch == '%') {
      if (!p[1] || !p[2] || hex(p[1]) < 0 || hex(p[2]) < 0) goto bad;
      ch = (unsigned char)(16 * hex(p[1]) + hex(p[2]));
      p += 2;
    } else if (ch == '?' || ch == '#') goto bad;
    if (!ch || ch == '\\' || n >= 4095) goto bad;
    out[n++] = (char)ch;
  }
  out[n] = 0;
  if (out[0] != '/' || !jx_utf8_valid(out, n)) goto bad;
  return out;
bad:
  free(out);
  return NULL;
}

int astools_lsp_offset(const astools_lsp_file *file, const jx_value *position, size_t *out) {
  const jx_value *line = jx_object_get(position, "line"),
                 *col = jx_object_get(position, "character");
  if (!jx_is_int(line) || !jx_is_int(col) || jx_int_value(line) < 0 || jx_int_value(col) < 0 ||
      jx_int_value(line) > INT32_MAX || jx_int_value(col) > INT32_MAX)
    return 0;
  size_t start = 0, end;
  for (long long i = 0; i < jx_int_value(line); i++) {
    const char *nl = memchr(file->text + start, '\n', file->len - start);
    if (!nl) return 0;
    start = (size_t)(nl - file->text) + 1;
  }
  end = start;
  while (end < file->len && file->text[end] != '\n') end++;
  if (end > start && file->text[end - 1] == '\r') end--;
  if ((unsigned long long)jx_int_value(col) > end - start) return 0;
  *out = start + (size_t)jx_int_value(col);
  return *out == file->len || ((unsigned char)file->text[*out] & 0xc0) != 0x80;
}
