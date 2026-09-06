/* Length-delimited JSON-RPC framing. Payloads remain opaque to the I/O layer. */
#include "execution.h"

#define HEADER_CAP 8192u

static int equal(const char *s, size_t n, const char *word) {
  if (strlen(word) != n) return 0;
  for (size_t i = 0; i < n; i++) {
    unsigned char ch = (unsigned char)s[i];
    if (ch >= 'A' && ch <= 'Z') ch = (unsigned char)(ch + 'a' - 'A');
    if (ch != (unsigned char)word[i]) return 0;
  }
  return 1;
}

astools_err astools_pp_take_frame(astools_buf *b, size_t cap, char **out, size_t *len) {
  size_t header = 0, length = 0, at = 0;
  int have_length = 0, have_type = 0;
  *out = NULL;
  *len = 0;
  for (size_t i = 0; i < b->len && i < HEADER_CAP; i++) {
    unsigned char ch = (unsigned char)b->data[i];
    if (!ch || ch > 127 || (ch < 32 && ch != '\r' && ch != '\n' && ch != '\t'))
      return ASTOOLS_ERR_PROTOCOL;
    if (i >= 3 && !memcmp(b->data + i - 3, "\r\n\r\n", 4)) {
      header = i + 1;
      break;
    }
  }
  if (!header) return b->len >= HEADER_CAP ? ASTOOLS_ERR_PROTOCOL : ASTOOLS_ERR_BUSY;
  while (at < header - 2) {
    size_t end = at, colon, value, finish;
    while (end + 1 < header && memcmp(b->data + end, "\r\n", 2)) end++;
    colon = at;
    while (colon < end && b->data[colon] != ':') colon++;
    if (colon == at || colon == end) return ASTOOLS_ERR_PROTOCOL;
    value = colon + 1;
    while (value < end && (b->data[value] == ' ' || b->data[value] == '\t')) value++;
    finish = end;
    while (finish > value && (b->data[finish - 1] == ' ' || b->data[finish - 1] == '\t')) finish--;
    if (equal(b->data + at, colon - at, "content-length")) {
      if (have_length++ || value == finish) return ASTOOLS_ERR_PROTOCOL;
      for (size_t i = value; i < finish; i++) {
        unsigned char digit = (unsigned char)b->data[i];
        if (digit < '0' || digit > '9') return ASTOOLS_ERR_PROTOCOL;
        if ((size_t)(digit - '0') > cap || length > (cap - (size_t)(digit - '0')) / 10)
          return ASTOOLS_ERR_TOOL;
        length = length * 10 + (size_t)(digit - '0');
      }
      if (!length) return ASTOOLS_ERR_PROTOCOL;
    } else if (equal(b->data + at, colon - at, "content-type")) {
      if (have_type++ ||
          (!equal(b->data + value, finish - value, "application/vscode-jsonrpc; charset=utf-8") &&
           !equal(b->data + value, finish - value, "application/vscode-jsonrpc")))
        return ASTOOLS_ERR_PROTOCOL;
    } else return ASTOOLS_ERR_PROTOCOL;
    at = end + 2;
  }
  if (!have_length) return ASTOOLS_ERR_PROTOCOL;
  if (length > b->len - header) return ASTOOLS_ERR_BUSY;
  char *text = astools_strndup(b->data + header, length);
  if (!text) return ASTOOLS_ERR_NOMEM;
  memmove(b->data, b->data + header + length, b->len - header - length);
  b->len -= header + length;
  b->data[b->len] = 0;
  *out = text;
  *len = length;
  return ASTOOLS_OK;
}
