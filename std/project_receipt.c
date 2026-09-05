/* Parse numeric attributes of JUnit suite summaries, never test output text.
 * Unsupported XML shapes cause abstention; this is not a general XML parser. */
#include "project_receipt.h"
#include <ctype.h>
#include <limits.h>
#include <string.h>

static int number(const char *start, const char *end, const char *name, long *out) {
  size_t n = strlen(name);
  int found = 0;
  *out = 0;
  for (const char *p = start; p < end;) {
    while (p < end && isspace((unsigned char)*p)) p++;
    if (p == end) break;
    const char *key = p;
    while (p < end && !isspace((unsigned char)*p) && *p != '=') p++;
    size_t len = (size_t)(p - key);
    while (p < end && isspace((unsigned char)*p)) p++;
    if (p == end || *p++ != '=') return -1;
    while (p < end && isspace((unsigned char)*p)) p++;
    if (p == end || (*p != '"' && *p != '\'')) return -1;
    char quote = *p++;
    const char *value = p;
    while (p < end && *p != quote) p++;
    if (p == end) return -1;
    if (len == n && !memcmp(key, name, n)) {
      if (value == p || found) return -1;
      for (; value < p; value++) {
        if (*value < '0' || *value > '9' || *out > (LONG_MAX - 9) / 10) return -1;
        *out = *out * 10 + (*value - '0');
      }
      found = 1;
    }
    p++;
  }
  return found;
}
int astd_junit_counts(const char *xml, long *tests, long *skipped, long *failed) {
  const char *p = xml;
  int suites = 0;
  *tests = *skipped = *failed = 0;
  if (!xml || strstr(xml, "<!DOCTYPE") || strstr(xml, "<![CDATA[")) return -1;
  while ((p = strstr(p, "<testsuite ")) != NULL) {
    const char *end = strchr(p, '>');
    long t, s, f, e, d;
    p += 11;
    if (!end || number(p, end, "tests", &t) != 1 ||
        number(p, end, "skipped", &s) < 0 || number(p, end, "failures", &f) < 0 ||
        number(p, end, "errors", &e) < 0 || number(p, end, "disabled", &d) < 0 ||
        t > 10000000 || s > t || d > t || f > t || e > t || s + d > t ||
        *tests > 10000000 - t || !strstr(end, "</testsuite>")) return -1;
    *tests += t; *skipped += s + d; *failed += f + e;
    suites++;
    p = end + 1;
  }
  return suites ? 0 : -1;
}
