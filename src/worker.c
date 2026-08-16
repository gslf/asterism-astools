/*
 * worker.c — execution backends and supervision: slot
 * admission, oneshot exec, persistent process table, library vtables,
 * supervisor thread, async tasks.
 *
 * Containment invariants: a child is always reaped (never leaked), stdout
 * is capped at max_output_bytes (overflow kills, never truncates and
 * continues), stderr keeps the newest bytes, every loop polls the
 * cancel flag and the monotonic deadline. Tool output is hostile input:
 * nothing from a pipe is trusted until proto.c has validated it.
 */

#include "astools_internal.h"
#include "astools_tool_abi.h"

#include <stdlib.h>
#include <string.h>

/* ---- shared helpers ------------------------------------------------------ */

static void result_clear(astools_result *r) {
  if (!r) return;
  free(r->result_xcdn);
  free(r->error_code);
  free(r->error_message);
  r->result_xcdn = NULL;
  r->error_code = NULL;
  r->error_message = NULL;
  r->ok = 0;
}

/* Engine-produced tool-visible error (reserved astools/... codes). Keeps
 * exit_code/duration untouched; strdup failures degrade to NULL fields. */
static void result_set(astools_result *r, const char *code, const char *fmt,
                       ...) {
  va_list ap;
  char msg[512];
  if (!r) return;
  result_clear(r);
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  r->error_code = astools_strdup(code);
  r->error_message = astools_strdup(msg);
}

/* Sleep without a dedicated primitive in os.h: timed wait on a private
 * condition variable nobody signals. */
static void worker_sleep_ms(int64_t ms) {
  os_mutex mu;
  os_cond cv;
  if (ms <= 0) return;
  os_mutex_init(&mu);
  os_cond_init(&cv);
  os_mutex_lock(&mu);
  (void)os_cond_timedwait(&cv, &mu, ms);
  os_mutex_unlock(&mu);
  os_cond_destroy(&cv);
  os_mutex_destroy(&mu);
}

/* Append to the stderr capture keeping the NEWEST bytes: when the cap is
 * exceeded the oldest half is discarded. Best effort: OOM drops. */
static void stderr_append(astools_buf *b, const char *data, size_t n,
                          int64_t cap_bytes) {
  size_t cap = cap_bytes > 0 ? (size_t)cap_bytes : (size_t)262144;
  if (n > cap) {
    data += n - cap;
    n = cap;
  }
  if (astools_buf_append(b, data, n) != ASTOOLS_OK) return;
  if (b->len > cap) {
    size_t keep = cap / 2;
    memmove(b->data, b->data + b->len - keep, keep);
    b->len = keep;
    b->data[b->len] = '\0';
  }
}

/* Last <= 512 bytes of captured stderr as a single-line-ish C string. */
static void stderr_excerpt(const astools_buf *errb, char *out, size_t cap) {
  size_t n = errb->len;
  size_t take = n < 512 ? n : 512;
  size_t i, o = 0;
  const char *src = errb->data ? errb->data + (n - take) : "";
  for (i = 0; i < take && o + 1 < cap; i++) {
    char ch = src[i];
    out[o++] = (ch == '\0') ? ' ' : ch;
  }
  out[o] = '\0';
}

/* ---- slot admission (invocation.max_concurrent) -------------------------- */

astools_err astools_slot_acquire(astools_ctx *c, int64_t deadline_mono) {
  int max;
  if (!c) return ASTOOLS_ERR_INVALID;
  max = c->cfg.max_concurrent > 0 ? c->cfg.max_concurrent : 1;
  os_mutex_lock(&c->slot_mu);
  while (c->slots_used >= max) {
    int64_t wait_ms = 100;
    if (deadline_mono > 0) {
      int64_t now = astools_mono(c);
      if (now >= deadline_mono) {
        os_mutex_unlock(&c->slot_mu);
        return ASTOOLS_ERR_TIMEOUT;
      }
      wait_ms = deadline_mono - now;
      /* short slices so injected test clocks are re-checked */
      if (wait_ms > 100) wait_ms = 100;
    }
    if (c->no_threads) {
      /* single-threaded: nobody can ever release a slot while we wait */
      os_mutex_unlock(&c->slot_mu);
      return deadline_mono > 0 ? ASTOOLS_ERR_TIMEOUT : ASTOOLS_ERR_BUSY;
    }
    (void)os_cond_timedwait(&c->slot_cv, &c->slot_mu, wait_ms);
  }
  c->slots_used++;
  os_mutex_unlock(&c->slot_mu);
  return ASTOOLS_OK;
}

void astools_slot_release(astools_ctx *c) {
  if (!c) return;
  os_mutex_lock(&c->slot_mu);
  if (c->slots_used > 0) c->slots_used--;
  os_cond_signal(&c->slot_cv);
  os_mutex_unlock(&c->slot_mu);
}

/* ---- oneshot execution -------------------------------------------- */

enum {
  ONE_RUNNING = 0,
  ONE_EOF,
  ONE_TIMEOUT,
  ONE_CANCEL,
  ONE_OVERFLOW,
  ONE_NOMEM
};

static int task_cancelled(astools_task *t) {
  int cancelled;
  if (!t) return 0;
  os_mutex_lock(&t->mu);
  cancelled = t->cancelled ? 1 : 0;
  os_mutex_unlock(&t->mu);
  return cancelled;
}

astools_err astools_exec_oneshot(astools_ctx *c,
                                 const astools_sandbox_setup *setup,
                                 const char *request_text,
                                 const char *invocation_id,
                                 int64_t deadline_mono,
                                 int64_t max_output_bytes,
                                 int64_t stderr_max_bytes,
                                 astools_task *cancel_task,
                                 astools_result *r, int *exit_code,
                                 char **stderr_cap) {
  os_spawn_opts o;
  os_proc p;
  astools_buf outb, errb;
  const char *wr;
  size_t wr_len, wr_off = 0;
  bool stdin_open = true;
  int status = ONE_RUNNING;
  astools_err e, verdict = ASTOOLS_OK;
  int64_t cpu, now;

  if (!c || !setup || !request_text || !invocation_id || !r || !exit_code)
    return ASTOOLS_ERR_INVALID;
  memset(r, 0, sizeof *r);
  *exit_code = 0;
  if (stderr_cap) *stderr_cap = NULL;

  now = astools_mono(c);
  /* CPU backstop derived from the wall deadline (+5s of slack). */
  cpu = (deadline_mono - now) / 1000 + 5;
  if (cpu < 1) cpu = 1;
  if (setup->limit_cpu_seconds > 0 && setup->limit_cpu_seconds < cpu)
    cpu = setup->limit_cpu_seconds;

  memset(&o, 0, sizeof o);
  o.argv = (const char *const *)setup->argv;
  o.envp = (const char *const *)setup->envp;
  o.cwd = setup->scratch_dir;
  o.limit_cpu_seconds = cpu;
  o.limit_mem_bytes = setup->limit_mem_bytes;
  o.limit_nproc = setup->limit_nproc;

  e = os_proc_spawn(&o, &p);
  if (e != ASTOOLS_OK) {
    result_set(r, "astools/tool-crashed", "failed to spawn tool process");
    return astools_seterr(c, ASTOOLS_ERR_TOOL,
                          "cannot spawn tool process (%s)",
                          astools_err_name(e));
  }

  astools_buf_init(&outb);
  astools_buf_init(&errb);
  wr = request_text;
  wr_len = strlen(request_text);

  for (;;) {
    unsigned ready = 0;
    int want_write;
    int64_t slice;
    now = astools_mono(c);
    if (task_cancelled(cancel_task)) {
      status = ONE_CANCEL;
      break;
    }
    if (now >= deadline_mono) {
      status = ONE_TIMEOUT;
      break;
    }
    slice = deadline_mono - now;
    if (slice > 100) slice = 100; /* keep polling cancel + deadline */
    want_write = stdin_open && wr_off < wr_len;
    if (os_proc_poll(&p, want_write, slice, &ready) != ASTOOLS_OK) {
      status = ONE_EOF;
      break;
    }
    if ((ready & OS_READY_IN) && want_write) {
      size_t wrote = 0;
      astools_err we =
          os_proc_write_stdin(&p, wr + wr_off, wr_len - wr_off, &wrote);
      if (we == ASTOOLS_OK) {
        wr_off += wrote;
      } else if (we != ASTOOLS_ERR_BUSY) {
        /* child closed stdin (may still emit a response) */
        os_proc_close_stdin(&p);
        stdin_open = false;
      }
      if (stdin_open && wr_off >= wr_len) {
        os_proc_close_stdin(&p);
        stdin_open = false;
      }
    }
    if (ready & OS_READY_OUT) {
      char tmp[8192];
      size_t got = 0;
      astools_err re = os_proc_read(&p, OS_PIPE_OUT, tmp, sizeof tmp, &got);
      if (re == ASTOOLS_OK && got == 0) {
        status = ONE_EOF;
        break;
      }
      if (re == ASTOOLS_OK) {
        if (astools_buf_append(&outb, tmp, got) != ASTOOLS_OK) {
          status = ONE_NOMEM;
          break;
        }
        if (max_output_bytes > 0 && (int64_t)outb.len > max_output_bytes) {
          status = ONE_OVERFLOW;
          break;
        }
      } else if (re != ASTOOLS_ERR_BUSY) {
        status = ONE_EOF;
        break;
      }
    }
    if (ready & OS_READY_ERR) {
      char tmp[4096];
      size_t got = 0;
      astools_err re = os_proc_read(&p, OS_PIPE_ERR, tmp, sizeof tmp, &got);
      if (re == ASTOOLS_OK && got > 0)
        stderr_append(&errb, tmp, got, stderr_max_bytes);
    }
    if (p.fd_out < 0) {
      status = ONE_EOF;
      break;
    }
  }

  /* Reap: natural EOF gets a 5s grace before the kill; every abnormal
   * termination kills the whole process group immediately. */
  if (status == ONE_EOF) {
    if (os_proc_wait(&p, 5000, exit_code) != ASTOOLS_OK) {
      os_proc_kill(&p);
      (void)os_proc_wait(&p, -1, exit_code);
    }
  } else {
    os_proc_kill(&p);
    (void)os_proc_wait(&p, -1, exit_code);
  }

  /* Drain any stderr the child left behind (it is dead; reads are cheap). */
  if (p.fd_err >= 0) {
    char tmp[4096];
    size_t got = 0;
    while (os_proc_read(&p, OS_PIPE_ERR, tmp, sizeof tmp, &got) ==
               ASTOOLS_OK &&
           got > 0)
      stderr_append(&errb, tmp, got, stderr_max_bytes);
  }
  /* A well-behaved oneshot exits without descendants.  If it deliberately
   * orphaned any, the process group can outlive its reaped leader; never let
   * those processes escape merely because the response was well formed. */
  os_proc_kill(&p);
  os_proc_free(&p);

  switch (status) {
    case ONE_CANCEL:
      result_set(r, "astools/cancelled", "invocation cancelled");
      verdict =
          astools_seterr(c, ASTOOLS_ERR_CANCELLED, "invocation cancelled");
      break;
    case ONE_TIMEOUT:
      result_set(r, "astools/timeout", "deadline exceeded");
      verdict = astools_seterr(c, ASTOOLS_ERR_TIMEOUT,
                               "tool exceeded its deadline");
      break;
    case ONE_OVERFLOW:
      result_set(r, "astools/overflow",
                 "stdout exceeded max_output_bytes (%lld)",
                 (long long)max_output_bytes);
      verdict = astools_seterr(c, ASTOOLS_ERR_TOOL,
                               "tool stdout exceeded max_output_bytes (%lld)",
                               (long long)max_output_bytes);
      break;
    case ONE_NOMEM:
      verdict = ASTOOLS_ERR_NOMEM;
      break;
    default: { /* ONE_EOF: parse the response */
      char *rid = NULL, *perr = NULL;
      astools_err pe = astools_proto_parse_response(
          outb.data ? outb.data : "", outb.len, invocation_id, &rid, r,
          &perr);
      if (pe == ASTOOLS_OK) {
        /* well-formed response is authoritative regardless of exit code */
        verdict = ASTOOLS_OK;
      } else if (*exit_code != 0) {
        char ex[520];
        result_clear(r);
        stderr_excerpt(&errb, ex, sizeof ex);
        if (*exit_code < 0)
          result_set(r, "astools/tool-crashed",
                     "tool killed by signal %d%s%s", -*exit_code,
                     ex[0] ? ": " : "", ex);
        else
          result_set(r, "astools/tool-crashed",
                     "tool exited with code %d without a response%s%s",
                     *exit_code, ex[0] ? ": " : "", ex);
        verdict = astools_seterr(c, ASTOOLS_ERR_TOOL, "%s",
                                 r->error_message ? r->error_message
                                                  : "tool crashed");
      } else {
        result_clear(r);
        result_set(r, "astools/protocol", "malformed tool response: %s",
                   perr ? perr : "unparsable output");
        verdict = astools_seterr(c, ASTOOLS_ERR_PROTOCOL,
                                 "malformed tool response: %s",
                                 perr ? perr : "unparsable output");
      }
      free(rid);
      free(perr);
      break;
    }
  }

  if (stderr_cap && errb.len > 0)
    *stderr_cap = astools_buf_detach(&errb);
  astools_buf_free(&errb);
  astools_buf_free(&outb);
  return verdict;
}

/* ---- persistent-process / library table --------------------- */

#define PP_KIND_PROC 0
#define PP_KIND_LIB 1

/* One member of a keyed persistent pool (guarded by c->pp_mu). The busy
 * count pins it against the idle reaper; io_mu preserves ordered exchanges
 * on its stream. A pool grows to the manifest's parallel limit. */
struct astools_pproc {
  int kind;  /* PP_KIND_* */
  char *key; /* proc: "id@version"; lib: package dir */
  int busy;
  os_mutex io_mu;
  struct astools_pproc *next;
  /* PP_KIND_PROC */
  os_proc proc;
  bool alive, hello_done;
  int64_t last_used_mono;
  int64_t backoff_ms, next_restart_mono;
  int64_t idle_timeout_ms;
  astools_buf pending; /* unconsumed stdout bytes between reads */
  /* PP_KIND_LIB */
  os_dylib lib;
  const astools_tool_vtable *vt;
  bool lib_loaded, lib_init_ok, lib_failed;
};

static void pp_node_free(astools_pproc *p) {
  if (!p) return;
  os_mutex_destroy(&p->io_mu);
  astools_buf_free(&p->pending);
  free(p->key);
  free(p);
}

/* Find the least-busy member of a keyed pool, or grow that pool up to the
 * manifest's parallel limit. Returned nodes stay pinned until pp_release;
 * the reaper skips busy nodes. Each stream remains strictly ordered. */
static astools_pproc *pp_acquire(astools_ctx *c, int kind, const char *key,
                                 int max_instances) {
  astools_pproc *p, *best = NULL;
  int count = 0;
  if (max_instances < 1) max_instances = 1;
  os_mutex_lock(&c->pp_mu);
  for (p = c->pprocs; p; p = p->next) {
    if (p->kind != kind || strcmp(p->key, key) != 0) continue;
    count++;
    if (!best || p->busy < best->busy) best = p;
  }
  /* Each process still speaks the simple ordered line protocol.  A pool
   * supplies the manifest's advertised parallelism without multiplexing
   * responses on a single stream or serializing every call on one io_mu. */
  p = best;
  if (!p || (kind == PP_KIND_PROC && p->busy > 0 && count < max_instances)) {
    p = calloc(1, sizeof *p);
    if (p) {
      p->kind = kind;
      p->key = astools_strdup(key);
      if (!p->key) {
        free(p);
        p = NULL;
      } else {
        os_mutex_init(&p->io_mu);
        astools_buf_init(&p->pending);
        p->proc.fd_in = p->proc.fd_out = p->proc.fd_err = -1;
        p->next = c->pprocs;
        c->pprocs = p;
      }
    }
  }
  if (p) p->busy++;
  os_mutex_unlock(&c->pp_mu);
  return p;
}

static void pp_release(astools_ctx *c, astools_pproc *p) {
  os_mutex_lock(&c->pp_mu);
  if (p->busy > 0) p->busy--;
  p->last_used_mono = astools_mono(c);
  os_mutex_unlock(&c->pp_mu);
}

/* Stop the child (graceful: SIGTERM, 2s, SIGKILL; else SIGKILL) and mark
 * the node dead. io_mu (or exclusivity at shutdown) must be held. */
static void pp_stop(astools_ctx *c, astools_pproc *p, bool graceful) {
  int ec = 0;
  (void)c;
  if (graceful) {
    os_proc_terminate(&p->proc);
    if (os_proc_wait(&p->proc, 2000, &ec) != ASTOOLS_OK) {
      os_proc_kill(&p->proc);
      (void)os_proc_wait(&p->proc, -1, &ec);
    }
  } else {
    os_proc_kill(&p->proc);
    (void)os_proc_wait(&p->proc, -1, &ec);
  }
  os_proc_free(&p->proc);
  memset(&p->proc, 0, sizeof p->proc);
  p->proc.fd_in = p->proc.fd_out = p->proc.fd_err = -1;
  p->alive = false;
  p->hello_done = false;
  astools_buf_free(&p->pending);
  astools_buf_init(&p->pending);
}

/* Crash/kill backoff: 1s doubling to 30s. */
static void pp_backoff(astools_ctx *c, astools_pproc *p) {
  p->backoff_ms = p->backoff_ms > 0 ? p->backoff_ms * 2 : 1000;
  if (p->backoff_ms > 30000) p->backoff_ms = 30000;
  p->next_restart_mono = astools_mono(c) + p->backoff_ms;
}

/* Detach the first '\n'-terminated line from b (newline consumed, not
 * included). NULL when no complete line is buffered. */
static char *pp_take_line(astools_buf *b, size_t *out_len) {
  char *nl, *line;
  size_t n;
  if (!b->data || b->len == 0) return NULL;
  nl = memchr(b->data, '\n', b->len);
  if (!nl) return NULL;
  n = (size_t)(nl - b->data);
  line = astools_strndup(b->data, n);
  if (!line) return NULL;
  memmove(b->data, nl + 1, b->len - n - 1);
  b->len -= n + 1;
  b->data[b->len] = '\0';
  *out_len = n;
  return line;
}

enum { PP_WHY_NONE = 0, PP_WHY_EOF, PP_WHY_OVERFLOW };

/* Read one complete line from the child's stdout (compact responses are
 * single lines). ASTOOLS_ERR_TOOL distinguishes crash vs flood via
 * *why. Stderr is drained and dropped to keep the child from blocking. */
static astools_err pp_read_line(astools_ctx *c, astools_pproc *p,
                                int64_t deadline_mono,
                                astools_task *cancel_task, char **out_line,
                                size_t *out_len, int *why) {
  int64_t cap = c->cfg.max_output_bytes > 0 ? c->cfg.max_output_bytes + 4096
                                            : (int64_t)1048576;
  *why = PP_WHY_NONE;
  *out_line = NULL;
  *out_len = 0;
  for (;;) {
    unsigned ready = 0;
    int64_t now, slice;
    char *line = pp_take_line(&p->pending, out_len);
    if (line) {
      *out_line = line;
      return ASTOOLS_OK;
    }
    if (task_cancelled(cancel_task)) return ASTOOLS_ERR_CANCELLED;
    now = astools_mono(c);
    if (now >= deadline_mono) return ASTOOLS_ERR_TIMEOUT;
    slice = deadline_mono - now;
    if (slice > 100) slice = 100;
    if (os_proc_poll(&p->proc, 0, slice, &ready) != ASTOOLS_OK) {
      *why = PP_WHY_EOF;
      return ASTOOLS_ERR_TOOL;
    }
    if (ready & OS_READY_ERR) {
      char junk[1024];
      size_t got = 0;
      (void)os_proc_read(&p->proc, OS_PIPE_ERR, junk, sizeof junk, &got);
    }
    if (ready & OS_READY_OUT) {
      char tmp[8192];
      size_t got = 0;
      astools_err re =
          os_proc_read(&p->proc, OS_PIPE_OUT, tmp, sizeof tmp, &got);
      if (re == ASTOOLS_OK && got == 0) {
        *why = PP_WHY_EOF;
        return ASTOOLS_ERR_TOOL;
      }
      if (re == ASTOOLS_OK) {
        if (astools_buf_append(&p->pending, tmp, got) != ASTOOLS_OK)
          return ASTOOLS_ERR_NOMEM;
        if ((int64_t)p->pending.len > cap) {
          *why = PP_WHY_OVERFLOW;
          return ASTOOLS_ERR_TOOL;
        }
      } else if (re != ASTOOLS_ERR_BUSY) {
        *why = PP_WHY_EOF;
        return ASTOOLS_ERR_TOOL;
      }
    }
    if (p->proc.fd_out < 0) {
      *why = PP_WHY_EOF;
      return ASTOOLS_ERR_TOOL;
    }
  }
}

/* Write text (+ terminating newline when missing) to the child's stdin. */
static astools_err pp_write_all(astools_ctx *c, astools_pproc *p,
                                const char *text, int64_t deadline_mono,
                                astools_task *cancel_task, int *why) {
  size_t len = strlen(text), off = 0;
  int nl_done = (len > 0 && text[len - 1] == '\n');
  int64_t cap = c->cfg.max_output_bytes > 0 ? c->cfg.max_output_bytes + 4096
                                            : (int64_t)1048576;
  *why = PP_WHY_NONE;
  while (off < len || !nl_done) {
    unsigned ready = 0;
    int64_t now, slice;
    if (task_cancelled(cancel_task)) return ASTOOLS_ERR_CANCELLED;
    now = astools_mono(c);
    if (now >= deadline_mono) return ASTOOLS_ERR_TIMEOUT;
    slice = deadline_mono - now;
    if (slice > 100) slice = 100;
    if (os_proc_poll(&p->proc, 1, slice, &ready) != ASTOOLS_OK) {
      *why = PP_WHY_EOF;
      return ASTOOLS_ERR_TOOL;
    }
    if (ready & OS_READY_ERR) {
      char junk[1024];
      size_t got = 0;
      (void)os_proc_read(&p->proc, OS_PIPE_ERR, junk, sizeof junk, &got);
    }
    if (ready & OS_READY_OUT) {
      /* early output (stale lines): buffer it for the read loop */
      char tmp[4096];
      size_t got = 0;
      astools_err re =
          os_proc_read(&p->proc, OS_PIPE_OUT, tmp, sizeof tmp, &got);
      if (re == ASTOOLS_OK && got == 0) {
        *why = PP_WHY_EOF;
        return ASTOOLS_ERR_TOOL;
      }
      if (re == ASTOOLS_OK && got > 0) {
        if (astools_buf_append(&p->pending, tmp, got) != ASTOOLS_OK)
          return ASTOOLS_ERR_NOMEM;
        if ((int64_t)p->pending.len > cap) {
          *why = PP_WHY_OVERFLOW;
          return ASTOOLS_ERR_TOOL;
        }
      }
    }
    if (ready & OS_READY_IN) {
      size_t wrote = 0;
      astools_err we;
      if (off < len)
        we = os_proc_write_stdin(&p->proc, text + off, len - off, &wrote);
      else
        we = os_proc_write_stdin(&p->proc, "\n", 1, &wrote);
      if (we == ASTOOLS_OK) {
        if (off < len)
          off += wrote;
        else if (wrote > 0)
          nl_done = 1;
      } else if (we != ASTOOLS_ERR_BUSY) {
        *why = PP_WHY_EOF;
        return ASTOOLS_ERR_TOOL; /* broken pipe: child is gone */
      }
    }
  }
  return ASTOOLS_OK;
}

/* Spawn + #tool_hello handshake, honoring the crash backoff window. */
static astools_err pp_ensure_alive(astools_ctx *c, const astools_tool *t,
                                   const astools_sandbox_setup *setup,
                                   astools_pproc *p, int64_t deadline_mono,
                                   astools_task *cancel_task,
                                   astools_result *r) {
  os_spawn_opts o;
  astools_err e;
  int64_t hello_deadline;

  if (p->alive) return ASTOOLS_OK;

  for (;;) {
    int64_t now = astools_mono(c), wait;
    if (p->next_restart_mono <= now) break;
    if (task_cancelled(cancel_task)) {
      result_set(r, "astools/cancelled", "invocation cancelled");
      return astools_seterr(c, ASTOOLS_ERR_CANCELLED,
                            "invocation cancelled");
    }
    if (now >= deadline_mono) {
      result_set(r, "astools/timeout",
                 "deadline exceeded while tool was in restart backoff");
      return astools_seterr(c, ASTOOLS_ERR_TIMEOUT,
                            "tool '%s' is in restart backoff", t->m->id);
    }
    wait = p->next_restart_mono - now;
    if (wait > deadline_mono - now) wait = deadline_mono - now;
    if (wait > 50) wait = 50;
    worker_sleep_ms(wait);
  }

  astools_buf_free(&p->pending); /* stale bytes of a previous process */
  astools_buf_init(&p->pending);

  memset(&o, 0, sizeof o);
  o.argv = (const char *const *)setup->argv;
  o.envp = (const char *const *)setup->envp;
  o.cwd = setup->scratch_dir;
  o.limit_cpu_seconds = setup->limit_cpu_seconds;
  o.limit_mem_bytes = setup->limit_mem_bytes;
  o.limit_nproc = setup->limit_nproc;
  e = os_proc_spawn(&o, &p->proc);
  if (e != ASTOOLS_OK) {
    pp_backoff(c, p);
    result_set(r, "astools/tool-crashed",
               "failed to spawn persistent tool process");
    return astools_seterr(c, ASTOOLS_ERR_TOOL,
                          "cannot spawn persistent tool '%s' (%s)", t->m->id,
                          astools_err_name(e));
  }

  hello_deadline =
      astools_mono(c) +
      (t->m->startup_timeout_ms > 0 ? t->m->startup_timeout_ms : 10000);
  {
    char *line = NULL;
    size_t llen = 0;
    int why = 0;
    e = pp_read_line(c, p, hello_deadline, cancel_task, &line, &llen, &why);
    if (e == ASTOOLS_OK) {
      char *emsg = NULL;
      if (astools_proto_parse_hello(line, llen, t, &emsg) != ASTOOLS_OK) {
        free(line);
        pp_stop(c, p, false);
        pp_backoff(c, p);
        result_set(r, "astools/protocol", "invalid #tool_hello: %s",
                   emsg ? emsg : "malformed");
        e = astools_seterr(c, ASTOOLS_ERR_PROTOCOL,
                           "tool '%s': invalid #tool_hello: %s", t->m->id,
                           emsg ? emsg : "malformed");
        free(emsg);
        return e;
      }
      free(emsg);
      free(line);
      p->alive = true;
      p->hello_done = true;
      p->backoff_ms = 0;
      p->next_restart_mono = 0;
      p->last_used_mono = astools_mono(c);
      astools_log(c, ASTOOLS_LOG_DEBUG, "proc",
                  "persistent tool %s started", p->key);
      return ASTOOLS_OK;
    }
    pp_stop(c, p, false);
    pp_backoff(c, p);
    if (e == ASTOOLS_ERR_CANCELLED) {
      result_set(r, "astools/cancelled", "invocation cancelled");
      return astools_seterr(c, ASTOOLS_ERR_CANCELLED,
                            "invocation cancelled");
    }
    result_set(r, "astools/protocol",
               "no #tool_hello within startup_timeout");
    return astools_seterr(c, ASTOOLS_ERR_PROTOCOL,
                          "tool '%s': no #tool_hello within startup_timeout",
                          t->m->id);
  }
}

/* Cancel protocol: #tool_cancel, 2s grace for an acknowledging
 * response, then kill + backoff. Keeps the process when it acknowledged. */
static void pp_cancel_and_settle(astools_ctx *c, astools_pproc *p,
                                 const char *invocation_id) {
  char *cl = astools_proto_cancel(invocation_id);
  int64_t grace_end = astools_mono(c) + 2000;
  bool acked = false;
  if (cl) {
    int why = 0;
    (void)pp_write_all(c, p, cl, astools_mono(c) + 500, NULL, &why);
    free(cl);
  }
  for (;;) {
    char *line = NULL;
    size_t llen = 0;
    int why = 0;
    if (pp_read_line(c, p, grace_end, NULL, &line, &llen, &why) !=
        ASTOOLS_OK)
      break;
    {
      astools_result tmp;
      char *rid = NULL, *perr = NULL;
      memset(&tmp, 0, sizeof tmp);
      if (astools_proto_parse_response(line, llen, NULL, &rid, &tmp,
                                       &perr) == ASTOOLS_OK &&
          rid && strcmp(rid, invocation_id) == 0)
        acked = true;
      result_clear(&tmp);
      free(rid);
      free(perr);
    }
    free(line);
    if (acked) break;
  }
  if (!acked) {
    pp_stop(c, p, false);
    pp_backoff(c, p);
  }
}

astools_err astools_exec_persistent(astools_ctx *c, astools_tool *t,
                                    const astools_sandbox_setup *setup,
                                    const char *request_text,
                                    const char *invocation_id,
                                    int64_t deadline_mono,
                                    astools_task *cancel_task,
                                    astools_result *r) {
  char key[160];
  astools_pproc *p;
  astools_err e;

  if (!c || !t || !setup || !request_text || !invocation_id || !r)
    return ASTOOLS_ERR_INVALID;
  memset(r, 0, sizeof *r);
  snprintf(key, sizeof key, "%s@%s", t->m->id, t->m->version);
  p = pp_acquire(c, PP_KIND_PROC, key,
                 t->m->parallel > 0 ? t->m->parallel : 1);
  if (!p)
    return astools_seterr(c, ASTOOLS_ERR_NOMEM,
                          "cannot allocate persistent-process entry");
  os_mutex_lock(&p->io_mu);
  p->idle_timeout_ms = t->m->idle_timeout_ms;

  e = pp_ensure_alive(c, t, setup, p, deadline_mono, cancel_task, r);
  if (e != ASTOOLS_OK) goto out;

  {
    int why = 0;
    e = pp_write_all(c, p, request_text, deadline_mono, cancel_task, &why);
    if (e == ASTOOLS_ERR_TIMEOUT || e == ASTOOLS_ERR_CANCELLED) {
      pp_cancel_and_settle(c, p, invocation_id);
      if (e == ASTOOLS_ERR_TIMEOUT) {
        result_set(r, "astools/timeout", "deadline exceeded");
        e = astools_seterr(c, ASTOOLS_ERR_TIMEOUT,
                           "tool '%s' exceeded its deadline", t->m->id);
      } else {
        result_set(r, "astools/cancelled", "invocation cancelled");
        e = astools_seterr(c, ASTOOLS_ERR_CANCELLED,
                           "invocation cancelled");
      }
      goto out;
    }
    if (e != ASTOOLS_OK) {
      pp_stop(c, p, false);
      pp_backoff(c, p);
      result_set(r, "astools/tool-crashed",
                 "persistent tool did not accept the request");
      e = astools_seterr(c, ASTOOLS_ERR_TOOL,
                         "persistent tool '%s' did not accept the request",
                         t->m->id);
      goto out;
    }
  }

  for (;;) {
    char *line = NULL;
    size_t llen = 0;
    int why = 0;
    e = pp_read_line(c, p, deadline_mono, cancel_task, &line, &llen, &why);
    if (e == ASTOOLS_OK) {
      char *rid = NULL, *perr = NULL;
      astools_err pe =
          astools_proto_parse_response(line, llen, NULL, &rid, r, &perr);
      if (pe == ASTOOLS_OK && rid && strcmp(rid, invocation_id) == 0) {
        free(rid);
        free(perr);
        free(line);
        break; /* r is authoritative */
      }
      /* defensively skip other invocation ids and unparsable lines */
      if (pe == ASTOOLS_OK)
        result_clear(r);
      else
        astools_log(c, ASTOOLS_LOG_DEBUG, "proc",
                    "%s: skipping unparsable output line", key);
      free(rid);
      free(perr);
      free(line);
      continue;
    }
    if (e == ASTOOLS_ERR_TIMEOUT || e == ASTOOLS_ERR_CANCELLED) {
      pp_cancel_and_settle(c, p, invocation_id);
      if (e == ASTOOLS_ERR_TIMEOUT) {
        result_set(r, "astools/timeout", "deadline exceeded");
        e = astools_seterr(c, ASTOOLS_ERR_TIMEOUT,
                           "tool '%s' exceeded its deadline", t->m->id);
      } else {
        result_set(r, "astools/cancelled", "invocation cancelled");
        e = astools_seterr(c, ASTOOLS_ERR_CANCELLED,
                           "invocation cancelled");
      }
      break;
    }
    if (e == ASTOOLS_ERR_NOMEM) break;
    if (why == PP_WHY_OVERFLOW) {
      pp_stop(c, p, false);
      pp_backoff(c, p);
      result_set(r, "astools/overflow",
                 "response exceeded max_output_bytes");
      e = astools_seterr(c, ASTOOLS_ERR_TOOL,
                         "persistent tool '%s' exceeded max_output_bytes",
                         t->m->id);
      break;
    }
    /* crash while waiting: the child is gone, reap gracefully */
    pp_stop(c, p, true);
    pp_backoff(c, p);
    result_set(r, "astools/tool-crashed",
               "persistent tool exited while processing the request");
    e = astools_seterr(c, ASTOOLS_ERR_TOOL, "persistent tool '%s' crashed",
                       t->m->id);
    break;
  }

out:
  p->last_used_mono = astools_mono(c);
  os_mutex_unlock(&p->io_mu);
  pp_release(c, p);
  return e;
}

/* ---- library dispatch --------------------------------------------- */

astools_err astools_exec_library(astools_ctx *c, astools_tool *t,
                                 const char *request_text,
                                 astools_result *r) {
  astools_pproc *p;
  const astools_tool_vtable *vt = NULL;
  astools_err e = ASTOOLS_OK;
  char *resp;

  if (!c || !t || !request_text || !r) return ASTOOLS_ERR_INVALID;
  memset(r, 0, sizeof *r);
  p = pp_acquire(c, PP_KIND_LIB, t->pkg_dir, 1);
  if (!p)
    return astools_seterr(c, ASTOOLS_ERR_NOMEM,
                          "cannot allocate library entry");

  os_mutex_lock(&p->io_mu);
  if (!p->lib_loaded && !p->lib_failed) {
    const char *a0 =
        (t->entry && t->entry->argv_len > 0) ? t->entry->argv[0] : NULL;
    char *path = NULL;
    if (!a0) {
      e = astools_seterr(c, ASTOOLS_ERR_CONFIG,
                         "library tool '%s' has no entry argv", t->m->id);
    } else if (os_path_is_abs(a0)) {
      path = astools_strdup(a0);
      if (!path) e = ASTOOLS_ERR_NOMEM;
    } else {
      path = os_path_join(t->pkg_dir, a0);
      if (!path) e = ASTOOLS_ERR_NOMEM;
    }
    if (e == ASTOOLS_OK) {
      e = os_dylib_open(path, &p->lib);
      if (e != ASTOOLS_OK) {
        e = astools_seterr(c, ASTOOLS_ERR_IO, "cannot load library '%s'",
                           path);
      } else {
        void *sym = os_dylib_sym(&p->lib, "astools_tool_entry");
        astools_tool_entry_fn fn = NULL;
        if (sym) memcpy(&fn, &sym, sizeof fn);
        vt = fn ? fn() : NULL;
        if (!vt || vt->abi_version != ASTOOLS_TOOL_ABI || !vt->init ||
            !vt->invoke || !vt->free_result) {
          os_dylib_close(&p->lib);
          vt = NULL;
          e = astools_seterr(c, ASTOOLS_ERR_PROTOCOL,
                             "library '%s': missing or incompatible "
                             "astools_tool_entry (need ABI %d)",
                             path, ASTOOLS_TOOL_ABI);
        } else {
          char *mtext = NULL;
          e = astools_manifest_render(t->m, &mtext);
          if (e == ASTOOLS_OK) {
            int rc = vt->init(mtext);
            free(mtext);
            p->lib_loaded = true;
            p->vt = vt;
            if (rc != 0) {
              /* ABI: nonzero init fails every invocation — sticky */
              p->lib_failed = true;
              e = astools_seterr(c, ASTOOLS_ERR_TOOL,
                                 "library '%s' init failed (%d)", t->m->id,
                                 rc);
            } else {
              p->lib_init_ok = true;
            }
          } else {
            os_dylib_close(&p->lib);
            vt = NULL;
          }
        }
      }
    }
    free(path);
  }
  if (e == ASTOOLS_OK && p->lib_failed) {
    e = astools_seterr(c, ASTOOLS_ERR_TOOL,
                       "library '%s' failed to initialize", t->m->id);
  }
  vt = (e == ASTOOLS_OK) ? p->vt : NULL;
  os_mutex_unlock(&p->io_mu);
  if (!vt) {
    result_set(r, "astools/tool-crashed", "library unavailable: %s",
               astools_last_error(c) ? astools_last_error(c) : "load failed");
    pp_release(c, p);
    return e != ASTOOLS_OK ? e : ASTOOLS_ERR_TOOL;
  }

  /* vtable->invoke is thread-safe by ABI contract: no lock held here.
   * The node stays pinned by the busy count until pp_release. */
  resp = vt->invoke(request_text);
  if (!resp) {
    result_set(r, "astools/tool-crashed", "library returned no response");
    e = astools_seterr(c, ASTOOLS_ERR_TOOL,
                       "library '%s' returned no response", t->m->id);
  } else {
    char *rid = NULL, *perr = NULL;
    astools_err pe =
        astools_proto_parse_response(resp, strlen(resp), NULL, &rid, r,
                                     &perr);
    if (pe != ASTOOLS_OK) {
      result_clear(r);
      result_set(r, "astools/protocol", "malformed library response: %s",
                 perr ? perr : "unparsable");
      e = astools_seterr(c, ASTOOLS_ERR_PROTOCOL,
                         "library '%s': malformed response: %s", t->m->id,
                         perr ? perr : "unparsable");
    }
    free(rid);
    free(perr);
    vt->free_result(resp);
  }
  pp_release(c, p);
  return e;
}

/* ---- supervisor ------------------------------------------------------ */

/* One pass of background work: registry poll + idle persistent reaping. */
static void worker_pass(astools_ctx *c) {
  astools_pproc *dead = NULL, *p;
  astools_pproc **pp;
  int64_t now;

  if (astools_registry_poll_due(c)) {
    int changed = 0;
    (void)astools_registry_scan(c, &changed);
  }

  now = astools_mono(c);
  os_mutex_lock(&c->pp_mu);
  pp = &c->pprocs;
  while (*pp) {
    p = *pp;
    if (p->kind == PP_KIND_PROC && p->alive && p->busy == 0 &&
        p->idle_timeout_ms > 0 &&
        now - p->last_used_mono >= p->idle_timeout_ms) {
      *pp = p->next;
      p->next = dead;
      dead = p;
    } else {
      pp = &p->next;
    }
  }
  os_mutex_unlock(&c->pp_mu);

  while (dead) {
    p = dead;
    dead = p->next;
    astools_log(c, ASTOOLS_LOG_DEBUG, "worker",
                "reaping idle persistent tool %s", p->key);
    pp_stop(c, p, true);
    pp_node_free(p);
  }
}

#if !defined(ASTOOLS_NO_THREADS)
static void *worker_main(void *arg) {
  astools_ctx *c = arg;
  os_mutex_lock(&c->ev_mu);
  while (!c->stop_worker) {
    (void)os_cond_timedwait(&c->ev_cv, &c->ev_mu, 1000);
    if (c->stop_worker) break;
    os_mutex_unlock(&c->ev_mu);
    worker_pass(c);
    os_mutex_lock(&c->ev_mu);
  }
  os_mutex_unlock(&c->ev_mu);
  return NULL;
}
#endif

astools_err astools_worker_start(astools_ctx *c) {
  if (!c) return ASTOOLS_ERR_INVALID;
#if defined(ASTOOLS_NO_THREADS)
  c->worker_running = false;
  return ASTOOLS_OK; /* tick-driven (astools_tick) */
#else
  if (c->no_threads || c->worker_running) return ASTOOLS_OK;
  c->stop_worker = false;
  {
    astools_err e = os_thread_start(&c->worker, worker_main, c);
    if (e != ASTOOLS_OK)
      return astools_seterr(c, e, "cannot start supervisor thread");
  }
  c->worker_running = true;
  return ASTOOLS_OK;
#endif
}

void astools_worker_stop(astools_ctx *c) {
  astools_pproc *p, *next;
  if (!c) return;
  if (c->worker_running) {
    os_mutex_lock(&c->ev_mu);
    c->stop_worker = true;
    os_cond_broadcast(&c->ev_cv);
    os_mutex_unlock(&c->ev_mu);
    os_thread_join(&c->worker);
    c->worker_running = false;
  }
  /* No invocations are in flight at close; drain the table. */
  os_mutex_lock(&c->pp_mu);
  p = c->pprocs;
  c->pprocs = NULL;
  os_mutex_unlock(&c->pp_mu);
  for (; p; p = next) {
    next = p->next;
    if (p->kind == PP_KIND_PROC) {
      if (p->alive) pp_stop(c, p, true);
    } else if (p->lib_loaded) {
      if (p->lib_init_ok && p->vt && p->vt->shutdown) p->vt->shutdown();
      os_dylib_close(&p->lib);
    }
    pp_node_free(p);
  }
}

astools_err astools_worker_tick(astools_ctx *c) {
  if (!c) return ASTOOLS_ERR_INVALID;
  worker_pass(c);
  return ASTOOLS_OK;
}

/* ---- async tasks (astools_invoke_async) ----------------------------------- */

#if !defined(ASTOOLS_NO_THREADS)
static void *task_main(void *arg) {
  astools_task *t = arg;
  astools_result res;
  astools_err v =
      astools_invoke_impl(t->c, t->ref, t->command, t->args,
                          t->deadline_ms, t, &res);
  os_mutex_lock(&t->mu);
  t->result = res;
  t->verdict = v;
  t->done = true;
  os_cond_broadcast(&t->cv);
  os_mutex_unlock(&t->mu);
  return NULL;
}
#endif

astools_err astools_invoke_async(astools_ctx *c, const char *ref,
                                 const char *command, const char *args_xcdn,
                                 uint32_t deadline_ms, astools_task **out) {
  if (out) *out = NULL;
  if (!c || !ref || !command || !out) return ASTOOLS_ERR_INVALID;
#if defined(ASTOOLS_NO_THREADS)
  (void)args_xcdn;
  (void)deadline_ms;
  return astools_seterr(c, ASTOOLS_ERR_UNSUPPORTED,
                        "async invocation needs threads "
                        "(ASTOOLS_NO_THREADS build)");
#else
  if (c->no_threads)
    return astools_seterr(c, ASTOOLS_ERR_UNSUPPORTED,
                          "async invocation is unavailable in "
                          "no-thread mode");
  {
    astools_task *t = calloc(1, sizeof *t);
    astools_err e;
    if (!t) return ASTOOLS_ERR_NOMEM;
    t->c = c;
    t->ref = astools_strdup(ref);
    t->command = astools_strdup(command);
    t->args = astools_strdup(args_xcdn); /* NULL-safe */
    t->deadline_ms = deadline_ms;
    os_mutex_init(&t->mu);
    os_cond_init(&t->cv);
    if (!t->ref || !t->command || (args_xcdn && !t->args)) {
      e = ASTOOLS_ERR_NOMEM;
      goto fail;
    }
    e = os_thread_start(&t->thread, task_main, t);
    if (e != ASTOOLS_OK) {
      (void)astools_seterr(c, e, "cannot start invocation thread");
      goto fail;
    }
    t->thread_valid = true;
    *out = t;
    return ASTOOLS_OK;
  fail:
    os_mutex_destroy(&t->mu);
    os_cond_destroy(&t->cv);
    free(t->ref);
    free(t->command);
    free(t->args);
    free(t);
    return e;
  }
#endif
}

astools_err astools_task_wait(astools_task *t, uint32_t timeout_ms,
                              astools_result *out) {
  int64_t end;
  astools_err v;
  if (!t) return ASTOOLS_ERR_INVALID;
  end = os_monotonic_ms() + (int64_t)timeout_ms;
  os_mutex_lock(&t->mu);
  while (!t->done) {
    int64_t rem = end - os_monotonic_ms();
    if (rem <= 0) {
      os_mutex_unlock(&t->mu);
      return ASTOOLS_ERR_BUSY;
    }
    (void)os_cond_timedwait(&t->cv, &t->mu, rem);
  }
  if (out) {
    /* Ownership of the result strings transfers on the first delivery;
     * later waits observe the same verdict/scalars with NULL strings. */
    *out = t->result;
    t->result.result_xcdn = NULL;
    t->result.error_code = NULL;
    t->result.error_message = NULL;
  }
  v = t->verdict;
  os_mutex_unlock(&t->mu);
  return v;
}

astools_err astools_task_cancel(astools_task *t) {
  if (!t) return ASTOOLS_ERR_INVALID;
  os_mutex_lock(&t->mu);
  t->cancelled = true;
  os_mutex_unlock(&t->mu);
  return ASTOOLS_OK;
}

void astools_task_free(astools_task *t) {
  if (!t) return;
  os_mutex_lock(&t->mu);
  t->cancelled = true; /* unblock a still-running invocation */
  os_mutex_unlock(&t->mu);
  if (t->thread_valid) {
    os_thread_join(&t->thread);
    t->thread_valid = false;
  }
  free(t->ref);
  free(t->command);
  free(t->args);
  free(t->result.result_xcdn);
  free(t->result.error_code);
  free(t->result.error_message);
  os_mutex_destroy(&t->mu);
  os_cond_destroy(&t->cv);
  free(t);
}
