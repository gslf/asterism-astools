#include "duration.h"

static bool number(const char **text, int64_t *out) {
  const char *s = *text;
  int64_t n = 0;
  if (*s < '0' || *s > '9')
    return false;
  unsigned digits = 0;
  while (*s >= '0' && *s <= '9') {
    int digit = *s++ - '0';
    if (++digits > 19 || n > (INT64_MAX - digit) / 10)
      return false;
    n = n * 10 + digit;
  }
  *text = s;
  *out = n;
  return true;
}
static bool add(int64_t *total, int64_t n, int64_t unit, int64_t fraction) {
  if (*total > INT64_MAX - fraction || n > (INT64_MAX - *total - fraction) / unit)
    return false;
  *total += n * unit + fraction;
  return true;
}
bool astools_duration_parse_ms(const char *s, int64_t *out) {
  int64_t total = 0, n;
  if (!s || !out || *s++ != 'P' || !*s)
    return false;
  if (*s != 'T') {
    if (!number(&s, &n))
      return false;
    char unit = *s++;
    if (unit == 'W') {
      if (*s || !add(&total, n, 604800000, 0))
        return false;
      *out = total;
      return true;
    }
    if (unit != 'D' || !add(&total, n, 86400000, 0))
      return false;
    if (!*s) {
      *out = total;
      return true;
    }
  }
  if (*s++ != 'T' || !*s)
    return false;
  int stage = 0;
  while (*s) {
    int64_t fraction = 0, unit;
    if (!number(&s, &n))
      return false;
    bool decimal = *s == '.';
    if (decimal) {
      s++;
      int scale = 100;
      if (*s < '0' || *s > '9')
        return false;
      while (*s >= '0' && *s <= '9') {
        if (!scale)
          return false;
        fraction += (*s++ - '0') * scale;
        scale /= 10;
      }
    }
    char suffix = *s++;
    if (suffix == 'H' && stage < 1 && !decimal) {
      unit = 3600000;
      stage = 1;
    } else if (suffix == 'M' && stage < 2 && !decimal) {
      unit = 60000;
      stage = 2;
    } else if (suffix == 'S' && stage < 3) {
      unit = 1000;
      stage = 3;
    } else
      return false;
    if (!add(&total, n, unit, fraction))
      return false;
  }
  *out = total;
  return true;
}
