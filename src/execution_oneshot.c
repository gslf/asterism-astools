/* execution oneshot — runtime implementation. */
#include "execution.h"

/* Append to the stderr capture keeping the NEWEST bytes: when the cap is
 * exceeded the oldest half is discarded. Best effort: OOM drops. */
static void stderr_append(astools_buf *b, const char *data, size_t n, int64_t cap_bytes) {
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

/* ---- oneshot execution -------------------------------------------- */

enum { ONE_RUNNING = 0, ONE_EOF, ONE_TIMEOUT, ONE_CANCEL, ONE_OVERFLOW, ONE_NOMEM };

astools_err astools_exec_oneshot(astools_ctx *c, const astools_sandbox_setup *setup,
                                 const char *request_text, const char *invocation_id,
                                 int64_t deadline_mono, int64_t max_output_bytes,
                                 int64_t stderr_max_bytes, astools_task *cancel_task,
                                 astools_result *r, int *exit_code, char **stderr_cap) {
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
    astools_exec_result_set(r, "astools/tool-crashed", "failed to spawn tool process");
    return astools_seterr(c, ASTOOLS_ERR_TOOL, "cannot spawn tool process (%s)",
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
    if (astools_task_cancelled(cancel_task)) {
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
      astools_err we = os_proc_write_stdin(&p, wr + wr_off, wr_len - wr_off, &wrote);
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
      if (re == ASTOOLS_OK && got > 0) stderr_append(&errb, tmp, got, stderr_max_bytes);
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
    while (os_proc_read(&p, OS_PIPE_ERR, tmp, sizeof tmp, &got) == ASTOOLS_OK && got > 0)
      stderr_append(&errb, tmp, got, stderr_max_bytes);
  }
  /* A well-behaved oneshot exits without descendants.  If it deliberately
   * orphaned any, the process group can outlive its reaped leader; never let
   * those processes escape merely because the response was well formed. */
  os_proc_kill(&p);
  os_proc_free(&p);

  switch (status) {
  case ONE_CANCEL:
    astools_exec_result_set(r, "astools/cancelled", "invocation cancelled");
    verdict = astools_seterr(c, ASTOOLS_ERR_CANCELLED, "invocation cancelled");
    break;
  case ONE_TIMEOUT:
    astools_exec_result_set(r, "astools/timeout", "deadline exceeded");
    verdict = astools_seterr(c, ASTOOLS_ERR_TIMEOUT, "tool exceeded its deadline");
    break;
  case ONE_OVERFLOW:
    astools_exec_result_set(r, "astools/overflow", "stdout exceeded max_output_bytes (%lld)",
                            (long long)max_output_bytes);
    verdict = astools_seterr(c, ASTOOLS_ERR_TOOL, "tool stdout exceeded max_output_bytes (%lld)",
                             (long long)max_output_bytes);
    break;
  case ONE_NOMEM:
    verdict = ASTOOLS_ERR_NOMEM;
    break;
  default: { /* ONE_EOF: parse the response */
    char *rid = NULL, *perr = NULL;
    astools_err pe = astools_proto_parse_response(outb.data ? outb.data : "", outb.len,
                                                  invocation_id, &rid, r, &perr);
    if (pe == ASTOOLS_OK) {
      /* well-formed response is authoritative regardless of exit code */
      verdict = ASTOOLS_OK;
    } else if (*exit_code != 0) {
      char ex[520];
      astools_exec_result_clear(r);
      stderr_excerpt(&errb, ex, sizeof ex);
      if (*exit_code < 0)
        astools_exec_result_set(r, "astools/tool-crashed", "tool killed by signal %d%s%s",
                                -*exit_code, ex[0] ? ": " : "", ex);
      else
        astools_exec_result_set(r, "astools/tool-crashed",
                                "tool exited with code %d without a response%s%s", *exit_code,
                                ex[0] ? ": " : "", ex);
      verdict = astools_seterr(c, ASTOOLS_ERR_TOOL, "%s",
                               r->error_message ? r->error_message : "tool crashed");
    } else {
      astools_exec_result_clear(r);
      astools_exec_result_set(r, "astools/protocol", "malformed tool response: %s",
                              perr ? perr : "unparsable output");
      verdict = astools_seterr(c, ASTOOLS_ERR_PROTOCOL, "malformed tool response: %s",
                               perr ? perr : "unparsable output");
    }
    free(rid);
    free(perr);
    break;
  }
  }

  if (stderr_cap && errb.len > 0) *stderr_cap = astools_buf_detach(&errb);
  astools_buf_free(&errb);
  astools_buf_free(&outb);
  return verdict;
}
