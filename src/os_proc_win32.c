/*
 * os_proc_win32.c — Win32 backend of the astools process/path extension
 * of os.h: stat/realpath helpers, directory walks, child processes over
 * three pipes inside a Job Object, LoadLibrary.
 *
 * Mapping onto os_proc (see os.h):
 *   pid    — Windows process id
 *   handle — process HANDLE (never recycled while open, so no reap race)
 *   pgid   — Job Object HANDLE; the "kill the whole tree" token that a
 *            process group is on POSIX. Every spawn puts the child in its
 *            own job with JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE, so a tool
 *            tree can survive neither os_proc_kill nor a crashed host.
 *   pgroup — 1 when the job assignment succeeded
 *   fd_*   — CRT descriptors wrapping the pipe HANDLEs (_open_osfhandle),
 *            so the int fields of the shared struct stay meaningful;
 *            Win32 calls recover the HANDLE with _get_osfhandle.
 *
 * Nonblocking pipe semantics: the stdout/stderr read ends are plain
 * anonymous pipes probed with PeekNamedPipe (no data + alive => BUSY,
 * broken => EOF). The stdin write end is an overlapped named pipe; a
 * write that cannot complete against the pipe buffer is cancelled and
 * reported as BUSY/partial, so a tool that stops reading can never block
 * the worker thread past its deadline slices. os_proc_poll reports stdin
 * writability optimistically (the buffer is large and the write path is
 * nonblocking); the worst case is a bounded busy-poll at the worker's
 * slice cadence while the pipe is full.
 *
 * Resource limits map onto the job: limit_cpu_seconds =>
 * JOB_OBJECT_LIMIT_PROCESS_TIME (per-process user time, inherited by
 * descendants), limit_mem_bytes => JOB_OBJECT_LIMIT_PROCESS_MEMORY,
 * limit_nproc => JOB_OBJECT_LIMIT_ACTIVE_PROCESS (absolute per-tree cap,
 * unlike the per-uid POSIX rlimit).
 *
 * Exit codes: os_proc_kill terminates the job with (UINT)-9, so a killed
 * tool reports exit_code -9 exactly like SIGKILL does on POSIX; NTSTATUS
 * crash codes (0xC...) surface as other negative values.
 */

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00 /* Windows 10 */
#endif

#include "os.h"

#include <windows.h>

#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Pipe buffer per stream; large enough that request writes practically
 * always complete in one shot. */
#define OS_PIPE_BUF_BYTES (1u << 20)
/* Cap one os_proc_write_stdin transfer so partial progress stays visible
 * to the caller's write loop. */
#define OS_WRITE_CHUNK_BYTES (1u << 16)

/* ---- small local helpers ------------------------------------------------ */

static char *dup_str(const char *s) {
  size_t n;
  char *p;
  if (!s) return NULL;
  n = strlen(s);
  p = malloc(n + 1);
  if (!p) return NULL;
  memcpy(p, s, n + 1);
  return p;
}

static void names_free(char **names, size_t n) {
  size_t i;
  for (i = 0; i < n; i++) free(names[i]);
  free(names);
}

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

static int err_is_missing(DWORD e) {
  return e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ||
         e == ERROR_INVALID_NAME || e == ERROR_BAD_NETPATH ||
         e == ERROR_INVALID_DRIVE || e == ERROR_DIRECTORY ||
         e == ERROR_NOT_READY;
}

static HANDLE fd_handle(int fd) {
  if (fd < 0) return INVALID_HANDLE_VALUE;
  return (HANDLE)_get_osfhandle(fd);
}

static void fd_close(int *fd) {
  if (*fd >= 0) {
    (void)_close(*fd); /* closes the underlying HANDLE too */
    *fd = -1;
  }
}

static int64_t filetime_unix(const FILETIME *ft) {
  ULARGE_INTEGER u;
  u.LowPart = ft->dwLowDateTime;
  u.HighPart = ft->dwHighDateTime;
  /* 100ns ticks since 1601-01-01 -> unix seconds */
  return (int64_t)(u.QuadPart / 10000000ULL) - 11644473600LL;
}

/* ---- paths and stat ------------------------------------------------------ */

astools_err os_stat(const char *path, os_stat_info *out) {
  wchar_t *w;
  HANDLE h;
  BY_HANDLE_FILE_INFORMATION info;

  if (!path || !out) return ASTOOLS_ERR_INVALID;
  memset(out, 0, sizeof(*out));
  w = u8_to_wide(path);
  if (!w) return ASTOOLS_ERR_NOMEM;
  /* dwDesiredAccess 0 queries metadata without an access check; the open
   * follows symlinks, matching stat(2). */
  h = CreateFileW(w, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
  free(w);
  if (h == INVALID_HANDLE_VALUE) {
    DWORD e = GetLastError();
    if (err_is_missing(e)) {
      out->type = OS_FT_MISSING;
      return ASTOOLS_OK;
    }
    return ASTOOLS_ERR_IO;
  }
  if (!GetFileInformationByHandle(h, &info)) {
    CloseHandle(h);
    return ASTOOLS_ERR_IO;
  }
  CloseHandle(h);
  if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
    out->type = OS_FT_DIR;
  else
    out->type = OS_FT_FILE;
  out->size = ((uint64_t)info.nFileSizeHigh << 32) | info.nFileSizeLow;
  out->mtime_unix = filetime_unix(&info.ftLastWriteTime);
  out->readonly = (info.dwFileAttributes & FILE_ATTRIBUTE_READONLY) ? 1 : 0;
  return ASTOOLS_OK;
}

astools_err os_lstat(const char *path, os_stat_info *out) {
  wchar_t *w;
  WIN32_FILE_ATTRIBUTE_DATA fad;

  if (!path || !out) return ASTOOLS_ERR_INVALID;
  memset(out, 0, sizeof(*out));
  w = u8_to_wide(path);
  if (!w) return ASTOOLS_ERR_NOMEM;
  /* GetFileAttributesEx reports the link itself, matching lstat(2);
   * junctions count as symlinks for the "never descend" contract. */
  if (!GetFileAttributesExW(w, GetFileExInfoStandard, &fad)) {
    DWORD e = GetLastError();
    free(w);
    if (err_is_missing(e)) {
      out->type = OS_FT_MISSING;
      return ASTOOLS_OK;
    }
    return ASTOOLS_ERR_IO;
  }
  free(w);
  if (fad.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
    out->type = OS_FT_SYMLINK;
  else if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
    out->type = OS_FT_DIR;
  else
    out->type = OS_FT_FILE;
  out->size = ((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
  out->mtime_unix = filetime_unix(&fad.ftLastWriteTime);
  out->readonly = (fad.dwFileAttributes & FILE_ATTRIBUTE_READONLY) ? 1 : 0;
  return ASTOOLS_OK;
}

astools_err os_realpath(const char *path, char **out) {
  wchar_t *w, *buf;
  HANDLE h;
  DWORD need, got;
  const wchar_t *from;

  if (!path || !out) return ASTOOLS_ERR_INVALID;
  *out = NULL;
  w = u8_to_wide(path);
  if (!w) return ASTOOLS_ERR_NOMEM;
  h = CreateFileW(w, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
  free(w);
  if (h == INVALID_HANDLE_VALUE) {
    DWORD e = GetLastError();
    if (err_is_missing(e)) return ASTOOLS_ERR_NOT_FOUND;
    return ASTOOLS_ERR_IO;
  }
  need = GetFinalPathNameByHandleW(h, NULL, 0,
                                   FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (need == 0) {
    CloseHandle(h);
    return ASTOOLS_ERR_IO;
  }
  buf = malloc(((size_t)need + 1) * sizeof(wchar_t));
  if (!buf) {
    CloseHandle(h);
    return ASTOOLS_ERR_NOMEM;
  }
  got = GetFinalPathNameByHandleW(h, buf, need + 1,
                                  FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  CloseHandle(h);
  if (got == 0 || got > need) {
    free(buf);
    return ASTOOLS_ERR_IO;
  }
  /* Strip the NT namespace lead-in: "\\?\C:\x" -> "C:\x",
   * "\\?\UNC\srv\share" -> "\\srv\share". */
  from = buf;
  if (wcsncmp(buf, L"\\\\?\\UNC\\", 8) == 0) {
    buf[6] = L'\\';
    from = buf + 6;
  } else if (wcsncmp(buf, L"\\\\?\\", 4) == 0) {
    from = buf + 4;
  }
  *out = wide_to_u8(from);
  free(buf);
  if (!*out) return ASTOOLS_ERR_NOMEM;
  /* Canonical paths are '/'-separated on every platform: the containment
   * checks in path.c compare them textually. */
  {
    char *p;
    for (p = *out; *p != '\0'; p++)
      if (*p == '\\') *p = '/';
  }
  return ASTOOLS_OK;
}

int os_path_is_abs(const char *path) {
  if (!path || path[0] == '\0') return 0;
  if (path[0] == '/' || path[0] == '\\') return 1;
  if (((path[0] >= 'A' && path[0] <= 'Z') ||
       (path[0] >= 'a' && path[0] <= 'z')) &&
      path[1] == ':')
    return 1;
  return 0;
}

char *os_getcwd_dup(void) {
  DWORD need, got;
  wchar_t *w;
  char *s;

  need = GetCurrentDirectoryW(0, NULL);
  if (need == 0) return NULL;
  w = malloc((size_t)need * sizeof(wchar_t));
  if (!w) return NULL;
  got = GetCurrentDirectoryW(need, w);
  if (got == 0 || got >= need) {
    free(w);
    return NULL;
  }
  s = wide_to_u8(w);
  free(w);
  return s;
}

astools_err os_exe_path(char **out) {
  DWORD cap = 512;

  if (!out) return ASTOOLS_ERR_INVALID;
  *out = NULL;
  for (;;) {
    wchar_t *w = malloc((size_t)cap * sizeof(wchar_t));
    DWORD got;
    if (!w) return ASTOOLS_ERR_NOMEM;
    got = GetModuleFileNameW(NULL, w, cap);
    if (got == 0) {
      free(w);
      return ASTOOLS_ERR_IO;
    }
    if (got < cap) {
      *out = wide_to_u8(w);
      free(w);
      return *out ? ASTOOLS_OK : ASTOOLS_ERR_NOMEM;
    }
    free(w);
    if (cap > 32768) return ASTOOLS_ERR_IO;
    cap *= 2;
  }
}

/* Build "<dir>\*" search pattern (trailing separators trimmed). */
static wchar_t *dir_pattern(const char *path) {
  wchar_t *w, *pat;
  size_t len;

  w = u8_to_wide(path);
  if (!w) return NULL;
  len = wcslen(w);
  while (len > 1 && (w[len - 1] == L'/' || w[len - 1] == L'\\'))
    w[--len] = L'\0';
  pat = malloc((len + 3) * sizeof(wchar_t));
  if (!pat) {
    free(w);
    return NULL;
  }
  memcpy(pat, w, len * sizeof(wchar_t));
  pat[len] = L'\\';
  pat[len + 1] = L'*';
  pat[len + 2] = L'\0';
  free(w);
  return pat;
}

astools_err os_list_subdirs(const char *path, char ***out_names,
                            size_t *out_n) {
  wchar_t *pat;
  HANDLE h;
  WIN32_FIND_DATAW fd;
  char **names = NULL;
  size_t n = 0, cap = 0;

  if (!path || !out_names || !out_n) return ASTOOLS_ERR_INVALID;
  *out_names = NULL;
  *out_n = 0;

  pat = dir_pattern(path);
  if (!pat) return ASTOOLS_ERR_NOMEM;
  h = FindFirstFileW(pat, &fd);
  free(pat);
  if (h == INVALID_HANDLE_VALUE) {
    DWORD e = GetLastError();
    if (err_is_missing(e) || e == ERROR_NO_MORE_FILES)
      return ASTOOLS_OK; /* missing dir => 0 entries */
    return ASTOOLS_ERR_IO;
  }
  do {
    char *name;
    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
    /* A link to a directory is not a subdirectory (POSIX lstat parity). */
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
    if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
      continue;
    name = wide_to_u8(fd.cFileName);
    if (!name) {
      FindClose(h);
      names_free(names, n);
      return ASTOOLS_ERR_NOMEM;
    }
    if (n == cap) {
      size_t ncap = cap == 0 ? 16 : cap * 2;
      char **nn = realloc(names, ncap * sizeof(*nn));
      if (!nn) {
        free(name);
        FindClose(h);
        names_free(names, n);
        return ASTOOLS_ERR_NOMEM;
      }
      names = nn;
      cap = ncap;
    }
    names[n++] = name;
  } while (FindNextFileW(h, &fd));
  FindClose(h);

  *out_names = names;
  *out_n = n;
  return ASTOOLS_OK;
}

/* Depth cap, as in the POSIX backend: bounds both recursion and the number
 * of concurrently open find handles. */
#define OS_RMTREE_MAX_DEPTH 64

/* "<dir>\<name>", both wide. */
static wchar_t *wpath_join(const wchar_t *dir, const wchar_t *name) {
  size_t dl = wcslen(dir), nl = wcslen(name);
  wchar_t *p = malloc((dl + nl + 2) * sizeof(wchar_t));
  if (!p) return NULL;
  memcpy(p, dir, dl * sizeof(wchar_t));
  p[dl] = L'\\';
  memcpy(p + dl + 1, name, (nl + 1) * sizeof(wchar_t));
  return p;
}

static astools_err rm_one(const wchar_t *p, DWORD attrs) {
  if (attrs & FILE_ATTRIBUTE_READONLY)
    (void)SetFileAttributesW(p, attrs & ~(DWORD)FILE_ATTRIBUTE_READONLY);
  if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
    /* Directory link (junction/symlink): removes the link, not the target. */
    if (!RemoveDirectoryW(p) && GetLastError() != ERROR_FILE_NOT_FOUND &&
        GetLastError() != ERROR_PATH_NOT_FOUND)
      return ASTOOLS_ERR_IO;
    return ASTOOLS_OK;
  }
  if (!DeleteFileW(p) && GetLastError() != ERROR_FILE_NOT_FOUND &&
      GetLastError() != ERROR_PATH_NOT_FOUND)
    return ASTOOLS_ERR_IO;
  return ASTOOLS_OK;
}

static astools_err rmtree_wide(const wchar_t *dir, int depth) {
  wchar_t *pat;
  HANDLE h;
  WIN32_FIND_DATAW fd;
  astools_err e = ASTOOLS_OK;
  size_t dl;

  if (depth >= OS_RMTREE_MAX_DEPTH) return ASTOOLS_ERR_IO;

  dl = wcslen(dir);
  pat = malloc((dl + 3) * sizeof(wchar_t));
  if (!pat) return ASTOOLS_ERR_NOMEM;
  memcpy(pat, dir, dl * sizeof(wchar_t));
  pat[dl] = L'\\';
  pat[dl + 1] = L'*';
  pat[dl + 2] = L'\0';
  h = FindFirstFileW(pat, &fd);
  free(pat);
  if (h == INVALID_HANDLE_VALUE) {
    DWORD ge = GetLastError();
    return (err_is_missing(ge) || ge == ERROR_NO_MORE_FILES) ? ASTOOLS_OK
                                                             : ASTOOLS_ERR_IO;
  }
  do {
    wchar_t *child;
    astools_err sub = ASTOOLS_OK;
    if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
      continue;
    child = wpath_join(dir, fd.cFileName);
    if (!child) {
      FindClose(h);
      return ASTOOLS_ERR_NOMEM;
    }
    if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
        !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
      sub = rmtree_wide(child, depth + 1);
      if (sub == ASTOOLS_OK) sub = rm_one(child, fd.dwFileAttributes);
    } else {
      /* Plain file, or any reparse point: unlinked, never descended. */
      sub = rm_one(child, fd.dwFileAttributes);
    }
    free(child);
    /* Best effort: keep deleting siblings, report the first failure. */
    if (sub != ASTOOLS_OK && e == ASTOOLS_OK) e = sub;
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  return e;
}

astools_err os_rmtree(const char *path) {
  wchar_t *w;
  DWORD attrs;
  astools_err e;
  size_t len;

  if (!path || path[0] == '\0') return ASTOOLS_ERR_INVALID;
  w = u8_to_wide(path);
  if (!w) return ASTOOLS_ERR_NOMEM;
  len = wcslen(w);
  while (len > 1 && (w[len - 1] == L'/' || w[len - 1] == L'\\'))
    w[--len] = L'\0';
  attrs = GetFileAttributesW(w);
  if (attrs == INVALID_FILE_ATTRIBUTES) {
    DWORD ge = GetLastError();
    free(w);
    return err_is_missing(ge) ? ASTOOLS_OK : ASTOOLS_ERR_IO;
  }
  if ((attrs & FILE_ATTRIBUTE_DIRECTORY) &&
      !(attrs & FILE_ATTRIBUTE_REPARSE_POINT)) {
    e = rmtree_wide(w, 0);
    if (e == ASTOOLS_OK) e = rm_one(w, attrs);
  } else {
    e = rm_one(w, attrs);
  }
  free(w);
  return e;
}

char *os_env_dup(const char *name) {
  wchar_t *wname, *wval;
  DWORD need, got;
  char *val;

  if (!name) return NULL;
  wname = u8_to_wide(name);
  if (!wname) return NULL;
  need = GetEnvironmentVariableW(wname, NULL, 0);
  if (need == 0) {
    free(wname);
    return NULL;
  }
  wval = malloc((size_t)need * sizeof(wchar_t));
  if (!wval) {
    free(wname);
    return NULL;
  }
  got = GetEnvironmentVariableW(wname, wval, need);
  free(wname);
  if (got == 0 || got >= need) {
    free(wval);
    return NULL;
  }
  val = wide_to_u8(wval);
  free(wval);
  return val;
}

/* ---- child processes ----------------------------------------------------- */

astools_err os_proc_user_tasks(int64_t *out) {
  /* Windows caps a tool tree with the job's ActiveProcessLimit, not a
   * per-uid rlimit, so the per-uid task count has no consumer here. */
  if (out) *out = 0;
  return ASTOOLS_ERR_UNSUPPORTED;
}

/* Append one argument to the command line with the MSVCRT quoting rules
 * (quote when needed; backslashes double only before a quote). */
static int cmdline_push(wchar_t **buf, size_t *len, size_t *cap,
                        const wchar_t *arg) {
  size_t need = wcslen(arg) * 2 + 4; /* worst case: all escapes + quotes */
  size_t i;
  int quote;

  if (*len + need + 2 > *cap) {
    size_t ncap = *cap ? *cap * 2 : 256;
    wchar_t *nb;
    while (ncap < *len + need + 2) ncap *= 2;
    nb = realloc(*buf, ncap * sizeof(wchar_t));
    if (!nb) return -1;
    *buf = nb;
    *cap = ncap;
  }
  if (*len > 0) (*buf)[(*len)++] = L' ';
  quote = (arg[0] == L'\0' || wcspbrk(arg, L" \t\"") != NULL);
  if (!quote) {
    size_t n = wcslen(arg);
    memcpy(*buf + *len, arg, n * sizeof(wchar_t));
    *len += n;
    (*buf)[*len] = L'\0';
    return 0;
  }
  (*buf)[(*len)++] = L'"';
  for (i = 0; arg[i] != L'\0';) {
    size_t bs = 0;
    while (arg[i] == L'\\') {
      bs++;
      i++;
    }
    if (arg[i] == L'\0') {
      size_t k;
      for (k = 0; k < bs * 2; k++) (*buf)[(*len)++] = L'\\';
      break;
    } else if (arg[i] == L'"') {
      size_t k;
      for (k = 0; k < bs * 2 + 1; k++) (*buf)[(*len)++] = L'\\';
      (*buf)[(*len)++] = L'"';
      i++;
    } else {
      size_t k;
      for (k = 0; k < bs; k++) (*buf)[(*len)++] = L'\\';
      (*buf)[(*len)++] = arg[i++];
    }
  }
  (*buf)[(*len)++] = L'"';
  (*buf)[*len] = L'\0';
  return 0;
}

static wchar_t *build_cmdline(const char *const *argv) {
  wchar_t *buf = NULL;
  size_t len = 0, cap = 0;
  size_t i;

  for (i = 0; argv[i] != NULL; i++) {
    wchar_t *wa = u8_to_wide(argv[i]);
    int rc;
    if (!wa) {
      free(buf);
      return NULL;
    }
    rc = cmdline_push(&buf, &len, &cap, wa);
    free(wa);
    if (rc != 0) {
      free(buf);
      return NULL;
    }
  }
  if (!buf) buf = _wcsdup(L"");
  return buf;
}

/* NAME=VALUE list -> contiguous wide environment block ("N=V\0...\0\0"). */
static wchar_t *build_envblock(const char *const *envp) {
  wchar_t *blk = NULL;
  size_t len = 0, cap = 0;
  size_t i;

  for (i = 0; envp[i] != NULL; i++) {
    wchar_t *we = u8_to_wide(envp[i]);
    size_t n;
    if (!we) {
      free(blk);
      return NULL;
    }
    n = wcslen(we) + 1;
    if (len + n + 1 > cap) {
      size_t ncap = cap ? cap * 2 : 512;
      wchar_t *nb;
      while (ncap < len + n + 1) ncap *= 2;
      nb = realloc(blk, ncap * sizeof(wchar_t));
      if (!nb) {
        free(we);
        free(blk);
        return NULL;
      }
      blk = nb;
      cap = ncap;
    }
    memcpy(blk + len, we, n * sizeof(wchar_t));
    len += n;
    free(we);
  }
  if (len + 1 > cap) {
    wchar_t *nb = realloc(blk, (len + 2) * sizeof(wchar_t));
    if (!nb) {
      free(blk);
      return NULL;
    }
    blk = nb;
  }
  if (len == 0) blk[len++] = L'\0'; /* empty block still needs two NULs */
  blk[len] = L'\0';
  return blk;
}

/* Resolve argv[0] to the executable image path. With a separator the path
 * is used directly (plus a ".exe" retry when the name has no extension);
 * a bare name searches the PATH from envp — never the host's — so the
 * caller keeps controlling the search exactly as on POSIX. */
static wchar_t *resolve_app(const char *a0, const char *const *envp) {
  int has_sep = (strchr(a0, '/') != NULL || strchr(a0, '\\') != NULL);

  if (has_sep) {
    wchar_t *w = u8_to_wide(a0);
    const char *base, *p;
    if (!w) return NULL;
    if (GetFileAttributesW(w) != INVALID_FILE_ATTRIBUTES) return w;
    base = a0;
    for (p = a0; *p != '\0'; p++)
      if (*p == '/' || *p == '\\') base = p + 1;
    if (strchr(base, '.') == NULL) {
      size_t n = strlen(a0);
      char *cand = malloc(n + 5);
      wchar_t *w2;
      if (!cand) {
        free(w);
        return NULL;
      }
      memcpy(cand, a0, n);
      memcpy(cand + n, ".exe", 5);
      w2 = u8_to_wide(cand);
      free(cand);
      if (w2 && GetFileAttributesW(w2) != INVALID_FILE_ATTRIBUTES) {
        free(w);
        return w2;
      }
      free(w2);
    }
    return w; /* let CreateProcess report the failure */
  }

  {
    const char *path_val = NULL;
    wchar_t *wpath = NULL, *wname, *found;
    DWORD cap = 512;
    size_t i;

    for (i = 0; envp[i] != NULL; i++) {
      if (_strnicmp(envp[i], "PATH=", 5) == 0) {
        path_val = envp[i] + 5;
        break;
      }
    }
    if (path_val) {
      wpath = u8_to_wide(path_val);
      if (!wpath) return NULL;
    }
    wname = u8_to_wide(a0);
    if (!wname) {
      free(wpath);
      return NULL;
    }
    for (;;) {
      DWORD got;
      found = malloc((size_t)cap * sizeof(wchar_t));
      if (!found) break;
      got = SearchPathW(wpath, wname, L".exe", cap, found, NULL);
      if (got == 0) {
        free(found);
        found = NULL;
        break;
      }
      if (got < cap) break;
      free(found);
      cap = got + 1;
    }
    free(wpath);
    free(wname);
    return found;
  }
}

/* stdout/stderr pair: anonymous pipe, child end inheritable. */
static int make_read_pipe(HANDLE *parent_rd, HANDLE *child_wr) {
  SECURITY_ATTRIBUTES sa;
  memset(&sa, 0, sizeof sa);
  sa.nLength = sizeof sa;
  sa.bInheritHandle = TRUE;
  *parent_rd = *child_wr = INVALID_HANDLE_VALUE;
  if (!CreatePipe(parent_rd, child_wr, &sa, OS_PIPE_BUF_BYTES)) return -1;
  if (!SetHandleInformation(*parent_rd, HANDLE_FLAG_INHERIT, 0)) {
    CloseHandle(*parent_rd);
    CloseHandle(*child_wr);
    *parent_rd = *child_wr = INVALID_HANDLE_VALUE;
    return -1;
  }
  return 0;
}

/* stdin pair: overlapped named pipe so parent-side writes can be cancelled
 * (anonymous pipes cannot do nonblocking writes). */
static int make_write_pipe(HANDLE *parent_wr, HANDLE *child_rd) {
  static volatile LONG counter;
  SECURITY_ATTRIBUTES sa;
  wchar_t name[96];
  LONG seq = InterlockedIncrement((volatile LONG *)&counter);

  *parent_wr = *child_rd = INVALID_HANDLE_VALUE;
  (void)_snwprintf_s(name, sizeof name / sizeof name[0], _TRUNCATE,
                     L"\\\\.\\pipe\\astools-%08lx-%08lx-%016llx",
                     (unsigned long)GetCurrentProcessId(), (unsigned long)seq,
                     (unsigned long long)GetTickCount64());
  *parent_wr = CreateNamedPipeW(
      name,
      PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, OS_PIPE_BUF_BYTES,
      OS_PIPE_BUF_BYTES, 0, NULL);
  if (*parent_wr == INVALID_HANDLE_VALUE) return -1;
  memset(&sa, 0, sizeof sa);
  sa.nLength = sizeof sa;
  sa.bInheritHandle = TRUE;
  *child_rd = CreateFileW(name, GENERIC_READ, 0, &sa, OPEN_EXISTING, 0, NULL);
  if (*child_rd == INVALID_HANDLE_VALUE) {
    CloseHandle(*parent_wr);
    *parent_wr = INVALID_HANDLE_VALUE;
    return -1;
  }
  return 0;
}

static void h_close(HANDLE *h) {
  if (*h != INVALID_HANDLE_VALUE && *h != NULL) CloseHandle(*h);
  *h = INVALID_HANDLE_VALUE;
}

static HANDLE make_job(const os_spawn_opts *o) {
  HANDLE job;
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli;

  job = CreateJobObjectW(NULL, NULL);
  if (!job) return NULL;
  memset(&jeli, 0, sizeof jeli);
  /* KILL_ON_JOB_CLOSE: the tool tree cannot outlive the host process even
   * if the host crashes without reaching os_proc_kill. */
  jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (o->limit_cpu_seconds > 0) {
    jeli.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_PROCESS_TIME;
    jeli.BasicLimitInformation.PerProcessUserTimeLimit.QuadPart =
        o->limit_cpu_seconds * 10000000LL; /* seconds -> 100ns ticks */
  }
  if (o->limit_mem_bytes > 0) {
    jeli.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_PROCESS_MEMORY;
    jeli.ProcessMemoryLimit = (SIZE_T)o->limit_mem_bytes;
  }
  if (o->limit_nproc > 0) {
    DWORD cap = (o->limit_nproc > (int64_t)MAXDWORD)
                    ? MAXDWORD
                    : (DWORD)o->limit_nproc;
    jeli.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
    jeli.BasicLimitInformation.ActiveProcessLimit = cap;
  }
  if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &jeli,
                               sizeof jeli)) {
    CloseHandle(job);
    return NULL;
  }
  return job;
}

astools_err os_proc_spawn(const os_spawn_opts *o, os_proc *p) {
  HANDLE in_wr = INVALID_HANDLE_VALUE, in_rd = INVALID_HANDLE_VALUE;
  HANDLE out_rd = INVALID_HANDLE_VALUE, out_wr = INVALID_HANDLE_VALUE;
  HANDLE err_rd = INVALID_HANDLE_VALUE, err_wr = INVALID_HANDLE_VALUE;
  HANDLE job = NULL;
  HANDLE inherit[3];
  wchar_t *app = NULL, *cmdline = NULL, *envblk = NULL, *wcwd = NULL;
  LPPROC_THREAD_ATTRIBUTE_LIST attrs = NULL;
  SIZE_T attrs_size = 0;
  STARTUPINFOEXW six;
  PROCESS_INFORMATION pi;
  BOOL ok;

  if (!o || !p || !o->argv || !o->argv[0] || !o->envp)
    return ASTOOLS_ERR_INVALID;
  if (o->cwd && !os_path_is_abs(o->cwd)) return ASTOOLS_ERR_INVALID;

  memset(p, 0, sizeof(*p));
  p->fd_in = p->fd_out = p->fd_err = -1;
  memset(&pi, 0, sizeof pi);

  if (make_write_pipe(&in_wr, &in_rd) != 0 ||
      make_read_pipe(&out_rd, &out_wr) != 0 ||
      make_read_pipe(&err_rd, &err_wr) != 0)
    goto fail;

  app = resolve_app(o->argv[0], o->envp);
  if (!app) goto fail;
  cmdline = build_cmdline(o->argv);
  envblk = build_envblock(o->envp);
  if (!cmdline || !envblk) goto fail;
  if (o->cwd) {
    wcwd = u8_to_wide(o->cwd);
    if (!wcwd) goto fail;
  }

  /* Explicit inheritance list: concurrent spawns from other threads can
   * never leak their pipe ends into this child. */
  (void)InitializeProcThreadAttributeList(NULL, 1, 0, &attrs_size);
  attrs = malloc(attrs_size);
  if (!attrs) goto fail;
  if (!InitializeProcThreadAttributeList(attrs, 1, 0, &attrs_size)) {
    free(attrs);
    attrs = NULL;
    goto fail;
  }
  inherit[0] = in_rd;
  inherit[1] = out_wr;
  inherit[2] = err_wr;
  if (!UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                 inherit, sizeof inherit, NULL, NULL))
    goto fail;

  memset(&six, 0, sizeof six);
  six.StartupInfo.cb = sizeof six;
  six.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  six.StartupInfo.hStdInput = in_rd;
  six.StartupInfo.hStdOutput = out_wr;
  six.StartupInfo.hStdError = err_wr;
  six.lpAttributeList = attrs;

  ok = CreateProcessW(app, cmdline, NULL, NULL, TRUE,
                      CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT |
                          CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
                      envblk, wcwd, &six.StartupInfo, &pi);
  if (!ok) goto fail;

  /* Child ends now live in the child; drop the parent's copies. */
  h_close(&in_rd);
  h_close(&out_wr);
  h_close(&err_wr);

  job = make_job(o);
  if (job && AssignProcessToJobObject(job, pi.hProcess)) {
    p->pgroup = 1;
    p->pgid = (int64_t)(intptr_t)job;
  } else {
    /* Best effort, like setpgid on POSIX: kill degrades to the leader. */
    if (job) CloseHandle(job);
    job = NULL;
    p->pgroup = 0;
    p->pgid = 0;
  }
  (void)ResumeThread(pi.hThread);
  CloseHandle(pi.hThread);

  p->pid = (int64_t)pi.dwProcessId;
  p->handle = (intptr_t)pi.hProcess;
  p->fd_in = _open_osfhandle((intptr_t)in_wr, _O_BINARY);
  p->fd_out = _open_osfhandle((intptr_t)out_rd, _O_BINARY);
  p->fd_err = _open_osfhandle((intptr_t)err_rd, _O_BINARY);
  if (p->fd_in < 0 || p->fd_out < 0 || p->fd_err < 0) {
    /* Handles that made it into a fd close via fd_close; the rest raw. */
    if (p->fd_in < 0) h_close(&in_wr);
    if (p->fd_out < 0) h_close(&out_rd);
    if (p->fd_err < 0) h_close(&err_rd);
    os_proc_kill(p);
    (void)os_proc_wait(p, -1, NULL);
    os_proc_free(p);
    goto fail_no_handles;
  }

  DeleteProcThreadAttributeList(attrs);
  free(attrs);
  free(app);
  free(cmdline);
  free(envblk);
  free(wcwd);
  return ASTOOLS_OK;

fail:
  h_close(&in_wr);
  h_close(&in_rd);
  h_close(&out_rd);
  h_close(&out_wr);
  h_close(&err_rd);
  h_close(&err_wr);
fail_no_handles:
  if (attrs) {
    DeleteProcThreadAttributeList(attrs);
    free(attrs);
  }
  free(app);
  free(cmdline);
  free(envblk);
  free(wcwd);
  p->fd_in = p->fd_out = p->fd_err = -1;
  return ASTOOLS_ERR_IO;
}

/* Readable when data is buffered or the pipe is broken (EOF surfaces via
 * the read). */
static int pipe_readable(int fd) {
  HANDLE h = fd_handle(fd);
  DWORD avail = 0;
  if (h == INVALID_HANDLE_VALUE) return 0;
  if (!PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL)) return 1; /* broken */
  return avail > 0;
}

astools_err os_proc_poll(os_proc *p, int want_write, int64_t timeout_ms,
                         unsigned *ready) {
  int64_t deadline = 0;

  if (!p || !ready) return ASTOOLS_ERR_INVALID;
  *ready = 0;

  if (timeout_ms >= 0) {
    int64_t now = os_monotonic_ms();
    deadline = (timeout_ms > INT64_MAX - now) ? INT64_MAX : now + timeout_ms;
  }
  for (;;) {
    unsigned bits = 0;
    if (p->fd_out >= 0 && pipe_readable(p->fd_out)) bits |= OS_READY_OUT;
    if (p->fd_err >= 0 && pipe_readable(p->fd_err)) bits |= OS_READY_ERR;
    /* Writability is reported optimistically: the write path is
     * nonblocking (overlapped + cancel) and returns BUSY on a full pipe,
     * so the worst case is a busy-poll at the caller's slice cadence. */
    if (want_write && p->fd_in >= 0) bits |= OS_READY_IN;
    if (bits) {
      *ready = bits;
      return ASTOOLS_OK;
    }
    /* Nothing to watch: report "timeout" instead of sleeping forever. */
    if (p->fd_out < 0 && p->fd_err < 0 && (!want_write || p->fd_in < 0))
      return ASTOOLS_OK;
    if (timeout_ms >= 0 && os_monotonic_ms() >= deadline) return ASTOOLS_OK;
    /* Sleep a slice on the process handle: child exit closes its pipe
     * ends, which the next peek sees as readable EOF. */
    if (p->handle)
      (void)WaitForSingleObject((HANDLE)p->handle, 1);
    else
      Sleep(1);
  }
}

astools_err os_proc_write_stdin(os_proc *p, const void *d, size_t n,
                                size_t *wrote) {
  HANDLE h;
  OVERLAPPED ov;
  DWORD done = 0, err;
  BOOL ok;

  if (!p || !wrote || (!d && n > 0)) return ASTOOLS_ERR_INVALID;
  *wrote = 0;
  if (p->fd_in < 0) return ASTOOLS_ERR_IO;
  if (n == 0) return ASTOOLS_OK;
  if (n > OS_WRITE_CHUNK_BYTES) n = OS_WRITE_CHUNK_BYTES;
  h = fd_handle(p->fd_in);
  if (h == INVALID_HANDLE_VALUE) return ASTOOLS_ERR_IO;

  memset(&ov, 0, sizeof ov);
  ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
  if (!ov.hEvent) return ASTOOLS_ERR_IO;

  ok = WriteFile(h, d, (DWORD)n, NULL, &ov);
  err = ok ? ERROR_SUCCESS : GetLastError();
  if (!ok && err == ERROR_IO_PENDING) {
    /* Pipe full: byte-mode pipe writes are all-or-nothing against the
     * buffer, so cancel and report what actually transferred. */
    if (WaitForSingleObject(ov.hEvent, 0) != WAIT_OBJECT_0)
      (void)CancelIoEx(h, &ov);
    ok = GetOverlappedResult(h, &ov, &done, TRUE);
    err = ok ? ERROR_SUCCESS : GetLastError();
  } else if (ok) {
    ok = GetOverlappedResult(h, &ov, &done, TRUE);
    err = ok ? ERROR_SUCCESS : GetLastError();
  }
  CloseHandle(ov.hEvent);

  if (ok) {
    *wrote = (size_t)done;
    return (done == 0) ? ASTOOLS_ERR_BUSY : ASTOOLS_OK;
  }
  if (err == ERROR_OPERATION_ABORTED) {
    *wrote = (size_t)done;
    return (done == 0) ? ASTOOLS_ERR_BUSY : ASTOOLS_OK;
  }
  /* ERROR_BROKEN_PIPE / ERROR_NO_DATA: reader gone (POSIX EPIPE). */
  return ASTOOLS_ERR_IO;
}

astools_err os_proc_read(os_proc *p, int which, void *buf, size_t n,
                         size_t *got) {
  int *fd;
  HANDLE h;
  DWORD avail = 0, take, done = 0;

  if (!p || !buf || !got) return ASTOOLS_ERR_INVALID;
  *got = 0;
  if (which == OS_PIPE_OUT)
    fd = &p->fd_out;
  else if (which == OS_PIPE_ERR)
    fd = &p->fd_err;
  else
    return ASTOOLS_ERR_INVALID;
  if (*fd < 0) return ASTOOLS_OK; /* EOF already delivered */
  if (n == 0) return ASTOOLS_OK;  /* never mistake a 0-read for EOF */

  h = fd_handle(*fd);
  if (h == INVALID_HANDLE_VALUE) {
    *fd = -1;
    return ASTOOLS_OK;
  }
  if (!PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL)) {
    /* Broken pipe: EOF — close and mark, like read() == 0. */
    fd_close(fd);
    return ASTOOLS_OK;
  }
  if (avail == 0) return ASTOOLS_ERR_BUSY;
  take = (n < (size_t)avail) ? (DWORD)n : avail;
  if (!ReadFile(h, buf, take, &done, NULL)) {
    fd_close(fd);
    return ASTOOLS_OK; /* raced into EOF */
  }
  if (done == 0) return ASTOOLS_ERR_BUSY; /* only got == 0 may mean EOF */
  *got = (size_t)done;
  return ASTOOLS_OK;
}

void os_proc_close_stdin(os_proc *p) {
  if (!p) return;
  fd_close(&p->fd_in);
}

void os_proc_kill(os_proc *p) {
  if (!p) return;
  if (p->pgroup && p->pgid != 0) {
    /* (UINT)-9 makes GetExitCodeProcess read back as -9, matching the
     * POSIX -(SIGKILL) convention in os_proc_wait. */
    (void)TerminateJobObject((HANDLE)(intptr_t)p->pgid, (UINT)-9);
    return;
  }
  if (p->handle) (void)TerminateProcess((HANDLE)p->handle, (UINT)-9);
}

void os_proc_terminate(os_proc *p) {
  /* No SIGTERM analogue for a console-less child; the contract allows a
   * no-op and callers escalate to os_proc_kill after their grace. */
  (void)p;
}

astools_err os_proc_wait(os_proc *p, int64_t timeout_ms, int *exit_code) {
  HANDLE h;
  DWORD tmo, rc, code = 0;

  if (!p || p->pid <= 0) return ASTOOLS_ERR_INVALID;
  h = (HANDLE)p->handle;
  if (!h) return ASTOOLS_ERR_INVALID;

  if (timeout_ms < 0)
    tmo = INFINITE;
  else
    tmo = (timeout_ms > (int64_t)(INFINITE - 1)) ? (INFINITE - 1)
                                                 : (DWORD)timeout_ms;
  rc = WaitForSingleObject(h, tmo);
  if (rc == WAIT_TIMEOUT) return ASTOOLS_ERR_BUSY;
  if (rc != WAIT_OBJECT_0) {
    p->pid = -1;
    return ASTOOLS_ERR_IO;
  }
  if (!GetExitCodeProcess(h, &code)) {
    p->pid = -1;
    return ASTOOLS_ERR_IO;
  }
  /* Reaped: further waits are invalid, but handle/job stay valid so
   * kill can still clean up descendants (POSIX keeps pgid the same way). */
  p->pid = -1;
  if (exit_code) *exit_code = (int)code;
  return ASTOOLS_OK;
}

void os_proc_free(os_proc *p) {
  if (!p) return;
  fd_close(&p->fd_in);
  fd_close(&p->fd_out);
  fd_close(&p->fd_err);
  if (p->handle) {
    CloseHandle((HANDLE)p->handle);
    p->handle = 0;
  }
  if (p->pgid != 0) {
    /* KILL_ON_JOB_CLOSE fires here for any process still in the job; the
     * runtime always kills before free, so this is the last-resort net. */
    CloseHandle((HANDLE)(intptr_t)p->pgid);
    p->pgid = 0;
  }
  p->pgroup = 0;
}

/* ---- dynamic libraries --------------------------------------------------- */

astools_err os_dylib_open(const char *path, os_dylib *out) {
  wchar_t *w;
  if (!path || !out) return ASTOOLS_ERR_INVALID;
  w = u8_to_wide(path);
  if (!w) return ASTOOLS_ERR_NOMEM;
  out->h = (void *)LoadLibraryExW(w, NULL,
                                  LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                      LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  free(w);
  return out->h ? ASTOOLS_OK : ASTOOLS_ERR_IO;
}

void *os_dylib_sym(os_dylib *d, const char *name) {
  if (!d || !d->h || !name) return NULL;
  /* FARPROC -> void*: both are pointer-sized on Windows. */
  return (void *)(intptr_t)GetProcAddress((HMODULE)d->h, name);
}

void os_dylib_close(os_dylib *d) {
  if (!d || !d->h) return;
  (void)FreeLibrary((HMODULE)d->h);
  d->h = NULL;
}

#else /* !_WIN32 */

typedef int astools_os_proc_win32_unused_on_posix; /* non-empty unit */

#endif /* _WIN32 */
