/*
 * os_proc_stub.c — honest ASTOOLS_ERR_UNSUPPORTED backend for the astools
 * process/path extension of os.h (Windows implementation deferred).
 *
 * Added to the build only where os_proc_posix.c is not (WIN32), but the
 * file itself compiles on every platform; it is never linked together
 * with the real backend. Out-parameters are still zeroed defensively so
 * a caller that ignores the verdict finds inert values, never garbage.
 */

#include "os.h"

#include <string.h>

/* ---- paths and stat ------------------------------------------------------ */

astools_err os_stat(const char *path, os_stat_info *out) {
  (void)path;
  if (out) memset(out, 0, sizeof(*out)); /* type == OS_FT_MISSING */
  return ASTOOLS_ERR_UNSUPPORTED;
}

astools_err os_lstat(const char *path, os_stat_info *out) {
  (void)path;
  if (out) memset(out, 0, sizeof(*out));
  return ASTOOLS_ERR_UNSUPPORTED;
}

astools_err os_realpath(const char *path, char **out) {
  (void)path;
  if (out) *out = NULL;
  return ASTOOLS_ERR_UNSUPPORTED;
}

int os_path_is_abs(const char *path) {
  (void)path;
  return 0;
}

char *os_getcwd_dup(void) { return NULL; }

astools_err os_exe_path(char **out) {
  if (out) *out = NULL;
  return ASTOOLS_ERR_UNSUPPORTED;
}

astools_err os_list_subdirs(const char *path, char ***out_names,
                            size_t *out_n) {
  (void)path;
  if (out_names) *out_names = NULL;
  if (out_n) *out_n = 0;
  return ASTOOLS_ERR_UNSUPPORTED;
}

astools_err os_rmtree(const char *path) {
  (void)path;
  return ASTOOLS_ERR_UNSUPPORTED;
}

char *os_env_dup(const char *name) {
  (void)name;
  return NULL;
}

/* ---- child processes ----------------------------------------------------- */

astools_err os_proc_spawn(const os_spawn_opts *o, os_proc *p) {
  (void)o;
  if (p) {
    memset(p, 0, sizeof(*p));
    p->fd_in = p->fd_out = p->fd_err = -1;
  }
  return ASTOOLS_ERR_UNSUPPORTED;
}

astools_err os_proc_user_tasks(int64_t *out) {
  /* Windows caps processes with a Job Object, not a per-uid rlimit. */
  if (out) *out = 0;
  return ASTOOLS_ERR_UNSUPPORTED;
}

astools_err os_proc_poll(os_proc *p, int want_write, int64_t timeout_ms,
                         unsigned *ready) {
  (void)p;
  (void)want_write;
  (void)timeout_ms;
  if (ready) *ready = 0;
  return ASTOOLS_ERR_UNSUPPORTED;
}

astools_err os_proc_write_stdin(os_proc *p, const void *d, size_t n,
                                size_t *wrote) {
  (void)p;
  (void)d;
  (void)n;
  if (wrote) *wrote = 0;
  return ASTOOLS_ERR_UNSUPPORTED;
}

astools_err os_proc_read(os_proc *p, int which, void *buf, size_t n,
                         size_t *got) {
  (void)p;
  (void)which;
  (void)buf;
  (void)n;
  if (got) *got = 0;
  return ASTOOLS_ERR_UNSUPPORTED;
}

void os_proc_close_stdin(os_proc *p) { (void)p; }

void os_proc_kill(os_proc *p) { (void)p; }

void os_proc_terminate(os_proc *p) { (void)p; }

astools_err os_proc_wait(os_proc *p, int64_t timeout_ms, int *exit_code) {
  (void)p;
  (void)timeout_ms;
  (void)exit_code;
  return ASTOOLS_ERR_UNSUPPORTED;
}

void os_proc_free(os_proc *p) { (void)p; }

/* ---- dynamic libraries --------------------------------------------------- */

astools_err os_dylib_open(const char *path, os_dylib *out) {
  (void)path;
  if (out) out->h = NULL;
  return ASTOOLS_ERR_UNSUPPORTED;
}

void *os_dylib_sym(os_dylib *d, const char *name) {
  (void)d;
  (void)name;
  return NULL;
}

void os_dylib_close(os_dylib *d) { (void)d; }
