/*
 * tool_git.c — git wrapper tool.
 *
 * Wraps the system git found by iterating the (scrubbed) PATH; missing
 * binary means git/not-installed for every command. Always a constructed
 * argv, never a shell. Environment: inherited entries plus
 * GIT_TERMINAL_PROMPT=0, GIT_CONFIG_NOSYSTEM=1, HOME=<scratch> (so no
 * user gitconfig is read) and LC_ALL=C for stable message parsing.
 *
 * Refs are validated (check-ref-format-ish: nonempty, no leading '-',
 * no "..", no control chars, space, '~' '^' ':' '?' '*' '[' '\\', not
 * ending in '/', '.' or '.lock'; HEAD and hex hashes pass naturally)
 * before ever reaching an argv; invalid => git/bad-ref. Pathspecs go
 * after a literal "--". checkout uses "checkout <ref> --" so git cannot
 * read <ref> as a pathspec; with create:true the dirty-tree check is
 * skipped because "checkout -b <new> " from HEAD never discards local
 * changes (an existing branch makes git itself fail, which is relayed).
 *
 * Local-only by construction: only status/diff/log/show/add/commit/
 * checkout exist; no network subcommand is ever assembled.
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

int astd_tool_git(astd_req *r) {
  astd_fail(r, "std/unsupported", "git is not supported on Windows yet");
  return 0;
}

#else /* POSIX */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define GIT_STREAM_CAP 1048576

/* ---- child process runner (same shape as tool_proc, private copy) ------- */

typedef struct {
  int exit_code;
  char *out;
  size_t out_n;
  int out_trunc;
  char *err;
  size_t err_n;
  int err_trunc;
} gres;

static void gres_free(gres *g) {
  free(g->out);
  free(g->err);
  g->out = g->err = NULL;
}

static int cap_append(char **buf, size_t *n, size_t *cap, size_t limit,
                      int *trunc, const char *src, size_t sn) {
  size_t keep = 0;
  if (*n < limit) {
    keep = limit - *n;
    if (keep > sn) keep = sn;
  }
  if (keep > 0) {
    if (*n + keep + 1 > *cap) {
      size_t nc = *cap ? *cap : 4096;
      char *p;
      while (nc < *n + keep + 1) nc *= 2;
      p = realloc(*buf, nc);
      if (!p) return -1;
      *buf = p;
      *cap = nc;
    }
    memcpy(*buf + *n, src, keep);
    *n += keep;
    (*buf)[*n] = '\0';
  }
  if (keep < sn) *trunc = 1;
  return 0;
}

static void close_fd(int *fd) {
  if (*fd >= 0) {
    close(*fd);
    *fd = -1;
  }
}

static void child_report(int fd, char stage) {
  char msg[1 + sizeof(int)];
  int e = errno;
  msg[0] = stage;
  memcpy(msg + 1, &e, sizeof e);
  if (write(fd, msg, sizeof msg) < 0) { /* best effort */
  }
  _exit(127);
}

static int spawn_capture(char *const *argv, char *const *envp,
                         const char *cwd, gres *g, char *emsg,
                         size_t emsg_sz) {
  int outp[2] = {-1, -1}, errp[2] = {-1, -1}, exep[2] = {-1, -1};
  int devnull = -1;
  size_t ocap = 0, ecap = 0;
  pid_t pid;
  int st = 0;
  memset(g, 0, sizeof *g);
  devnull = open("/dev/null", O_RDONLY);
  if (devnull < 0 || pipe(outp) != 0 || pipe(errp) != 0 || pipe(exep) != 0) {
    snprintf(emsg, emsg_sz, "pipe: %s", strerror(errno));
    goto spawn_fail;
  }
  if (fcntl(exep[1], F_SETFD, FD_CLOEXEC) != 0) {
    snprintf(emsg, emsg_sz, "fcntl: %s", strerror(errno));
    goto spawn_fail;
  }
  pid = fork();
  if (pid < 0) {
    snprintf(emsg, emsg_sz, "fork: %s", strerror(errno));
    goto spawn_fail;
  }
  if (pid == 0) {
    if (dup2(devnull, 0) < 0 || dup2(outp[1], 1) < 0 || dup2(errp[1], 2) < 0)
      child_report(exep[1], 'p');
    close(devnull);
    close(outp[0]);
    close(outp[1]);
    close(errp[0]);
    close(errp[1]);
    close(exep[0]);
    if (cwd && chdir(cwd) != 0) child_report(exep[1], 'c');
    environ = (char **)envp;
    execv(argv[0], argv); /* argv[0] is the absolute git path */
    child_report(exep[1], 'x');
  }
  close_fd(&devnull);
  close_fd(&outp[1]);
  close_fd(&errp[1]);
  close_fd(&exep[1]);
  {
    char m[1 + sizeof(int)];
    ssize_t got = read(exep[0], m, sizeof m);
    close_fd(&exep[0]);
    if (got == (ssize_t)sizeof m) {
      int ce;
      memcpy(&ce, m + 1, sizeof ce);
      while (waitpid(pid, &st, 0) < 0 && errno == EINTR) continue;
      snprintf(emsg, emsg_sz, "cannot run git: %s", strerror(ce));
      goto spawn_fail;
    }
  }
  fcntl(outp[0], F_SETFL, O_NONBLOCK);
  fcntl(errp[0], F_SETFL, O_NONBLOCK);
  while (outp[0] >= 0 || errp[0] >= 0) {
    fd_set rf;
    int maxfd = -1, rc;
    FD_ZERO(&rf);
    if (outp[0] >= 0) {
      FD_SET(outp[0], &rf);
      if (outp[0] > maxfd) maxfd = outp[0];
    }
    if (errp[0] >= 0) {
      FD_SET(errp[0], &rf);
      if (errp[0] > maxfd) maxfd = errp[0];
    }
    rc = select(maxfd + 1, &rf, NULL, NULL, NULL);
    if (rc < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (outp[0] >= 0 && FD_ISSET(outp[0], &rf)) {
      char tmp[8192];
      ssize_t n = read(outp[0], tmp, sizeof tmp);
      if (n > 0) {
        if (cap_append(&g->out, &g->out_n, &ocap, GIT_STREAM_CAP,
                       &g->out_trunc, tmp, (size_t)n) != 0)
          close_fd(&outp[0]);
      } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
        close_fd(&outp[0]);
      }
    }
    if (errp[0] >= 0 && FD_ISSET(errp[0], &rf)) {
      char tmp[8192];
      ssize_t n = read(errp[0], tmp, sizeof tmp);
      if (n > 0) {
        if (cap_append(&g->err, &g->err_n, &ecap, GIT_STREAM_CAP,
                       &g->err_trunc, tmp, (size_t)n) != 0)
          close_fd(&errp[0]);
      } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
        close_fd(&errp[0]);
      }
    }
  }
  while (waitpid(pid, &st, 0) < 0 && errno == EINTR) continue;
  if (WIFEXITED(st))
    g->exit_code = WEXITSTATUS(st);
  else if (WIFSIGNALED(st))
    g->exit_code = -WTERMSIG(st);
  else
    g->exit_code = -1;
  if (!g->out) {
    g->out = malloc(1);
    if (g->out) g->out[0] = '\0';
  }
  if (!g->err) {
    g->err = malloc(1);
    if (g->err) g->err[0] = '\0';
  }
  if (!g->out || !g->err) {
    snprintf(emsg, emsg_sz, "out of memory");
    gres_free(g);
    return -1;
  }
  return 0;
spawn_fail:
  close_fd(&devnull);
  close_fd(&outp[0]);
  close_fd(&outp[1]);
  close_fd(&errp[0]);
  close_fd(&errp[1]);
  close_fd(&exep[0]);
  close_fd(&exep[1]);
  gres_free(g);
  return -1;
}

/* ---- git plumbing ------------------------------------------------------- */

typedef struct {
  astd_req *r;
  char *git;   /* absolute path of the git binary */
  char **envp; /* prepared child environment */
} gtool;

static char *find_git(void) {
  const char *path = getenv("PATH");
  const char *p;
  if (!path || !*path) return NULL;
  p = path;
  for (;;) {
    const char *q = strchr(p, ':');
    size_t dl = q ? (size_t)(q - p) : strlen(p);
    if (dl > 0) { /* empty component means cwd: never exec from there */
      char *cand = malloc(dl + 5);
      if (!cand) return NULL;
      memcpy(cand, p, dl);
      memcpy(cand + dl, "/git", 5);
      if (access(cand, X_OK) == 0) return cand;
      free(cand);
    }
    if (!q) break;
    p = q + 1;
  }
  return NULL;
}

static char **git_env(const astd_req *r) {
  const char *home = r->scratch ? r->scratch : r->workspace;
  const char *keys[4] = {"GIT_TERMINAL_PROMPT", "GIT_CONFIG_NOSYSTEM",
                         "LC_ALL", "HOME"};
  const char *vals[4];
  size_t base_n = 0, i, o = 0, nov = 0;
  char **out;
  vals[0] = "0";
  vals[1] = "1";
  vals[2] = "C";
  vals[3] = home; /* may be NULL: then HOME is simply not overridden */
  nov = home ? 4 : 3;
  while (environ[base_n]) base_n++;
  out = calloc(base_n + nov + 1, sizeof *out);
  if (!out) return NULL;
  for (i = 0; i < base_n; i++) {
    const char *e = environ[i];
    const char *eq = strchr(e, '=');
    size_t klen = eq ? (size_t)(eq - e) : strlen(e);
    size_t j;
    int skip = 0;
    for (j = 0; j < nov; j++) {
      if (strlen(keys[j]) == klen && strncmp(keys[j], e, klen) == 0) {
        skip = 1;
        break;
      }
    }
    if (skip) continue;
    out[o] = malloc(strlen(e) + 1);
    if (!out[o]) goto fail;
    memcpy(out[o], e, strlen(e) + 1);
    o++;
  }
  for (i = 0; i < nov; i++) {
    size_t kl = strlen(keys[i]), vl = strlen(vals[i]);
    out[o] = malloc(kl + vl + 2);
    if (!out[o]) goto fail;
    memcpy(out[o], keys[i], kl);
    out[o][kl] = '=';
    memcpy(out[o] + kl + 1, vals[i], vl + 1);
    o++;
  }
  out[o] = NULL;
  return out;
fail:
  for (i = 0; out[i]; i++) free(out[i]);
  free(out);
  return NULL;
}

static void free_env(char **e) {
  size_t i;
  if (!e) return;
  for (i = 0; e[i]; i++) free(e[i]);
  free(e);
}

/* Run git with the given argument vector (git path prepended). */
static int run_git(gtool *t, const char *const *args, size_t nargs, gres *g,
                   char *emsg, size_t emsg_sz) {
  char **argv = calloc(nargs + 2, sizeof *argv);
  size_t i;
  int rc;
  if (!argv) {
    snprintf(emsg, emsg_sz, "out of memory");
    memset(g, 0, sizeof *g);
    return -1;
  }
  argv[0] = t->git;
  for (i = 0; i < nargs; i++) argv[i + 1] = (char *)args[i];
  argv[nargs + 1] = NULL;
  rc = spawn_capture(argv, t->envp, t->r->workspace, g, emsg, emsg_sz);
  free(argv);
  return rc;
}

static void chomp(char *s) {
  size_t n;
  if (!s) return;
  n = strlen(s);
  while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = '\0';
}

/* Fail with an excerpt of git's stderr (or stdout as fallback). */
static void fail_git(astd_req *r, const char *code, const gres *g,
                     const char *fallback) {
  const char *src = (g && g->err && g->err[0]) ? g->err
                    : (g && g->out && g->out[0]) ? g->out
                                                 : fallback;
  size_t n = strlen(src);
  while (n > 0 &&
         (src[n - 1] == '\n' || src[n - 1] == '\r' || src[n - 1] == ' '))
    n--;
  if (n > 400) n = 400;
  astd_fail(r, code, "git: %.*s", (int)n, src);
}

static int ci_contains(const char *hay, const char *needle) {
  size_t nl = strlen(needle), i, j;
  if (nl == 0) return 1;
  for (i = 0; hay[i]; i++) {
    for (j = 0; j < nl && hay[i + j]; j++) {
      char a = hay[i + j], b = needle[j];
      if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
      if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
      if (a != b) break;
    }
    if (j == nl) return 1;
  }
  return 0;
}

/* check-ref-format-ish validation; HEAD and hex hashes pass naturally */
static int ref_valid(const char *ref) {
  size_t i, n;
  if (!ref || ref[0] == '\0') return 0;
  n = strlen(ref);
  if (n > 512) return 0;
  if (ref[0] == '-') return 0;
  if (strstr(ref, "..")) return 0;
  for (i = 0; i < n; i++) {
    unsigned char c = (unsigned char)ref[i];
    if (c < 0x20 || c == 0x7f || c == ' ' || c == '~' || c == '^' ||
        c == ':' || c == '?' || c == '*' || c == '[' || c == '\\')
      return 0;
  }
  if (ref[n - 1] == '/' || ref[n - 1] == '.') return 0;
  if (n >= 5 && strcmp(ref + n - 5, ".lock") == 0) return 0;
  return 1;
}

static const char *ws_rel(const astd_req *r, const char *abs) {
  size_t wl = r->workspace ? strlen(r->workspace) : 0;
  if (wl > 0 && strncmp(abs, r->workspace, wl) == 0 && abs[wl] == '/')
    return abs + wl + 1;
  if (wl > 0 && strcmp(abs, r->workspace) == 0) return ".";
  return abs;
}

/* Split NUL-terminated text into lines in place; returns count. The
 * trailing empty segment after a final newline is dropped. */
static size_t split_lines_inplace(char *s, char ***out) {
  size_t n = 0, i;
  char **lines;
  size_t len = strlen(s);
  for (i = 0; i < len; i++)
    if (s[i] == '\n') n++;
  lines = malloc((n + 2) * sizeof *lines);
  if (!lines) {
    *out = NULL;
    return 0;
  }
  n = 0;
  lines[n++] = s;
  for (i = 0; i < len; i++) {
    if (s[i] == '\n') {
      s[i] = '\0';
      if (i > 0 && s[i - 1] == '\r') s[i - 1] = '\0';
      lines[n++] = s + i + 1;
    }
  }
  if (n > 0 && lines[n - 1][0] == '\0') n--;
  *out = lines;
  return n;
}

static const char *skip_fields(const char *s, int k) {
  while (k > 0) {
    while (*s && *s != ' ') s++;
    if (*s != ' ') return NULL;
    s++;
    k--;
  }
  return s;
}

static const char *change_word(char c) {
  switch (c) {
    case 'A':
      return "added";
    case 'D':
      return "deleted";
    case 'R':
      return "renamed";
    case 'C':
      return "copied";
    default:
      return "modified"; /* M, T, U and anything exotic */
  }
}

static int push_change(xcdn_value_t *arr, const char *path, char xy) {
  xcdn_value_t *o = xcdn_value_object();
  if (!o) return -1;
  if (astd_set_str(o, "path", path) != 0 ||
      astd_set_str(o, "change", change_word(xy)) != 0 ||
      astd_arr_push_val(arr, o) != 0) {
    xcdn_value_free(o);
    return -1;
  }
  return 0;
}

/* Drop the trailing partial hunk after a capped capture. */
static void trim_at_hunk(char *s, size_t *n) {
  size_t i;
  for (i = *n; i >= 4; i--) {
    if (memcmp(s + i - 4, "\n@@ ", 4) == 0) {
      s[i - 4] = '\0';
      *n = i - 4;
      return;
    }
  }
}

/* ---- commands ----------------------------------------------------------- */

static void cmd_status(gtool *t) {
  const char *args[3] = {"status", "--porcelain=v2", "--branch"};
  gres g;
  char emsg[512];
  char **lines = NULL;
  size_t nlines, i;
  const char *branch = "";
  int detached = 0;
  int64_t ahead = 0, behind = 0;
  xcdn_value_t *staged = NULL, *unstaged = NULL, *untracked = NULL;
  xcdn_value_t *res = NULL;
  int oom = 0;
  if (run_git(t, args, 3, &g, emsg, sizeof emsg) != 0) {
    astd_fail(t->r, "git/failed", "%s", emsg);
    return;
  }
  if (g.exit_code != 0) {
    fail_git(t->r, "git/failed", &g, "status failed");
    gres_free(&g);
    return;
  }
  staged = xcdn_value_array();
  unstaged = xcdn_value_array();
  untracked = xcdn_value_array();
  if (!staged || !unstaged || !untracked) {
    oom = 1;
    goto done;
  }
  nlines = split_lines_inplace(g.out, &lines);
  if (!lines) {
    oom = 1;
    goto done;
  }
  for (i = 0; i < nlines && !oom; i++) {
    const char *ln = lines[i];
    if (strncmp(ln, "# branch.head ", 14) == 0) {
      branch = ln + 14;
      detached = strcmp(branch, "(detached)") == 0;
    } else if (strncmp(ln, "# branch.ab ", 12) == 0) {
      const char *plus = strchr(ln + 12, '+');
      const char *minus = strchr(ln + 12, '-');
      if (plus) ahead = strtoll(plus + 1, NULL, 10);
      if (minus) behind = strtoll(minus + 1, NULL, 10);
    } else if (ln[0] == '1' && ln[1] == ' ') {
      char x = ln[2], y = ln[3];
      const char *path = skip_fields(ln, 8);
      if (!path) continue;
      if (x != '.' && push_change(staged, path, x) != 0) oom = 1;
      if (!oom && y != '.' && push_change(unstaged, path, y) != 0) oom = 1;
    } else if (ln[0] == '2' && ln[1] == ' ') {
      char x = ln[2], y = ln[3];
      const char *path = skip_fields(ln, 9);
      char *tab;
      if (!path) continue;
      tab = strchr(path, '\t');
      if (tab) *tab = '\0'; /* new path before the tab */
      if (x != '.' && push_change(staged, path, x) != 0) oom = 1;
      if (!oom && y != '.' && push_change(unstaged, path, y) != 0) oom = 1;
    } else if (ln[0] == 'u' && ln[1] == ' ') {
      const char *path = skip_fields(ln, 10);
      if (path && push_change(unstaged, path, 'U') != 0) oom = 1;
    } else if (ln[0] == '?' && ln[1] == ' ') {
      xcdn_value_t *sv = xcdn_value_string(ln + 2);
      if (!sv || astd_arr_push_val(untracked, sv) != 0) {
        if (sv) xcdn_value_free(sv);
        oom = 1;
      }
    }
  }
  if (!oom) {
    int clean = xcdn_array_len(staged) == 0 && xcdn_array_len(unstaged) == 0 &&
                xcdn_array_len(untracked) == 0;
    int rc = 0;
    res = xcdn_value_object();
    if (!res) {
      oom = 1;
      goto done;
    }
    rc |= astd_set_str(res, "branch", branch);
    rc |= astd_set_bool(res, "detached", detached);
    rc |= astd_set_int(res, "ahead", ahead);
    rc |= astd_set_int(res, "behind", behind);
    if (rc == 0 && astd_set_val(res, "staged", staged) == 0)
      staged = NULL;
    else
      rc = -1;
    if (rc == 0 && astd_set_val(res, "unstaged", unstaged) == 0)
      unstaged = NULL;
    else
      rc = -1;
    if (rc == 0 && astd_set_val(res, "untracked", untracked) == 0)
      untracked = NULL;
    else
      rc = -1;
    rc |= astd_set_bool(res, "clean", clean);
    if (rc != 0) oom = 1;
  }
done:
  free(lines);
  gres_free(&g);
  if (staged) xcdn_value_free(staged);
  if (unstaged) xcdn_value_free(unstaged);
  if (untracked) xcdn_value_free(untracked);
  if (oom) {
    if (res) xcdn_value_free(res);
    astd_fail(t->r, "git/failed", "out of memory");
    return;
  }
  astd_ok(t->r, res);
}

static void cmd_diff(gtool *t) {
  const char *args[8];
  size_t n = 0;
  int staged = astd_arg_bool(t->r, "staged", 0);
  int64_t ctx = astd_arg_int(t->r, "context", 3);
  const char *path = astd_arg_str(t->r, "path", NULL);
  char ubuf[24];
  gres g;
  char emsg[512];
  int truncated;
  if (ctx < 0) ctx = 0;
  if (ctx > 20) ctx = 20;
  snprintf(ubuf, sizeof ubuf, "-U%lld", (long long)ctx);
  args[n++] = "diff";
  args[n++] = "--no-color";
  args[n++] = "--no-ext-diff";
  args[n++] = ubuf;
  if (staged) args[n++] = "--staged";
  args[n++] = "--";
  if (path) args[n++] = ws_rel(t->r, path);
  if (run_git(t, args, n, &g, emsg, sizeof emsg) != 0) {
    astd_fail(t->r, "git/failed", "%s", emsg);
    return;
  }
  if (g.exit_code != 0) {
    fail_git(t->r, "git/failed", &g, "diff failed");
    gres_free(&g);
    return;
  }
  truncated = g.out_trunc;
  if (truncated) trim_at_hunk(g.out, &g.out_n);
  {
    xcdn_value_t *res = xcdn_value_object();
    if (!res || astd_set_str(res, "diff", g.out) != 0 ||
        astd_set_bool(res, "truncated", truncated) != 0) {
      if (res) xcdn_value_free(res);
      gres_free(&g);
      astd_fail(t->r, "git/failed", "out of memory");
      return;
    }
    gres_free(&g);
    astd_ok(t->r, res);
  }
}

static size_t split_us(char *s, char **f, size_t maxf) {
  size_t c = 1;
  f[0] = s;
  while (c < maxf) {
    char *u = strchr(f[c - 1], 0x1f);
    if (!u) break;
    *u = '\0';
    f[c] = u + 1;
    c++;
  }
  return c;
}

static void cmd_log(gtool *t) {
  const char *args[6];
  size_t n = 0;
  int64_t maxc = astd_arg_int(t->r, "max_count", 20);
  const char *path = astd_arg_str(t->r, "path", NULL);
  char mbuf[40];
  gres g;
  char emsg[512];
  char **lines = NULL;
  size_t nlines, i;
  xcdn_value_t *commits = NULL, *res = NULL;
  int oom = 0;
  if (maxc < 1) maxc = 1;
  if (maxc > 200) maxc = 200;
  snprintf(mbuf, sizeof mbuf, "--max-count=%lld", (long long)maxc);
  args[n++] = "log";
  args[n++] = mbuf;
  args[n++] = "--format=%H%x1f%h%x1f%an%x1f%aI%x1f%s";
  args[n++] = "--";
  if (path) args[n++] = ws_rel(t->r, path);
  if (run_git(t, args, n, &g, emsg, sizeof emsg) != 0) {
    astd_fail(t->r, "git/failed", "%s", emsg);
    return;
  }
  if (g.exit_code != 0) {
    fail_git(t->r, "git/failed", &g, "log failed");
    gres_free(&g);
    return;
  }
  commits = xcdn_value_array();
  if (!commits) {
    gres_free(&g);
    astd_fail(t->r, "git/failed", "out of memory");
    return;
  }
  nlines = split_lines_inplace(g.out, &lines);
  for (i = 0; i < nlines && lines && !oom; i++) {
    char *f[5];
    xcdn_value_t *c;
    int rc = 0;
    if (lines[i][0] == '\0') continue;
    if (split_us(lines[i], f, 5) != 5) continue; /* defensive */
    c = xcdn_value_object();
    if (!c) {
      oom = 1;
      break;
    }
    rc |= astd_set_str(c, "hash", f[0]);
    rc |= astd_set_str(c, "short_hash", f[1]);
    rc |= astd_set_str(c, "author", f[2]);
    if (rc == 0) {
      xcdn_value_t *dv = xcdn_value_datetime(f[3]);
      if (!dv) {
        rc = -1;
      } else if (astd_set_val(c, "date", dv) != 0) {
        xcdn_value_free(dv);
        rc = -1;
      }
    }
    rc |= astd_set_str(c, "subject", f[4]);
    if (rc == 0) rc = astd_arr_push_val(commits, c);
    if (rc != 0) {
      xcdn_value_free(c);
      oom = 1;
    }
  }
  free(lines);
  gres_free(&g);
  if (!oom) {
    res = xcdn_value_object();
    if (!res || astd_set_val(res, "commits", commits) != 0)
      oom = 1;
    else
      commits = NULL;
  }
  if (oom) {
    if (commits) xcdn_value_free(commits);
    if (res) xcdn_value_free(res);
    astd_fail(t->r, "git/failed", "out of memory");
    return;
  }
  astd_ok(t->r, res);
}

static void cmd_show(gtool *t) {
  const char *ref = astd_arg_str(t->r, "ref", NULL);
  const char *path = astd_arg_str(t->r, "path", NULL);
  gres g;
  char emsg[512];
  if (!ref_valid(ref)) {
    astd_fail(t->r, "git/bad-ref", "invalid ref");
    return;
  }
  if (path) {
    const char *rel = ws_rel(t->r, path);
    char *spec;
    int64_t size;
    int truncated;
    xcdn_value_t *res;
    if (strchr(rel, ':')) {
      /* a colon would splice into the <ref>:<path> syntax */
      astd_fail(t->r, "git/bad-ref", "path must not contain ':'");
      return;
    }
    spec = malloc(strlen(ref) + strlen(rel) + 2);
    if (!spec) {
      astd_fail(t->r, "git/failed", "out of memory");
      return;
    }
    sprintf(spec, "%s:%s", ref, rel); /* bounded: sized above */
    {
      const char *args[2] = {"show", NULL};
      args[1] = spec;
      if (run_git(t, args, 2, &g, emsg, sizeof emsg) != 0) {
        free(spec);
        astd_fail(t->r, "git/failed", "%s", emsg);
        return;
      }
    }
    if (g.exit_code != 0) {
      fail_git(t->r, "git/failed", &g, "show failed");
      free(spec);
      gres_free(&g);
      return;
    }
    truncated = g.out_trunc;
    size = (int64_t)g.out_n;
    {
      /* true blob size even when the capture was capped */
      const char *args[3] = {"cat-file", "-s", NULL};
      gres g2;
      args[2] = spec;
      if (run_git(t, args, 3, &g2, emsg, sizeof emsg) == 0) {
        if (g2.exit_code == 0 && g2.out) {
          chomp(g2.out);
          size = strtoll(g2.out, NULL, 10);
        }
        gres_free(&g2);
      }
    }
    free(spec);
    res = xcdn_value_object();
    if (!res || astd_set_str(res, "content", g.out) != 0 ||
        astd_set_int(res, "size", size) != 0 ||
        astd_set_bool(res, "truncated", truncated) != 0) {
      if (res) xcdn_value_free(res);
      gres_free(&g);
      astd_fail(t->r, "git/failed", "out of memory");
      return;
    }
    gres_free(&g);
    astd_ok(t->r, res);
    return;
  }
  /* no path: commit metadata + patch */
  {
    const char *args1[5] = {"show", "-s",
                            "--format=%H%x1f%an%x1f%aI%x1f%s%x1f%b", NULL,
                            "--"};
    const char *args2[7] = {"show", "--no-color", "--no-ext-diff",
                            "--format=", "--patch", NULL, "--"};
    gres gd;
    char *f[5];
    int truncated;
    xcdn_value_t *res;
    int rc = 0;
    args1[3] = ref;
    if (run_git(t, args1, 5, &g, emsg, sizeof emsg) != 0) {
      astd_fail(t->r, "git/failed", "%s", emsg);
      return;
    }
    if (g.exit_code != 0) {
      fail_git(t->r, "git/failed", &g, "show failed");
      gres_free(&g);
      return;
    }
    if (split_us(g.out, f, 5) != 5) {
      astd_fail(t->r, "git/failed", "unexpected show output");
      gres_free(&g);
      return;
    }
    chomp(f[4]); /* body: drop trailing newlines */
    args2[5] = ref;
    if (run_git(t, args2, 7, &gd, emsg, sizeof emsg) != 0) {
      gres_free(&g);
      astd_fail(t->r, "git/failed", "%s", emsg);
      return;
    }
    if (gd.exit_code != 0) {
      fail_git(t->r, "git/failed", &gd, "show failed");
      gres_free(&g);
      gres_free(&gd);
      return;
    }
    truncated = gd.out_trunc;
    if (truncated) trim_at_hunk(gd.out, &gd.out_n);
    res = xcdn_value_object();
    if (!res) rc = -1;
    if (rc == 0) rc |= astd_set_str(res, "hash", f[0]);
    if (rc == 0) rc |= astd_set_str(res, "author", f[1]);
    if (rc == 0) {
      xcdn_value_t *dv = xcdn_value_datetime(f[2]);
      if (!dv) {
        rc = -1;
      } else if (astd_set_val(res, "date", dv) != 0) {
        xcdn_value_free(dv);
        rc = -1;
      }
    }
    if (rc == 0) rc |= astd_set_str(res, "subject", f[3]);
    if (rc == 0) rc |= astd_set_str(res, "body", f[4]);
    if (rc == 0) rc |= astd_set_str(res, "diff", gd.out);
    if (rc == 0) rc |= astd_set_bool(res, "truncated", truncated);
    gres_free(&g);
    gres_free(&gd);
    if (rc != 0) {
      if (res) xcdn_value_free(res);
      astd_fail(t->r, "git/failed", "out of memory");
      return;
    }
    astd_ok(t->r, res);
  }
}

static void cmd_add(gtool *t) {
  const xcdn_value_t *pv = astd_arg(t->r, "paths");
  size_t n, i;
  const char **args;
  gres g;
  char emsg[512];
  if (!pv || pv->type != XCDN_VAL_ARRAY || (n = xcdn_array_len(pv)) < 1) {
    astd_fail(t->r, "astools/invalid-args",
              "paths must be a non-empty array");
    return;
  }
  args = calloc(n + 2, sizeof *args);
  if (!args) {
    astd_fail(t->r, "git/failed", "out of memory");
    return;
  }
  args[0] = "add";
  args[1] = "--";
  for (i = 0; i < n; i++) {
    xcdn_node_t *nd = xcdn_array_get(pv, i);
    const char *s = nd && nd->value ? xcdn_value_as_string(nd->value) : NULL;
    if (!s) {
      free(args);
      astd_fail(t->r, "astools/invalid-args", "paths[%lu] is not a string",
                (unsigned long)i);
      return;
    }
    args[i + 2] = ws_rel(t->r, s);
  }
  if (run_git(t, args, n + 2, &g, emsg, sizeof emsg) != 0) {
    free(args);
    astd_fail(t->r, "git/failed", "%s", emsg);
    return;
  }
  free(args);
  if (g.exit_code != 0) {
    fail_git(t->r, "git/failed", &g, "add failed");
    gres_free(&g);
    return;
  }
  gres_free(&g);
  {
    xcdn_value_t *res = xcdn_value_object();
    if (!res || astd_set_int(res, "added", (int64_t)n) != 0) {
      if (res) xcdn_value_free(res);
      astd_fail(t->r, "git/failed", "out of memory");
      return;
    }
    astd_ok(t->r, res);
  }
}

/* Run a small read-only git query, return trimmed stdout (malloc'd) or
 * NULL (a git/failed response was already sent). */
static char *query_git(gtool *t, const char *const *args, size_t nargs) {
  gres g;
  char emsg[512];
  char *out;
  if (run_git(t, args, nargs, &g, emsg, sizeof emsg) != 0) {
    astd_fail(t->r, "git/failed", "%s", emsg);
    return NULL;
  }
  if (g.exit_code != 0) {
    fail_git(t->r, "git/failed", &g, "git query failed");
    gres_free(&g);
    return NULL;
  }
  out = g.out;
  g.out = NULL;
  gres_free(&g);
  chomp(out);
  return out;
}

static void cmd_commit(gtool *t) {
  const char *msg = astd_arg_str(t->r, "message", NULL);
  int all = astd_arg_bool(t->r, "all", 0);
  const char *args[4];
  size_t n = 0;
  gres g;
  char emsg[512];
  char *hash = NULL, *shorth = NULL, *files = NULL;
  int64_t files_changed = 0;
  if (!msg || msg[0] == '\0') {
    astd_fail(t->r, "astools/invalid-args", "message must not be empty");
    return;
  }
  args[n++] = "commit";
  if (all) args[n++] = "-a";
  args[n++] = "-m";
  args[n++] = msg;
  if (run_git(t, args, n, &g, emsg, sizeof emsg) != 0) {
    astd_fail(t->r, "git/failed", "%s", emsg);
    return;
  }
  if (g.exit_code != 0) {
    /* covers "nothing to commit" (git prints it on stdout) and missing
     * identity alike */
    fail_git(t->r, "git/failed", &g, "commit failed");
    gres_free(&g);
    return;
  }
  gres_free(&g);
  {
    const char *qh[2] = {"rev-parse", "HEAD"};
    const char *qs[3] = {"rev-parse", "--short", "HEAD"};
    const char *qf[6] = {"diff-tree", "--no-commit-id", "--name-only", "-r",
                         "--root", "HEAD"};
    hash = query_git(t, qh, 2);
    if (!hash) return;
    shorth = query_git(t, qs, 3);
    if (!shorth) {
      free(hash);
      return;
    }
    files = query_git(t, qf, 6);
    if (!files) {
      free(hash);
      free(shorth);
      return;
    }
  }
  {
    size_t i, len = strlen(files);
    int in_line = 0;
    for (i = 0; i < len; i++) {
      if (files[i] == '\n') {
        in_line = 0;
      } else if (!in_line) {
        files_changed++;
        in_line = 1;
      }
    }
  }
  free(files);
  {
    xcdn_value_t *res = xcdn_value_object();
    int rc = 0;
    if (!res) rc = -1;
    if (rc == 0) rc |= astd_set_str(res, "hash", hash);
    if (rc == 0) rc |= astd_set_str(res, "short_hash", shorth);
    if (rc == 0) rc |= astd_set_int(res, "files_changed", files_changed);
    free(hash);
    free(shorth);
    if (rc != 0) {
      if (res) xcdn_value_free(res);
      astd_fail(t->r, "git/failed", "out of memory");
      return;
    }
    astd_ok(t->r, res);
  }
}

static void cmd_checkout(gtool *t) {
  const char *ref = astd_arg_str(t->r, "ref", NULL);
  int create = astd_arg_bool(t->r, "create", 0);
  gres g;
  char emsg[512];
  char *branch = NULL;
  int detached = 0;
  if (!ref_valid(ref)) {
    astd_fail(t->r, "git/bad-ref", "invalid ref");
    return;
  }
  if (!create) {
    const char *qs[2] = {"status", "--porcelain"};
    if (run_git(t, qs, 2, &g, emsg, sizeof emsg) != 0) {
      astd_fail(t->r, "git/failed", "%s", emsg);
      return;
    }
    if (g.exit_code != 0) {
      fail_git(t->r, "git/failed", &g, "status failed");
      gres_free(&g);
      return;
    }
    if (g.out && g.out[0] != '\0') {
      gres_free(&g);
      astd_fail(t->r, "git/dirty",
                "working tree has uncommitted changes; commit them first");
      return;
    }
    gres_free(&g);
  }
  /* create:true branches from HEAD without touching the working tree,
   * so it is allowed on a dirty tree; an existing branch name makes git
   * itself fail and the error is relayed below */
  {
    const char *args[3];
    size_t n = 0;
    if (create) {
      args[n++] = "checkout";
      args[n++] = "-b";
      args[n++] = ref;
    } else {
      args[n++] = "checkout";
      args[n++] = ref;
      args[n++] = "--";
    }
    if (run_git(t, args, n, &g, emsg, sizeof emsg) != 0) {
      astd_fail(t->r, "git/failed", "%s", emsg);
      return;
    }
    if (g.exit_code != 0) {
      const char *code =
          (g.err && ci_contains(g.err, "conflict")) ? "git/conflict"
                                                    : "git/failed";
      fail_git(t->r, code, &g, "checkout failed");
      gres_free(&g);
      return;
    }
    gres_free(&g);
  }
  {
    const char *qb[3] = {"rev-parse", "--abbrev-ref", "HEAD"};
    branch = query_git(t, qb, 3);
    if (!branch) return;
    if (strcmp(branch, "HEAD") == 0) {
      const char *qs[3] = {"rev-parse", "--short", "HEAD"};
      char *sh = query_git(t, qs, 3);
      if (!sh) {
        free(branch);
        return;
      }
      free(branch);
      branch = sh;
      detached = 1;
    }
  }
  {
    xcdn_value_t *res = xcdn_value_object();
    if (!res || astd_set_str(res, "branch", branch) != 0 ||
        astd_set_bool(res, "detached", detached) != 0) {
      if (res) xcdn_value_free(res);
      free(branch);
      astd_fail(t->r, "git/failed", "out of memory");
      return;
    }
    free(branch);
    astd_ok(t->r, res);
  }
}

/* ---- entry -------------------------------------------------------------- */

int astd_tool_git(astd_req *r) {
  gtool t;
  gres g;
  char emsg[512];
  if (!r->workspace) {
    astd_fail(r, "astools/protocol", "request carries no workspace root");
    return 0;
  }
  t.r = r;
  t.git = find_git();
  t.envp = NULL;
  if (!t.git) {
    astd_fail(r, "git/not-installed", "git was not found on PATH");
    return 0;
  }
  t.envp = git_env(r);
  if (!t.envp) {
    free(t.git);
    astd_fail(r, "git/failed", "out of memory");
    return 0;
  }
  signal(SIGPIPE, SIG_IGN);
  {
    const char *args[2] = {"rev-parse", "--git-dir"};
    if (run_git(&t, args, 2, &g, emsg, sizeof emsg) != 0) {
      free(t.git);
      free_env(t.envp);
      astd_fail(r, "git/failed", "%s", emsg);
      return 0;
    }
    if (g.exit_code != 0) {
      fail_git(r, "git/not-a-repo", &g, "not a git repository");
      gres_free(&g);
      free(t.git);
      free_env(t.envp);
      return 0;
    }
    gres_free(&g);
  }
  if (strcmp(r->command, "status") == 0)
    cmd_status(&t);
  else if (strcmp(r->command, "diff") == 0)
    cmd_diff(&t);
  else if (strcmp(r->command, "log") == 0)
    cmd_log(&t);
  else if (strcmp(r->command, "show") == 0)
    cmd_show(&t);
  else if (strcmp(r->command, "add") == 0)
    cmd_add(&t);
  else if (strcmp(r->command, "commit") == 0)
    cmd_commit(&t);
  else if (strcmp(r->command, "checkout") == 0)
    cmd_checkout(&t);
  else
    astd_fail(r, "astools/protocol", "unknown git command '%s'", r->command);
  free(t.git);
  free_env(t.envp);
  return 0;
}

#endif /* POSIX */
