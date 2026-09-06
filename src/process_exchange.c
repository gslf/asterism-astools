/* process exchange — runtime implementation. */
#include "execution.h"
#include "lsp.h"

static void pp_cancel_and_settle(astools_ctx *c, astools_pproc *p, const char *invocation_id) {
  char *cl = astools_proto_cancel(invocation_id);
  int64_t grace_end = astools_mono(c) + 2000;
  bool acked = false;
  if (cl) {
    int why = 0;
    (void)astools_pp_write_all(c, p, cl, astools_mono(c) + 500, NULL, &why);
    free(cl);
  }
  for (;;) {
    char *line = NULL;
    size_t llen = 0;
    int why = 0;
    if (astools_pp_read_line(c, p, grace_end, NULL, &line, &llen, &why) != ASTOOLS_OK) break;
    {
      astools_result tmp;
      char *rid = NULL, *perr = NULL;
      memset(&tmp, 0, sizeof tmp);
      if (astools_proto_parse_response(line, llen, NULL, &rid, &tmp, &perr) == ASTOOLS_OK && rid &&
          strcmp(rid, invocation_id) == 0)
        acked = true;
      astools_exec_result_clear(&tmp);
      free(rid);
      free(perr);
    }
    free(line);
    if (acked) break;
  }
  if (!acked) {
    astools_pp_stop(c, p, false);
    astools_pp_backoff(c, p);
  }
}

astools_err astools_exec_persistent(astools_ctx *c, astools_tool *t, const astools_cmd *cmd,
                                    const xcdn_node_t *args, const astools_effective *eff,
                                    const char *invocation_id, int64_t deadline_mono,
                                    astools_task *cancel_task, astools_result *r,
                                    int *sandbox_level) {
  astools_pproc *p = NULL;
  char *request_text = NULL;
  astools_err e = astools_pp_acquire(c, PP_KIND_PROC, t, deadline_mono, cancel_task, &p);
  if (e != ASTOOLS_OK) return e;
  /* PROC acquisition is exclusive; no blocking mutex hides cancellation. */
  p->idle_timeout_ms = t->m->idle_timeout_ms;
  e = astools_pp_validate(c, t);
  if (e != ASTOOLS_OK) {
    astools_pp_stop(c, p, false);
    goto out;
  }
  e = astools_pp_ensure_alive(c, t, eff, p, deadline_mono, cancel_task, r);
  if (e != ASTOOLS_OK) goto out;
  *sandbox_level = p->setup.level;
  if (t->m->protocol == ASTOOLS_PROTOCOL_LSP) {
    e = astools_lsp_invoke(c, p, cmd, args, eff, deadline_mono, cancel_task, r);
    if (e != ASTOOLS_OK) {
      /* No pending replies or open documents survive an uncertain exchange. */
      astools_pp_stop(c, p, false);
      const char *detail = e == ASTOOLS_ERR_BUSY ? "Source or referenced content changed; reopen the file before retrying" :
                           e == ASTOOLS_ERR_INVALID ? "Invalid file, SHA-256 or UTF-8 source position" :
                           e == ASTOOLS_ERR_UNSUPPORTED ? "The server or file does not support this semantic operation" :
                           e == ASTOOLS_ERR_TIMEOUT ? "Deadline expired without a current, correlated semantic result" :
                           astools_err_name(e);
      astools_exec_result_set(r, "astools/lsp-query", "%s", p->lsp_error[0] ? p->lsp_error : detail);
    }
    goto out;
  }
  astools_time wall = astools_clock_now(&c->clock) + (deadline_mono - astools_mono(c) + 999) / 1000;
  e = astools_proto_request(t, cmd, args, invocation_id, wall, c->cfg.max_output_bytes, eff,
                            c->workspace, p->setup.scratch_dir, &request_text);
  if (e != ASTOOLS_OK) goto out;

  {
    int why = 0;
    e = astools_pp_write_all(c, p, request_text, deadline_mono, cancel_task, &why);
    if (e == ASTOOLS_ERR_TIMEOUT || e == ASTOOLS_ERR_CANCELLED) {
      pp_cancel_and_settle(c, p, invocation_id);
      if (e == ASTOOLS_ERR_TIMEOUT) {
        astools_exec_result_set(r, "astools/timeout", "deadline exceeded");
        e = astools_seterr(c, ASTOOLS_ERR_TIMEOUT, "tool '%s' exceeded its deadline", t->m->id);
      } else {
        astools_exec_result_set(r, "astools/cancelled", "invocation cancelled");
        e = astools_seterr(c, ASTOOLS_ERR_CANCELLED, "invocation cancelled");
      }
      goto out;
    }
    if (e != ASTOOLS_OK) {
      astools_pp_stop(c, p, false);
      astools_pp_backoff(c, p);
      astools_exec_result_set(r, "astools/tool-crashed",
                              "persistent tool did not accept the request");
      e = astools_seterr(c, ASTOOLS_ERR_TOOL, "persistent tool '%s' did not accept the request",
                         t->m->id);
      goto out;
    }
  }

  for (;;) {
    char *line = NULL;
    size_t llen = 0;
    int why = 0;
    e = astools_pp_read_line(c, p, deadline_mono, cancel_task, &line, &llen, &why);
    if (e == ASTOOLS_OK) {
      char *rid = NULL, *perr = NULL;
      e = astools_proto_parse_response(line, llen, invocation_id, &rid, r, &perr);
      if (e != ASTOOLS_OK) {
        astools_exec_result_set(r, "astools/protocol", "invalid tool response: %s",
                                perr ? perr : "malformed");
        astools_pp_stop(c, p, false);
        astools_pp_backoff(c, p);
      }
      free(rid);
      free(perr);
      free(line);
      break;
    }
    if (e == ASTOOLS_ERR_TIMEOUT || e == ASTOOLS_ERR_CANCELLED) {
      pp_cancel_and_settle(c, p, invocation_id);
      if (e == ASTOOLS_ERR_TIMEOUT) {
        astools_exec_result_set(r, "astools/timeout", "deadline exceeded");
        e = astools_seterr(c, ASTOOLS_ERR_TIMEOUT, "tool '%s' exceeded its deadline", t->m->id);
      } else {
        astools_exec_result_set(r, "astools/cancelled", "invocation cancelled");
        e = astools_seterr(c, ASTOOLS_ERR_CANCELLED, "invocation cancelled");
      }
      break;
    }
    if (e == ASTOOLS_ERR_NOMEM) {
      astools_pp_stop(c, p, false);
      break;
    }
    if (why == PP_WHY_OVERFLOW) {
      astools_pp_stop(c, p, false);
      astools_pp_backoff(c, p);
      astools_exec_result_set(r, "astools/overflow", "response exceeded max_output_bytes");
      e = astools_seterr(c, ASTOOLS_ERR_TOOL, "persistent tool '%s' exceeded max_output_bytes",
                         t->m->id);
      break;
    }
    /* crash while waiting: the child is gone, reap gracefully */
    astools_pp_stop(c, p, true);
    astools_pp_backoff(c, p);
    astools_exec_result_set(r, "astools/tool-crashed",
                            "persistent tool exited while processing the request");
    e = astools_seterr(c, ASTOOLS_ERR_TOOL, "persistent tool '%s' crashed", t->m->id);
    break;
  }

out:
  p->last_used_mono = astools_mono(c);
  free(request_text);
  astools_pp_release(c, p);
  return e;
}
