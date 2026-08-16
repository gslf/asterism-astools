/*
 * log.c — leveled rotating file log + host callback fan-out + audit trail
 *.
 *
 * Sink independence: the file sink is gated by cfg.log_level; the host
 * callback receives EVERY record at any level and filters on its own. The
 * callback `msg` is the complete formatted single-line record
 * ("<RFC3339> LEVEL SUBSYS  message", no trailing newline).
 * astools_set_logger(NULL) disables the callback sink entirely, per
 * astools.h.
 *
 * Every failure in this module is silent: logging and auditing never fail
 * the caller.
 */

#include "astools_internal.h"

#include <stdlib.h>
#include <string.h>

static const char *level_name(int level) {
  switch (level) {
    case ASTOOLS_LOG_ERROR: return "ERROR";
    case ASTOOLS_LOG_WARN:  return "WARN";
    case ASTOOLS_LOG_INFO:  return "INFO";
    default:                return "DEBUG";
  }
}

astools_err astools_log_open(astools_ctx *c) {
  FILE *f;
  uint64_t sz = 0;
  if (!c) return ASTOOLS_ERR_INVALID;
  if (!c->cfg.log_path || c->cfg.log_path[0] == '\0') return ASTOOLS_OK;
  f = os_fopen(c->cfg.log_path, "ab");
  if (!f)
    return astools_seterr(c, ASTOOLS_ERR_IO, "log: cannot open '%s'",
                          c->cfg.log_path);
  if (os_file_size(c->cfg.log_path, &sz) != ASTOOLS_OK) sz = 0;
  os_mutex_lock(&c->log_mu);
  if (c->log_fp) fclose(c->log_fp);
  c->log_fp = f;
  c->log_size = sz;
  os_mutex_unlock(&c->log_mu);
  return ASTOOLS_OK;
}

void astools_log_close(astools_ctx *c) {
  if (!c) return;
  os_mutex_lock(&c->log_mu);
  if (c->log_fp) {
    fflush(c->log_fp);
    fclose(c->log_fp);
    c->log_fp = NULL;
  }
  c->log_size = 0;
  os_mutex_unlock(&c->log_mu);
}

/* Rotate <path> -> <path>.1 -> ... -> <path>.(max_files-1); the overflow
 * file is deleted. Reopens a fresh current file. c->log_mu held. On any
 * failure the sink may end up disabled (log_fp NULL) — silent by design;
 * a later astools_log_open can revive it. */
static void log_rotate_locked(astools_ctx *c) {
  const char *path = c->cfg.log_path;
  int maxf = c->cfg.log_max_files;
  size_t plen = strlen(path);
  char *pa, *pb;
  int i;
  if (c->log_fp) {
    fflush(c->log_fp);
    fclose(c->log_fp);
    c->log_fp = NULL;
  }
  if (maxf < 1) maxf = 1;
  pa = malloc(plen + 32);
  pb = malloc(plen + 32);
  if (pa && pb) {
    if (maxf == 1) {
      (void)os_remove_file(path); /* no rotated copies kept */
    } else {
      snprintf(pa, plen + 32, "%s.%d", path, maxf - 1);
      (void)os_remove_file(pa); /* delete overflow beyond max_files */
      for (i = maxf - 1; i >= 2; i--) {
        snprintf(pa, plen + 32, "%s.%d", path, i - 1);
        snprintf(pb, plen + 32, "%s.%d", path, i);
        (void)os_rename(pa, pb); /* best effort; source may be absent */
      }
      snprintf(pa, plen + 32, "%s.1", path);
      (void)os_rename(path, pa);
    }
  }
  free(pa);
  free(pb);
  c->log_fp = os_fopen(path, "ab");
  c->log_size = 0;
  if (c->log_fp) {
    uint64_t sz = 0;
    /* if the rename failed we appended to the old file: track real size */
    if (os_file_size(path, &sz) == ASTOOLS_OK) c->log_size = sz;
  }
}

void astools_log(astools_ctx *c, int level, const char *subsys,
                 const char *fmt, ...) {
  char stamp[32];
  char msg_stack[512];
  char *msg = msg_stack;
  char *msg_heap = NULL;
  char line_stack[768];
  char *line = line_stack;
  char *line_heap = NULL;
  char head[64];
  char *q;
  int need, headlen;
  size_t msglen, linelen;
  va_list ap;
  astools_log_fn cb;
  void *cb_ud;

  if (!c || !fmt) return;
  if (!subsys) subsys = "-";

  va_start(ap, fmt);
  need = vsnprintf(msg_stack, sizeof msg_stack, fmt, ap);
  va_end(ap);
  if (need < 0) return;
  if ((size_t)need >= sizeof msg_stack) {
    msg_heap = malloc((size_t)need + 1);
    if (msg_heap) {
      va_start(ap, fmt);
      vsnprintf(msg_heap, (size_t)need + 1, fmt, ap);
      va_end(ap);
      msg = msg_heap;
    } /* else: proceed with the truncated stack copy */
  }
  for (q = msg; *q; q++) /* records are single-line by contract */
    if (*q == '\n' || *q == '\r') *q = ' ';
  msglen = strlen(msg);

  astools_time_format_rfc3339(astools_clock_now(&c->clock), stamp);
  headlen = snprintf(head, sizeof head, "%s %-5s %-8s ", stamp,
                     level_name(level), subsys);
  if (headlen < 0) {
    free(msg_heap);
    return;
  }
  if ((size_t)headlen >= sizeof head) headlen = (int)sizeof head - 1;

  linelen = (size_t)headlen + msglen;
  if (linelen + 2 > sizeof line_stack) {
    line_heap = malloc(linelen + 2);
    if (line_heap) {
      line = line_heap;
    } else {
      msglen = sizeof line_stack - 2 - (size_t)headlen;
      linelen = (size_t)headlen + msglen;
    }
  }
  memcpy(line, head, (size_t)headlen);
  memcpy(line + headlen, msg, msglen);
  line[linelen] = '\0';

  os_mutex_lock(&c->log_mu);
  cb = c->log_cb;
  cb_ud = c->log_ud;
  if (c->log_fp && level <= c->cfg.log_level) {
    size_t wire = linelen + 1; /* + '\n' */
    if (c->cfg.log_max_size_kb > 0 &&
        c->log_size + wire > (uint64_t)c->cfg.log_max_size_kb * 1024u)
      log_rotate_locked(c);
    if (c->log_fp) {
      if (fwrite(line, 1, linelen, c->log_fp) == linelen &&
          fputc('\n', c->log_fp) != EOF)
        c->log_size += wire;
      fflush(c->log_fp);
      if (c->cfg.log_sync) (void)os_fsync(c->log_fp);
    }
  }
  os_mutex_unlock(&c->log_mu);

  /* Callback outside log_mu: a callback that re-enters astools_log must not
   * deadlock. NULL means the host disabled the callback sink. */
  if (cb)
    cb(level, line, cb_ud);

  free(msg_heap);
  free(line_heap);
}

/* ---- audit trail --------------------------------------------------- */

/* Append s as the body of an xCDN double-quoted string, escaping quote,
 * backslash and control characters. tool/version/command/error come from
 * untrusted manifests: an unescaped value could forge audit records. */
static astools_err audit_escape(astools_buf *b, const char *s) {
  astools_err e = ASTOOLS_OK;
  const unsigned char *p;
  if (!s) return ASTOOLS_OK;
  for (p = (const unsigned char *)s; *p && e == ASTOOLS_OK; p++) {
    unsigned char ch = *p;
    if (ch == '"') {
      e = astools_buf_appends(b, "\\\"");
    } else if (ch == '\\') {
      e = astools_buf_appends(b, "\\\\");
    } else if (ch == '\n') {
      e = astools_buf_appends(b, "\\n");
    } else if (ch == '\r') {
      e = astools_buf_appends(b, "\\r");
    } else if (ch == '\t') {
      e = astools_buf_appends(b, "\\t");
    } else if (ch < 0x20) {
      e = astools_buf_printf(b, "\\u%04x", (unsigned)ch);
    } else {
      e = astools_buf_appendc(b, (char)ch);
    }
  }
  return e;
}

static astools_err audit_append_str(astools_err e, astools_buf *b,
                                    const char *s) {
  return e != ASTOOLS_OK ? e : astools_buf_appends(b, s);
}

/* One #invocation record appended to <config_dir>/audit.xcdn. Append-only,
 * never rotated; args are hashed, not stored. Never fails the
 * caller. */
void astools_audit_append(astools_ctx *c, const char *tool,
                          const char *version, const char *command, bool ok,
                          const char *error_code, uint64_t duration_ms,
                          const uint8_t args_sha256[32]) {
  astools_buf b;
  char stamp[32];
  char *b64 = NULL;
  char *path = NULL;
  astools_err e = ASTOOLS_OK;

  if (!c || !c->cfg.audit) return;

  astools_time_format_rfc3339(astools_clock_now(&c->clock), stamp);
  astools_buf_init(&b);

  e = audit_append_str(e, &b, "#invocation { at: t\"");
  e = audit_append_str(e, &b, stamp);
  e = audit_append_str(e, &b, "\", tool: \"");
  if (e == ASTOOLS_OK) e = audit_escape(&b, tool ? tool : "");
  e = audit_append_str(e, &b, "\", version: \"");
  if (e == ASTOOLS_OK) e = audit_escape(&b, version ? version : "");
  e = audit_append_str(e, &b, "\", command: \"");
  if (e == ASTOOLS_OK) e = audit_escape(&b, command ? command : "");
  e = audit_append_str(e, &b, ok ? "\", ok: true" : "\", ok: false");
  if (!ok && error_code) {
    e = audit_append_str(e, &b, ", error: \"");
    if (e == ASTOOLS_OK) e = audit_escape(&b, error_code);
    e = audit_append_str(e, &b, "\"");
  }
  if (e == ASTOOLS_OK)
    e = astools_buf_printf(&b, ", duration_ms: %llu",
                           (unsigned long long)duration_ms);
  if (args_sha256) {
    b64 = astools_base64_encode(args_sha256, 32);
    if (b64) {
      e = audit_append_str(e, &b, ", args_sha256: b\"");
      e = audit_append_str(e, &b, b64);
      e = audit_append_str(e, &b, "\"");
    } else {
      e = ASTOOLS_ERR_NOMEM;
    }
  }
  e = audit_append_str(e, &b, " }\n");
  if (e != ASTOOLS_OK) goto cleanup;

  path = os_path_join(c->cfg.config_dir && c->cfg.config_dir[0] != '\0'
                          ? c->cfg.config_dir
                          : ".",
                      "audit.xcdn");
  if (!path) goto cleanup;

  os_mutex_lock(&c->audit_mu);
  {
    FILE *f = os_fopen(path, "ab");
    if (f) {
      size_t wrote = fwrite(b.data, 1, b.len, f);
      if (wrote == b.len && c->cfg.log_sync) (void)os_fsync(f);
      fclose(f);
    }
  }
  os_mutex_unlock(&c->audit_mu);

cleanup:
  free(b64);
  free(path);
  astools_buf_free(&b);
}
