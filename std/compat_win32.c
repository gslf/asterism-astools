/*
 * compat_win32.c — Win32 backing of the POSIX surface in compat_win32.h.
 *
 * Everything crosses this boundary as UTF-8 and is converted to UTF-16
 * for the wide Win32 APIs; returned names/paths are converted back with
 * '\\' folded to '/', matching the canonical form the runtime uses.
 * Reparse points (symlinks AND junctions) are reported as S_IFLNK by
 * lstat so the traversal code never follows either out of the granted
 * tree.
 */

#define ASTD_COMPAT_IMPL 1
#include "compat_win32.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>

/* ---- UTF-8 <-> UTF-16 ---------------------------------------------------- */

static wchar_t *u8_to_wide(const char *s) {
  int n;
  wchar_t *w;
  if (!s) return NULL;
  n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
  if (n <= 0) return NULL;
  w = malloc((size_t)n * sizeof(wchar_t));
  if (!w) return NULL;
  if (MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) <= 0) {
    free(w);
    return NULL;
  }
  return w;
}

static char *wide_to_u8(const wchar_t *w) {
  int n;
  char *s;
  if (!w) return NULL;
  n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
  if (n <= 0) return NULL;
  s = malloc((size_t)n);
  if (!s) return NULL;
  if (WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL) <= 0) {
    free(s);
    return NULL;
  }
  return s;
}

/* ---- errno mapping ------------------------------------------------------- */

static int err_to_errno(DWORD e) {
  switch (e) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_NAME:
      return ENOENT;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
      return EACCES;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS:
      return EEXIST;
    case ERROR_DIR_NOT_EMPTY:
      return ENOTEMPTY;
    case ERROR_DIRECTORY:
      return ENOTDIR;
    case ERROR_NOT_SAME_DEVICE:
      return EXDEV;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
      return ENOMEM;
    case ERROR_PRIVILEGE_NOT_HELD:
      return EPERM;
    default:
      return EIO;
  }
}

/* FILETIME (100ns since 1601) -> unix seconds */
static long long ft_to_unix(const FILETIME *ft) {
  ULARGE_INTEGER u;
  u.LowPart = ft->dwLowDateTime;
  u.HighPart = ft->dwHighDateTime;
  return (long long)((u.QuadPart - 116444736000000000ULL) / 10000000ULL);
}

static unsigned int attrs_to_mode(DWORD attrs) {
  unsigned int mode;
  if (attrs & FILE_ATTRIBUTE_REPARSE_POINT)
    mode = S_IFLNK | 0777;
  else if (attrs & FILE_ATTRIBUTE_DIRECTORY)
    mode = 0x4000 | 0755;
  else
    mode = 0x8000 | ((attrs & FILE_ATTRIBUTE_READONLY) ? 0444 : 0644);
  return mode;
}

/* ---- fd I/O -------------------------------------------------------------- */

int astd_c_open(const char *path, int flags, ...) {
  wchar_t *w;
  DWORD access, create, flattr = FILE_ATTRIBUTE_NORMAL;
  HANDLE h;
  int osf = 0, fd;

  w = u8_to_wide(path);
  if (!w) {
    errno = ENOMEM;
    return -1;
  }
  if (flags & O_NOFOLLOW) {
    /* Fast rejection for an existing final reparse point. The open below
     * also uses OPEN_REPARSE_POINT and validates the resulting handle, so a
     * swap between this check and CreateFileW cannot make us follow it. */
    DWORD attrs = GetFileAttributesW(w);
    if (attrs != INVALID_FILE_ATTRIBUTES &&
        (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) {
      free(w);
      errno = ELOOP;
      return -1;
    }
  }
  if (flags & (O_WRONLY | O_RDWR)) {
    access = (flags & O_APPEND) ? FILE_APPEND_DATA : GENERIC_WRITE;
    if (flags & O_RDWR) access |= GENERIC_READ;
  } else {
    access = GENERIC_READ;
  }
  if ((flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL))
    create = CREATE_NEW;
  else if ((flags & (O_CREAT | O_TRUNC)) == (O_CREAT | O_TRUNC))
    create = CREATE_ALWAYS;
  else if (flags & O_CREAT)
    create = OPEN_ALWAYS;
  else if (flags & O_TRUNC)
    create = TRUNCATE_EXISTING;
  else
    create = OPEN_EXISTING;

  if (flags & O_NOFOLLOW) flattr |= FILE_FLAG_OPEN_REPARSE_POINT;
  h = CreateFileW(w, access,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  NULL, create, flattr, NULL);
  if (h == INVALID_HANDLE_VALUE && GetLastError() == ERROR_ACCESS_DENIED &&
      access == GENERIC_READ) {
    /* directories need BACKUP_SEMANTICS; let the caller's fstat see the
     * directory bit and produce its own is-a-directory error */
    DWORD attrs = GetFileAttributesW(w);
    if (attrs != INVALID_FILE_ATTRIBUTES &&
        (attrs & FILE_ATTRIBUTE_DIRECTORY))
      h = CreateFileW(w, access,
                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                      NULL, create,
                      FILE_FLAG_BACKUP_SEMANTICS |
                          ((flags & O_NOFOLLOW)
                               ? FILE_FLAG_OPEN_REPARSE_POINT
                               : 0),
                      NULL);
  }
  free(w);
  if (h == INVALID_HANDLE_VALUE) {
    errno = err_to_errno(GetLastError());
    return -1;
  }
  if (flags & O_NOFOLLOW) {
    FILE_ATTRIBUTE_TAG_INFO tag = {0};
    if (!GetFileInformationByHandleEx(h, FileAttributeTagInfo, &tag,
                                      sizeof tag) ||
        (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
      DWORD err = GetLastError();
      CloseHandle(h);
      errno = (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
                  ? ELOOP
                  : err_to_errno(err);
      return -1;
    }
  }
  if (flags & O_APPEND) osf |= _O_APPEND;
  fd = _open_osfhandle((intptr_t)h, osf);
  if (fd < 0) {
    CloseHandle(h);
    errno = EMFILE;
    return -1;
  }
  return fd;
}

ssize_t astd_c_read(int fd, void *buf, size_t n) {
  if (n > INT_MAX) n = INT_MAX;
  return (ssize_t)_read(fd, buf, (unsigned int)n);
}

ssize_t astd_c_write(int fd, const void *buf, size_t n) {
  if (n > INT_MAX) n = INT_MAX;
  return (ssize_t)_write(fd, buf, (unsigned int)n);
}

/* ---- stat family --------------------------------------------------------- */

int astd_c_fstat(int fd, struct astd_c_stat *st) {
  BY_HANDLE_FILE_INFORMATION info;
  HANDLE h = (HANDLE)_get_osfhandle(fd);
  if (h == INVALID_HANDLE_VALUE) {
    errno = EBADF;
    return -1;
  }
  if (!GetFileInformationByHandle(h, &info)) {
    errno = err_to_errno(GetLastError());
    return -1;
  }
  st->st_mode = attrs_to_mode(info.dwFileAttributes &
                              (DWORD)~FILE_ATTRIBUTE_REPARSE_POINT);
  st->st_size =
      ((long long)info.nFileSizeHigh << 32) | (long long)info.nFileSizeLow;
  st->st_mtime = ft_to_unix(&info.ftLastWriteTime);
  return 0;
}

int astd_c_lstat(const char *path, struct astd_c_stat *st) {
  WIN32_FILE_ATTRIBUTE_DATA fa;
  wchar_t *w = u8_to_wide(path);
  if (!w) {
    errno = ENOMEM;
    return -1;
  }
  if (!GetFileAttributesExW(w, GetFileExInfoStandard, &fa)) {
    free(w);
    errno = err_to_errno(GetLastError());
    return -1;
  }
  free(w);
  st->st_mode = attrs_to_mode(fa.dwFileAttributes);
  st->st_size =
      ((long long)fa.nFileSizeHigh << 32) | (long long)fa.nFileSizeLow;
  st->st_mtime = ft_to_unix(&fa.ftLastWriteTime);
  return 0;
}

int astd_c_stat(const char *path, struct astd_c_stat *st) {
  wchar_t *w = u8_to_wide(path);
  HANDLE h;
  BY_HANDLE_FILE_INFORMATION info;
  if (!w) {
    errno = ENOMEM;
    return -1;
  }
  /* 0 access + BACKUP_SEMANTICS opens files, directories and follows
   * symlinks — exactly stat(2) semantics */
  h = CreateFileW(w, 0,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
  free(w);
  if (h == INVALID_HANDLE_VALUE) {
    errno = err_to_errno(GetLastError());
    return -1;
  }
  if (!GetFileInformationByHandle(h, &info)) {
    errno = err_to_errno(GetLastError());
    CloseHandle(h);
    return -1;
  }
  CloseHandle(h);
  st->st_mode = attrs_to_mode(info.dwFileAttributes &
                              (DWORD)~FILE_ATTRIBUTE_REPARSE_POINT);
  st->st_size =
      ((long long)info.nFileSizeHigh << 32) | (long long)info.nFileSizeLow;
  st->st_mtime = ft_to_unix(&info.ftLastWriteTime);
  return 0;
}

int astd_c_access(const char *path, int mode) {
  wchar_t *w = u8_to_wide(path);
  int rc;
  if (!w) {
    errno = ENOMEM;
    return -1;
  }
  rc = _waccess(w, mode == W_OK ? 2 : 0);
  free(w);
  return rc;
}

/* ---- namespace ops ------------------------------------------------------- */

int astd_c_mkdir(const char *path) {
  wchar_t *w = u8_to_wide(path);
  int rc;
  if (!w) {
    errno = ENOMEM;
    return -1;
  }
  rc = _wmkdir(w); /* CRT sets EEXIST / ENOENT */
  free(w);
  return rc;
}

int astd_c_rmdir(const char *path) {
  wchar_t *w = u8_to_wide(path);
  int rc;
  if (!w) {
    errno = ENOMEM;
    return -1;
  }
  rc = _wrmdir(w); /* CRT sets ENOTEMPTY / ENOENT */
  free(w);
  return rc;
}

int astd_c_unlink(const char *path) {
  wchar_t *w = u8_to_wide(path);
  int rc;
  if (!w) {
    errno = ENOMEM;
    return -1;
  }
  rc = _wunlink(w);
  if (rc != 0 && errno == EACCES) {
    /* POSIX unlink removes read-only files when the directory allows it;
     * clear the attribute and retry once */
    DWORD attrs = GetFileAttributesW(w);
    if (attrs != INVALID_FILE_ATTRIBUTES &&
        (attrs & FILE_ATTRIBUTE_READONLY)) {
      SetFileAttributesW(w, attrs & ~(DWORD)FILE_ATTRIBUTE_READONLY);
      rc = _wunlink(w);
    }
  }
  free(w);
  return rc;
}

int astd_c_rename(const char *from, const char *to) {
  wchar_t *wf = u8_to_wide(from), *wt = u8_to_wide(to);
  BOOL ok;
  if (!wf || !wt) {
    free(wf);
    free(wt);
    errno = ENOMEM;
    return -1;
  }
  /* POSIX rename replaces an existing destination atomically; no
   * MOVEFILE_COPY_ALLOWED — cross-volume moves must fail with EXDEV so
   * the caller's copy+unlink fallback runs */
  ok = MoveFileExW(wf, wt, MOVEFILE_REPLACE_EXISTING);
  free(wf);
  free(wt);
  if (!ok) {
    errno = err_to_errno(GetLastError());
    return -1;
  }
  return 0;
}

int astd_c_symlink(const char *target, const char *linkpath) {
  wchar_t *wt = u8_to_wide(target), *wl = u8_to_wide(linkpath);
  DWORD flags = 0x2 /* SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE */;
  DWORD attrs;
  BOOL ok;
  if (!wt || !wl) {
    free(wt);
    free(wl);
    errno = ENOMEM;
    return -1;
  }
  attrs = GetFileAttributesW(wt); /* best effort: relative targets may miss */
  if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY))
    flags |= SYMBOLIC_LINK_FLAG_DIRECTORY;
  ok = CreateSymbolicLinkW(wl, wt, flags);
  if (!ok && (flags & 0x2)) /* older Windows rejects the new flag */
    ok = CreateSymbolicLinkW(wl, wt, flags & ~(DWORD)0x2);
  free(wt);
  free(wl);
  if (!ok) {
    errno = err_to_errno(GetLastError());
    return -1;
  }
  return 0;
}

/* Minimal REPARSE_DATA_BUFFER (ntifs.h is a driver-kit header) */
typedef struct {
  ULONG ReparseTag;
  USHORT ReparseDataLength;
  USHORT Reserved;
  union {
    struct {
      USHORT SubstituteNameOffset;
      USHORT SubstituteNameLength;
      USHORT PrintNameOffset;
      USHORT PrintNameLength;
      ULONG Flags;
      WCHAR PathBuffer[1];
    } SymbolicLink;
    struct {
      USHORT SubstituteNameOffset;
      USHORT SubstituteNameLength;
      USHORT PrintNameOffset;
      USHORT PrintNameLength;
      WCHAR PathBuffer[1];
    } MountPoint;
  } u;
} astd_reparse_data;

ssize_t astd_c_readlink(const char *path, char *buf, size_t cap) {
  wchar_t *w = u8_to_wide(path);
  HANDLE h;
  char raw[MAXIMUM_REPARSE_DATA_BUFFER_SIZE];
  astd_reparse_data *rd = (astd_reparse_data *)raw;
  DWORD got = 0;
  const WCHAR *nm = NULL;
  USHORT nlen = 0;
  wchar_t *tmp;
  char *u8;
  size_t n;

  if (!w) {
    errno = ENOMEM;
    return -1;
  }
  h = CreateFileW(w, 0,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  NULL, OPEN_EXISTING,
                  FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                  NULL);
  free(w);
  if (h == INVALID_HANDLE_VALUE) {
    errno = err_to_errno(GetLastError());
    return -1;
  }
  if (!DeviceIoControl(h, FSCTL_GET_REPARSE_POINT, NULL, 0, raw, sizeof raw,
                       &got, NULL)) {
    CloseHandle(h);
    errno = EINVAL; /* not a reparse point */
    return -1;
  }
  CloseHandle(h);
  if (rd->ReparseTag == IO_REPARSE_TAG_SYMLINK) {
    nm = rd->u.SymbolicLink.PathBuffer +
         rd->u.SymbolicLink.PrintNameOffset / sizeof(WCHAR);
    nlen = rd->u.SymbolicLink.PrintNameLength;
    if (nlen == 0) {
      nm = rd->u.SymbolicLink.PathBuffer +
           rd->u.SymbolicLink.SubstituteNameOffset / sizeof(WCHAR);
      nlen = rd->u.SymbolicLink.SubstituteNameLength;
    }
  } else if (rd->ReparseTag == IO_REPARSE_TAG_MOUNT_POINT) {
    nm = rd->u.MountPoint.PathBuffer +
         rd->u.MountPoint.PrintNameOffset / sizeof(WCHAR);
    nlen = rd->u.MountPoint.PrintNameLength;
    if (nlen == 0) {
      nm = rd->u.MountPoint.PathBuffer +
           rd->u.MountPoint.SubstituteNameOffset / sizeof(WCHAR);
      nlen = rd->u.MountPoint.SubstituteNameLength;
    }
  } else {
    errno = EINVAL;
    return -1;
  }
  tmp = malloc((size_t)(nlen / sizeof(WCHAR)) * sizeof(WCHAR) +
               sizeof(WCHAR));
  if (!tmp) {
    errno = ENOMEM;
    return -1;
  }
  memcpy(tmp, nm, nlen);
  tmp[nlen / sizeof(WCHAR)] = L'\0';
  u8 = wide_to_u8(tmp);
  free(tmp);
  if (!u8) {
    errno = ENOMEM;
    return -1;
  }
  { /* strip the NT "\??\" lead-in of substitute names, fold separators */
    char *p = u8;
    if (strncmp(p, "\\??\\", 4) == 0) memmove(p, p + 4, strlen(p + 4) + 1);
    for (; *p; p++)
      if (*p == '\\') *p = '/';
  }
  n = strlen(u8);
  if (n > cap) n = cap; /* readlink(2) truncates without a NUL */
  memcpy(buf, u8, n);
  free(u8);
  return (ssize_t)n;
}

char *astd_c_realpath(const char *path, char *resolved) {
  wchar_t *w, *full;
  DWORD need, got;
  char *u8, *p;

  (void)resolved; /* callers pass NULL (GNU malloc'd form) */
  w = u8_to_wide(path);
  if (!w) {
    errno = ENOMEM;
    return NULL;
  }
  need = GetFullPathNameW(w, 0, NULL, NULL);
  if (need == 0) {
    free(w);
    errno = err_to_errno(GetLastError());
    return NULL;
  }
  full = malloc(((size_t)need + 1) * sizeof(wchar_t));
  if (!full) {
    free(w);
    errno = ENOMEM;
    return NULL;
  }
  got = GetFullPathNameW(w, need + 1, full, NULL);
  free(w);
  if (got == 0 || got > need) {
    free(full);
    errno = EIO;
    return NULL;
  }
  u8 = wide_to_u8(full);
  free(full);
  if (!u8) {
    errno = ENOMEM;
    return NULL;
  }
  for (p = u8; *p; p++)
    if (*p == '\\') *p = '/';
  return u8;
}

/* ---- dirent -------------------------------------------------------------- */

struct astd_c_dir {
  HANDLE h;
  WIN32_FIND_DATAW fd;
  int pending; /* first entry from FindFirstFileW not yet returned */
  struct dirent ent;
};

DIR *opendir(const char *path) {
  size_t n = strlen(path);
  char *pat = malloc(n + 3);
  wchar_t *w;
  DIR *d;
  if (!pat) {
    errno = ENOMEM;
    return NULL;
  }
  memcpy(pat, path, n);
  while (n > 0 && (pat[n - 1] == '/' || pat[n - 1] == '\\')) n--;
  pat[n] = '/';
  pat[n + 1] = '*';
  pat[n + 2] = '\0';
  w = u8_to_wide(pat);
  free(pat);
  if (!w) {
    errno = ENOMEM;
    return NULL;
  }
  d = calloc(1, sizeof *d);
  if (!d) {
    free(w);
    errno = ENOMEM;
    return NULL;
  }
  d->h = FindFirstFileW(w, &d->fd);
  free(w);
  if (d->h == INVALID_HANDLE_VALUE) {
    DWORD e = GetLastError();
    free(d);
    /* an empty directory still yields "." and ".." — NO_MORE_FILES here
     * means the path pattern itself failed */
    errno = e == ERROR_DIRECTORY ? ENOTDIR : err_to_errno(e);
    return NULL;
  }
  d->pending = 1;
  return d;
}

struct dirent *readdir(DIR *d) {
  char *u8;
  size_t len;
  if (!d) return NULL;
  for (;;) {
    if (d->pending) {
      d->pending = 0;
    } else if (!FindNextFileW(d->h, &d->fd)) {
      return NULL;
    }
    u8 = wide_to_u8(d->fd.cFileName);
    if (!u8) continue; /* unconvertible name: skip, never fabricate */
    len = strlen(u8);
    if (len >= sizeof d->ent.d_name) {
      free(u8);
      continue;
    }
    memcpy(d->ent.d_name, u8, len + 1);
    free(u8);
    return &d->ent;
  }
}

int closedir(DIR *d) {
  if (!d) return -1;
  FindClose(d->h);
  free(d);
  return 0;
}

/* ---- spawn + capture ----------------------------------------------------- */

int64_t astd_mono_ms(void) { return (int64_t)GetTickCount64(); }

/* argv -> one command line under the MS C runtime parsing rules */
static int cl_append_arg(char **buf, size_t *n, size_t *cap, const char *arg) {
  size_t need = strlen(arg) * 2 + 4;
  size_t i, bs;
  char *p;
  if (*cap - *n < need + 2) {
    size_t nc = *cap ? *cap : 256;
    while (nc - *n < need + 2) {
      if (nc > (size_t)-1 / 2) return -1;
      nc *= 2;
    }
    p = realloc(*buf, nc);
    if (!p) return -1;
    *buf = p;
    *cap = nc;
  }
  p = *buf;
  if (*n > 0) p[(*n)++] = ' ';
  if (arg[0] != '\0' && !strpbrk(arg, " \t\"")) {
    size_t l = strlen(arg);
    memcpy(p + *n, arg, l);
    *n += l;
  } else {
    p[(*n)++] = '"';
    bs = 0;
    for (i = 0; arg[i] != '\0'; i++) {
      if (arg[i] == '\\') {
        bs++;
      } else if (arg[i] == '"') {
        size_t k;
        for (k = 0; k < bs * 2 + 1; k++) p[(*n)++] = '\\';
        p[(*n)++] = '"';
        bs = 0;
      } else {
        size_t k;
        for (k = 0; k < bs; k++) p[(*n)++] = '\\';
        bs = 0;
        p[(*n)++] = arg[i];
      }
    }
    {
      size_t k;
      for (k = 0; k < bs * 2; k++) p[(*n)++] = '\\';
    }
    p[(*n)++] = '"';
  }
  p[*n] = '\0';
  return 0;
}

static wchar_t *build_cmdline(char *const *argv) {
  char *cl = NULL;
  size_t n = 0, cap = 0, i;
  wchar_t *w;
  for (i = 0; argv[i] != NULL; i++) {
    if (cl_append_arg(&cl, &n, &cap, argv[i]) != 0) {
      free(cl);
      return NULL;
    }
  }
  if (!cl) return NULL;
  w = u8_to_wide(cl);
  free(cl);
  return w;
}

static wchar_t *build_envblock(char *const *envp) {
  size_t total = 1, i;
  wchar_t *blk, *at;
  if (!envp) return NULL;
  for (i = 0; envp[i] != NULL; i++) {
    int wn = MultiByteToWideChar(CP_UTF8, 0, envp[i], -1, NULL, 0);
    if (wn <= 0) return NULL;
    total += (size_t)wn;
  }
  blk = malloc((total + 1) * sizeof(wchar_t));
  if (!blk) return NULL;
  at = blk;
  for (i = 0; envp[i] != NULL; i++) {
    int wn = MultiByteToWideChar(CP_UTF8, 0, envp[i], -1, at,
                                 (int)(total - (size_t)(at - blk)));
    if (wn <= 0) {
      free(blk);
      return NULL;
    }
    at += wn;
  }
  *at++ = L'\0';
  return blk;
}

typedef struct {
  HANDLE h;
  size_t cap;
  char *buf;
  size_t n;
  size_t alloc;
  int trunc;
  int oom;
} pump_read;

static unsigned __stdcall pump_read_main(void *arg) {
  pump_read *p = arg;
  char tmp[8192];
  DWORD got;
  while (ReadFile(p->h, tmp, sizeof tmp, &got, NULL) && got > 0) {
    size_t keep = 0;
    if (p->n < p->cap) {
      keep = p->cap - p->n;
      if (keep > got) keep = got;
    }
    if (keep > 0) {
      if (p->n + keep + 1 > p->alloc) {
        size_t nc = p->alloc ? p->alloc : 4096;
        char *np;
        while (nc < p->n + keep + 1) nc *= 2;
        np = realloc(p->buf, nc);
        if (!np) {
          p->oom = 1;
          return 0;
        }
        p->buf = np;
        p->alloc = nc;
      }
      memcpy(p->buf + p->n, tmp, keep);
      p->n += keep;
      p->buf[p->n] = '\0';
    }
    if (keep < got) p->trunc = 1; /* keep draining until EOF */
  }
  return 0;
}

typedef struct {
  HANDLE h; /* owned: closed when done */
  const char *data;
  size_t n;
} pump_write;

static unsigned __stdcall pump_write_main(void *arg) {
  pump_write *p = arg;
  size_t off = 0;
  while (off < p->n) {
    DWORD chunk = p->n - off > 65536 ? 65536 : (DWORD)(p->n - off);
    DWORD put;
    if (!WriteFile(p->h, p->data + off, chunk, &put, NULL)) break;
    off += put;
  }
  CloseHandle(p->h);
  return 0;
}

static void np_close(HANDLE *h) {
  if (*h != NULL && *h != INVALID_HANDLE_VALUE) CloseHandle(*h);
  *h = NULL;
}

int astd_spawn_capture(char *const *argv, char *const *envp, const char *cwd,
                       const char *input, size_t input_n, size_t out_cap,
                       size_t err_cap, int64_t timeout_ms, astd_spawn_res *rr,
                       char *emsg, size_t emsg_sz) {
  SECURITY_ATTRIBUTES sa;
  HANDLE in_rd = NULL, in_wr = NULL;
  HANDLE out_rd = NULL, out_wr = NULL;
  HANDLE err_rd = NULL, err_wr = NULL;
  wchar_t *cmdline = NULL, *envblk = NULL, *wcwd = NULL;
  STARTUPINFOW si;
  PROCESS_INFORMATION pi;
  pump_read pout, perr;
  pump_write pin;
  HANDLE threads[3];
  int nthreads = 0;
  DWORD wait, code = 0;

  memset(rr, 0, sizeof *rr);
  memset(&pout, 0, sizeof pout);
  memset(&perr, 0, sizeof perr);
  memset(&pin, 0, sizeof pin);
  memset(&pi, 0, sizeof pi);

  sa.nLength = sizeof sa;
  sa.lpSecurityDescriptor = NULL;
  sa.bInheritHandle = TRUE;

  if (input && input_n > 0) {
    if (!CreatePipe(&in_rd, &in_wr, &sa, 0)) goto os_fail;
    SetHandleInformation(in_wr, HANDLE_FLAG_INHERIT, 0);
  } else {
    in_rd = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        &sa, OPEN_EXISTING, 0, NULL);
    if (in_rd == INVALID_HANDLE_VALUE) goto os_fail;
  }
  if (!CreatePipe(&out_rd, &out_wr, &sa, 0)) goto os_fail;
  SetHandleInformation(out_rd, HANDLE_FLAG_INHERIT, 0);
  if (!CreatePipe(&err_rd, &err_wr, &sa, 0)) goto os_fail;
  SetHandleInformation(err_rd, HANDLE_FLAG_INHERIT, 0);

  cmdline = build_cmdline(argv);
  if (!cmdline) goto oom_fail;
  if (envp) {
    envblk = build_envblock(envp);
    if (!envblk) goto oom_fail;
  }
  if (cwd) {
    wcwd = u8_to_wide(cwd);
    if (!wcwd) goto oom_fail;
  }

  memset(&si, 0, sizeof si);
  si.cb = sizeof si;
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = in_rd;
  si.hStdOutput = out_wr;
  si.hStdError = err_wr;

  if (!CreateProcessW(NULL, cmdline, NULL, NULL, TRUE,
                      CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW, envblk,
                      wcwd, &si, &pi)) {
    DWORD e = GetLastError();
    snprintf(emsg, emsg_sz, "cannot execute '%s': %s", argv[0],
             e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND
                 ? "not found"
                 : "spawn failed");
    goto spawn_fail;
  }
  CloseHandle(pi.hThread);
  free(cmdline);
  cmdline = NULL;
  free(envblk);
  envblk = NULL;
  free(wcwd);
  wcwd = NULL;
  np_close(&in_rd);
  np_close(&out_wr);
  np_close(&err_wr);

  pout.h = out_rd;
  pout.cap = out_cap;
  perr.h = err_rd;
  perr.cap = err_cap;
  threads[nthreads] = (HANDLE)_beginthreadex(NULL, 0, pump_read_main, &pout,
                                             0, NULL);
  if (threads[nthreads]) nthreads++;
  threads[nthreads] = (HANDLE)_beginthreadex(NULL, 0, pump_read_main, &perr,
                                             0, NULL);
  if (threads[nthreads]) nthreads++;
  if (in_wr != NULL) {
    pin.h = in_wr;
    in_wr = NULL; /* ownership moves to the writer thread */
    pin.data = input;
    pin.n = input_n;
    threads[nthreads] = (HANDLE)_beginthreadex(NULL, 0, pump_write_main, &pin,
                                               0, NULL);
    if (threads[nthreads])
      nthreads++;
    else
      CloseHandle(pin.h);
  }

  wait = WaitForSingleObject(pi.hProcess,
                             timeout_ms > 0 ? (DWORD)timeout_ms : INFINITE);
  if (wait == WAIT_TIMEOUT) {
    TerminateProcess(pi.hProcess, 137);
    WaitForSingleObject(pi.hProcess, INFINITE);
    rr->timed_out = 1;
  }
  /* grandchildren may hold the pipes open past the child's exit; give the
   * pumps a grace period, then cancel their blocked reads */
  if (nthreads > 0 &&
      WaitForMultipleObjects((DWORD)nthreads, threads, TRUE, 2000) ==
          WAIT_TIMEOUT) {
    int i;
    for (i = 0; i < nthreads; i++) CancelSynchronousIo(threads[i]);
    WaitForMultipleObjects((DWORD)nthreads, threads, TRUE, 1000);
  }
  {
    int i;
    for (i = 0; i < nthreads; i++) CloseHandle(threads[i]);
  }
  np_close(&out_rd);
  np_close(&err_rd);

  if (rr->timed_out) {
    rr->exit_code = -9; /* SIGKILL parity with the POSIX build */
  } else {
    GetExitCodeProcess(pi.hProcess, &code);
    rr->exit_code = (int)code;
  }
  CloseHandle(pi.hProcess);

  if (pout.oom || perr.oom) {
    free(pout.buf);
    free(perr.buf);
    snprintf(emsg, emsg_sz, "out of memory");
    return -1;
  }
  rr->out = pout.buf ? pout.buf : calloc(1, 1);
  rr->out_n = pout.n;
  rr->out_trunc = pout.trunc;
  rr->err = perr.buf ? perr.buf : calloc(1, 1);
  rr->err_n = perr.n;
  rr->err_trunc = perr.trunc;
  if (!rr->out || !rr->err) {
    free(rr->out);
    free(rr->err);
    rr->out = rr->err = NULL;
    snprintf(emsg, emsg_sz, "out of memory");
    return -1;
  }
  return 0;

os_fail:
  snprintf(emsg, emsg_sz, "pipe: windows error %lu",
           (unsigned long)GetLastError());
  goto spawn_fail;
oom_fail:
  snprintf(emsg, emsg_sz, "out of memory");
spawn_fail:
  free(cmdline);
  free(envblk);
  free(wcwd);
  np_close(&in_rd);
  np_close(&in_wr);
  np_close(&out_rd);
  np_close(&out_wr);
  np_close(&err_rd);
  np_close(&err_wr);
  if (pi.hProcess) CloseHandle(pi.hProcess);
  return -1;
}
