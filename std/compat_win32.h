/*
 * compat_win32.h — POSIX face of the std tools on Windows.
 *
 * The tool_*.c sources are written against a small POSIX surface
 * (fd I/O, stat/lstat, dirent, unlink/rename/mkdir, one spawn helper).
 * This header maps that surface onto Win32 implementations from
 * compat_win32.c so the tool logic compiles unchanged. All paths are
 * UTF-8 at this boundary and converted to UTF-16 inside the shim; the
 * runtime hands tools canonical '/'-separated absolute paths.
 *
 * Include order matters: this header must come AFTER every system
 * include of the translation unit — the macros below would otherwise
 * rewrite CRT declarations. compat_win32.c defines ASTD_COMPAT_IMPL to
 * get the declarations without the macro renames.
 */

#ifndef ASTOOLS_STD_COMPAT_WIN32_H
#define ASTOOLS_STD_COMPAT_WIN32_H
#ifdef _WIN32

#include <direct.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifndef _SSIZE_T_DEFINED
typedef long long ssize_t;
#define _SSIZE_T_DEFINED
#endif
typedef int astd_c_mode_t;

/* stat shape: only the fields the tools read. st_mtime is unix seconds. */
struct astd_c_stat {
  unsigned int st_mode;
  long long st_size;
  long long st_mtime;
};

/* mode bits (POSIX values; MSVC's _S_IFDIR/_S_IFREG agree) */
#ifndef S_IFLNK
#define S_IFLNK 0xA000
#endif
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & 0xF000) == 0x4000)
#endif
#ifndef S_ISREG
#define S_ISREG(m) (((m) & 0xF000) == 0x8000)
#endif
#ifndef S_ISLNK
#define S_ISLNK(m) (((m) & 0xF000) == 0xA000)
#endif

/* open flags the CRT lacks. O_NOFOLLOW opens the final component as a
 * reparse point and rejects that handle atomically. */
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0x40000000
#endif

#ifndef W_OK
#define W_OK 2
#endif
#ifndef X_OK
#define X_OK 0 /* execute is not a Windows permission; existence check */
#endif

int astd_c_open(const char *path, int flags, ...);
ssize_t astd_c_read(int fd, void *buf, size_t n);
ssize_t astd_c_write(int fd, const void *buf, size_t n);
int astd_c_stat(const char *path, struct astd_c_stat *st);
int astd_c_lstat(const char *path, struct astd_c_stat *st);
int astd_c_fstat(int fd, struct astd_c_stat *st);
int astd_c_access(const char *path, int mode);
int astd_c_mkdir(const char *path);
int astd_c_rmdir(const char *path);
int astd_c_unlink(const char *path);
int astd_c_rename(const char *from, const char *to); /* replaces like POSIX */
int astd_c_symlink(const char *target, const char *linkpath);
ssize_t astd_c_readlink(const char *path, char *buf, size_t cap);
char *astd_c_realpath(const char *path, char *resolved /* must be NULL */);

/* dirent (d_name only, UTF-8) */
typedef struct astd_c_dir DIR;
struct dirent {
  char d_name[1024];
};
DIR *opendir(const char *path);
struct dirent *readdir(DIR *d);
int closedir(DIR *d);

/* spawn + capture: the one process helper proc/git need. timeout_ms 0
 * means no local deadline; a timeout kill reports exit_code -9 with
 * timed_out set (POSIX -SIGKILL parity). out/err are always non-NULL
 * NUL-terminated buffers on success. */
typedef struct {
  int exit_code;
  int timed_out;
  char *out;
  size_t out_n;
  int out_trunc;
  char *err;
  size_t err_n;
  int err_trunc;
} astd_spawn_res;

int astd_spawn_capture(char *const *argv, char *const *envp, const char *cwd,
                       const char *input, size_t input_n, size_t out_cap,
                       size_t err_cap, int64_t timeout_ms, astd_spawn_res *rr,
                       char *emsg, size_t emsg_sz);
int64_t astd_mono_ms(void);

#ifndef ASTD_COMPAT_IMPL
/* POSIX names -> shim. `stat` doubles as struct tag and function name,
 * exactly as in POSIX, so `struct stat` and `stat(p,&st)` both map. */
#define mode_t astd_c_mode_t
#define off_t long long
#define stat astd_c_stat
#define lstat astd_c_lstat
#define fstat astd_c_fstat
#define open astd_c_open
#define close _close
#define read(f, b, n) astd_c_read((f), (b), (n))
#define write(f, b, n) astd_c_write((f), (b), (n))
#define lseek _lseeki64
#define access astd_c_access
#define mkdir(p, m) ((void)(m), astd_c_mkdir(p))
#define rmdir astd_c_rmdir
#define unlink astd_c_unlink
#define rename astd_c_rename
#define chmod(p, m) ((void)(p), (void)(m), 0)   /* modes: best-effort no-op */
#define fchmod(f, m) ((void)(f), (void)(m), 0)
#define symlink astd_c_symlink
#define readlink astd_c_readlink
#define realpath astd_c_realpath
#define fsync _commit
#define getpid _getpid
#define environ _environ
#endif /* ASTD_COMPAT_IMPL */

#endif /* _WIN32 */
#endif /* ASTOOLS_STD_COMPAT_WIN32_H */
