/*
 * tool_grep.c — grep.search.
 *
 * Line-based search under one root path. lstat walk that never follows
 * symlinks (symlink entries count as skipped); directory entries are
 * processed in strcmp order at every level so results are deterministic.
 * Binary files (NUL in the first 4 KiB) and files larger than
 * max_file_bytes are skipped and counted. At most one match is reported
 * per line (the first); matching tolerates CRLF by trimming the trailing
 * '\r', which cannot shift byte columns because it only drops the last
 * byte. Result paths are workspace-relative when under the workspace.
 */

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#endif

#include "sdk.h"
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include "compat_win32.h" /* must come after every system include */
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#define GREP_TEXT_CAP 512
#define GREP_MAX_DEPTH 64

typedef struct {
  size_t off;
  size_t len; /* raw length, including a trailing '\r' if present */
} lspan;

typedef struct {
  const char *ws;
  size_t ws_len;
  astd_regex *re; /* NULL => literal search */
  const char *pattern;
  size_t pat_len;
  int case_sensitive;
  const char *glob; /* may be NULL */
  int recursive;
  int64_t context;
  int64_t max_results;
  int64_t max_file_bytes;
  xcdn_value_t *matches;
  int64_t files_searched;
  int64_t files_skipped;
  int64_t count;
  int truncated;
  int stop;
  int oom;
} gctx;

static const char *rel_of(const gctx *g, const char *abs) {
  if (g->ws && g->ws_len > 0 && strncmp(abs, g->ws, g->ws_len) == 0 &&
      abs[g->ws_len] == '/')
    return abs + g->ws_len + 1;
  return abs;
}

static char *path_join(const char *dir, const char *name) {
  size_t dl = strlen(dir), nl = strlen(name);
  char *p = malloc(dl + nl + 2);
  if (!p) return NULL;
  memcpy(p, dir, dl);
  p[dl] = '/';
  memcpy(p + dl + 1, name, nl + 1);
  return p;
}

/* Line text value capped at GREP_TEXT_CAP bytes plus an ellipsis marker;
 * the cut is moved off a UTF-8 continuation byte so the serialized
 * string stays valid for UTF-8 input. */
static xcdn_value_t *text_value(const char *s, size_t len) {
  size_t n = len;
  int cut = 0;
  char *buf;
  if (n > GREP_TEXT_CAP) {
    n = GREP_TEXT_CAP;
    while (n > 0 && ((unsigned char)s[n] & 0xc0) == 0x80) n--;
    cut = 1;
  }
  buf = malloc(n + 4);
  if (!buf) return NULL;
  memcpy(buf, s, n);
  if (cut) {
    memcpy(buf + n, "\xe2\x80\xa6", 3);
    n += 3;
  }
  buf[n] = '\0';
  return xcdn_value_string_owned(buf);
}

static size_t trim_len(const char *s, size_t len) {
  if (len > 0 && s[len - 1] == '\r') return len - 1;
  return len;
}

/* Lines are the segments before each '\n'; a trailing segment without a
 * final newline is still a line, an empty trailing segment is not. */
static lspan *split_lines(const char *buf, size_t n, size_t *count) {
  size_t nl = 0, i, start;
  lspan *ls;
  for (i = 0; i < n; i++)
    if (buf[i] == '\n') nl++;
  if (n > 0 && buf[n - 1] != '\n') nl++;
  *count = nl;
  if (nl == 0)
    return malloc(sizeof *ls); /* non-NULL, correctly typed sentinel */
  ls = malloc(nl * sizeof *ls);
  if (!ls) return NULL;
  start = 0;
  nl = 0;
  for (i = 0; i < n; i++) {
    if (buf[i] == '\n') {
      ls[nl].off = start;
      ls[nl].len = i - start;
      nl++;
      start = i + 1;
    }
  }
  if (start < n) {
    ls[nl].off = start;
    ls[nl].len = n - start;
  }
  return ls;
}

static long find_literal(const char *hay, size_t n, const char *pat, size_t m,
                         int cs) {
  size_t i, j;
  if (m == 0 || m > n) return -1;
  for (i = 0; i + m <= n; i++) {
    for (j = 0; j < m; j++) {
      unsigned char a = (unsigned char)hay[i + j];
      unsigned char b = (unsigned char)pat[j];
      if (!cs) {
        if (a >= 'A' && a <= 'Z') a = (unsigned char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (unsigned char)(b + 32);
      }
      if (a != b) break;
    }
    if (j == m) return (long)i;
  }
  return -1;
}

static int push_context(xcdn_value_t *m, const char *key, const char *buf,
                        const lspan *ls, size_t from,
                        size_t to /* exclusive */) {
  xcdn_value_t *arr = xcdn_value_array();
  size_t k;
  if (!arr) return -1;
  for (k = from; k < to; k++) {
    size_t tl = trim_len(buf + ls[k].off, ls[k].len);
    xcdn_value_t *tv = text_value(buf + ls[k].off, tl);
    if (!tv || astd_arr_push_val(arr, tv) != 0) {
      if (tv) xcdn_value_free(tv);
      xcdn_value_free(arr);
      return -1;
    }
  }
  if (astd_set_val(m, key, arr) != 0) {
    xcdn_value_free(arr);
    return -1;
  }
  return 0;
}

static void scan_buffer(gctx *g, const char *rel, const char *buf, size_t n) {
  size_t nlines = 0, i;
  lspan *ls = split_lines(buf, n, &nlines);
  if (!ls) {
    g->oom = 1;
    return;
  }
  for (i = 0; i < nlines && !g->stop && !g->oom; i++) {
    const char *line = buf + ls[i].off;
    size_t tl = trim_len(line, ls[i].len);
    long col = -1;
    if (g->re) {
      size_t ms, me;
      if (astd_regex_search(g->re, line, tl, &ms, &me)) col = (long)ms;
    } else {
      col = find_literal(line, tl, g->pattern, g->pat_len, g->case_sensitive);
    }
    if (col >= 0) {
      xcdn_value_t *m = xcdn_value_object();
      int rc = 0;
      if (!m) {
        g->oom = 1;
        break;
      }
      rc |= astd_set_str(m, "path", rel);
      rc |= astd_set_int(m, "line", (int64_t)(i + 1));
      rc |= astd_set_int(m, "column", (int64_t)col + 1);
      if (rc == 0) {
        xcdn_value_t *tv = text_value(line, tl);
        if (!tv) {
          rc = -1;
        } else if (astd_set_val(m, "text", tv) != 0) {
          xcdn_value_free(tv);
          rc = -1;
        }
      }
      if (rc == 0 && g->context > 0) {
        size_t from = i > (size_t)g->context ? i - (size_t)g->context : 0;
        size_t to = i + 1 + (size_t)g->context;
        if (to > nlines) to = nlines;
        rc |= push_context(m, "before", buf, ls, from, i);
        rc |= push_context(m, "after", buf, ls, i + 1, to);
      }
      if (rc == 0) rc = astd_arr_push_val(g->matches, m);
      if (rc != 0) {
        xcdn_value_free(m);
        g->oom = 1;
        break;
      }
      g->count++;
      if (g->count >= g->max_results) {
        g->truncated = 1;
        g->stop = 1;
      }
    }
  }
  free(ls);
}

static void scan_file(gctx *g, const char *abs, const char *rel,
                      int64_t fsize) {
  int fd;
  char *buf;
  size_t n = 0, want;
  if (fsize < 0 || fsize > g->max_file_bytes) {
    g->files_skipped++;
    return;
  }
  fd = open(abs, O_RDONLY);
  if (fd < 0) {
    g->files_skipped++;
    return;
  }
  want = (size_t)fsize;
  buf = malloc(want + 1);
  if (!buf) {
    /* cannot hold this file in memory: skip it, keep searching */
    close(fd);
    g->files_skipped++;
    return;
  }
  while (n < want) {
    ssize_t rd = read(fd, buf + n, want - n);
    if (rd < 0) {
      close(fd);
      free(buf);
      g->files_skipped++;
      return;
    }
    if (rd == 0) break;
    n += (size_t)rd;
  }
  close(fd);
  buf[n] = '\0';
  if (astd_looks_binary(buf, n)) {
    free(buf);
    g->files_skipped++;
    return;
  }
  g->files_searched++;
  scan_buffer(g, rel, buf, n);
  free(buf);
}

static void maybe_scan(gctx *g, const char *abs, int64_t fsize) {
  const char *rel = rel_of(g, abs);
  if (g->glob && !astd_glob_match(g->glob, rel)) return;
  scan_file(g, abs, rel, fsize);
}

static int name_cmp(const void *a, const void *b) {
  return strcmp(*(char *const *)a, *(char *const *)b);
}

static void walk_dir(gctx *g, const char *dir, int depth) {
  DIR *d;
  struct dirent *de;
  char **names = NULL;
  size_t nn = 0, cap = 0, i;
  d = opendir(dir);
  if (!d) return; /* unreadable directory: nothing to search here */
  while ((de = readdir(d)) != NULL) {
    char *dup;
    if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
      continue;
    if (nn == cap) {
      size_t nc = cap ? cap * 2 : 32;
      char **p = realloc(names, nc * sizeof *names);
      if (!p) {
        g->oom = 1;
        break;
      }
      names = p;
      cap = nc;
    }
    {
      size_t nl = strlen(de->d_name);
      dup = malloc(nl + 1);
      if (!dup) {
        g->oom = 1;
        break;
      }
      memcpy(dup, de->d_name, nl + 1);
    }
    names[nn++] = dup;
  }
  closedir(d);
  if (nn > 1) qsort(names, nn, sizeof *names, name_cmp);
  for (i = 0; i < nn; i++) {
    if (!g->stop && !g->oom) {
      char *full = path_join(dir, names[i]);
      struct stat st;
      if (!full) {
        g->oom = 1;
      } else if (lstat(full, &st) != 0) {
        g->files_skipped++;
      } else if (S_ISLNK(st.st_mode)) {
        /* never follow symlinks; count the entry as skipped */
        g->files_skipped++;
      } else if (S_ISDIR(st.st_mode)) {
        if (g->recursive && depth < GREP_MAX_DEPTH)
          walk_dir(g, full, depth + 1);
      } else if (S_ISREG(st.st_mode)) {
        maybe_scan(g, full, (int64_t)st.st_size);
      } /* fifos, sockets, devices: ignored */
      free(full);
    }
    free(names[i]);
  }
  free(names);
}

static int64_t clamp64(int64_t v, int64_t lo, int64_t hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

static const char *grep_error_code(const astd_req *r, const char *code) {
  if (r->tool && strcmp(r->tool, "code") == 0) return "code/failed";
  return code;
}

int astd_tool_grep(astd_req *r) {
  gctx g;
  const char *path;
  int use_regex;
  struct stat st;
  xcdn_value_t *res;
  int rc = 0;
  if (strcmp(r->command, "search") != 0) {
    astd_fail(r, "astools/protocol", "unknown grep command '%s'", r->command);
    return 0;
  }
  memset(&g, 0, sizeof g);
  g.pattern = astd_arg_str(r, "pattern", NULL);
  if (!g.pattern || g.pattern[0] == '\0') {
    astd_fail(r, "grep/bad-pattern", "pattern must not be empty");
    return 0;
  }
  g.pat_len = strlen(g.pattern);
  path = astd_arg_str(r, "path", NULL);
  /* the runtime canonicalizes "." to the workspace root; cover hosts
   * running with validation off */
  if (!path || strcmp(path, ".") == 0)
    path = r->workspace ? r->workspace : ".";
  use_regex = astd_arg_bool(r, "regex", 1);
  g.case_sensitive = astd_arg_bool(r, "case_sensitive", 1);
  g.glob = astd_arg_str(r, "glob", NULL);
  g.recursive = astd_arg_bool(r, "recursive", 1);
  g.context = clamp64(astd_arg_int(r, "context", 0), 0, 10);
  g.max_results = clamp64(astd_arg_int(r, "max_results", 200), 1, 1000);
  g.max_file_bytes = astd_arg_int(r, "max_file_bytes", 1048576);
  if (g.max_file_bytes < 0) g.max_file_bytes = 0;
  g.ws = r->workspace;
  g.ws_len = g.ws ? strlen(g.ws) : 0;
  if (use_regex) {
    const char *err = NULL;
    g.re = astd_regex_compile(g.pattern, g.case_sensitive, &err);
    if (!g.re) {
      astd_fail(r, grep_error_code(r, "grep/bad-pattern"), "%s",
                err ? err : "invalid pattern");
      return 0;
    }
  }
  g.matches = xcdn_value_array();
  if (!g.matches) {
    astd_regex_free(g.re);
    astd_fail(r, grep_error_code(r, "grep/failed"), "out of memory");
    return 0;
  }
  if (stat(path, &st) == 0) {
    if (S_ISDIR(st.st_mode))
      walk_dir(&g, path, 0);
    else if (S_ISREG(st.st_mode))
      maybe_scan(&g, path, (int64_t)st.st_size);
  }
  astd_regex_free(g.re);
  if (g.oom) {
    xcdn_value_free(g.matches);
    astd_fail(r, grep_error_code(r, "grep/failed"), "out of memory");
    return 0;
  }
  res = xcdn_value_object();
  if (!res) {
    xcdn_value_free(g.matches);
    astd_fail(r, grep_error_code(r, "grep/failed"), "out of memory");
    return 0;
  }
  rc |= astd_set_val(res, "matches", g.matches);
  if (rc != 0) xcdn_value_free(g.matches); /* not adopted on failure */
  rc |= astd_set_int(res, "files_searched", g.files_searched);
  rc |= astd_set_int(res, "files_skipped", g.files_skipped);
  rc |= astd_set_bool(res, "truncated", g.truncated);
  if (rc != 0) {
    xcdn_value_free(res);
    astd_fail(r, grep_error_code(r, "grep/failed"), "out of memory");
    return 0;
  }
  astd_ok(r, res);
  return 0;
}
