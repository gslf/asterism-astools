/* process io — runtime implementation. */
#include "execution.h"

static size_t payload_limit(const astools_ctx *c) {
  uint64_t n = c->cfg.max_output_bytes > 0 ? (uint64_t)c->cfg.max_output_bytes : 1048576u;
  return n > SIZE_MAX - 8192 ? SIZE_MAX - 8192 : (size_t)n;
}

/* Detach the first '\n'-terminated line from b (newline consumed, not
 * included). BUSY means incomplete input; allocation failure stays explicit. */
static astools_err pp_take_line(astools_buf *b, char **out, size_t *out_len) {
  char *nl, *line;
  size_t n;
  if (!b->data || b->len == 0) return ASTOOLS_ERR_BUSY;
  nl = memchr(b->data, '\n', b->len);
  if (!nl) return ASTOOLS_ERR_BUSY;
  n = (size_t)(nl - b->data);
  line = astools_strndup(b->data, n);
  if (!line) return ASTOOLS_ERR_NOMEM;
  memmove(b->data, nl + 1, b->len - n - 1);
  b->len -= n + 1;
  b->data[b->len] = '\0';
  *out_len = n;
  *out = line;
  return ASTOOLS_OK;
}

/* Read one complete line from the child's stdout (compact responses are
 * single lines). ASTOOLS_ERR_TOOL distinguishes crash vs flood via
 * *why. Stderr is drained and dropped to keep the child from blocking. */
static astools_err read_message(astools_ctx *c, astools_pproc *p, int framed, int64_t deadline_mono,
                                 astools_task *cancel_task, char **out_line, size_t *out_len,
                                 int *why) {
  size_t cap = payload_limit(c) + 8192;
  *why = PP_WHY_NONE;
  *out_line = NULL;
  *out_len = 0;
  for (;;) {
    unsigned ready = 0;
    int64_t now, slice;
    if (astools_task_cancelled(cancel_task)) return ASTOOLS_ERR_CANCELLED;
    if (astools_mono(c) >= deadline_mono) return ASTOOLS_ERR_TIMEOUT;
    astools_err buffered = framed ? astools_pp_take_frame(&p->pending, payload_limit(c), out_line, out_len) :
                                    pp_take_line(&p->pending, out_line, out_len);
    if (buffered != ASTOOLS_ERR_BUSY) {
      if (buffered == ASTOOLS_ERR_TOOL) *why = PP_WHY_OVERFLOW;
      return buffered;
    }
    if (astools_task_cancelled(cancel_task)) return ASTOOLS_ERR_CANCELLED;
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
      astools_err re = os_proc_read(&p->proc, OS_PIPE_OUT, tmp, sizeof tmp, &got);
      if (re == ASTOOLS_OK && got == 0) {
        *why = PP_WHY_EOF;
        return ASTOOLS_ERR_TOOL;
      }
      if (re == ASTOOLS_OK) {
        if (astools_buf_append(&p->pending, tmp, got) != ASTOOLS_OK) return ASTOOLS_ERR_NOMEM;
        if (p->pending.len > cap) {
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
static astools_err write_message(astools_ctx *c, astools_pproc *p, const char *text, size_t len, int newline,
                                 int64_t deadline_mono, astools_task *cancel_task, int *why) {
  size_t off = 0;
  int nl_done = !newline || (len > 0 && text[len - 1] == '\n');
  size_t cap = payload_limit(c) + 8192;
  *why = PP_WHY_NONE;
  while (off < len || !nl_done) {
    unsigned ready = 0;
    int64_t now, slice;
    if (astools_task_cancelled(cancel_task)) return ASTOOLS_ERR_CANCELLED;
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
      astools_err re = os_proc_read(&p->proc, OS_PIPE_OUT, tmp, sizeof tmp, &got);
      if (re == ASTOOLS_OK && got == 0) {
        *why = PP_WHY_EOF;
        return ASTOOLS_ERR_TOOL;
      }
      if (re == ASTOOLS_OK && got > 0) {
        if (astools_buf_append(&p->pending, tmp, got) != ASTOOLS_OK) return ASTOOLS_ERR_NOMEM;
        if (p->pending.len > cap) {
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


astools_err astools_pp_read_line(astools_ctx *c, astools_pproc *p, int64_t deadline,
                                 astools_task *cancel, char **out, size_t *len, int *why) {
  return read_message(c, p, 0, deadline, cancel, out, len, why);
}

astools_err astools_pp_read_frame(astools_ctx *c, astools_pproc *p, int64_t deadline,
                                  astools_task *cancel, char **out, size_t *len, int *why) {
  return read_message(c, p, 1, deadline, cancel, out, len, why);
}

astools_err astools_pp_write_all(astools_ctx *c, astools_pproc *p, const char *text,
                                 int64_t deadline, astools_task *cancel, int *why) {
  return write_message(c, p, text, strlen(text), 1, deadline, cancel, why);
}

astools_err astools_pp_write_frame(astools_ctx *c, astools_pproc *p, const char *json,
                                   int64_t deadline, astools_task *cancel, int *why) {
  char header[80];
  size_t len = strlen(json);
  int n = snprintf(header, sizeof header, "Content-Length: %zu\r\n\r\n", len);
  if (n < 0 || (size_t)n >= sizeof header) return ASTOOLS_ERR_TOOL;
  astools_err e = write_message(c, p, header, (size_t)n, 0, deadline, cancel, why);
  return e == ASTOOLS_OK ? write_message(c, p, json, len, 0, deadline, cancel, why) : e;
}
