/* Runtime-internal reads obey effective grants and never follow path aliases. */
#include "lsp.h"
#ifndef _WIN32
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

static int same(const struct stat *a, const struct stat *b) {
#if defined(__APPLE__)
  return a->st_dev == b->st_dev && a->st_ino == b->st_ino && a->st_mode == b->st_mode &&
         a->st_size == b->st_size && a->st_mtimespec.tv_sec == b->st_mtimespec.tv_sec &&
         a->st_mtimespec.tv_nsec == b->st_mtimespec.tv_nsec &&
         a->st_ctimespec.tv_sec == b->st_ctimespec.tv_sec &&
         a->st_ctimespec.tv_nsec == b->st_ctimespec.tv_nsec;
#else
  return a->st_dev == b->st_dev && a->st_ino == b->st_ino && a->st_mode == b->st_mode &&
         a->st_size == b->st_size && a->st_mtim.tv_sec == b->st_mtim.tv_sec &&
         a->st_mtim.tv_nsec == b->st_mtim.tv_nsec && a->st_ctim.tv_sec == b->st_ctim.tv_sec &&
         a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
#endif
}

static astools_err read_file(int fd, astools_lsp_file *out) {
  struct stat before, after;
  if (fstat(fd, &before)) return ASTOOLS_ERR_IO;
  if (!S_ISREG(before.st_mode)) return ASTOOLS_ERR_DENIED;
  if (before.st_size < 0 || (uint64_t)before.st_size > LSP_FILE_BYTES) return ASTOOLS_ERR_TOOL;
  size_t n = (size_t)before.st_size, got = 0;
  out->text = malloc(n + 1);
  if (!out->text) return ASTOOLS_ERR_NOMEM;
  while (got < n) {
    ssize_t count = read(fd, out->text + got, n - got);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) return ASTOOLS_ERR_IO;
    got += (size_t)count;
  }
  if (fstat(fd, &after)) return ASTOOLS_ERR_IO;
  if (!same(&before, &after)) return ASTOOLS_ERR_BUSY;
  if (memchr(out->text, 0, n) || !jx_utf8_valid(out->text, n)) return ASTOOLS_ERR_INVALID;
  out->text[n] = 0;
  out->len = n;
  uint8_t hash[32];
  static const char hex[] = "0123456789abcdef";
  astools_sha256(out->text, n, hash);
  for (size_t i = 0; i < sizeof hash; i++) {
    out->sha256[2 * i] = hex[hash[i] >> 4];
    out->sha256[2 * i + 1] = hex[hash[i] & 15];
  }
  out->sha256[64] = 0;
  return ASTOOLS_OK;
}

/* Reopen relative to the same authorized root when checking for replacement. */
static astools_err open_file(int root, const char *relative, int as_directory, int *out) {
  char *parts = astools_strdup(relative);
  int directory = dup(root), fd = -1;
  astools_err e = ASTOOLS_ERR_DENIED;
  *out = -1;
  if (!parts) {
    if (directory >= 0) close(directory);
    return ASTOOLS_ERR_NOMEM;
  }
  if (directory < 0) goto done;
  char *part = parts;
  if (*part == '/') part++;
  for (;;) {
    char *end = strchr(part, '/');
    if (end) *end = 0;
    if (!*part || !strcmp(part, ".") || !strcmp(part, "..")) goto done;
    fd = openat(directory, part,
                O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK |
                    (end || as_directory ? O_DIRECTORY : 0));
    if (fd < 0) {
      e = errno == ENOENT ? ASTOOLS_ERR_NOT_FOUND : ASTOOLS_ERR_DENIED;
      goto done;
    }
    if (!end) {
      *out = fd;
      fd = -1;
      e = ASTOOLS_OK;
      break;
    }
    close(directory);
    directory = fd;
    fd = -1;
    part = end + 1;
  }
done:
  if (fd >= 0) close(fd);
  if (directory >= 0) close(directory);
  free(parts);
  return e;
}

astools_err astools_lsp_file_read(const char *root, const char *path,
                                  const astools_effective *grants, astools_lsp_file *out) {
  memset(out, 0, sizeof *out);
  if (!root || root[0] != '/' || !path || !grants || !astools_path_under(path, root) ||
      !strcmp(path, root) || strlen(path) >= 4096)
    return ASTOOLS_ERR_DENIED;
  int allowed = 0;
  for (size_t i = 0; i < grants->fs_len; i++)
    if ((grants->fs[i].access & ASTOOLS_ACCESS_READ) &&
        astools_path_under(path, grants->fs[i].path))
      allowed = 1;
  if (!allowed) return ASTOOLS_ERR_DENIED;
  int anchor = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC), directory = -1, fd = -1;
  if (anchor < 0) return ASTOOLS_ERR_IO;
  astools_err e = ASTOOLS_OK;
  if (!strcmp(root, "/")) {
    directory = dup(anchor);
    if (directory < 0) e = ASTOOLS_ERR_IO;
  } else e = open_file(anchor, root, 1, &directory);
  close(anchor);
  if (e != ASTOOLS_OK) return e;
  const char *relative = path + strlen(root);
  e = open_file(directory, relative, 0, &fd);
  if (e == ASTOOLS_OK) e = read_file(fd, out);
  if (e == ASTOOLS_OK) {
    struct stat opened, current;
    int check = -1;
    e = open_file(directory, relative, 0, &check);
    if (e == ASTOOLS_OK) {
      if (fstat(fd, &opened) || fstat(check, &current)) e = ASTOOLS_ERR_IO;
      else if (!same(&opened, &current)) e = ASTOOLS_ERR_BUSY;
    } else if (e != ASTOOLS_ERR_NOMEM) e = ASTOOLS_ERR_BUSY;
    if (check >= 0) close(check);
  }
  if (e == ASTOOLS_OK) {
    out->path = astools_strdup(path);
    out->uri = astools_lsp_uri(path);
    if (!out->path || !out->uri) e = ASTOOLS_ERR_NOMEM;
  }
  if (fd >= 0) close(fd);
  close(directory);
  if (e != ASTOOLS_OK) astools_lsp_file_free(out);
  return e;
}
#else
astools_err astools_lsp_file_read(const char *root, const char *path,
                                  const astools_effective *grants, astools_lsp_file *out) {
  (void)root;
  (void)path;
  (void)grants;
  memset(out, 0, sizeof *out);
  return ASTOOLS_ERR_UNSUPPORTED;
}
#endif

void astools_lsp_file_free(astools_lsp_file *file) {
  free(file->path);
  free(file->uri);
  free(file->text);
  memset(file, 0, sizeof *file);
}
