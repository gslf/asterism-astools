/*
 * tool_fs.c — the `fs` standard tool: read / write / list / stat /
 * mkdir / remove / move / copy inside the workspace.
 *
 * Path arguments arrive canonical and absolute from the runtime
 * and are used verbatim. Traversal (list/remove/copy) is lstat-based and
 * never follows symlinks.
 */

#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#endif

#include "sdk.h"
#ifdef _WIN32

int astd_tool_fs(astd_req *r) {
  astd_fail(r, "std/unsupported", "fs is not implemented on this platform");
  return 0;
}

#else /* POSIX */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h> /* rename */
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum {
  FS_IO_CHUNK = 65536,
  FS_MAX_DEPTH = 512
};

/* ---- shared helpers ------------------------------------------------------ */

static void fail_oom(astd_req *r) {
  astd_fail(r, "fs/io", "out of memory");
}

static const char *errno_code(int e) {
  switch (e) {
    case ENOENT:
    case ENOTDIR:
      return "fs/not-found";
    case EEXIST:
      return "fs/exists";
    case ENOTEMPTY:
      return "fs/not-empty";
    case EISDIR:
      return "fs/is-dir";
    default:
      return "fs/io";
  }
}

static int fail_errno(astd_req *r, int e, const char *path) {
  astd_fail(r, errno_code(e), "%s: %s", path, strerror(e));
  return -1;
}

static const char *mode_type(mode_t m) {
  if (S_ISREG(m)) return "file";
  if (S_ISDIR(m)) return "dir";
  if (S_ISLNK(m)) return "symlink";
  return "other";
}

static char *path_join(const char *a, const char *b) {
  size_t la = strlen(a), lb = strlen(b);
  char *p;
  if (la > 0 && a[la - 1] == '/') la--;
  if (la > SIZE_MAX - lb - 2) return NULL;
  p = malloc(la + lb + 2);
  if (!p) return NULL;
  if (la == 0) {
    memcpy(p, b, lb + 1);
    return p;
  }
  memcpy(p, a, la);
  p[la] = '/';
  memcpy(p + la + 1, b, lb + 1);
  return p;
}

static void free_names(char **v, size_t n) {
  size_t i;
  for (i = 0; i < n; i++) free(v[i]);
  free(v);
}

static int name_cmp(const void *a, const void *b) {
  return strcmp(*(char *const *)a, *(char *const *)b);
}

/* Read every entry name of a directory (minus "." and ".."), so callers
 * never mutate or recurse while a DIR handle is open (bounds fd usage).
 * Returns 0 with a malloc'd vector, or -1 with errno set. */
static int dir_names(const char *path, char ***out, size_t *out_n) {
  DIR *d;
  struct dirent *de;
  char **v = NULL;
  size_t n = 0, cap = 0;

  d = opendir(path);
  if (!d) return -1;
  while ((de = readdir(d)) != NULL) {
    char *name;
    if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
      continue;
    if (n == cap) {
      size_t ncap = cap ? cap * 2 : 16;
      char **nv;
      if (ncap > SIZE_MAX / sizeof *nv) goto oom;
      nv = realloc(v, ncap * sizeof *nv);
      if (!nv) goto oom;
      v = nv;
      cap = ncap;
    }
    name = strdup(de->d_name);
    if (!name) goto oom;
    v[n++] = name;
  }
  closedir(d);
  if (n > 1) qsort(v, n, sizeof *v, name_cmp);
  *out = v;
  *out_n = n;
  return 0;

oom:
  free_names(v, n);
  closedir(d);
  errno = ENOMEM;
  return -1;
}

/* ---- fs.read ------------------------------------------------------------- */

static int cmd_read(astd_req *r) {
  const char *path = astd_arg_str(r, "path", NULL);
  int64_t offset = astd_arg_int(r, "offset", 0);
  int64_t max_bytes = astd_arg_int(r, "max_bytes", 65536);
  const char *enc = astd_arg_str(r, "encoding", "utf8");
  struct stat st;
  char *data = NULL;
  size_t cap = 0, total = 0;
  int fd, truncated = 0;
  xcdn_value_t *res, *content;

  if (!path) {
    astd_fail(r, "astools/invalid-args", "path is required");
    return 0;
  }
  if (offset < 0) {
    astd_fail(r, "astools/invalid-args", "offset must be >= 0");
    return 0;
  }
  if (max_bytes < 0) {
    astd_fail(r, "astools/invalid-args", "max_bytes must be >= 0");
    return 0;
  }
  if (strcmp(enc, "utf8") != 0 && strcmp(enc, "base64") != 0) {
    astd_fail(r, "astools/invalid-args", "encoding must be utf8 or base64");
    return 0;
  }

  fd = open(path, O_RDONLY);
  if (fd < 0) {
    fail_errno(r, errno, path);
    return 0;
  }
  if (fstat(fd, &st) != 0) {
    int e = errno;
    close(fd);
    fail_errno(r, e, path);
    return 0;
  }
  if (S_ISDIR(st.st_mode)) {
    close(fd);
    astd_fail(r, "fs/is-dir", "%s: is a directory", path);
    return 0;
  }
  if (offset > 0 && lseek(fd, (off_t)offset, SEEK_SET) < 0) {
    int e = errno;
    close(fd);
    fail_errno(r, e, path);
    return 0;
  }

  while ((int64_t)total < max_bytes) {
    size_t want = FS_IO_CHUNK;
    uint64_t left = (uint64_t)max_bytes - total;
    ssize_t n;
    if (left < want) want = (size_t)left;
    if (cap - total < want + 1) {
      size_t ncap = cap ? cap : 8192;
      char *p;
      int grown = 1;
      while (ncap - total < want + 1) {
        if (ncap > SIZE_MAX / 2) {
          grown = 0;
          break;
        }
        ncap *= 2;
      }
      p = grown ? realloc(data, ncap) : NULL;
      if (!p) {
        free(data);
        close(fd);
        fail_oom(r);
        return 0;
      }
      data = p;
      cap = ncap;
    }
    n = read(fd, data + total, want);
    if (n < 0) {
      if (errno == EINTR) continue;
      {
        int e = errno;
        free(data);
        close(fd);
        fail_errno(r, e, path);
      }
      return 0;
    }
    if (n == 0) break;
    total += (size_t)n;
  }
  if ((int64_t)total == max_bytes) {
    char probe;
    ssize_t n;
    do {
      n = read(fd, &probe, 1);
    } while (n < 0 && errno == EINTR);
    if (n > 0) truncated = 1;
  }
  close(fd);
  if (!data) {
    data = malloc(1);
    if (!data) {
      fail_oom(r);
      return 0;
    }
  }
  data[total] = '\0';

  if (strcmp(enc, "base64") == 0) {
    char *b64 = astd_base64_encode((const uint8_t *)data, total);
    free(data);
    if (!b64) {
      fail_oom(r);
      return 0;
    }
    content = xcdn_value_string_owned(b64);
    if (!content) {
      free(b64);
      fail_oom(r);
      return 0;
    }
  } else {
    content = xcdn_value_string_owned(data);
    if (!content) {
      free(data);
      fail_oom(r);
      return 0;
    }
  }

  res = xcdn_value_object();
  if (!res) {
    xcdn_value_free(content);
    fail_oom(r);
    return 0;
  }
  if (astd_set_val(res, "content", content) != 0 ||
      astd_set_int(res, "size", (int64_t)st.st_size) != 0 ||
      astd_set_bool(res, "truncated", truncated) != 0) {
    xcdn_value_free(res);
    fail_oom(r);
    return 0;
  }
  astd_ok(r, res);
  return 0;
}

/* ---- fs.write ------------------------------------------------------------ */

static int cmd_write(astd_req *r) {
  const char *path = astd_arg_str(r, "path", NULL);
  const char *content = astd_arg_str(r, "content", NULL);
  const char *mode = astd_arg_str(r, "mode", "create");
  const char *enc = astd_arg_str(r, "encoding", "utf8");
  uint8_t *dec = NULL;
  const uint8_t *data;
  size_t dlen, off = 0;
  int fd, flags;
  xcdn_value_t *res;

  if (!path) {
    astd_fail(r, "astools/invalid-args", "path is required");
    return 0;
  }
  if (!content) {
    astd_fail(r, "astools/invalid-args", "content is required");
    return 0;
  }
  if (strcmp(mode, "create") == 0)
    flags = O_WRONLY | O_CREAT | O_EXCL;
  else if (strcmp(mode, "overwrite") == 0)
    flags = O_WRONLY | O_CREAT | O_TRUNC;
  else if (strcmp(mode, "append") == 0)
    flags = O_WRONLY | O_CREAT | O_APPEND;
  else {
    astd_fail(r, "astools/invalid-args",
              "mode must be create, overwrite or append");
    return 0;
  }
  if (strcmp(enc, "base64") == 0) {
    if (astd_base64_decode(content, &dec, &dlen) != 0) {
      astd_fail(r, "fs/io", "content is not valid base64");
      return 0;
    }
    data = dec;
  } else if (strcmp(enc, "utf8") == 0) {
    data = (const uint8_t *)content;
    dlen = strlen(content);
  } else {
    astd_fail(r, "astools/invalid-args", "encoding must be utf8 or base64");
    return 0;
  }

  fd = open(path, flags, 0644);
  if (fd < 0) {
    int e = errno;
    free(dec);
    fail_errno(r, e, path);
    return 0;
  }
  while (off < dlen) {
    ssize_t n = write(fd, data + off, dlen - off);
    if (n < 0) {
      if (errno == EINTR) continue;
      {
        int e = errno;
        close(fd);
        free(dec);
        astd_fail(r, "fs/io", "%s: %s", path, strerror(e));
      }
      return 0;
    }
    off += (size_t)n;
  }
  free(dec);
  if (close(fd) != 0) {
    astd_fail(r, "fs/io", "%s: %s", path, strerror(errno));
    return 0;
  }

  res = xcdn_value_object();
  if (!res) {
    fail_oom(r);
    return 0;
  }
  if (astd_set_int(res, "bytes_written", (int64_t)dlen) != 0) {
    xcdn_value_free(res);
    fail_oom(r);
    return 0;
  }
  astd_ok(r, res);
  return 0;
}

/* ---- fs.list ------------------------------------------------------------- */

typedef struct {
  char *name;
  const char *type; /* static string */
  int64_t size;
  int64_t mtime;
} fs_ent;

typedef struct {
  fs_ent *v;
  size_t len, cap;
} ent_vec;

/* Takes ownership of name (freed on failure too). */
static int ent_push(ent_vec *ev, char *name, const char *type, int64_t size,
                    int64_t mtime) {
  if (ev->len == ev->cap) {
    size_t ncap = ev->cap ? ev->cap * 2 : 32;
    fs_ent *nv;
    if (ncap > SIZE_MAX / sizeof *nv) {
      free(name);
      return -1;
    }
    nv = realloc(ev->v, ncap * sizeof *nv);
    if (!nv) {
      free(name);
      return -1;
    }
    ev->v = nv;
    ev->cap = ncap;
  }
  ev->v[ev->len].name = name;
  ev->v[ev->len].type = type;
  ev->v[ev->len].size = size;
  ev->v[ev->len].mtime = mtime;
  ev->len++;
  return 0;
}

static void ent_vec_free(ent_vec *ev) {
  size_t i;
  for (i = 0; i < ev->len; i++) free(ev->v[i].name);
  free(ev->v);
  ev->v = NULL;
  ev->len = 0;
  ev->cap = 0;
}

static int ent_cmp(const void *a, const void *b) {
  const fs_ent *x = (const fs_ent *)a;
  const fs_ent *y = (const fs_ent *)b;
  return strcmp(x->name, y->name);
}

/* Keep only the lexicographically smallest `keep` entries.  This bounds
 * memory to max_entries+1 (the extra item records truncation) while still
 * producing the same globally sorted result for arbitrarily nested trees. */
static int ent_keep(ent_vec *ev, char *name, const char *type, int64_t size,
                    int64_t mtime, size_t keep) {
  fs_ent *last;
  if (ev->len < keep) {
    if (ent_push(ev, name, type, size, mtime) != 0) return -1;
    if (ev->len > 1) qsort(ev->v, ev->len, sizeof *ev->v, ent_cmp);
    return 0;
  }
  last = &ev->v[ev->len - 1];
  if (strcmp(name, last->name) >= 0) {
    free(name);
    return 0;
  }
  free(last->name);
  last->name = name;
  last->type = type;
  last->size = size;
  last->mtime = mtime;
  qsort(ev->v, ev->len, sizeof *ev->v, ent_cmp);
  return 0;
}

/* Collect entries under abs. rel is "" at the root; recursive entries are
 * named by their full relative path. Only the smallest limit+1 matches are
 * retained. Returns 0 on success, -1 after an error response. Unreadable or
 * vanished sub-entries are skipped. */
static int list_walk(astd_req *r, const char *abs, const char *rel,
                     int recursive, const char *glob, int depth,
                     size_t limit, ent_vec *ev) {
  char **names = NULL;
  size_t n = 0, i;

  if (dir_names(abs, &names, &n) != 0) {
    int e = errno;
    if (depth > 0) return 0;
    if (e == ENOMEM) {
      fail_oom(r);
      return -1;
    }
    if (e == ENOTDIR) {
      astd_fail(r, "fs/io", "%s: not a directory", abs);
      return -1;
    }
    fail_errno(r, e, abs);
    return -1;
  }
  for (i = 0; i < n; i++) {
    char *full = path_join(abs, names[i]);
    char *reln = rel[0] != '\0' ? path_join(rel, names[i]) : strdup(names[i]);
    struct stat st;
    if (!full || !reln) {
      free(full);
      free(reln);
      free_names(names, n);
      fail_oom(r);
      return -1;
    }
    if (lstat(full, &st) != 0) {
      free(full);
      free(reln);
      continue;
    }
    if (recursive && S_ISDIR(st.st_mode) && depth < FS_MAX_DEPTH) {
      if (list_walk(r, full, reln, recursive, glob, depth + 1, limit, ev) !=
          0) {
        free(full);
        free(reln);
        free_names(names, n);
        return -1;
      }
    }
    free(full);
    if (!glob || astd_glob_match(glob, reln)) {
      if (ent_keep(ev, reln, mode_type(st.st_mode), (int64_t)st.st_size,
                   (int64_t)st.st_mtime, limit + 1) != 0) {
        free_names(names, n);
        fail_oom(r);
        return -1;
      }
    } else {
      free(reln);
    }
  }
  free_names(names, n);
  return 0;
}

static int emit_list(astd_req *r, const fs_ent *v, size_t emit_n,
                     int truncated) {
  xcdn_value_t *res = NULL, *arr = NULL;
  size_t i;

  res = xcdn_value_object();
  arr = xcdn_value_array();
  if (!res || !arr) goto oom;
  for (i = 0; i < emit_n; i++) {
    char ts[32];
    xcdn_value_t *e = xcdn_value_object();
    xcdn_value_t *mod;
    int ok = 0;
    if (!e) goto oom;
    astd_rfc3339(v[i].mtime, ts);
    mod = xcdn_value_datetime(ts);
    do {
      if (astd_set_str(e, "name", v[i].name) != 0) break;
      if (astd_set_str(e, "type", v[i].type) != 0) break;
      if (astd_set_int(e, "size", v[i].size) != 0) break;
      if (astd_set_val(e, "modified", mod) != 0) {
        mod = NULL;
        break;
      }
      mod = NULL;
      ok = 1;
    } while (0);
    if (!ok) {
      xcdn_value_free(mod);
      xcdn_value_free(e);
      goto oom;
    }
    if (astd_arr_push_val(arr, e) != 0) goto oom; /* e consumed */
  }
  if (astd_set_val(res, "entries", arr) != 0) {
    arr = NULL;
    goto oom;
  }
  arr = NULL;
  if (astd_set_bool(res, "truncated", truncated) != 0) goto oom;
  astd_ok(r, res);
  return 0;

oom:
  xcdn_value_free(arr);
  xcdn_value_free(res);
  fail_oom(r);
  return -1;
}

static int cmd_list(astd_req *r) {
  const char *path = astd_arg_str(r, "path", NULL);
  int recursive = astd_arg_bool(r, "recursive", 0);
  const char *glob = astd_arg_str(r, "glob", NULL);
  int64_t max_entries = astd_arg_int(r, "max_entries", 1000);
  ent_vec ev = { NULL, 0, 0 };
  size_t emit_n;
  int truncated;

  if (!path) {
    astd_fail(r, "astools/invalid-args", "path is required");
    return 0;
  }
  if (max_entries < 0) {
    astd_fail(r, "astools/invalid-args", "max_entries must be >= 0");
    return 0;
  }
  {
    size_t limit = (uint64_t)max_entries >= (uint64_t)SIZE_MAX
                       ? SIZE_MAX - 1
                       : (size_t)max_entries;
    int wr = list_walk(r, path, "", recursive, glob, 0, limit, &ev);
    if (wr < 0) {
      ent_vec_free(&ev);
      return 0;
    }
  }
  if (ev.len > 1) qsort(ev.v, ev.len, sizeof *ev.v, ent_cmp);
  emit_n = ev.len;
  if ((uint64_t)emit_n > (uint64_t)max_entries) emit_n = (size_t)max_entries;
  truncated = emit_n < ev.len;
  (void)emit_list(r, ev.v, emit_n, truncated);
  ent_vec_free(&ev);
  return 0;
}

/* ---- fs.stat ------------------------------------------------------------- */

static int cmd_stat(astd_req *r) {
  const char *path = astd_arg_str(r, "path", NULL);
  struct stat st;
  char ts[32];
  xcdn_value_t *res, *mod;
  int readonly, ok = 0;

  if (!path) {
    astd_fail(r, "astools/invalid-args", "path is required");
    return 0;
  }
  if (lstat(path, &st) != 0) {
    fail_errno(r, errno, path);
    return 0;
  }
  readonly = access(path, W_OK) != 0;
  astd_rfc3339((int64_t)st.st_mtime, ts);

  res = xcdn_value_object();
  mod = xcdn_value_datetime(ts);
  if (!res) {
    xcdn_value_free(mod);
    fail_oom(r);
    return 0;
  }
  do {
    if (astd_set_str(res, "type", mode_type(st.st_mode)) != 0) break;
    if (astd_set_int(res, "size", (int64_t)st.st_size) != 0) break;
    if (astd_set_val(res, "modified", mod) != 0) {
      mod = NULL;
      break;
    }
    mod = NULL;
    if (astd_set_bool(res, "readonly", readonly) != 0) break;
    ok = 1;
  } while (0);
  if (!ok) {
    xcdn_value_free(mod);
    xcdn_value_free(res);
    fail_oom(r);
    return 0;
  }
  astd_ok(r, res);
  return 0;
}

/* ---- fs.mkdir ------------------------------------------------------------ */

static int emit_created(astd_req *r, int created) {
  xcdn_value_t *res = xcdn_value_object();
  if (!res) {
    fail_oom(r);
    return 0;
  }
  if (astd_set_bool(res, "created", created) != 0) {
    xcdn_value_free(res);
    fail_oom(r);
    return 0;
  }
  astd_ok(r, res);
  return 0;
}

static int cmd_mkdir(astd_req *r) {
  const char *path = astd_arg_str(r, "path", NULL);
  int parents = astd_arg_bool(r, "parents", 1);
  struct stat st;

  if (!path) {
    astd_fail(r, "astools/invalid-args", "path is required");
    return 0;
  }
  if (stat(path, &st) == 0) {
    if (S_ISDIR(st.st_mode)) return emit_created(r, 0);
    astd_fail(r, "fs/exists", "%s: exists and is not a directory", path);
    return 0;
  }
  if (parents) {
    char *copy = strdup(path);
    size_t i;
    if (!copy) {
      fail_oom(r);
      return 0;
    }
    for (i = 1; copy[i] != '\0'; i++) {
      if (copy[i] == '/') {
        if (copy[i - 1] == '/') continue; /* skip empty segments */
        copy[i] = '\0';
        (void)mkdir(copy, 0755); /* failures surface at the final mkdir */
        copy[i] = '/';
      }
    }
    free(copy);
  }
  if (mkdir(path, 0755) != 0) {
    int e = errno;
    if (e == EEXIST) {
      if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
        return emit_created(r, 0);
      astd_fail(r, "fs/exists", "%s: exists and is not a directory", path);
      return 0;
    }
    if (e == ENOTDIR) {
      astd_fail(r, "fs/exists", "%s: a path component is not a directory",
                path);
      return 0;
    }
    fail_errno(r, e, path); /* ENOENT: parent missing (parents:false) */
    return 0;
  }
  return emit_created(r, 1);
}

/* ---- fs.remove ----------------------------------------------------------- */

static int rmtree(astd_req *r, const char *path, int depth,
                  int64_t *removed) {
  char **names = NULL;
  size_t n = 0, i;

  if (depth > FS_MAX_DEPTH) {
    astd_fail(r, "fs/io", "%s: directory tree too deep", path);
    return -1;
  }
  if (dir_names(path, &names, &n) != 0) {
    if (errno == ENOMEM)
      fail_oom(r);
    else
      fail_errno(r, errno, path);
    return -1;
  }
  for (i = 0; i < n; i++) {
    char *full = path_join(path, names[i]);
    struct stat st;
    if (!full) {
      free_names(names, n);
      fail_oom(r);
      return -1;
    }
    if (lstat(full, &st) != 0) {
      free(full);
      continue; /* vanished under us */
    }
    if (S_ISDIR(st.st_mode)) {
      if (rmtree(r, full, depth + 1, removed) != 0) {
        free(full);
        free_names(names, n);
        return -1;
      }
    } else {
      if (unlink(full) == 0) {
        (*removed)++;
      } else if (errno != ENOENT) {
        fail_errno(r, errno, full);
        free(full);
        free_names(names, n);
        return -1;
      }
    }
    free(full);
  }
  free_names(names, n);
  if (rmdir(path) != 0) {
    fail_errno(r, errno, path);
    return -1;
  }
  (*removed)++;
  return 0;
}

static int cmd_remove(astd_req *r) {
  const char *path = astd_arg_str(r, "path", NULL);
  int recursive = astd_arg_bool(r, "recursive", 0);
  struct stat st;
  int64_t removed = 0;
  xcdn_value_t *res;

  if (!path) {
    astd_fail(r, "astools/invalid-args", "path is required");
    return 0;
  }
  if (lstat(path, &st) != 0) {
    fail_errno(r, errno, path);
    return 0;
  }
  if (S_ISDIR(st.st_mode)) {
    if (!recursive) {
      if (rmdir(path) != 0) {
        int e = errno;
        if (e == ENOTEMPTY || e == EEXIST)
          astd_fail(r, "fs/not-empty",
                    "%s: directory not empty (use recursive)", path);
        else
          fail_errno(r, e, path);
        return 0;
      }
      removed = 1;
    } else {
      if (rmtree(r, path, 0, &removed) != 0) return 0;
    }
  } else {
    if (unlink(path) != 0) {
      fail_errno(r, errno, path);
      return 0;
    }
    removed = 1;
  }

  res = xcdn_value_object();
  if (!res) {
    fail_oom(r);
    return 0;
  }
  if (astd_set_int(res, "removed", removed) != 0) {
    xcdn_value_free(res);
    fail_oom(r);
    return 0;
  }
  astd_ok(r, res);
  return 0;
}

/* ---- fs.copy / fs.move --------------------------------------------------- */

static int copy_file(astd_req *r, const char *src, const char *dst,
                     int64_t *bytes) {
  /* static: astools-std is a single-threaded oneshot process */
  static char buf[FS_IO_CHUNK];
  struct stat st;
  int sfd, dfd;

  sfd = open(src, O_RDONLY);
  if (sfd < 0) {
    fail_errno(r, errno, src);
    return -1;
  }
  if (fstat(sfd, &st) != 0) {
    int e = errno;
    close(sfd);
    fail_errno(r, e, src);
    return -1;
  }
  if (S_ISDIR(st.st_mode)) {
    close(sfd);
    astd_fail(r, "fs/is-dir", "%s: is a directory", src);
    return -1;
  }
  /* O_NOFOLLOW: during a recursive copy the per-entry destination paths are
   * never seen by the runtime pre-flight, so a symlink planted at the leaf
   * must not be followed out of the granted tree (containment). */
  dfd = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, st.st_mode & 0777);
  if (dfd < 0) {
    int e = errno;
    close(sfd);
    if (e == ELOOP)
      astd_fail(r, "fs/denied", "%s: refusing to write through a symlink",
                dst);
    else
      fail_errno(r, e, dst);
    return -1;
  }
  for (;;) {
    ssize_t n = read(sfd, buf, sizeof buf);
    size_t off = 0;
    if (n < 0) {
      if (errno == EINTR) continue;
      {
        int e = errno;
        close(sfd);
        close(dfd);
        fail_errno(r, e, src);
      }
      return -1;
    }
    if (n == 0) break;
    while (off < (size_t)n) {
      ssize_t w = write(dfd, buf + off, (size_t)n - off);
      if (w < 0) {
        if (errno == EINTR) continue;
        {
          int e = errno;
          close(sfd);
          close(dfd);
          fail_errno(r, e, dst);
        }
        return -1;
      }
      off += (size_t)w;
    }
    *bytes += (int64_t)n;
  }
  (void)fchmod(dfd, st.st_mode & 07777); /* best effort */
  close(sfd);
  if (close(dfd) != 0) {
    fail_errno(r, errno, dst);
    return -1;
  }
  return 0;
}

static int copy_symlink(astd_req *r, const char *src, const char *dst) {
  char *target;
  size_t cap = 256;
  ssize_t n;

  for (;;) {
    target = malloc(cap);
    if (!target) {
      fail_oom(r);
      return -1;
    }
    n = readlink(src, target, cap - 1);
    if (n < 0) {
      int e = errno;
      free(target);
      fail_errno(r, e, src);
      return -1;
    }
    if ((size_t)n < cap - 1) break;
    free(target);
    if (cap > SIZE_MAX / 2) {
      fail_oom(r);
      return -1;
    }
    cap *= 2;
  }
  target[n] = '\0';
  if (symlink(target, dst) != 0) {
    if (!(errno == EEXIST && unlink(dst) == 0 &&
          symlink(target, dst) == 0)) {
      int e = errno;
      free(target);
      fail_errno(r, e, dst);
      return -1;
    }
  }
  free(target);
  return 0;
}

static int copy_tree(astd_req *r, const char *src, const char *dst,
                     int depth, int64_t *bytes) {
  char **names = NULL;
  size_t n = 0, i;
  struct stat st;

  if (depth > FS_MAX_DEPTH) {
    astd_fail(r, "fs/io", "%s: directory tree too deep", src);
    return -1;
  }
  if (stat(src, &st) != 0) {
    fail_errno(r, errno, src);
    return -1;
  }
  if (mkdir(dst, 0755) != 0) {
    struct stat dstat;
    if (errno != EEXIST) {
      fail_errno(r, errno, dst);
      return -1;
    }
    /* lstat, not stat: a symlinked directory in the destination tree must
     * be refused, never descended — following it would let the recursion
     * write outside the granted subtree. */
    if (lstat(dst, &dstat) != 0 || !S_ISDIR(dstat.st_mode)) {
      if (lstat(dst, &dstat) == 0 && S_ISLNK(dstat.st_mode))
        astd_fail(r, "fs/denied",
                  "%s: refusing to descend into a symlinked directory", dst);
      else
        astd_fail(r, "fs/exists", "%s: exists and is not a directory", dst);
      return -1;
    }
  }
  (void)chmod(dst, st.st_mode & 07777); /* best effort */
  if (dir_names(src, &names, &n) != 0) {
    if (errno == ENOMEM)
      fail_oom(r);
    else
      fail_errno(r, errno, src);
    return -1;
  }
  for (i = 0; i < n; i++) {
    char *sfull = path_join(src, names[i]);
    char *dfull = path_join(dst, names[i]);
    struct stat se;
    int rc = 0;
    if (!sfull || !dfull) {
      free(sfull);
      free(dfull);
      free_names(names, n);
      fail_oom(r);
      return -1;
    }
    if (lstat(sfull, &se) == 0) {
      if (S_ISDIR(se.st_mode))
        rc = copy_tree(r, sfull, dfull, depth + 1, bytes);
      else if (S_ISREG(se.st_mode))
        rc = copy_file(r, sfull, dfull, bytes);
      else if (S_ISLNK(se.st_mode))
        rc = copy_symlink(r, sfull, dfull);
      /* other kinds (fifos, sockets, devices) are skipped */
    }
    free(sfull);
    free(dfull);
    if (rc != 0) {
      free_names(names, n);
      return -1;
    }
  }
  free_names(names, n);
  return 0;
}

static int cmd_copy(astd_req *r) {
  const char *src = astd_arg_str(r, "src", NULL);
  const char *dst = astd_arg_str(r, "dst", NULL);
  int recursive = astd_arg_bool(r, "recursive", 0);
  struct stat st;
  int64_t bytes = 0;
  xcdn_value_t *res;

  if (!src || !dst) {
    astd_fail(r, "astools/invalid-args", "src and dst are required");
    return 0;
  }
  if (stat(src, &st) != 0) {
    fail_errno(r, errno, src);
    return 0;
  }
  if (S_ISDIR(st.st_mode)) {
    if (!recursive) {
      astd_fail(r, "fs/is-dir", "%s: is a directory (use recursive)", src);
      return 0;
    }
    if (copy_tree(r, src, dst, 0, &bytes) != 0) return 0;
  } else if (S_ISREG(st.st_mode)) {
    if (copy_file(r, src, dst, &bytes) != 0) return 0;
  } else {
    astd_fail(r, "fs/io", "%s: unsupported file type", src);
    return 0;
  }

  res = xcdn_value_object();
  if (!res) {
    fail_oom(r);
    return 0;
  }
  if (astd_set_int(res, "bytes_copied", bytes) != 0) {
    xcdn_value_free(res);
    fail_oom(r);
    return 0;
  }
  astd_ok(r, res);
  return 0;
}

static int cmd_move(astd_req *r) {
  const char *src = astd_arg_str(r, "src", NULL);
  const char *dst = astd_arg_str(r, "dst", NULL);
  xcdn_value_t *res;
  int e;

  if (!src || !dst) {
    astd_fail(r, "astools/invalid-args", "src and dst are required");
    return 0;
  }
  if (rename(src, dst) != 0) {
    e = errno;
    if (e == EXDEV) {
      struct stat st;
      int64_t bytes = 0;
      if (lstat(src, &st) != 0) {
        fail_errno(r, errno, src);
        return 0;
      }
      if (!S_ISREG(st.st_mode)) {
        astd_fail(r, "fs/io",
                  "%s: cross-device move is supported only for regular files",
                  src);
        return 0;
      }
      if (copy_file(r, src, dst, &bytes) != 0) return 0;
      if (unlink(src) != 0) {
        fail_errno(r, errno, src);
        return 0;
      }
    } else {
      astd_fail(r, errno_code(e), "%s -> %s: %s", src, dst, strerror(e));
      return 0;
    }
  }
  res = xcdn_value_object();
  astd_ok(r, res); /* astd_ok handles a NULL result */
  return 0;
}

/* ---- dispatch ------------------------------------------------------------ */

int astd_tool_fs(astd_req *r) {
  const char *cmd = r ? r->command : NULL;
  if (!cmd) {
    astd_fail(r, "astools/invalid-args", "missing command");
    return 0;
  }
  if (strcmp(cmd, "read") == 0) return cmd_read(r);
  if (strcmp(cmd, "write") == 0) return cmd_write(r);
  if (strcmp(cmd, "list") == 0) return cmd_list(r);
  if (strcmp(cmd, "stat") == 0) return cmd_stat(r);
  if (strcmp(cmd, "mkdir") == 0) return cmd_mkdir(r);
  if (strcmp(cmd, "remove") == 0) return cmd_remove(r);
  if (strcmp(cmd, "move") == 0) return cmd_move(r);
  if (strcmp(cmd, "copy") == 0) return cmd_copy(r);
  astd_fail(r, "astools/invalid-args", "unknown fs command \"%s\"", cmd);
  return 0;
}

#endif /* _WIN32 */
