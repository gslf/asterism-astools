/* One-shot process capture shared by proc.run and project workflows. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#endif

#include "sdk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <errno.h>
#ifdef _WIN32
#include "compat_win32.h" /* must come after every system include */
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;
#endif


#ifdef _WIN32

int64_t astd_run_clock_ms(void) { return astd_mono_ms(); }

int astd_run_capture(char *const *argv, char *const *envp, const char *cwd,
                     const char *input, size_t input_n, size_t out_cap,
                     size_t err_cap, int64_t timeout_ms, astd_run_res *rr,
                     char *emsg, size_t emsg_sz) {
  astd_spawn_res wr;
  int rc = astd_spawn_capture(argv, envp, cwd, input, input_n, out_cap,
                              err_cap, timeout_ms, &wr, emsg, emsg_sz);
  if (rc != 0) return rc;
  rr->exit_code = wr.exit_code;
  rr->timed_out = wr.timed_out;
  rr->out = wr.out;
  rr->out_n = wr.out_n;
  rr->out_trunc = wr.out_trunc;
  rr->err = wr.err;
  rr->err_n = wr.err_n;
  rr->err_trunc = wr.err_trunc;
  return 0;
}

#else /* POSIX */

int64_t astd_run_clock_ms(void) {
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
  /* A partial report is an infrastructure error, never exec success. */
  if (write(fd, msg, sizeof msg) < 0) { /* nothing to do */
  }
  _exit(127);
}

static int exec_ready(int fd, int64_t deadline) {
  for (;;) {
    int64_t remaining = deadline ? deadline-astd_run_clock_ms() : -1;
    if (deadline && remaining <= 0) return 0;
    struct pollfd p = {.fd=fd,.events=POLLIN};
    int ready = poll(&p,1,remaining > INT32_MAX ? INT32_MAX : (int)remaining);
    if (!ready && deadline && astd_run_clock_ms() < deadline) continue;
    if (ready >= 0 || errno != EINTR) return ready;
  }
}

/* Pipe EOF does not imply process exit: preserve the deadline while reaping. */
static int wait_child(pid_t pid, int *status, int64_t deadline, astd_run_res *rr) {
  for (;;) {
    pid_t got = waitpid(pid,status,deadline ? WNOHANG : 0);
    if (got == pid) return 0;
    if (got < 0) { if (errno == EINTR) continue; return -1; }
    int64_t remaining = deadline - astd_run_clock_ms();
    if (remaining <= 0) {
      if (kill(pid,SIGKILL) != 0 && errno != ESRCH) return -1;
      rr->timed_out = 1;
      deadline = 0;
    } else {
      if (remaining > 20) remaining = 20;
      struct timespec pause = {0,(long)remaining * 1000000};
      (void)nanosleep(&pause,NULL);
    }
  }
}

/* Spawn argv with envp, feed input, capture both streams with caps.
 * timeout_ms 0 = no local deadline. Returns 0, or -1 with emsg set
 * (spawn, exec or capture failure; effects may already have occurred). */
int astd_run_capture(char *const *argv, char *const *envp, const char *cwd,
                     const char *input, size_t input_n, size_t out_cap,
                     size_t err_cap, int64_t timeout_ms, astd_run_res *rr,
                     char *emsg, size_t emsg_sz) {
  int inp[2] = {-1, -1}, outp[2] = {-1, -1};
  int errp[2] = {-1, -1}, exep[2] = {-1, -1};
  size_t ocap = 0, ecap = 0, inoff = 0;
  pid_t pid;
  int st = 0, killed = 0;
  int64_t deadline = 0;
  memset(rr, 0, sizeof *rr);
  if (timeout_ms > 0) {
    int64_t now = astd_run_clock_ms();
    deadline = timeout_ms > INT64_MAX-now ? INT64_MAX : now+timeout_ms;
  }
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
    size_t used = 0;
    ssize_t got = 0;
    while (used < sizeof m) {
      int ready = exec_ready(exep[0],deadline);
      if (!ready) {
        rr->timed_out = rr->out_trunc = rr->err_trunc = 1;
        (void)kill(pid,SIGKILL);
        close_fd(&exep[0]); close_fd(&inp[1]); close_fd(&outp[0]); close_fd(&errp[0]);
        goto child_done;
      }
      if (ready < 0) { got = -1; break; }
      got = read(exep[0],m+used,sizeof m-used);
      if (got > 0) used += (size_t)got;
      else if (!got || errno != EINTR) break;
    }
    close_fd(&exep[0]);
    if (got < 0 || (used && used != sizeof m)) {
      snprintf(emsg,emsg_sz,"incomplete exec-status report");
      goto capture_fail;
    }
    if (used == sizeof m) {
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
  if (fcntl(inp[1], F_SETFL, O_NONBLOCK) < 0 || fcntl(outp[0], F_SETFL, O_NONBLOCK) < 0 ||
      fcntl(errp[0], F_SETFL, O_NONBLOCK) < 0 || inp[1] >= FD_SETSIZE ||
      outp[0] >= FD_SETSIZE || errp[0] >= FD_SETSIZE) {
    snprintf(emsg,emsg_sz,"capture descriptors cannot be polled safely");
    goto capture_fail;
  }
  if (!input || input_n == 0) close_fd(&inp[1]);
  while (outp[0] >= 0 || errp[0] >= 0 || inp[1] >= 0) {
    fd_set rf, wf;
    struct timeval tv;
    struct timeval *ptv = NULL;
    int maxfd = -1, rc;
    if (deadline != 0 && !killed) {
      int64_t rem = deadline - astd_run_clock_ms();
      if (rem <= 0) {
        kill(pid, SIGKILL);
        killed = 1;
        rr->timed_out = 1;
        rr->out_trunc |= outp[0] >= 0;
        rr->err_trunc |= errp[0] >= 0;
        /* Descendants may inherit these pipe ends.  Stop waiting for their
         * EOF after the direct child deadline; the enclosing ⁂ astools runtime
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
      snprintf(emsg,emsg_sz,"capture select: %s",strerror(errno));
      goto capture_fail;
    }
    if (rc == 0) continue; /* deadline re-checked at loop top */
    if (outp[0] >= 0 && FD_ISSET(outp[0], &rf)) {
      char tmp[8192];
      ssize_t n = read(outp[0], tmp, sizeof tmp);
      if (n > 0) {
        if (cap_append(&rr->out, &rr->out_n, &ocap, out_cap, &rr->out_trunc,
                       tmp, (size_t)n) != 0) {
          snprintf(emsg,emsg_sz,"capture: out of memory");
          goto capture_fail;
        }
      } else if (n < 0 && errno != EAGAIN && errno != EINTR) {
        snprintf(emsg,emsg_sz,"capture stdout: %s",strerror(errno));
        goto capture_fail;
      } else if (n == 0) {
        close_fd(&outp[0]);
      }
    }
    if (errp[0] >= 0 && FD_ISSET(errp[0], &rf)) {
      char tmp[8192];
      ssize_t n = read(errp[0], tmp, sizeof tmp);
      if (n > 0) {
        if (cap_append(&rr->err, &rr->err_n, &ecap, err_cap, &rr->err_trunc,
                       tmp, (size_t)n) != 0) {
          snprintf(emsg,emsg_sz,"capture: out of memory");
          goto capture_fail;
        }
      } else if (n < 0 && errno != EAGAIN && errno != EINTR) {
        snprintf(emsg,emsg_sz,"capture stderr: %s",strerror(errno));
        goto capture_fail;
      } else if (n == 0) {
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
child_done:
  if (wait_child(pid,&st,deadline,rr) != 0) {
    snprintf(emsg,emsg_sz,"capture wait: %s",strerror(errno));
    goto spawn_fail;
  }
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
  if (!rr->out || !rr->err) {
    snprintf(emsg,emsg_sz,"capture: out of memory");
    goto spawn_fail;
  }
  return 0;
capture_fail:
  (void)kill(pid,SIGKILL);
  while (waitpid(pid,&st,0) < 0 && errno == EINTR) continue;
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

#endif /* POSIX spawn */
