/*
 * semver.c — strict SemVer 2.0.0 parse and precedence.
 *
 * Build metadata ("+..." suffix) is validated and then discarded: it never
 * participates in precedence and is not stored.
 */

#include "astools_internal.h"

#include <stdlib.h>
#include <string.h>

static bool sv_digit(char ch) { return ch >= '0' && ch <= '9'; }

static bool sv_ident_char(char ch) {
  return sv_digit(ch) || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
         ch == '-';
}

/* One numeric component: digits only, no leading zero unless the component
 * is exactly "0", must fit int64. Returns the first unconsumed character,
 * NULL on malformed input or overflow. */
static const char *sv_number(const char *p, int64_t *out) {
  int64_t v = 0;
  if (!sv_digit(*p)) return NULL;
  if (p[0] == '0' && sv_digit(p[1])) return NULL;
  for (; sv_digit(*p); p++) {
    int d = *p - '0';
    if (v > (INT64_MAX - d) / 10) return NULL;
    v = v * 10 + d;
  }
  *out = v;
  return p;
}

/* A dot-separated identifier series; every identifier is [0-9A-Za-z-]+ and
 * non-empty. When strict_num, all-digit identifiers must not have leading
 * zeros (the pre-release rule; build metadata has no such rule). Returns
 * the first unconsumed character, NULL on malformed input. */
static const char *sv_idents(const char *p, bool strict_num) {
  for (;;) {
    const char *field = p;
    bool numeric = true;
    if (!sv_ident_char(*p)) return NULL; /* empty identifier */
    for (; sv_ident_char(*p); p++)
      if (!sv_digit(*p)) numeric = false;
    if (strict_num && numeric && p - field > 1 && field[0] == '0') return NULL;
    if (*p != '.') return p;
    p++;
  }
}

bool astools_semver_parse(const char *s, astools_semver *out) {
  int64_t maj = 0, min = 0, pat = 0;
  const char *p;
  const char *pre = NULL, *pre_end = NULL;

  if (!s || !out) return false;
  p = sv_number(s, &maj);
  if (!p || *p != '.') return false;
  p = sv_number(p + 1, &min);
  if (!p || *p != '.') return false;
  p = sv_number(p + 1, &pat);
  if (!p) return false;
  if (*p == '-') {
    pre = p + 1;
    p = sv_idents(pre, true);
    if (!p) return false;
    pre_end = p;
  }
  if (*p == '+') {
    p = sv_idents(p + 1, false); /* accepted, then discarded */
    if (!p) return false;
  }
  if (*p != '\0') return false;

  out->major = maj;
  out->minor = min;
  out->patch = pat;
  out->prerelease = NULL;
  if (pre) {
    out->prerelease = astools_strndup(pre, (size_t)(pre_end - pre));
    if (!out->prerelease) return false;
  }
  return true;
}

static size_t sv_field_len(const char *p) {
  size_t n = 0;
  while (p[n] != '\0' && p[n] != '.') n++;
  return n;
}

static bool sv_field_numeric(const char *p, size_t n) {
  size_t i;
  if (n == 0) return false;
  for (i = 0; i < n; i++)
    if (!sv_digit(p[i])) return false;
  return true;
}

int astools_semver_cmp(const astools_semver *a, const astools_semver *b) {
  const char *pa, *pb;

  if (a == b) return 0;
  if (!a) return -1;
  if (!b) return 1;
  if (a->major != b->major) return a->major < b->major ? -1 : 1;
  if (a->minor != b->minor) return a->minor < b->minor ? -1 : 1;
  if (a->patch != b->patch) return a->patch < b->patch ? -1 : 1;
  if (!a->prerelease && !b->prerelease) return 0;
  if (!a->prerelease) return 1; /* release > pre-release */
  if (!b->prerelease) return -1;

  pa = a->prerelease;
  pb = b->prerelease;
  for (;;) {
    size_t la, lb, minlen;
    bool na, nb;
    bool done_a = (*pa == '\0'), done_b = (*pb == '\0');
    int r;

    if (done_a && done_b) return 0;
    if (done_a) return -1; /* fewer fields, prefix-equal so far */
    if (done_b) return 1;

    la = sv_field_len(pa);
    lb = sv_field_len(pb);
    na = sv_field_numeric(pa, la);
    nb = sv_field_numeric(pb, lb);
    if (na && !nb) return -1; /* numeric < alphanumeric */
    if (!na && nb) return 1;
    if (na) {
      /* no leading zeros: a longer decimal string is a larger number */
      if (la != lb) return la < lb ? -1 : 1;
      r = memcmp(pa, pb, la);
      if (r != 0) return r < 0 ? -1 : 1;
    } else {
      minlen = la < lb ? la : lb;
      r = memcmp(pa, pb, minlen);
      if (r != 0) return r < 0 ? -1 : 1;
      if (la != lb) return la < lb ? -1 : 1; /* strcmp semantics */
    }
    pa += la;
    if (*pa == '.') pa++;
    pb += lb;
    if (*pb == '.') pb++;
  }
}

void astools_semver_free(astools_semver *v) {
  if (!v) return;
  free(v->prerelease);
  v->prerelease = NULL;
}
