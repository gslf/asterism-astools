/*
 * path.c — path canonicalization and prefix containment.
 *
 * SAFETY CRITICAL. Canonical paths produced here are the sole basis of the
 * filesystem grant checks in policy.c: every symlink and dot segment must be
 * resolved by the kernel (os_realpath) through an existing prefix. Dot
 * segments in a non-existing tail are rejected outright — re-appending them
 * textually would let "a/missing/../../etc" escape the canonical ancestor.
 */

#include "astools_internal.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Lexical cleanup that is always meaning-preserving on POSIX: collapse runs
 * of '/' and drop one trailing '/' (unless the whole path is "/"). Never
 * touches "." or ".." segments — those go through the kernel or are
 * rejected. */
static void path_squeeze(char *s) {
  size_t r = 0, w = 0;
  while (s[r] != '\0') {
    if (s[r] == '/' && w > 0 && s[w - 1] == '/') {
      r++;
      continue;
    }
    s[w++] = s[r++];
  }
  if (w > 1 && s[w - 1] == '/') w--;
  s[w] = '\0';
}

static bool comp_rejected(const char *comp) {
  return comp[0] == '\0' || strcmp(comp, ".") == 0 || strcmp(comp, "..") == 0;
}

/* Remove the last component of work in place, handing it out malloc'd.
 * ASTOOLS_ERR_IO when nothing strippable remains (path degenerated);
 * ASTOOLS_ERR_INVALID when the component is "", "." or "..". */
static astools_err strip_last(char *work, char **out_comp) {
  char *slash;
  const char *comp;
  *out_comp = NULL;
  if (work[0] == '\0' || strcmp(work, "/") == 0) return ASTOOLS_ERR_IO;
  slash = strrchr(work, '/');
  comp = slash ? slash + 1 : work;
  if (comp_rejected(comp)) return ASTOOLS_ERR_INVALID;
  *out_comp = astools_strdup(comp);
  if (!*out_comp) return ASTOOLS_ERR_NOMEM;
  if (!slash)
    work[0] = '\0'; /* relative path fully consumed */
  else if (slash == work)
    work[1] = '\0'; /* keep the root "/" */
  else
    *slash = '\0';
  return ASTOOLS_OK;
}

astools_err astools_path_canonical(const char *workspace, const char *in,
                                   bool missing_ok, char **out) {
  char *work = NULL, *canon = NULL;
  char **tail = NULL;
  size_t tail_n = 0, tail_cap = 0, i;
  astools_err e;

  if (!out) return ASTOOLS_ERR_INVALID;
  *out = NULL;
  if (!in || in[0] == '\0') return ASTOOLS_ERR_INVALID;

  if (os_path_is_abs(in))
    work = astools_strdup(in);
  else
    work = os_path_join(workspace, in);
  if (!work) return ASTOOLS_ERR_NOMEM;
  path_squeeze(work);
  if (work[0] == '\0') {
    free(work);
    return ASTOOLS_ERR_INVALID;
  }

  e = os_realpath(work, &canon);
  if (e == ASTOOLS_OK) {
    free(work);
    if (!canon || canon[0] == '\0') {
      free(canon);
      return ASTOOLS_ERR_IO;
    }
    *out = canon;
    return ASTOOLS_OK;
  }
  if (e != ASTOOLS_ERR_NOT_FOUND) {
    free(work);
    return e;
  }
  if (!missing_ok) {
    free(work);
    return ASTOOLS_ERR_NOT_FOUND;
  }

  /* Walk up to the deepest existing ancestor, collecting the missing tail
   * (deepest component first in tail[]); then re-append it verbatim. */
  for (;;) {
    char *comp = NULL;
    e = strip_last(work, &comp);
    if (e != ASTOOLS_OK) goto fail;
    if (tail_n == tail_cap) {
      size_t ncap = tail_cap ? tail_cap * 2 : 8;
      char **nt;
      if (ncap > SIZE_MAX / sizeof(*nt)) {
        free(comp);
        e = ASTOOLS_ERR_NOMEM;
        goto fail;
      }
      nt = realloc(tail, ncap * sizeof(*nt));
      if (!nt) {
        free(comp);
        e = ASTOOLS_ERR_NOMEM;
        goto fail;
      }
      tail = nt;
      tail_cap = ncap;
    }
    tail[tail_n++] = comp;
    if (work[0] == '\0') {
      /* relative path with no existing ancestor */
      e = ASTOOLS_ERR_IO;
      goto fail;
    }
    canon = NULL;
    e = os_realpath(work, &canon);
    if (e == ASTOOLS_OK) break;
    if (e != ASTOOLS_ERR_NOT_FOUND) goto fail;
  }
  if (!canon || canon[0] == '\0') {
    e = ASTOOLS_ERR_IO;
    goto fail;
  }

  {
    astools_buf b;
    astools_buf_init(&b);
    e = astools_buf_appends(&b, canon);
    for (i = tail_n; e == ASTOOLS_OK && i > 0; i--) {
      if (b.len == 0 || b.data[b.len - 1] != '/')
        e = astools_buf_appendc(&b, '/');
      if (e == ASTOOLS_OK) e = astools_buf_appends(&b, tail[i - 1]);
    }
    if (e != ASTOOLS_OK) {
      astools_buf_free(&b);
      goto fail;
    }
    if (b.len == 0) {
      astools_buf_free(&b);
      e = ASTOOLS_ERR_IO;
      goto fail;
    }
    *out = astools_buf_detach(&b);
    if (!*out) {
      e = ASTOOLS_ERR_NOMEM;
      goto fail;
    }
  }
  free(canon);
  free(work);
  for (i = 0; i < tail_n; i++) free(tail[i]);
  free(tail);
  return ASTOOLS_OK;

fail:
  free(canon);
  free(work);
  for (i = 0; i < tail_n; i++) free(tail[i]);
  free(tail);
  return e;
}

int astools_path_under(const char *path, const char *prefix) {
  size_t plen;
  /* POSIX-first: containment is defined over '/'-separated absolute paths
   * (the only form astools_path_canonical produces). Anything else is not
   * under anything — deny by default. */
  if (!path || !prefix) return 0;
  if (path[0] != '/' || prefix[0] != '/') return 0;
  plen = strlen(prefix);
  while (plen > 1 && prefix[plen - 1] == '/') plen--;
  if (plen == 1) return 1; /* root: every absolute path lies under "/" */
  if (strncmp(path, prefix, plen) != 0) return 0;
  return (path[plen] == '\0' || path[plen] == '/') ? 1 : 0;
}
