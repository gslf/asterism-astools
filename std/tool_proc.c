/*
 * tool_proc.c — proc.run.
 *
 * Direct exec, never a shell: argv[0] with a '/' is exec'd as a path,
 * otherwise resolved through PATH (execvp). The child stays in the
 * invocation's process group (no setpgid) so the runtime's deadline kill
 * of the group reaps it too. env entries are ADDED onto the inherited —
 * already scrubbed — environment, overriding same-named variables.
 *
 * A local 'timeout' (ISO 8601 PnDTnHnMnS subset) SIGKILLs the child;
 * that is a NORMAL result, not a tool error: exit_code is reported as
 * -signal (-9 for the timeout kill), with whatever output was captured.
 * Without the arg no local deadline exists — the runtime's invocation
 * deadline covers the call.
 *
 * Both streams are captured up to 262144 bytes each; the rest is drained
 * and discarded with the matching *_truncated flag set. Captured bytes
 * are returned as strings; a NUL byte in the output ends the string (the
 * wire type is string, not bytes).
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

int astd_tool_proc(astd_req *r) {
  astd_fail(r, "std/unsupported", "proc is not supported on Windows yet");
  return 0;
}

#else /* POSIX */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#define PROC_STREAM_CAP 262144

typedef struct {
  int exit_code;
  int timed_out;
  char *out;
  size_t out_n;
  int out_trunc;
  char *err;
  size_t err_n;
  int err_trunc;
} prun;

static int64_t mono_ms(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
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
  /* best effort; the parent treats a short read as exec success */
  if (write(fd, msg, sizeof msg) < 0) { /* nothing to do */
  }
  _exit(127);
}

/* Spawn argv with envp, feed input, capture both streams with caps.
 * timeout_ms 0 = no local deadline. Returns 0, or -1 with emsg set
 * (spawn/exec failure — nothing ran). */
static int run_child(char *const *argv, char *const *envp, const char *cwd,
                     const char *input, size_t input_n, size_t out_cap,
                     size_t err_cap, int64_t timeout_ms, prun *rr, char *emsg,
                     size_t emsg_sz) {
  int inp[2] = {-1, -1}, outp[2] = {-1, -1};
  int errp[2] = {-1, -1}, exep[2] = {-1, -1};
  size_t ocap = 0, ecap = 0, inoff = 0;
  pid_t pid;
  int st = 0, killed = 0;
  int64_t deadline = 0;
  memset(rr, 0, sizeof *rr);
  if (pipe(inp) != 0 || pipe(outp) != 0 || pipe(errp) != 0 ||
      pipe(exep) != 0) {
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
    /* child: same process group by design */
    if (dup2(inp[0], 0) < 0 || dup2(outp[1], 1) < 0 || dup2(errp[1], 2) < 0)
      child_report(exep[1], 'p');
    close(inp[0]);
    close(inp[1]);
    close(outp[0]);
    close(outp[1]);
    close(errp[0]);
    close(errp[1]);
    close(exep[0]);
    if (cwd && chdir(cwd) != 0) child_report(exep[1], 'c');
    environ = (char **)envp;
    if (strchr(argv[0], '/'))
      execv(argv[0], argv);
    else
      execvp(argv[0], argv);
    child_report(exep[1], 'x');
  }
  close_fd(&inp[0]);
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
      if (m[0] == 'c')
        snprintf(emsg, emsg_sz, "cannot chdir to '%s': %s",
                 cwd ? cwd : "(null)", strerror(ce));
      else
        snprintf(emsg, emsg_sz, "cannot execute '%s': %s", argv[0],
                 strerror(ce));
      goto spawn_fail;
    }
  }
  fcntl(inp[1], F_SETFL, O_NONBLOCK);
  fcntl(outp[0], F_SETFL, O_NONBLOCK);
  fcntl(errp[0], F_SETFL, O_NONBLOCK);
  if (!input || input_n == 0) close_fd(&inp[1]);
  if (timeout_ms > 0) deadline = mono_ms() + timeout_ms;
  while (outp[0] >= 0 || errp[0] >= 0 || inp[1] >= 0) {
    fd_set rf, wf;
    struct timeval tv;
    struct timeval *ptv = NULL;
    int maxfd = -1, rc;
    if (deadline != 0 && !killed) {
      int64_t rem = deadline - mono_ms();
      if (rem <= 0) {
        kill(pid, SIGKILL);
        killed = 1;
        rr->timed_out = 1;
        /* Descendants may inherit these pipe ends.  Stop waiting for their
         * EOF after the direct child deadline; the enclosing astools runtime
         * owns the invocation process group and removes any residual tree. */
        close_fd(&inp[1]);
        close_fd(&outp[0]);
        close_fd(&errp[0]);
        /* Re-evaluate the loop condition now.  Falling through would call
         * select(0, ..., NULL) after all descriptors were closed and block
         * forever. */
        continue;
      } else {
        tv.tv_sec = (time_t)(rem / 1000);
        tv.tv_usec = (suseconds_t)((rem % 1000) * 1000);
        ptv = &tv;
      }
    }
    FD_ZERO(&rf);
    FD_ZERO(&wf);
    if (outp[0] >= 0) {
      FD_SET(outp[0], &rf);
      if (outp[0] > maxfd) maxfd = outp[0];
    }
    if (errp[0] >= 0) {
      FD_SET(errp[0], &rf);
      if (errp[0] > maxfd) maxfd = errp[0];
    }
    if (inp[1] >= 0) {
      FD_SET(inp[1], &wf);
      if (inp[1] > maxfd) maxfd = inp[1];
    }
    rc = select(maxfd + 1, &rf, &wf, NULL, ptv);
    if (rc < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (rc == 0) continue; /* deadline re-checked at loop top */
    if (outp[0] >= 0 && FD_ISSET(outp[0], &rf)) {
      char tmp[8192];
      ssize_t n = read(outp[0], tmp, sizeof tmp);
      if (n > 0) {
        if (cap_append(&rr->out, &rr->out_n, &ocap, out_cap, &rr->out_trunc,
                       tmp, (size_t)n) != 0)
          close_fd(&outp[0]);
      } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
        close_fd(&outp[0]);
      }
    }
    if (errp[0] >= 0 && FD_ISSET(errp[0], &rf)) {
      char tmp[8192];
      ssize_t n = read(errp[0], tmp, sizeof tmp);
      if (n > 0) {
        if (cap_append(&rr->err, &rr->err_n, &ecap, err_cap, &rr->err_trunc,
                       tmp, (size_t)n) != 0)
          close_fd(&errp[0]);
      } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
        close_fd(&errp[0]);
      }
    }
    if (inp[1] >= 0 && FD_ISSET(inp[1], &wf)) {
      size_t chunk = input_n - inoff;
      ssize_t n;
      if (chunk > 65536) chunk = 65536;
      n = write(inp[1], input + inoff, chunk);
      if (n > 0) {
        inoff += (size_t)n;
        if (inoff >= input_n) close_fd(&inp[1]);
      } else if (n < 0 && errno != EAGAIN && errno != EINTR) {
        close_fd(&inp[1]); /* EPIPE: child stopped reading */
      }
    }
  }
  while (waitpid(pid, &st, 0) < 0 && errno == EINTR) continue;
  if (WIFEXITED(st))
    rr->exit_code = WEXITSTATUS(st);
  else if (WIFSIGNALED(st))
    rr->exit_code = -WTERMSIG(st);
  else
    rr->exit_code = -1;
  if (!rr->out) {
    rr->out = malloc(1);
    if (rr->out) rr->out[0] = '\0';
  }
  if (!rr->err) {
    rr->err = malloc(1);
    if (rr->err) rr->err[0] = '\0';
  }
  return 0;
spawn_fail:
  close_fd(&inp[0]);
  close_fd(&inp[1]);
  close_fd(&outp[0]);
  close_fd(&outp[1]);
  close_fd(&errp[0]);
  close_fd(&errp[1]);
  close_fd(&exep[0]);
  close_fd(&exep[1]);
  free(rr->out);
  free(rr->err);
  rr->out = rr->err = NULL;
  return -1;
}

/* ISO 8601 duration, PnDTnHnMnS subset; fractions allowed. Returns 0
 * and milliseconds, or -1 on malformed input. */
static int parse_duration_ms(const char *s, int64_t *out) {
  const char *p = s;
  double total = 0;
  int any = 0, in_time = 0;
  if (!p || *p != 'P') return -1;
  p++;
  while (*p) {
    double v;
    char *endp;
    if (*p == 'T') {
      if (in_time) return -1;
      in_time = 1;
      p++;
      continue;
    }
    if ((*p < '0' || *p > '9') && *p != '.') return -1;
    v = strtod(p, &endp);
    if (endp == p || v < 0) return -1;
    p = endp;
    switch (*p) {
      case 'D':
        if (in_time) return -1;
        total += v * 86400000.0;
        break;
      case 'H':
        if (!in_time) return -1;
        total += v * 3600000.0;
        break;
      case 'M':
        if (!in_time) return -1;
        total += v * 60000.0;
        break;
      case 'S':
        if (!in_time) return -1;
        total += v * 1000.0;
        break;
      default:
        return -1;
    }
    p++;
    any = 1;
  }
  if (!any || total > 9e15) return -1;
  *out = (int64_t)total;
  return 0;
}

/* Inherited environment plus overrides from the env arg (map<string>).
 * Every entry is duplicated so free_env can free uniformly. */
static char **build_env(const xcdn_value_t *envmap, int *bad) {
  size_t base_n = 0, add_n = 0, i, o = 0;
  char **out;
  *bad = 0;
  while (environ[base_n]) base_n++;
  if (envmap && envmap->type == XCDN_VAL_OBJECT)
    add_n = xcdn_object_len(envmap);
  out = calloc(base_n + add_n + 1, sizeof *out);
  if (!out) return NULL;
  for (i = 0; i < base_n; i++) {
    const char *e = environ[i];
    const char *eq = strchr(e, '=');
    size_t klen = eq ? (size_t)(eq - e) : strlen(e);
    int overridden = 0;
    size_t j;
    for (j = 0; j < add_n; j++) {
      const char *k = xcdn_object_key_at(envmap, j);
      if (k && strlen(k) == klen && strncmp(k, e, klen) == 0) {
        overridden = 1;
        break;
      }
    }
    if (overridden) continue;
    out[o] = malloc(strlen(e) + 1);
    if (!out[o]) goto fail;
    memcpy(out[o], e, strlen(e) + 1);
    o++;
  }
  for (i = 0; i < add_n; i++) {
    const char *k = xcdn_object_key_at(envmap, i);
    xcdn_node_t *nd = xcdn_object_node_at(envmap, i);
    const char *v = nd && nd->value ? xcdn_value_as_string(nd->value) : NULL;
    size_t kl, vl;
    if (!k || !v) {
      *bad = 1;
      goto fail;
    }
    kl = strlen(k);
    vl = strlen(v);
    out[o] = malloc(kl + vl + 2);
    if (!out[o]) goto fail;
    memcpy(out[o], k, kl);
    out[o][kl] = '=';
    memcpy(out[o] + kl + 1, v, vl + 1);
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

int astd_tool_proc(astd_req *r) {
  const xcdn_value_t *av, *envmap, *tv;
  const char *cwd, *input;
  char **argv = NULL, **envp = NULL;
  size_t n, i;
  int64_t timeout_ms = 0, t0, elapsed;
  int bad_env = 0;
  prun rr;
  char emsg[512];
  if (strcmp(r->command, "run") != 0) {
    astd_fail(r, "astools/protocol", "unknown proc command '%s'", r->command);
    return 0;
  }
  av = astd_arg(r, "argv");
  if (!av || av->type != XCDN_VAL_ARRAY || (n = xcdn_array_len(av)) < 1) {
    astd_fail(r, "astools/invalid-args",
              "argv must be a non-empty array of strings");
    return 0;
  }
  argv = calloc(n + 1, sizeof *argv);
  if (!argv) {
    astd_fail(r, "proc/failed", "out of memory");
    return 0;
  }
  for (i = 0; i < n; i++) {
    xcdn_node_t *nd = xcdn_array_get(av, i);
    const char *s = nd && nd->value ? xcdn_value_as_string(nd->value) : NULL;
    if (!s) {
      free(argv);
      astd_fail(r, "astools/invalid-args", "argv[%lu] is not a string",
                (unsigned long)i);
      return 0;
    }
    argv[i] = (char *)s; /* borrowed from the request document */
  }
  cwd = astd_arg_str(r, "cwd", NULL);
  if (!cwd) cwd = r->scratch ? r->scratch : r->workspace;
  input = astd_arg_str(r, "stdin", NULL);
  envmap = astd_arg(r, "env");
  tv = astd_arg(r, "timeout");
  if (tv) {
    const char *ts = xcdn_value_as_string(tv);
    if (!ts || parse_duration_ms(ts, &timeout_ms) != 0) {
      free(argv);
      astd_fail(r, "astools/invalid-args",
                "timeout is not a valid ISO 8601 duration");
      return 0;
    }
  }
  envp = build_env(envmap, &bad_env);
  if (!envp) {
    free(argv);
    astd_fail(r, bad_env ? "astools/invalid-args" : "proc/failed",
              bad_env ? "env values must be strings" : "out of memory");
    return 0;
  }
  signal(SIGPIPE, SIG_IGN);
  t0 = mono_ms();
  if (run_child(argv, envp, cwd, input, input ? strlen(input) : 0,
                PROC_STREAM_CAP, PROC_STREAM_CAP, timeout_ms, &rr, emsg,
                sizeof emsg) != 0) {
    free(argv);
    free_env(envp);
    astd_fail(r, "proc/failed", "%s", emsg);
    return 0;
  }
  elapsed = mono_ms() - t0;
  if (elapsed < 0) elapsed = 0;
  free(argv);
  free_env(envp);
  {
    xcdn_value_t *res = xcdn_value_object();
    char dur[48];
    int rc = 0;
    snprintf(dur, sizeof dur, "PT%lld.%03dS", (long long)(elapsed / 1000),
             (int)(elapsed % 1000));
    if (!res) {
      free(rr.out);
      free(rr.err);
      astd_fail(r, "proc/failed", "out of memory");
      return 0;
    }
    rc |= astd_set_int(res, "exit_code", (int64_t)rr.exit_code);
    rc |= astd_set_str(res, "stdout", rr.out ? rr.out : "");
    rc |= astd_set_str(res, "stderr", rr.err ? rr.err : "");
    rc |= astd_set_bool(res, "stdout_truncated", rr.out_trunc);
    rc |= astd_set_bool(res, "stderr_truncated", rr.err_trunc);
    {
      xcdn_value_t *dv = xcdn_value_duration(dur);
      if (!dv) {
        rc = -1;
      } else if (astd_set_val(res, "duration", dv) != 0) {
        xcdn_value_free(dv);
        rc = -1;
      }
    }
    free(rr.out);
    free(rr.err);
    if (rc != 0) {
      xcdn_value_free(res);
      astd_fail(r, "proc/failed", "out of memory");
      return 0;
    }
    astd_ok(r, res);
  }
  return 0;
}

#endif /* POSIX */
