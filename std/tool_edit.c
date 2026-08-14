/*
 * tool_edit.c — edit.replace / edit.insert / edit.patch (SPEC §8.5, A1,
 * D15).
 *
 * Common behavior: the target is read whole; a NUL byte in the first
 * 4 KiB means edit/binary. Content is normalized to LF for editing; the
 * dominant line ending (count of "\r\n" vs bare "\n") is restored on
 * write, so mixed-ending files come out uniform in their dominant
 * ending. Every write goes to "<path>.astools-tmp-<pid>" first, is
 * fsynced, then rename(2)-ed over the target (atomic replace).
 *
 * replace: literal or per-line regex. With all:false the find must occur
 * exactly once in the whole file (0 => edit/not-found, N>1 =>
 * edit/ambiguous). replace_with is always literal text: the regex engine
 * has no capture groups, so there are no backreference substitutions.
 * Empty regex matches follow sed's rule: an empty match directly after a
 * previous match is skipped.
 *
 * insert: content becomes line 'line'+1 (line 0 prepends, past-EOF
 * appends).
 *
 * patch: unified diff. Target paths come from the patch text itself, so
 * the runtime could not pre-check them; the tool enforces containment:
 * after stripping, absolute paths, ".." components and paths above the
 * workspace are rejected. (The check is lexical; the sandbox remains the
 * backstop for symlink tricks inside the workspace.) Hunks must apply
 * exactly at their stated position — no fuzz. All hunks are applied to
 * in-memory copies first; files are only written when every hunk
 * applied, and a failed write triggers a best-effort restore of the
 * files already written (all-or-nothing).
 */

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#endif

#include "sdk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32

int astd_tool_edit(astd_req *r) {
  astd_fail(r, "std/unsupported", "edit is not supported on Windows yet");
  return 0;
}

#else /* POSIX */

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* ---- small growable buffer ---------------------------------------------- */

typedef struct {
  char *d;
  size_t n, cap;
} ebuf;

static int eb_put(ebuf *b, const char *s, size_t len) {
  if (b->n + len + 1 > b->cap) {
    size_t nc = b->cap ? b->cap : 256;
    char *p;
    while (nc < b->n + len + 1) {
      if (nc > (size_t)-1 / 2) return -1;
      nc *= 2;
    }
    p = realloc(b->d, nc);
    if (!p) return -1;
    b->d = p;
    b->cap = nc;
  }
  if (len > 0) memcpy(b->d + b->n, s, len);
  b->n += len;
  b->d[b->n] = '\0';
  return 0;
}

static int eb_ch(ebuf *b, char c) { return eb_put(b, &c, 1); }

/* ---- file I/O helpers --------------------------------------------------- */

static int read_raw(const char *path, char **out, size_t *out_n, mode_t *mode,
                    char *emsg, size_t emsg_sz) {
  int fd;
  struct stat st;
  char *buf;
  size_t n = 0, want;
  fd = open(path, O_RDONLY);
  if (fd < 0) {
    snprintf(emsg, emsg_sz, "cannot open '%s': %s", path, strerror(errno));
    return -1;
  }
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    snprintf(emsg, emsg_sz, "'%s' is not a regular file", path);
    close(fd);
    return -1;
  }
  want = (size_t)st.st_size;
  buf = malloc(want + 1);
  if (!buf) {
    snprintf(emsg, emsg_sz, "out of memory");
    close(fd);
    return -1;
  }
  while (n < want) {
    ssize_t rd = read(fd, buf + n, want - n);
    if (rd < 0) {
      snprintf(emsg, emsg_sz, "cannot read '%s': %s", path, strerror(errno));
      close(fd);
      free(buf);
      return -1;
    }
    if (rd == 0) break;
    n += (size_t)rd;
  }
  close(fd);
  buf[n] = '\0';
  *out = buf;
  *out_n = n;
  if (mode) *mode = st.st_mode & 07777;
  return 0;
}

/* Normalize to LF; *crlf reports whether CRLF dominates bare LF. */
static char *normalize_lf(const char *raw, size_t n, size_t *out_n,
                          int *crlf) {
  size_t crlf_c = 0, lf_c = 0, i, o = 0;
  char *buf;
  for (i = 0; i < n; i++) {
    if (raw[i] == '\n') {
      if (i > 0 && raw[i - 1] == '\r')
        crlf_c++;
      else
        lf_c++;
    }
  }
  *crlf = crlf_c > lf_c;
  buf = malloc(n + 1);
  if (!buf) return NULL;
  for (i = 0; i < n; i++) {
    if (raw[i] == '\r' && i + 1 < n && raw[i + 1] == '\n') continue;
    buf[o++] = raw[i];
  }
  buf[o] = '\0';
  *out_n = o;
  return buf;
}

static char *denorm(const char *s, size_t n, int crlf, size_t *out_n) {
  size_t extra = 0, i, o = 0;
  char *buf;
  if (crlf) {
    for (i = 0; i < n; i++)
      if (s[i] == '\n') extra++;
  }
  buf = malloc(n + extra + 1);
  if (!buf) return NULL;
  for (i = 0; i < n; i++) {
    if (crlf && s[i] == '\n') buf[o++] = '\r';
    buf[o++] = s[i];
  }
  buf[o] = '\0';
  *out_n = o;
  return buf;
}

static int write_atomic(const char *path, const char *data, size_t n,
                        mode_t mode, char *emsg, size_t emsg_sz) {
  size_t pl = strlen(path);
  char *tmp = malloc(pl + 32);
  int fd;
  size_t off = 0;
  if (!tmp) {
    snprintf(emsg, emsg_sz, "out of memory");
    return -1;
  }
  snprintf(tmp, pl + 32, "%s.astools-tmp-%ld", path, (long)getpid());
  unlink(tmp); /* leftover from a crashed run */
  fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, mode);
  if (fd < 0) {
    snprintf(emsg, emsg_sz, "cannot create '%s': %s", tmp, strerror(errno));
    free(tmp);
    return -1;
  }
  while (off < n) {
    ssize_t wr = write(fd, data + off, n - off);
    if (wr < 0) {
      if (errno == EINTR) continue;
      snprintf(emsg, emsg_sz, "cannot write '%s': %s", tmp, strerror(errno));
      close(fd);
      unlink(tmp);
      free(tmp);
      return -1;
    }
    off += (size_t)wr;
  }
  if (fsync(fd) != 0 || close(fd) != 0) {
    snprintf(emsg, emsg_sz, "cannot flush '%s': %s", tmp, strerror(errno));
    unlink(tmp);
    free(tmp);
    return -1;
  }
  if (rename(tmp, path) != 0) {
    snprintf(emsg, emsg_sz, "cannot replace '%s': %s", path, strerror(errno));
    unlink(tmp);
    free(tmp);
    return -1;
  }
  free(tmp);
  return 0;
}

/* ---- line spans --------------------------------------------------------- */

typedef struct {
  size_t off, len;
} espan;

/* Segments before each '\n'; an unterminated trailing segment is a line,
 * the empty segment after a final '\n' is not. */
static espan *split_spans(const char *buf, size_t n, size_t *count) {
  size_t nl = 0, i, start = 0;
  espan *ls;
  for (i = 0; i < n; i++)
    if (buf[i] == '\n') nl++;
  if (n > 0 && buf[n - 1] != '\n') nl++;
  *count = nl;
  ls = malloc((nl ? nl : 1) * sizeof *ls);
  if (!ls) return NULL;
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

/* ---- replace ------------------------------------------------------------ */

static int64_t count_literal(const char *h, size_t n, const char *pat,
                             size_t m) {
  int64_t c = 0;
  size_t i = 0;
  while (i + m <= n) {
    if (memcmp(h + i, pat, m) == 0) {
      c++;
      i += m;
    } else {
      i++;
    }
  }
  return c;
}

static int replace_literal(const char *h, size_t n, const char *pat, size_t m,
                           const char *rep, size_t rl, ebuf *out) {
  size_t i = 0;
  while (i < n) {
    if (i + m <= n && memcmp(h + i, pat, m) == 0) {
      if (eb_put(out, rep, rl) != 0) return -1;
      i += m;
    } else {
      if (eb_ch(out, h[i]) != 0) return -1;
      i++;
    }
  }
  return 0;
}

/* Count regex matches in one line under the sed rule: an empty match
 * immediately after a previous match does not count. */
static int64_t re_count_line(const astd_regex *re, const char *s, size_t len) {
  int64_t c = 0;
  size_t off = 0, prev_end = 0;
  int have_prev = 0;
  while (off <= len) {
    size_t ms, me, as, ae;
    if (!astd_regex_search(re, s + off, len - off, &ms, &me)) break;
    as = off + ms;
    ae = off + me;
    if (ms == me && have_prev && as == prev_end) {
      off = as + 1;
      continue;
    }
    c++;
    have_prev = 1;
    prev_end = ae;
    off = (me > ms) ? ae : as + 1;
  }
  return c;
}

static int re_replace_line(const astd_regex *re, const char *s, size_t len,
                           const char *rep, size_t rl, ebuf *out) {
  size_t off = 0, prev_end = 0;
  int have_prev = 0;
  while (off <= len) {
    size_t ms, me, as, ae;
    if (!astd_regex_search(re, s + off, len - off, &ms, &me)) {
      if (off < len && eb_put(out, s + off, len - off) != 0) return -1;
      return 0;
    }
    as = off + ms;
    ae = off + me;
    if (as > off && eb_put(out, s + off, as - off) != 0) return -1;
    if (ms == me && have_prev && as == prev_end) {
      if (as < len && eb_ch(out, s[as]) != 0) return -1;
      off = as + 1;
      continue;
    }
    if (eb_put(out, rep, rl) != 0) return -1;
    have_prev = 1;
    prev_end = ae;
    if (me > ms) {
      off = ae;
    } else {
      if (as < len && eb_ch(out, s[as]) != 0) return -1;
      off = as + 1;
    }
  }
  return 0;
}

static void cmd_replace(astd_req *r) {
  const char *path = astd_arg_str(r, "path", NULL);
  const char *find = astd_arg_str(r, "find", NULL);
  const char *rep = astd_arg_str(r, "replace_with", "");
  int all = astd_arg_bool(r, "all", 0);
  int use_re = astd_arg_bool(r, "regex", 0);
  char emsg[512];
  char *raw = NULL, *norm = NULL, *outraw = NULL;
  size_t rawn = 0, nn = 0, outn = 0, flen, rl;
  mode_t mode = 0644;
  int crlf = 0;
  int64_t total = 0;
  astd_regex *re = NULL;
  ebuf out;
  out.d = NULL;
  out.n = out.cap = 0;
  if (!path || !find || find[0] == '\0') {
    astd_fail(r, "astools/invalid-args",
              "replace requires 'path' and a non-empty 'find'");
    return;
  }
  flen = strlen(find);
  rl = strlen(rep);
  if (read_raw(path, &raw, &rawn, &mode, emsg, sizeof emsg) != 0) {
    astd_fail(r, "edit/io", "%s", emsg);
    return;
  }
  if (astd_looks_binary(raw, rawn)) {
    free(raw);
    astd_fail(r, "edit/binary", "'%s' looks binary (NUL in first 4KiB)",
              path);
    return;
  }
  norm = normalize_lf(raw, rawn, &nn, &crlf);
  free(raw);
  raw = NULL;
  if (!norm) {
    astd_fail(r, "edit/io", "out of memory");
    return;
  }
  if (use_re) {
    const char *rerr = NULL;
    re = astd_regex_compile(find, 1, &rerr);
    if (!re) {
      free(norm);
      astd_fail(r, "edit/bad-pattern", "%s", rerr ? rerr : "invalid pattern");
      return;
    }
    {
      size_t off = 0;
      while (off < nn) {
        char *nlp = memchr(norm + off, '\n', nn - off);
        size_t end = nlp ? (size_t)(nlp - norm) : nn;
        total += re_count_line(re, norm + off, end - off);
        off = nlp ? end + 1 : end;
      }
    }
  } else {
    total = count_literal(norm, nn, find, flen);
  }
  if (!all && total == 0) {
    free(norm);
    astd_regex_free(re);
    astd_fail(r, "edit/not-found", "text not found in '%s'", path);
    return;
  }
  if (!all && total > 1) {
    free(norm);
    astd_regex_free(re);
    astd_fail(r, "edit/ambiguous",
              "found %lld occurrences; pass all:true or a more specific find",
              (long long)total);
    return;
  }
  if (total == 0) {
    /* all:true with nothing to do: report zero replacements, leave the
     * file untouched */
    xcdn_value_t *res = xcdn_value_object();
    free(norm);
    astd_regex_free(re);
    if (!res || astd_set_int(res, "replacements", 0) != 0 ||
        astd_set_int(res, "size", (int64_t)rawn) != 0) {
      if (res) xcdn_value_free(res);
      astd_fail(r, "edit/io", "out of memory");
      return;
    }
    astd_ok(r, res);
    return;
  }
  {
    int rc;
    if (use_re) {
      size_t off = 0;
      rc = 0;
      while (off < nn && rc == 0) {
        char *nlp = memchr(norm + off, '\n', nn - off);
        size_t end = nlp ? (size_t)(nlp - norm) : nn;
        rc = re_replace_line(re, norm + off, end - off, rep, rl, &out);
        if (rc == 0 && nlp) rc = eb_ch(&out, '\n');
        off = nlp ? end + 1 : end;
      }
    } else {
      rc = replace_literal(norm, nn, find, flen, rep, rl, &out);
    }
    free(norm);
    astd_regex_free(re);
    if (rc != 0) {
      free(out.d);
      astd_fail(r, "edit/io", "out of memory");
      return;
    }
  }
  outraw = denorm(out.d ? out.d : "", out.n, crlf, &outn);
  free(out.d);
  if (!outraw) {
    astd_fail(r, "edit/io", "out of memory");
    return;
  }
  if (write_atomic(path, outraw, outn, mode, emsg, sizeof emsg) != 0) {
    free(outraw);
    astd_fail(r, "edit/io", "%s", emsg);
    return;
  }
  free(outraw);
  {
    xcdn_value_t *res = xcdn_value_object();
    if (!res || astd_set_int(res, "replacements", total) != 0 ||
        astd_set_int(res, "size", (int64_t)outn) != 0) {
      if (res) xcdn_value_free(res);
      astd_fail(r, "edit/io", "out of memory");
      return;
    }
    astd_ok(r, res);
  }
}

/* ---- insert ------------------------------------------------------------- */

static void cmd_insert(astd_req *r) {
  const char *path = astd_arg_str(r, "path", NULL);
  const char *content = astd_arg_str(r, "content", NULL);
  int64_t line = astd_arg_int(r, "line", 0);
  char emsg[512];
  char *raw = NULL, *norm = NULL, *cnorm = NULL, *outraw = NULL;
  size_t rawn = 0, nn = 0, cn = 0, outn = 0;
  mode_t mode = 0644;
  int crlf = 0, cdummy = 0;
  espan *ls = NULL, *cs = NULL;
  size_t nls = 0, ncs = 0, N, k, total;
  int had_nl, ins_nl, trailing;
  ebuf out;
  out.d = NULL;
  out.n = out.cap = 0;
  if (!path || !content) {
    astd_fail(r, "astools/invalid-args",
              "insert requires 'path' and 'content'");
    return;
  }
  if (line < 0) line = 0;
  if (read_raw(path, &raw, &rawn, &mode, emsg, sizeof emsg) != 0) {
    astd_fail(r, "edit/io", "%s", emsg);
    return;
  }
  if (astd_looks_binary(raw, rawn)) {
    free(raw);
    astd_fail(r, "edit/binary", "'%s' looks binary (NUL in first 4KiB)",
              path);
    return;
  }
  norm = normalize_lf(raw, rawn, &nn, &crlf);
  free(raw);
  cnorm = normalize_lf(content, strlen(content), &cn, &cdummy);
  if (!norm || !cnorm) goto oom;
  ls = split_spans(norm, nn, &nls);
  cs = split_spans(cnorm, cn, &ncs);
  if (!ls || !cs) goto oom;
  N = (line > (int64_t)nls) ? nls : (size_t)line;
  had_nl = nn > 0 && norm[nn - 1] == '\n';
  ins_nl = cn > 0 && cnorm[cn - 1] == '\n';
  /* an empty original file takes its trailing-newline convention from
   * the inserted content */
  trailing = (nn > 0) ? had_nl : ins_nl;
  total = nls + ncs;
  for (k = 0; k < total; k++) {
    const char *p;
    size_t len;
    if (k < N) {
      p = norm + ls[k].off;
      len = ls[k].len;
    } else if (k < N + ncs) {
      p = cnorm + cs[k - N].off;
      len = cs[k - N].len;
    } else {
      p = norm + ls[k - ncs].off;
      len = ls[k - ncs].len;
    }
    if (eb_put(&out, p, len) != 0) goto oom;
    if ((k + 1 < total || trailing) && eb_ch(&out, '\n') != 0) goto oom;
  }
  outraw = denorm(out.d ? out.d : "", out.n, crlf, &outn);
  if (!outraw) goto oom;
  free(out.d);
  out.d = NULL;
  free(norm);
  free(cnorm);
  free(ls);
  free(cs);
  if (write_atomic(path, outraw, outn, mode, emsg, sizeof emsg) != 0) {
    free(outraw);
    astd_fail(r, "edit/io", "%s", emsg);
    return;
  }
  free(outraw);
  {
    xcdn_value_t *res = xcdn_value_object();
    if (!res || astd_set_int(res, "size", (int64_t)outn) != 0) {
      if (res) xcdn_value_free(res);
      astd_fail(r, "edit/io", "out of memory");
      return;
    }
    astd_ok(r, res);
  }
  return;
oom:
  free(out.d);
  free(norm);
  free(cnorm);
  free(ls);
  free(cs);
  astd_fail(r, "edit/io", "out of memory");
}

/* ---- patch -------------------------------------------------------------- */

typedef struct {
  long a, b, c, d;
  size_t body_start, body_len; /* indices into the patch line array */
} phunk;

typedef struct {
  char *abs;
  char *raw_orig; /* original raw bytes for rollback (NULL if new file) */
  size_t raw_orig_n;
  char *cur; /* normalized working content */
  size_t cur_n;
  int existed;
  int crlf;
  int is_delete;
  int no_final_nl;
  mode_t mode;
  int written;
  int64_t hunks;
} ptgt;

static int starts_with(const char *s, const char *pfx) {
  return strncmp(s, pfx, strlen(pfx)) == 0;
}

static char *path_field(const char *s) {
  const char *e = strchr(s, '\t');
  size_t len = e ? (size_t)(e - s) : strlen(s);
  char *p = malloc(len + 1);
  if (!p) return NULL;
  memcpy(p, s, len);
  p[len] = '\0';
  return p;
}

static const char *strip_and_check(const char *p, int64_t strip,
                                   const char **why) {
  int64_t k;
  const char *q = p;
  for (k = 0; k < strip; k++) {
    const char *slash = strchr(q, '/');
    if (!slash) {
      *why = "not enough leading components to strip";
      return NULL;
    }
    q = slash + 1;
  }
  if (*q == '\0') {
    *why = "empty path after strip";
    return NULL;
  }
  if (*q == '/') {
    *why = "absolute path is outside the workspace";
    return NULL;
  }
  {
    const char *s = q;
    while (*s) {
      const char *e = strchr(s, '/');
      size_t l = e ? (size_t)(e - s) : strlen(s);
      if (l == 2 && s[0] == '.' && s[1] == '.') {
        *why = "path escapes outside the workspace";
        return NULL;
      }
      if (!e) break;
      s = e + 1;
    }
  }
  return q;
}

static int parse_hunk_header(const char *s, phunk *h) {
  char *e;
  if (!starts_with(s, "@@ -")) return -1;
  s += 4;
  h->a = strtol(s, &e, 10);
  if (e == s || h->a < 0) return -1;
  s = e;
  if (*s == ',') {
    s++;
    h->b = strtol(s, &e, 10);
    if (e == s || h->b < 0) return -1;
    s = e;
  } else {
    h->b = 1;
  }
  if (s[0] != ' ' || s[1] != '+') return -1;
  s += 2;
  h->c = strtol(s, &e, 10);
  if (e == s || h->c < 0) return -1;
  s = e;
  if (*s == ',') {
    s++;
    h->d = strtol(s, &e, 10);
    if (e == s || h->d < 0) return -1;
    s = e;
  } else {
    h->d = 1;
  }
  while (*s == ' ') s++;
  if (s[0] != '@' || s[1] != '@') return -1;
  return 0;
}

static void tgts_free(ptgt *tg, size_t n) {
  size_t i;
  for (i = 0; i < n; i++) {
    free(tg[i].abs);
    free(tg[i].raw_orig);
    free(tg[i].cur);
  }
  free(tg);
}

/* 1 if path lies under prefix (component boundary aware); both absolute. */
static int under_prefix(const char *path, const char *prefix) {
  size_t pl = strlen(prefix);
  while (pl > 1 && prefix[pl - 1] == '/') pl--; /* ignore trailing slash */
  if (strncmp(path, prefix, pl) != 0) return 0;
  return path[pl] == '\0' || path[pl] == '/' || (pl == 1 && prefix[0] == '/');
}

/* Symlink-safe containment (SPEC §8.5 / §6.4): the patch target path lives
 * inside the diff text, so the runtime pre-flight never saw it. Resolve it
 * through the kernel (deepest existing ancestor + verbatim remainder) and
 * require the real path to stay under the workspace root, closing the
 * symlink-escape hole that lexical checks alone leave open at level basic.
 * Returns a malloc'd canonical path, or NULL with *why set on escape/OOM. */
static char *contain_target(const char *abs, const char *workspace,
                            const char **why) {
  char *head = strdup(abs);
  char *tail = NULL; /* accumulated non-existing suffix, leading '/' */
  char *canon = NULL, *result = NULL;
  if (!head) {
    *why = "out of memory";
    return NULL;
  }
  for (;;) {
    char *slash;
    canon = realpath(head, NULL);
    if (canon) break;
    slash = strrchr(head, '/');
    if (!slash || slash == head) {
      /* nothing left to strip: resolve the root itself */
      canon = realpath("/", NULL);
      if (!canon) {
        *why = "cannot resolve path";
        goto done;
      }
      break;
    }
    {
      size_t comp = strlen(slash); /* "/name..." */
      size_t tl = tail ? strlen(tail) : 0;
      char *nt = malloc(comp + tl + 1);
      if (!nt) {
        *why = "out of memory";
        goto done;
      }
      memcpy(nt, slash, comp);
      if (tail) memcpy(nt + comp, tail, tl);
      nt[comp + tl] = '\0';
      free(tail);
      tail = nt;
      *slash = '\0'; /* strip the component from head */
    }
  }
  /* canon = real ancestor; append the unresolved remainder verbatim */
  {
    size_t cl = strlen(canon), tl = tail ? strlen(tail) : 0;
    result = malloc(cl + tl + 1);
    if (!result) {
      *why = "out of memory";
      goto done;
    }
    memcpy(result, canon, cl);
    if (tail) memcpy(result + cl, tail, tl);
    result[cl + tl] = '\0';
  }
  if (!under_prefix(result, workspace)) {
    free(result);
    result = NULL;
    *why = "path escapes the workspace (symlink)";
  }
done:
  free(head);
  free(tail);
  free(canon);
  return result;
}

static void mkdirs_parent(const char *abs) {
  char *tmp = malloc(strlen(abs) + 1);
  char *p;
  if (!tmp) return;
  memcpy(tmp, abs, strlen(abs) + 1);
  for (p = tmp + 1; *p; p++) {
    if (*p == '/') {
      *p = '\0';
      mkdir(tmp, 0755); /* EEXIST and friends are fine; open will judge */
      *p = '/';
    }
  }
  free(tmp);
}

/* Apply one file section's hunks to t->cur. Returns 0 on success; on
 * failure writes a message and returns -1. hunk_no counts globally. */
static int apply_section(ptgt *t, char **plines, const phunk *hs, size_t nh,
                         int64_t *hunk_no, char *emsg, size_t emsg_sz) {
  espan *sp = NULL;
  size_t nlines = 0, old_idx = 0, i, j;
  const char **np = NULL; /* new file: line pointers */
  size_t *nl = NULL;      /* new file: line lengths */
  size_t nn = 0, ncap = 0;
  int body_no_nl = 0;
  int cur_had_nl = t->cur_n > 0 && t->cur[t->cur_n - 1] == '\n';
  size_t tail_count;
  sp = split_spans(t->cur, t->cur_n, &nlines);
  if (!sp) {
    snprintf(emsg, emsg_sz, "out of memory");
    return -1;
  }
#define PUSH_LINE(P, L)                                                     \
  do {                                                                      \
    if (nn == ncap) {                                                       \
      size_t nc2 = ncap ? ncap * 2 : 64;                                    \
      const char **pp = realloc(np, nc2 * sizeof *np);                      \
      size_t *ll = pp ? realloc(nl, nc2 * sizeof *nl) : NULL;               \
      if (!pp || !ll) {                                                     \
        free(pp ? (void *)pp : (void *)np);                                 \
        np = NULL;                                                          \
        free(ll ? ll : nl);                                                 \
        nl = NULL;                                                          \
        free(sp);                                                           \
        snprintf(emsg, emsg_sz, "out of memory");                           \
        return -1;                                                          \
      }                                                                     \
      np = pp;                                                              \
      nl = ll;                                                              \
      ncap = nc2;                                                           \
    }                                                                       \
    np[nn] = (P);                                                           \
    nl[nn] = (L);                                                           \
    nn++;                                                                   \
  } while (0)
  for (i = 0; i < nh; i++) {
    const phunk *h = &hs[i];
    size_t start;
    char prev_kind = 0;
    (*hunk_no)++;
    if (h->b > 0) {
      if (h->a < 1) {
        snprintf(emsg, emsg_sz, "hunk #%lld is malformed",
                 (long long)*hunk_no);
        goto bad;
      }
      start = (size_t)h->a - 1;
    } else {
      start = (size_t)h->a; /* pure insertion goes after old line a */
    }
    if (start < old_idx || start > nlines) {
      snprintf(emsg, emsg_sz, "hunk #%lld does not apply",
               (long long)*hunk_no);
      goto bad;
    }
    while (old_idx < start) {
      PUSH_LINE(t->cur + sp[old_idx].off, sp[old_idx].len);
      old_idx++;
    }
    for (j = 0; j < h->body_len; j++) {
      const char *bl = plines[h->body_start + j];
      char c0 = bl[0];
      const char *text;
      size_t tlen;
      char kind;
      if (c0 == '\\') {
        /* "\ No newline at end of file": relevant when it follows a
         * new-side line (git only emits it at the very end of a file) */
        if (prev_kind == '+' || prev_kind == ' ') body_no_nl = 1;
        continue;
      }
      kind = (c0 == '\0') ? ' ' : c0;
      text = (c0 == '\0') ? bl : bl + 1;
      tlen = strlen(text);
      if (kind == ' ' || kind == '-') {
        if (old_idx >= nlines || sp[old_idx].len != tlen ||
            (tlen > 0 && memcmp(t->cur + sp[old_idx].off, text, tlen) != 0)) {
          snprintf(emsg, emsg_sz,
                   "hunk #%lld does not apply (mismatch at line %lu)",
                   (long long)*hunk_no, (unsigned long)(old_idx + 1));
          goto bad;
        }
        if (kind == ' ') PUSH_LINE(t->cur + sp[old_idx].off, sp[old_idx].len);
        old_idx++;
      } else if (kind == '+') {
        PUSH_LINE(text, tlen);
      } else {
        snprintf(emsg, emsg_sz, "hunk #%lld contains a malformed line",
                 (long long)*hunk_no);
        goto bad;
      }
      prev_kind = kind;
    }
    t->hunks++;
  }
  tail_count = nlines - old_idx;
  while (old_idx < nlines) {
    PUSH_LINE(t->cur + sp[old_idx].off, sp[old_idx].len);
    old_idx++;
  }
#undef PUSH_LINE
  {
    char *nc;
    size_t total = 0, o = 0;
    int final_nl;
    if (nn == 0) {
      nc = malloc(1);
      if (!nc) goto oom;
      nc[0] = '\0';
      free(t->cur);
      t->cur = nc;
      t->cur_n = 0;
      t->no_final_nl = 0;
    } else {
      final_nl = tail_count > 0 ? cur_had_nl : !body_no_nl;
      for (i = 0; i < nn; i++) total += nl[i] + 1;
      if (!final_nl) total--;
      nc = malloc(total + 1);
      if (!nc) goto oom;
      for (i = 0; i < nn; i++) {
        memcpy(nc + o, np[i], nl[i]);
        o += nl[i];
        if (i + 1 < nn || final_nl) nc[o++] = '\n';
      }
      nc[o] = '\0';
      free(t->cur);
      t->cur = nc;
      t->cur_n = o;
      t->no_final_nl = !final_nl;
    }
  }
  free(sp);
  free(np);
  free(nl);
  return 0;
oom:
  snprintf(emsg, emsg_sz, "out of memory");
bad:
  free(sp);
  free(np);
  free(nl);
  return -1;
}

static void cmd_patch(astd_req *r) {
  const char *ptext = astd_arg_str(r, "patch", NULL);
  int64_t strip = astd_arg_int(r, "strip", 1);
  char emsg[512];
  const char *fail_code = NULL;
  char *copy = NULL;
  char **plines = NULL;
  size_t np = 0, i;
  ptgt *tg = NULL;
  size_t ntg = 0, tgcap = 0;
  phunk *hs = NULL;
  size_t nh = 0, hcap = 0;
  int64_t hunk_no = 0, hunks_applied = 0;
  int sections = 0;
  size_t li;
  if (strip < 0) strip = 0;
  if (!ptext || ptext[0] == '\0') {
    astd_fail(r, "astools/invalid-args", "patch text must not be empty");
    return;
  }
  if (!r->workspace) {
    astd_fail(r, "astools/protocol", "request carries no workspace root");
    return;
  }
  /* split a private copy of the patch into lines, trimming CR */
  {
    size_t n = strlen(ptext), nl = 1, o = 0;
    copy = malloc(n + 1);
    if (!copy) goto oom;
    memcpy(copy, ptext, n + 1);
    for (i = 0; i < n; i++)
      if (copy[i] == '\n') nl++;
    plines = malloc(nl * sizeof *plines);
    if (!plines) goto oom;
    plines[np++] = copy;
    for (i = 0; i < n; i++) {
      if (copy[i] == '\n') {
        copy[i] = '\0';
        if (i > o && copy[i - 1] == '\r') copy[i - 1] = '\0';
        o = i + 1;
        if (o < n + 1) plines[np++] = copy + o;
      }
    }
    /* drop a trailing empty line produced by a final newline */
    if (np > 0 && plines[np - 1][0] == '\0') np--;
  }
  li = 0;
  while (li < np) {
    char *old_name = NULL, *new_name = NULL;
    int is_create, is_delete;
    const char *rel, *why = NULL, *eff;
    ptgt *t = NULL;
    if (!(starts_with(plines[li], "--- ") && li + 1 < np &&
          starts_with(plines[li + 1], "+++ "))) {
      li++;
      continue;
    }
    old_name = path_field(plines[li] + 4);
    new_name = path_field(plines[li + 1] + 4);
    if (!old_name || !new_name) {
      free(old_name);
      free(new_name);
      goto oom;
    }
    li += 2;
    sections++;
    is_create = strcmp(old_name, "/dev/null") == 0;
    is_delete = strcmp(new_name, "/dev/null") == 0;
    if (is_create && is_delete) {
      free(old_name);
      free(new_name);
      fail_code = "edit/patch-failed";
      snprintf(emsg, sizeof emsg, "patch has /dev/null on both sides");
      goto fail;
    }
    eff = is_delete ? old_name : new_name;
    rel = strip_and_check(eff, strip, &why);
    if (!rel) {
      fail_code = "edit/patch-failed";
      snprintf(emsg, sizeof emsg, "patch path '%s' rejected: %s", eff,
               why ? why : "invalid");
      free(old_name);
      free(new_name);
      goto fail;
    }
    /* find or load the target */
    {
      size_t wl = strlen(r->workspace), reln = strlen(rel);
      char *abs = malloc(wl + reln + 2);
      size_t k;
      if (!abs) {
        free(old_name);
        free(new_name);
        goto oom;
      }
      memcpy(abs, r->workspace, wl);
      abs[wl] = '/';
      memcpy(abs + wl + 1, rel, reln + 1);
      /* §8.5: run the diff target through the §6.4 pipeline ourselves —
       * resolve symlinks and confirm containment before any write. */
      {
        const char *why = NULL;
        char *canon = contain_target(abs, r->workspace, &why);
        if (!canon) {
          fail_code = "edit/patch-failed";
          snprintf(emsg, sizeof emsg, "patch path '%s' rejected: %s", rel,
                   why ? why : "outside workspace");
          free(abs);
          free(old_name);
          free(new_name);
          goto fail;
        }
        free(abs);
        abs = canon;
      }
      for (k = 0; k < ntg; k++) {
        if (strcmp(tg[k].abs, abs) == 0) {
          t = &tg[k];
          break;
        }
      }
      if (t && is_create) {
        fail_code = "edit/patch-failed";
        snprintf(emsg, sizeof emsg, "cannot create '%s': already patched",
                 rel);
        free(abs);
        free(old_name);
        free(new_name);
        goto fail;
      }
      if (!t) {
        struct stat st;
        if (ntg == tgcap) {
          size_t nc = tgcap ? tgcap * 2 : 8;
          ptgt *p = realloc(tg, nc * sizeof *tg);
          if (!p) {
            free(abs);
            free(old_name);
            free(new_name);
            goto oom;
          }
          tg = p;
          tgcap = nc;
        }
        t = &tg[ntg];
        memset(t, 0, sizeof *t);
        t->abs = abs;
        abs = NULL;
        t->mode = 0644;
        if (is_create) {
          if (lstat(t->abs, &st) == 0) {
            fail_code = "edit/patch-failed";
            snprintf(emsg, sizeof emsg, "cannot create '%s': file exists",
                     rel);
            free(t->abs);
            free(old_name);
            free(new_name);
            goto fail;
          }
          t->cur = malloc(1);
          if (!t->cur) {
            free(t->abs);
            free(old_name);
            free(new_name);
            goto oom;
          }
          t->cur[0] = '\0';
          t->cur_n = 0;
          t->existed = 0;
        } else {
          char rmsg[256];
          if (read_raw(t->abs, &t->raw_orig, &t->raw_orig_n, &t->mode, rmsg,
                       sizeof rmsg) != 0) {
            fail_code = "edit/patch-failed";
            snprintf(emsg, sizeof emsg, "%s", rmsg);
            free(t->abs);
            free(old_name);
            free(new_name);
            goto fail;
          }
          if (astd_looks_binary(t->raw_orig, t->raw_orig_n)) {
            fail_code = "edit/binary";
            snprintf(emsg, sizeof emsg,
                     "'%s' looks binary (NUL in first 4KiB)", rel);
            free(t->abs);
            free(t->raw_orig);
            free(old_name);
            free(new_name);
            goto fail;
          }
          t->cur = normalize_lf(t->raw_orig, t->raw_orig_n, &t->cur_n,
                                &t->crlf);
          if (!t->cur) {
            free(t->abs);
            free(t->raw_orig);
            free(old_name);
            free(new_name);
            goto oom;
          }
          t->existed = 1;
        }
        ntg++;
      } else {
        free(abs);
      }
    }
    free(old_name);
    free(new_name);
    old_name = new_name = NULL;
    /* collect this section's hunks */
    nh = 0;
    while (li < np && starts_with(plines[li], "@@ -")) {
      phunk h;
      long need_old, need_new;
      if (parse_hunk_header(plines[li], &h) != 0) {
        fail_code = "edit/patch-failed";
        snprintf(emsg, sizeof emsg, "malformed hunk header: %.80s",
                 plines[li]);
        goto fail;
      }
      li++;
      h.body_start = li;
      need_old = h.b;
      need_new = h.d;
      while ((need_old > 0 || need_new > 0) && li < np) {
        char c0 = plines[li][0];
        if (c0 == '\\') {
          /* no-newline marker */
        } else if (c0 == ' ' || c0 == '\0') {
          need_old--;
          need_new--;
        } else if (c0 == '-') {
          need_old--;
        } else if (c0 == '+') {
          need_new--;
        } else {
          break;
        }
        li++;
      }
      if (need_old > 0 || need_new > 0) {
        fail_code = "edit/patch-failed";
        snprintf(emsg, sizeof emsg, "hunk #%lld is truncated",
                 (long long)(hunk_no + (int64_t)nh + 1));
        goto fail;
      }
      if (li < np && plines[li][0] == '\\') li++;
      h.body_len = li - h.body_start;
      if (nh == hcap) {
        size_t nc = hcap ? hcap * 2 : 8;
        phunk *p = realloc(hs, nc * sizeof *hs);
        if (!p) goto oom;
        hs = p;
        hcap = nc;
      }
      hs[nh++] = h;
    }
    if (apply_section(t, plines, hs, nh, &hunk_no, emsg, sizeof emsg) != 0) {
      fail_code = strcmp(emsg, "out of memory") == 0 ? "edit/io"
                                                     : "edit/patch-failed";
      goto fail;
    }
    hunks_applied += (int64_t)nh;
    t->is_delete = is_delete;
    if (is_delete && t->cur_n != 0) {
      fail_code = "edit/patch-failed";
      snprintf(emsg, sizeof emsg, "deletion patch leaves content behind");
      goto fail;
    }
  }
  if (sections == 0) {
    fail_code = "edit/patch-failed";
    snprintf(emsg, sizeof emsg, "no '---' / '+++' file headers found");
    goto fail;
  }
  /* every hunk applied in memory: write everything out */
  for (i = 0; i < ntg; i++) {
    ptgt *t = &tg[i];
    if (t->is_delete) {
      if (unlink(t->abs) != 0) {
        fail_code = "edit/io";
        snprintf(emsg, sizeof emsg, "cannot remove '%s': %s", t->abs,
                 strerror(errno));
        goto rollback;
      }
      t->written = 1;
    } else {
      char *rawout;
      size_t rawn;
      char wmsg[256];
      rawout = denorm(t->cur, t->cur_n, t->crlf, &rawn);
      if (!rawout) {
        fail_code = "edit/io";
        snprintf(emsg, sizeof emsg, "out of memory");
        goto rollback;
      }
      if (!t->existed) mkdirs_parent(t->abs);
      if (write_atomic(t->abs, rawout, rawn, t->mode, wmsg, sizeof wmsg) !=
          0) {
        free(rawout);
        fail_code = "edit/io";
        snprintf(emsg, sizeof emsg, "%s", wmsg);
        goto rollback;
      }
      free(rawout);
      t->written = 1;
    }
  }
  {
    /* everything is on disk; a response-building failure must not roll
     * the files back (the edit did happen), so report it as edit/io */
    xcdn_value_t *res = xcdn_value_object();
    int rc = 0;
    if (res) {
      rc |= astd_set_int(res, "files_changed", (int64_t)ntg);
      rc |= astd_set_int(res, "hunks_applied", hunks_applied);
    }
    if (!res || rc != 0) {
      if (res) xcdn_value_free(res);
      free(copy);
      free(plines);
      free(hs);
      tgts_free(tg, ntg);
      astd_fail(r, "edit/io", "patch applied but result reporting failed");
      return;
    }
    free(copy);
    free(plines);
    free(hs);
    tgts_free(tg, ntg);
    astd_ok(r, res);
    return;
  }
oom:
  fail_code = "edit/io";
  snprintf(emsg, sizeof emsg, "out of memory");
  goto fail;
rollback:
  /* best-effort restore of files already written (all-or-nothing) */
  for (i = 0; i < ntg; i++) {
    ptgt *t = &tg[i];
    char wmsg[64];
    if (!t->written) continue;
    if (t->existed && t->raw_orig) {
      (void)write_atomic(t->abs, t->raw_orig, t->raw_orig_n, t->mode, wmsg,
                         sizeof wmsg);
    } else if (!t->existed) {
      (void)unlink(t->abs);
    }
  }
fail:
  free(copy);
  free(plines);
  free(hs);
  tgts_free(tg, ntg);
  astd_fail(r, fail_code ? fail_code : "edit/patch-failed", "%s", emsg);
}

/* ---- dispatch ----------------------------------------------------------- */

int astd_tool_edit(astd_req *r) {
  if (strcmp(r->command, "replace") == 0)
    cmd_replace(r);
  else if (strcmp(r->command, "insert") == 0)
    cmd_insert(r);
  else if (strcmp(r->command, "patch") == 0)
    cmd_patch(r);
  else
    astd_fail(r, "astools/protocol", "unknown edit command '%s'", r->command);
  return 0;
}

#endif /* POSIX */
