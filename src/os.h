/*
 * os.h — platform shim for libastools: threads, filesystem, time,
 * processes, pipes.
 *
 * The baseline section (threads/fs/misc) is shared with the Asper sibling
 * and implemented by os_posix.c + os_common.c (os_win32.c on Windows).
 * The process/path section below is astools-specific and implemented by
 * os_proc_posix.c (os_proc_win32.c on Windows: pipes and kill-trees ride
 * on Job Objects; see that file's header for the exact mapping).
 *
 * With ASTOOLS_NO_THREADS defined, the locking primitives compile to no-ops
 * and os_thread_start returns ASTOOLS_ERR_INVALID.
 *
 * All paths are UTF-8; the Win32 backend converts to UTF-16 internally.
 */

#ifndef ASTOOLS_OS_H
#define ASTOOLS_OS_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "astools.h" /* astools_err */

/* Link-time prefix: libastools coexists in one process with its siblings'
 * identically-named OS shims, so every shim symbol is renamed to
 * astls_x_* at compile time. Call sites keep the short names. */
#define os_thread_start       astls_x_thread_start
#define os_thread_join        astls_x_thread_join
#define os_mutex_init         astls_x_mutex_init
#define os_mutex_destroy      astls_x_mutex_destroy
#define os_mutex_lock         astls_x_mutex_lock
#define os_mutex_unlock       astls_x_mutex_unlock
#define os_cond_init          astls_x_cond_init
#define os_cond_destroy       astls_x_cond_destroy
#define os_cond_timedwait     astls_x_cond_timedwait
#define os_cond_wait          astls_x_cond_wait
#define os_cond_signal        astls_x_cond_signal
#define os_cond_broadcast     astls_x_cond_broadcast
#define os_rwlock_init        astls_x_rwlock_init
#define os_rwlock_destroy     astls_x_rwlock_destroy
#define os_rwlock_rdlock      astls_x_rwlock_rdlock
#define os_rwlock_rdunlock    astls_x_rwlock_rdunlock
#define os_rwlock_wrlock      astls_x_rwlock_wrlock
#define os_rwlock_wrunlock    astls_x_rwlock_wrunlock
#define os_file_replace       astls_x_file_replace
#define os_fsync              astls_x_fsync
#define os_mkdir_p            astls_x_mkdir_p
#define os_file_exists        astls_x_file_exists
#define os_read_file          astls_x_read_file
#define os_write_file         astls_x_write_file
#define os_remove_file        astls_x_remove_file
#define os_truncate           astls_x_truncate
#define os_file_size          astls_x_file_size
#define os_rename             astls_x_rename
#define os_list_dir           astls_x_list_dir
#define os_list_subdirs       astls_x_list_subdirs
#define os_rmtree             astls_x_rmtree
#define os_fopen              astls_x_fopen
#define os_now_unix           astls_x_now_unix
#define os_monotonic_ms       astls_x_monotonic_ms
#define os_random_bytes       astls_x_random_bytes
#define os_hardware_threads   astls_x_hardware_threads
#define os_path_join          astls_x_path_join
#define os_stat               astls_x_stat
#define os_lstat              astls_x_lstat
#define os_realpath           astls_x_realpath
#define os_path_is_abs        astls_x_path_is_abs
#define os_getcwd_dup         astls_x_getcwd_dup
#define os_exe_path           astls_x_exe_path
#define os_env_dup            astls_x_env_dup
#define os_dylib_open         astls_x_dylib_open
#define os_dylib_close        astls_x_dylib_close
#define os_proc_spawn         astls_x_proc_spawn
#define os_proc_user_tasks    astls_x_proc_user_tasks
#define os_proc_wait          astls_x_proc_wait
#define os_proc_poll          astls_x_proc_poll
#define os_proc_kill          astls_x_proc_kill
#define os_proc_terminate     astls_x_proc_terminate
#define os_proc_read          astls_x_proc_read
#define os_proc_write_stdin   astls_x_proc_write_stdin
#define os_proc_close_stdin   astls_x_proc_close_stdin
#define os_proc_free          astls_x_proc_free


#ifdef __cplusplus
extern "C" {
#endif

/* ---- opaque-ish primitive storage -------------------------------------- */

#if defined(ASTOOLS_NO_THREADS)

typedef struct { int unused; } os_mutex;
typedef struct { int unused; } os_cond;
typedef struct { int unused; } os_rwlock;
typedef struct { int unused; } os_thread;

#elif defined(_WIN32)

typedef struct { void *h; } os_mutex;  /* SRWLOCK used exclusively */
typedef struct { void *h; } os_cond;   /* CONDITION_VARIABLE       */
typedef struct { void *h; } os_rwlock; /* SRWLOCK                  */
typedef struct { void *h; } os_thread; /* HANDLE                   */

#else /* POSIX */

#include <pthread.h>
typedef struct { pthread_mutex_t m; } os_mutex;
typedef struct { pthread_cond_t c; } os_cond;
typedef struct { pthread_rwlock_t l; } os_rwlock;
typedef struct { pthread_t t; int valid; } os_thread;

#endif

/* ---- threads ------------------------------------------------------------ */

astools_err os_thread_start(os_thread *t, void *(*fn)(void *), void *arg);
void        os_thread_join(os_thread *t);

void os_mutex_init(os_mutex *m);
void os_mutex_destroy(os_mutex *m);
void os_mutex_lock(os_mutex *m);
void os_mutex_unlock(os_mutex *m);

void os_cond_init(os_cond *c);
void os_cond_destroy(os_cond *c);
/* Wait with timeout in milliseconds; returns 1 if signaled, 0 on timeout. */
int  os_cond_timedwait(os_cond *c, os_mutex *m, int64_t timeout_ms);
void os_cond_wait(os_cond *c, os_mutex *m);
void os_cond_signal(os_cond *c);
void os_cond_broadcast(os_cond *c);

void os_rwlock_init(os_rwlock *l);
void os_rwlock_destroy(os_rwlock *l);
void os_rwlock_rdlock(os_rwlock *l);
void os_rwlock_rdunlock(os_rwlock *l);
void os_rwlock_wrlock(os_rwlock *l);
void os_rwlock_wrunlock(os_rwlock *l);

/* ---- filesystem (baseline) ---------------------------------------------- */

/* Atomically replace dst with src (rename(2) / ReplaceFileW+MoveFileExW).
 * src is consumed on success. */
astools_err os_file_replace(const char *src, const char *dst);
astools_err os_fsync(FILE *f);
/* Create directory and any missing parents. Existing dir is OK. */
astools_err os_mkdir_p(const char *path);
int         os_file_exists(const char *path); /* 1 = yes (any type) */
/* Read whole file. *out is NUL-terminated (free with free()). */
astools_err os_read_file(const char *path, char **out, size_t *out_len);
/* Write whole buffer (truncate). Not atomic — pair with os_file_replace. */
astools_err os_write_file(const char *path, const void *data, size_t len);
astools_err os_remove_file(const char *path);
astools_err os_truncate(const char *path, uint64_t size);
astools_err os_file_size(const char *path, uint64_t *out);
astools_err os_rename(const char *src, const char *dst);
/* List regular file names (no dirs) in path, malloc'd array of malloc'd
 * names, unsorted; caller frees each + array. Missing dir => 0 entries. */
astools_err os_list_dir(const char *path, char ***out_names, size_t *out_n);
FILE       *os_fopen(const char *path, const char *mode);

/* ---- misc (baseline) ----------------------------------------------------- */

int64_t os_now_unix(void);     /* wall clock, unix seconds UTC */
int64_t os_monotonic_ms(void); /* monotonic milliseconds      */
void    os_random_bytes(void *buf, size_t n);
int     os_hardware_threads(void); /* >= 1 */
/* Path join with '/'. Returns malloc'd string. */
char   *os_path_join(const char *a, const char *b);

/* ═════════════════════ astools process/path extension ═════════════════════
 * Implemented by os_proc_posix.c. Everything below returns
 * ASTOOLS_ERR_UNSUPPORTED from stub backends.
 */

/* ---- paths and stat ------------------------------------------------------ */

typedef enum {
  OS_FT_MISSING = 0,
  OS_FT_FILE,
  OS_FT_DIR,
  OS_FT_SYMLINK, /* lstat view */
  OS_FT_OTHER
} os_file_type;

typedef struct {
  os_file_type type;
  uint64_t size;
  int64_t mtime_unix;
  int readonly; /* best effort */
} os_stat_info;

/* stat (follows symlinks). OS_FT_MISSING + OK when absent. */
astools_err os_stat(const char *path, os_stat_info *out);
/* lstat view: reports OS_FT_SYMLINK for links. */
astools_err os_lstat(const char *path, os_stat_info *out);

/* Canonicalize an EXISTING path: resolve symlinks, "." and "..".
 * Returns malloc'd absolute path; ASTOOLS_ERR_NOT_FOUND if missing. */
astools_err os_realpath(const char *path, char **out);

int   os_path_is_abs(const char *path);
char *os_getcwd_dup(void); /* malloc'd; NULL on failure */

/* Absolute path of the current executable (install-root discovery).
 * malloc'd. */
astools_err os_exe_path(char **out);

/* Immediate subdirectory names of path (no files), malloc'd array of
 * malloc'd names, unsorted. Missing dir => 0 entries. */
astools_err os_list_subdirs(const char *path, char ***out_names,
                            size_t *out_n);

/* Best-effort recursive delete (scratch cleanup). Never follows symlinks
 * out of the tree: symlinks are unlinked, not descended. */
astools_err os_rmtree(const char *path);

/* Duplicate of getenv(name); NULL if unset. malloc'd. */
char *os_env_dup(const char *name);

/* ---- child processes ----------------------------------------------------- */

typedef struct {
  int64_t pid;      /* pid_t / process id                    */
  intptr_t handle;  /* Win32 HANDLE; unused on POSIX         */
  int fd_in;        /* child stdin (write end); -1 = closed  */
  int fd_out;       /* child stdout (read end); -1 = closed  */
  int fd_err;       /* child stderr (read end); -1 = closed  */
  int pgroup;       /* POSIX: child leads its own group      */
  int64_t pgid;     /* retained after leader reap for tree cleanup */
} os_proc;

typedef struct {
  const char *const *argv; /* NULL-terminated; argv[0] is the executable
                            * path exactly as to be exec'd (no PATH search
                            * unless it contains no separator) */
  const char *const *envp; /* complete environment, NULL-terminated */
  const char *cwd;         /* absolute; NULL = inherit */
  /* rlimits applied in the child before exec (0 = leave unlimited) */
  int64_t limit_cpu_seconds;
  int64_t limit_mem_bytes;
  int64_t limit_nproc;
} os_spawn_opts;

/* Spawn with all three stdio pipes, child in its own process group
 * (POSIX) so the whole tree can be killed. Pipes are non-blocking on the
 * parent side. */
astools_err os_proc_spawn(const os_spawn_opts *o, os_proc *p);

/* Tasks (threads) currently charged to the caller's real UID — exactly the
 * quantity the kernel tests RLIMIT_NPROC against at fork(). The limit is
 * per-UID and system-wide, not per process tree, so an absolute cap is only
 * meaningful relative to what the account already uses.
 * ASTOOLS_ERR_UNSUPPORTED where the platform cannot report it. */
astools_err os_proc_user_tasks(int64_t *out);

/* Multiplexed wait for pipe readiness. want_write: include fd_in
 * writability. timeout_ms < 0 waits forever. *ready is a bitmask; 0 on
 * timeout. */
#define OS_READY_IN 1u  /* fd_in writable  */
#define OS_READY_OUT 2u /* fd_out readable */
#define OS_READY_ERR 4u /* fd_err readable */
astools_err os_proc_poll(os_proc *p, int want_write, int64_t timeout_ms,
                         unsigned *ready);

/* Nonblocking pipe I/O. Reads: *got = 0 with ASTOOLS_OK means EOF (the fd
 * is closed and set to -1). Writes: partial writes are normal. EAGAIN
 * surfaces as ASTOOLS_ERR_BUSY with zero bytes moved. */
astools_err os_proc_write_stdin(os_proc *p, const void *d, size_t n,
                                size_t *wrote);
#define OS_PIPE_OUT 1
#define OS_PIPE_ERR 2
astools_err os_proc_read(os_proc *p, int which, void *buf, size_t n,
                         size_t *got);
void os_proc_close_stdin(os_proc *p);

/* Kill the whole process group/job (SIGKILL; TerminateJobObject). */
void os_proc_kill(os_proc *p);
/* Graceful stop request (SIGTERM; no-op where unsupported). */
void os_proc_terminate(os_proc *p);

/* Reap. timeout_ms < 0 waits forever; ASTOOLS_ERR_BUSY if still running.
 * exit_code: >= 0 exit status, < 0 = -(terminating signal). */
astools_err os_proc_wait(os_proc *p, int64_t timeout_ms, int *exit_code);

/* Close any remaining fds/handles. Does not kill or reap. */
void os_proc_free(os_proc *p);

/* ---- dynamic libraries (kind "library") ---------------------------- */

typedef struct { void *h; } os_dylib;

astools_err os_dylib_open(const char *path, os_dylib *out);
void       *os_dylib_sym(os_dylib *d, const char *name);
void        os_dylib_close(os_dylib *d);

#ifdef __cplusplus
}
#endif

#endif /* ASTOOLS_OS_H */
