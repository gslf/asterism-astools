/* Shared byte validation for JSON and exact process-output transport. */
#include "json.h"

/* Length (1..4) of the valid UTF-8 sequence starting at p, or 0 when the
 * bytes are not well-formed (overlongs, surrogates and > U+10FFFF are
 * rejected). */
size_t jx_utf8_seq(const unsigned char *p, size_t avail) {
  if (!p || !avail)
    return 0;
  unsigned char c = p[0];
  if (c < 0x80)
    return 1;
  if (c < 0xC2)
    return 0; /* continuation byte or overlong lead */
  if (c < 0xE0) {
    if (avail < 2 || (p[1] & 0xC0) != 0x80)
      return 0;
    return 2;
  }
  if (c < 0xF0) {
    if (avail < 3)
      return 0;
    if ((p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80)
      return 0;
    if (c == 0xE0 && p[1] < 0xA0)
      return 0; /* overlong */
    if (c == 0xED && p[1] > 0x9F)
      return 0; /* UTF-16 surrogates */
    return 3;
  }
  if (c < 0xF5) {
    if (avail < 4)
      return 0;
    if ((p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80 || (p[3] & 0xC0) != 0x80)
      return 0;
    if (c == 0xF0 && p[1] < 0x90)
      return 0; /* overlong */
    if (c == 0xF4 && p[1] > 0x8F)
      return 0; /* > U+10FFFF */
    return 4;
  }
  return 0;
}

int jx_utf8_valid(const char *s, size_t len) {
  const unsigned char *p = (const unsigned char *)s;
  size_t i = 0;
  while (i < len) {
    size_t n = jx_utf8_seq(p + i, len - i);
    if (!n)
      return 0;
    i += n;
  }
  return 1;
}
