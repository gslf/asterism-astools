/*
 * os_proc_posix.c — POSIX backend of the astools process/path extension
 * of os.h: stat/realpath helpers, directory walks, child processes over
 * three CLOEXEC pipes with rlimits and a private process group, dlopen.
 *
 * macOS is the primary target; the file also compiles on Linux. Known
 * divergences: os_exe_path uses _NSGetExecutablePath vs
 * /proc/self/exe, and RLIMIT_AS is best effort on macOS. SIGPIPE from
 * stdin writes is contained per-thread (block, write, consume the pending
 * signal, restore) so a broken pipe can never kill the host process and
 * no process-global signal state is touched.
 */

#if !defined(_WIN32)

/* Feature-test macros must precede every include. */
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif

#include "os.h"

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#if !defined(ASTOOLS_NO_THREADS)
#include <pthread.h>
#endif

#if defined(__APPLE__)
#include <crt_externs.h>
#include <mach-o/dyld.h>
/* A macOS dylib may not reference `environ` directly. */
#define OS_ENVIRON (*_NSGetEnviron())
#else
extern char **environ;
#define OS_ENVIRON environ
#endif

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

static int fd_set_cloexec(int fd) {
  int fl = fcntl(fd, F_GETFD, 0);
  if (fl < 0) return -1;
  return fcntl(fd, F_SETFD, fl | FD_CLOEXEC);
}

static int fd_set_nonblock(int fd) {
  int fl = fcntl(fd, F_GETFL, 0);
  if (fl < 0) return -1;
  return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static void fd_close(int *fd) {
  if (*fd >= 0) {
    close(*fd);
    *fd = -1;
  }
}

/* ---- paths and stat ------------------------------------------------------ */

static astools_err stat_common(const char *path, os_stat_info *out,
                               int follow) {
  struct stat st;
  int rc;

  if (!path || !out) return ASTOOLS_ERR_INVALID;
  memset(out, 0, sizeof(*out));
  rc = follow ? stat(path, &st) : lstat(path, &st);
  if (rc != 0) {
    if (errno == ENOENT || errno == ENOTDIR) {
      out->type = OS_FT_MISSING;
      return ASTOOLS_OK;
    }
    return ASTOOLS_ERR_IO;
  }
  if (S_ISREG(st.st_mode))
    out->type = OS_FT_FILE;
  else if (S_ISDIR(st.st_mode))
    out->type = OS_FT_DIR;
  else if (S_ISLNK(st.st_mode))
    out->type = OS_FT_SYMLINK;
  else
    out->type = OS_FT_OTHER;
  out->size = (uint64_t)st.st_size;
  out->mtime_unix = (int64_t)st.st_mtime;
  /* Best effort: no write bit for anyone == readonly. */
  out->readonly = ((st.st_mode & (S_IWUSR | S_IWGRP | S_IWOTH)) == 0) ? 1 : 0;
  return ASTOOLS_OK;
}

astools_err os_stat(const char *path, os_stat_info *out) {
  return stat_common(path, out, 1);
}

astools_err os_lstat(const char *path, os_stat_info *out) {
  return stat_common(path, out, 0);
}

astools_err os_realpath(const char *path, char **out) {
  char *r;

  if (!path || !out) return ASTOOLS_ERR_INVALID;
  *out = NULL;
  r = realpath(path, NULL);
  if (!r) {
    if (errno == ENOENT || errno == ENOTDIR) return ASTOOLS_ERR_NOT_FOUND;
    if (errno == ENOMEM) return ASTOOLS_ERR_NOMEM;
    return ASTOOLS_ERR_IO;
  }
  *out = r;
  return ASTOOLS_OK;
}

int os_path_is_abs(const char *path) {
  return (path != NULL && path[0] == '/') ? 1 : 0;
}

char *os_getcwd_dup(void) {
  size_t cap = 256;

  for (;;) {
    char *buf = malloc(cap);
    if (!buf) return NULL;
    if (getcwd(buf, cap) != NULL) return buf;
    free(buf);
    if (errno != ERANGE || cap > ((size_t)1 << 20)) return NULL;
    cap *= 2;
  }
}

astools_err os_exe_path(char **out) {
  if (!out) return ASTOOLS_ERR_INVALID;
  *out = NULL;
#if defined(__APPLE__)
  {
    uint32_t size = 0;
    char *buf, *real;

    (void)_NSGetExecutablePath(NULL, &size); /* fails; sets needed size */
    if (size == 0) return ASTOOLS_ERR_IO;
    buf = malloc((size_t)size + 1);
    if (!buf) return ASTOOLS_ERR_NOMEM;
    if (_NSGetExecutablePath(buf, &size) != 0) {
      free(buf);
      return ASTOOLS_ERR_IO;
    }
    real = realpath(buf, NULL);
    free(buf);
    if (!real)
      return (errno == ENOMEM) ? ASTOOLS_ERR_NOMEM : ASTOOLS_ERR_IO;
    *out = real;
    return ASTOOLS_OK;
  }
#elif defined(__linux__)
  {
    size_t cap = 256;

    for (;;) {
      char *buf = malloc(cap);
      ssize_t n;
      if (!buf) return ASTOOLS_ERR_NOMEM;
      n = readlink("/proc/self/exe", buf, cap);
      if (n < 0) {
        free(buf);
        return ASTOOLS_ERR_IO;
      }
      if ((size_t)n < cap) {
        buf[n] = '\0';
        *out = buf;
        return ASTOOLS_OK;
      }
      free(buf);
      if (cap > ((size_t)1 << 16)) return ASTOOLS_ERR_IO;
      cap *= 2;
    }
  }
#else
  return ASTOOLS_ERR_UNSUPPORTED;
#endif
}

astools_err os_list_subdirs(const char *path, char ***out_names,
                            size_t *out_n) {
  DIR *d;
  struct dirent *de;
  char **names = NULL;
  size_t n = 0, cap = 0;

  if (!path || !out_names || !out_n) return ASTOOLS_ERR_INVALID;
  *out_names = NULL;
  *out_n = 0;

  d = opendir(path);
  if (!d) {
    if (errno == ENOENT || errno == ENOTDIR)
      return ASTOOLS_OK; /* missing dir => 0 entries */
    return ASTOOLS_ERR_IO;
  }

  while ((de = readdir(d)) != NULL) {
    char *full, *name;
    struct stat st;

    if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
      continue;

    full = os_path_join(path, de->d_name);
    if (!full) {
      closedir(d);
      names_free(names, n);
      return ASTOOLS_ERR_NOMEM;
    }
    /* lstat: a symlink to a directory is not a subdirectory. */
    if (lstat(full, &st) != 0 || !S_ISDIR(st.st_mode)) {
      free(full);
      continue;
    }
    free(full);

    if (n == cap) {
      size_t ncap = cap == 0 ? 16 : cap * 2;
      char **nn = realloc(names, ncap * sizeof(*nn));
      if (!nn) {
        closedir(d);
        names_free(names, n);
        return ASTOOLS_ERR_NOMEM;
      }
      names = nn;
      cap = ncap;
    }
    name = dup_str(de->d_name);
    if (!name) {
      closedir(d);
      names_free(names, n);
      return ASTOOLS_ERR_NOMEM;
    }
    names[n++] = name;
  }
  closedir(d);

  *out_names = names;
  *out_n = n;
  return ASTOOLS_OK;
}

/* Depth cap: each recursion level keeps one directory fd open, so this
 * also bounds fd usage (macOS default RLIMIT_NOFILE is small). Deeper
 * trees fail with ASTOOLS_ERR_IO — acceptable for best-effort cleanup. */
#define OS_RMTREE_MAX_DEPTH 64

/* fd-relative walk (fstatat/unlinkat/openat with O_NOFOLLOW) so a hostile
 * tool cannot race a directory into a symlink and route the delete
 * outside the tree; symlinks are unlinked, never descended. */
static astools_err rmtree_at(int pfd, const char *name, int depth) {
  struct stat st;
  DIR *d;
  struct dirent *de;
  astools_err e = ASTOOLS_OK;
  int fd;

  if (fstatat(pfd, name, &st, AT_SYMLINK_NOFOLLOW) != 0)
    return (errno == ENOENT || errno == ENOTDIR) ? ASTOOLS_OK
                                                 : ASTOOLS_ERR_IO;
  if (!S_ISDIR(st.st_mode)) {
    if (unlinkat(pfd, name, 0) != 0 && errno != ENOENT)
      return ASTOOLS_ERR_IO;
    return ASTOOLS_OK;
  }
  if (depth >= OS_RMTREE_MAX_DEPTH) return ASTOOLS_ERR_IO;

  fd = openat(pfd, name, O_RDONLY | O_NOFOLLOW | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) {
    if (errno == ENOENT) return ASTOOLS_OK;
    if (errno == ELOOP || errno == EMLINK || errno == ENOTDIR) {
      /* Raced into a symlink: remove the link itself. */
      if (unlinkat(pfd, name, 0) != 0 && errno != ENOENT)
        return ASTOOLS_ERR_IO;
      return ASTOOLS_OK;
    }
    return ASTOOLS_ERR_IO;
  }
  d = fdopendir(fd);
  if (!d) {
    close(fd);
    return ASTOOLS_ERR_IO;
  }
  while ((de = readdir(d)) != NULL) {
    astools_err sub;
    if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
      continue;
    /* Best effort: keep deleting siblings, report the first failure. */
    sub = rmtree_at(dirfd(d), de->d_name, depth + 1);
    if (sub != ASTOOLS_OK && e == ASTOOLS_OK) e = sub;
  }
  closedir(d);
  if (unlinkat(pfd, name, AT_REMOVEDIR) != 0 && errno != ENOENT &&
      e == ASTOOLS_OK)
    e = ASTOOLS_ERR_IO;
  return e;
}

astools_err os_rmtree(const char *path) {
  if (!path || path[0] == '\0') return ASTOOLS_ERR_INVALID;
  return rmtree_at(AT_FDCWD, path, 0);
}

char *os_env_dup(const char *name) {
  const char *v;
  if (!name) return NULL;
  v = getenv(name);
  return v ? dup_str(v) : NULL;
}

/* ---- child processes ----------------------------------------------------- */

static int make_pipe(int fds[2]) {
  if (pipe(fds) != 0) {
    fds[0] = fds[1] = -1;
    return -1;
  }
  if (fd_set_cloexec(fds[0]) != 0 || fd_set_cloexec(fds[1]) != 0) {
    close(fds[0]);
    close(fds[1]);
    fds[0] = fds[1] = -1;
    return -1;
  }
  return 0;
}

/* Child side: route fd onto the stdio target. dup2 clears CLOEXEC on the
 * duplicate; when the pipe fd already IS the target (parent had stdio
 * closed) clear CLOEXEC explicitly instead. */
static int child_stdio(int fd, int target) {
  if (fd == target) {
    int fl = fcntl(fd, F_GETFD, 0);
    if (fl < 0) return -1;
    return fcntl(fd, F_SETFD, fl & ~FD_CLOEXEC);
  }
  return (dup2(fd, target) < 0) ? -1 : 0;
}

/* Lower both soft and hard limit (never above the current hard limit, so
 * unprivileged setrlimit cannot fail by raising, and the tool cannot lift
 * the cap back up). */
static int child_one_limit(int resource, int64_t value) {
  struct rlimit rl;
  rlim_t want = (rlim_t)value;

  if (getrlimit(resource, &rl) != 0) return -1;
  if (rl.rlim_max != RLIM_INFINITY && rl.rlim_max < want) want = rl.rlim_max;
  rl.rlim_cur = want;
  rl.rlim_max = want;
  return setrlimit(resource, &rl);
}

static int child_limits(const os_spawn_opts *o) {
  if (o->limit_cpu_seconds > 0 &&
      child_one_limit(RLIMIT_CPU, o->limit_cpu_seconds) != 0)
    return -1;
#ifdef RLIMIT_AS
  /* Best effort: RLIMIT_AS is unreliable on macOS. */
  if (o->limit_mem_bytes > 0)
    (void)child_one_limit(RLIMIT_AS, o->limit_mem_bytes);
#endif
#ifdef RLIMIT_NPROC
  /* Best effort, like RLIMIT_AS. The cap is containment hardening, not a
   * correctness precondition, so a kernel that refuses it must not turn a
   * valid invocation into a spawn failure. */
  if (o->limit_nproc > 0)
    (void)child_one_limit(RLIMIT_NPROC, o->limit_nproc);
#endif
  return 0;
}

#if defined(__linux__)
/*
 * Threads: field of /proc/<pid>/status. 0 when unreadable — a process may
 * exit mid-scan, which is normal and simply drops it from the total.
 */
static int64_t proc_task_count(const char *pid) {
  char path[48], buf[4096];
  int fd;
  ssize_t off = 0;
  const char *p;
  (void)snprintf(path, sizeof path, "/proc/%s/status", pid);
  fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return 0;
  while (off < (ssize_t)sizeof buf - 1) {
    ssize_t n = read(fd, buf + off, (size_t)((ssize_t)sizeof buf - 1 - off));
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (n == 0) break;
    off += n;
  }
  close(fd);
  buf[off < 0 ? 0 : off] = '\0';
  p = strstr(buf, "\nThreads:");
  if (!p) return 0;
  return (int64_t)strtol(p + 9, NULL, 10);
}
#endif

astools_err os_proc_user_tasks(int64_t *out) {
#if defined(__linux__)
  DIR *d;
  struct dirent *de;
  uid_t me;
  int64_t total = 0;

  if (!out) return ASTOOLS_ERR_INVALID;
  me = getuid(); /* RLIMIT_NPROC is charged to the real UID */
  d = opendir("/proc");
  if (!d) return ASTOOLS_ERR_UNSUPPORTED;
  while ((de = readdir(d)) != NULL) {
    char pid[16];
    char path[32];
    struct stat st;
    size_t i;
    /* numeric entries only; the bounded copy also keeps the two snprintf
     * destinations provably large enough for -Wformat-truncation */
    for (i = 0; de->d_name[i] >= '0' && de->d_name[i] <= '9'; i++) {}
    if (i == 0 || de->d_name[i] != '\0' || i >= sizeof pid) continue;
    memcpy(pid, de->d_name, i + 1);
    (void)snprintf(path, sizeof path, "/proc/%s", pid);
    if (stat(path, &st) != 0 || st.st_uid != me) continue;
    total += proc_task_count(pid);
  }
  closedir(d);
  *out = total;
  return ASTOOLS_OK;
#else
  /* No /proc: macOS deliberately leaves RLIMIT_NPROC alone (it is per-uid
   * there too, and would throttle the whole login session). */
  if (out) *out = 0;
  return ASTOOLS_ERR_UNSUPPORTED;
#endif
}

astools_err os_proc_spawn(const os_spawn_opts *o, os_proc *p) {
  int in_pipe[2] = {-1, -1};
  int out_pipe[2] = {-1, -1};
  int err_pipe[2] = {-1, -1};
  pid_t child;

  if (!o || !p || !o->argv || !o->argv[0] || !o->envp)
    return ASTOOLS_ERR_INVALID;
  if (o->cwd && !os_path_is_abs(o->cwd)) return ASTOOLS_ERR_INVALID;

  memset(p, 0, sizeof(*p));
  p->fd_in = p->fd_out = p->fd_err = -1;

  if (make_pipe(in_pipe) != 0 || make_pipe(out_pipe) != 0 ||
      make_pipe(err_pipe) != 0)
    goto fail;

  child = fork();
  if (child < 0) goto fail;

  if (child == 0) {
    /* Child: only async-signal-safe calls until exec. */
    sigset_t empty;
    (void)setpgid(0, 0);
    sigemptyset(&empty);
    (void)sigprocmask(SIG_SETMASK, &empty, NULL);
    if (child_stdio(in_pipe[0], STDIN_FILENO) != 0) _exit(127);
    if (child_stdio(out_pipe[1], STDOUT_FILENO) != 0) _exit(127);
    if (child_stdio(err_pipe[1], STDERR_FILENO) != 0) _exit(127);
    if (o->cwd && chdir(o->cwd) != 0) _exit(127);
    if (child_limits(o) != 0) _exit(127);
    /* Caller controls PATH through envp: install it as the process
     * environment, then let execvp search the new PATH (execv when
     * argv[0] carries a slash). */
    OS_ENVIRON = (char **)o->envp;
    if (strchr(o->argv[0], '/'))
      execv(o->argv[0], (char *const *)o->argv);
    else
      execvp(o->argv[0], (char *const *)o->argv);
    _exit(127);
  }

  /* Parent. Mirror setpgid to close the race with os_proc_kill; EACCES
   * after the child exec'd is expected and harmless. */
  (void)setpgid(child, child);
  fd_close(&in_pipe[0]);
  fd_close(&out_pipe[1]);
  fd_close(&err_pipe[1]);
  if (fd_set_nonblock(in_pipe[1]) != 0 || fd_set_nonblock(out_pipe[0]) != 0 ||
      fd_set_nonblock(err_pipe[0]) != 0) {
    (void)kill(-child, SIGKILL);
    while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {
    }
    goto fail;
  }

  p->pid = (int64_t)child;
  p->pgid = (int64_t)child;
  p->handle = 0;
  p->pgroup = 1;
  p->fd_in = in_pipe[1];
  p->fd_out = out_pipe[0];
  p->fd_err = err_pipe[0];
  return ASTOOLS_OK;

fail:
  fd_close(&in_pipe[0]);
  fd_close(&in_pipe[1]);
  fd_close(&out_pipe[0]);
  fd_close(&out_pipe[1]);
  fd_close(&err_pipe[0]);
  fd_close(&err_pipe[1]);
  p->fd_in = p->fd_out = p->fd_err = -1;
  return ASTOOLS_ERR_IO;
}

astools_err os_proc_poll(os_proc *p, int want_write, int64_t timeout_ms,
                         unsigned *ready) {
  struct pollfd fds[3];
  unsigned bits[3];
  int n = 0, rc, i;
  int64_t deadline = 0;

  if (!p || !ready) return ASTOOLS_ERR_INVALID;
  *ready = 0;

  if (want_write && p->fd_in >= 0) {
    fds[n].fd = p->fd_in;
    fds[n].events = POLLOUT;
    fds[n].revents = 0;
    bits[n++] = OS_READY_IN;
  }
  if (p->fd_out >= 0) {
    fds[n].fd = p->fd_out;
    fds[n].events = POLLIN;
    fds[n].revents = 0;
    bits[n++] = OS_READY_OUT;
  }
  if (p->fd_err >= 0) {
    fds[n].fd = p->fd_err;
    fds[n].events = POLLIN;
    fds[n].revents = 0;
    bits[n++] = OS_READY_ERR;
  }
  /* Nothing to watch: report "timeout" instead of sleeping forever. */
  if (n == 0) return ASTOOLS_OK;

  if (timeout_ms >= 0) {
    int64_t now = os_monotonic_ms();
    deadline = (timeout_ms > INT64_MAX - now) ? INT64_MAX : now + timeout_ms;
  }
  for (;;) {
    int tmo = -1;
    if (timeout_ms >= 0) {
      int64_t left = deadline - os_monotonic_ms();
      if (left < 0) left = 0;
      tmo = (left > (int64_t)INT_MAX) ? INT_MAX : (int)left;
    }
    rc = poll(fds, (nfds_t)n, tmo);
    if (rc >= 0) break;
    if (errno != EINTR && errno != EAGAIN) return ASTOOLS_ERR_IO;
    if (timeout_ms >= 0 && os_monotonic_ms() >= deadline) return ASTOOLS_OK;
  }
  if (rc == 0) return ASTOOLS_OK; /* timeout */

  for (i = 0; i < n; i++) {
    short re = fds[i].revents;
    if (bits[i] == OS_READY_IN) {
      /* ERR/HUP on stdin: report writable so the caller's write surfaces
       * the broken pipe instead of polling forever. */
      if (re & (POLLOUT | POLLERR | POLLHUP | POLLNVAL)) *ready |= bits[i];
    } else {
      /* HUP/ERR on a read fd: readable; EOF surfaces via read() == 0. */
      if (re & (POLLIN | POLLERR | POLLHUP | POLLNVAL)) *ready |= bits[i];
    }
  }
  return ASTOOLS_OK;
}

/* Write without ever raising SIGPIPE into the host: block SIGPIPE on this
 * thread, write, consume a SIGPIPE our write made pending, restore the
 * caller's mask. No global signal disposition is changed (sigwait on a
 * pending signal returns immediately; macOS has no sigtimedwait). */
static astools_err write_no_sigpipe(int fd, const void *d, size_t n,
                                    size_t *wrote) {
  sigset_t pipe_set, prev, pending;
  int newly_blocked = 0, was_pending = 0, saved;
  ssize_t r;

  sigemptyset(&pipe_set);
  sigaddset(&pipe_set, SIGPIPE);
#if defined(ASTOOLS_NO_THREADS)
  if (sigprocmask(SIG_BLOCK, &pipe_set, &prev) == 0)
#else
  if (pthread_sigmask(SIG_BLOCK, &pipe_set, &prev) == 0)
#endif
  {
    newly_blocked = !sigismember(&prev, SIGPIPE);
    sigemptyset(&pending);
    if (sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE))
      was_pending = 1;
  }

  do {
    r = write(fd, d, n);
  } while (r < 0 && errno == EINTR);
  saved = errno;

  if (newly_blocked) {
    if (r < 0 && saved == EPIPE && !was_pending) {
      sigemptyset(&pending);
      if (sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE)) {
        int sig;
        (void)sigwait(&pipe_set, &sig);
      }
    }
#if defined(ASTOOLS_NO_THREADS)
    (void)sigprocmask(SIG_SETMASK, &prev, NULL);
#else
    (void)pthread_sigmask(SIG_SETMASK, &prev, NULL);
#endif
  }

  if (r >= 0) {
    *wrote = (size_t)r;
    return ASTOOLS_OK;
  }
  if (saved == EAGAIN || saved == EWOULDBLOCK) return ASTOOLS_ERR_BUSY;
  return ASTOOLS_ERR_IO;
}

astools_err os_proc_write_stdin(os_proc *p, const void *d, size_t n,
                                size_t *wrote) {
  if (!p || !wrote || (!d && n > 0)) return ASTOOLS_ERR_INVALID;
  *wrote = 0;
  if (p->fd_in < 0) return ASTOOLS_ERR_IO;
  if (n == 0) return ASTOOLS_OK;
  if (n > (size_t)SSIZE_MAX) n = (size_t)SSIZE_MAX;
  return write_no_sigpipe(p->fd_in, d, n, wrote);
}

astools_err os_proc_read(os_proc *p, int which, void *buf, size_t n,
                         size_t *got) {
  int *fd;
  ssize_t r;

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
  if (n > (size_t)SSIZE_MAX) n = (size_t)SSIZE_MAX;

  do {
    r = read(*fd, buf, n);
  } while (r < 0 && errno == EINTR);
  if (r > 0) {
    *got = (size_t)r;
    return ASTOOLS_OK;
  }
  if (r == 0) { /* EOF: close and mark */
    close(*fd);
    *fd = -1;
    return ASTOOLS_OK;
  }
  if (errno == EAGAIN || errno == EWOULDBLOCK) return ASTOOLS_ERR_BUSY;
  return ASTOOLS_ERR_IO;
}

void os_proc_close_stdin(os_proc *p) {
  if (!p) return;
  fd_close(&p->fd_in);
}

static void proc_signal(os_proc *p, int sig) {
  pid_t pid;

  if (!p) return;
  if (p->pgroup && p->pgid > 0) {
    /* The leader may already have been reaped while descendants still keep
     * the group alive.  Retaining pgid lets the runtime clean that tail. */
    (void)kill(-(pid_t)p->pgid, sig);
    return;
  }
  if (p->pid <= 0) return; /* never kill(-1)/kill(0) */
  pid = (pid_t)p->pid;
  (void)kill(pid, sig);
}

void os_proc_kill(os_proc *p) { proc_signal(p, SIGKILL); }

void os_proc_terminate(os_proc *p) { proc_signal(p, SIGTERM); }

astools_err os_proc_wait(os_proc *p, int64_t timeout_ms, int *exit_code) {
  pid_t pid, r;
  int status = 0, code;

  if (!p || p->pid <= 0) return ASTOOLS_ERR_INVALID;
  pid = (pid_t)p->pid;

  if (timeout_ms < 0) {
    for (;;) {
      r = waitpid(pid, &status, 0);
      if (r == pid) break;
      if (r < 0 && errno == EINTR) continue;
      p->pid = -1; /* ECHILD etc: never reapable; disarm kill/wait */
      return ASTOOLS_ERR_IO;
    }
  } else {
    int64_t now = os_monotonic_ms();
    int64_t deadline =
        (timeout_ms > INT64_MAX - now) ? INT64_MAX : now + timeout_ms;
    for (;;) {
      r = waitpid(pid, &status, WNOHANG);
      if (r == pid) break;
      if (r < 0) {
        if (errno == EINTR) continue;
        p->pid = -1;
        return ASTOOLS_ERR_IO;
      }
      if (os_monotonic_ms() >= deadline) return ASTOOLS_ERR_BUSY;
      {
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = 2000000L; /* 2 ms */
        (void)nanosleep(&ts, NULL);
      }
    }
  }

  if (WIFEXITED(status))
    code = WEXITSTATUS(status);
  else if (WIFSIGNALED(status))
    code = -WTERMSIG(status);
  else
    code = 127; /* unreachable without WUNTRACED/WCONTINUED */
  /* Reaped: disarm kill/terminate so a recycled pid is never signaled. */
  p->pid = -1;
  if (exit_code) *exit_code = code;
  return ASTOOLS_OK;
}

void os_proc_free(os_proc *p) {
  if (!p) return;
  fd_close(&p->fd_in);
  fd_close(&p->fd_out);
  fd_close(&p->fd_err);
  p->pgid = 0;
}

/* ---- dynamic libraries --------------------------------------------------- */

astools_err os_dylib_open(const char *path, os_dylib *out) {
  if (!path || !out) return ASTOOLS_ERR_INVALID;
  out->h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  return out->h ? ASTOOLS_OK : ASTOOLS_ERR_IO;
}

void *os_dylib_sym(os_dylib *d, const char *name) {
  if (!d || !d->h || !name) return NULL;
  return dlsym(d->h, name);
}

void os_dylib_close(os_dylib *d) {
  if (!d || !d->h) return;
  (void)dlclose(d->h);
  d->h = NULL;
}

#else /* _WIN32 */

typedef int astools_os_proc_posix_unused_on_win32; /* non-empty unit */

#endif /* !_WIN32 */
