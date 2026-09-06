/* process start — runtime implementation. */
#include "execution.h"
#include "lsp.h"
#include "mcp_client.h"

/* Spawn + #tool_hello handshake, honoring the crash backoff window. */
astools_err astools_pp_ensure_alive(astools_ctx *c, const astools_tool *t,
                                    const astools_effective *eff, astools_pproc *p,
                                    int64_t deadline_mono, astools_task *cancel_task,
                                    astools_result *r) {
  os_spawn_opts o;
  astools_err e;
  int64_t hello_deadline;

  if (astools_task_cancelled(cancel_task)) return ASTOOLS_ERR_CANCELLED;
  if (astools_mono(c) >= deadline_mono) return ASTOOLS_ERR_TIMEOUT;
  if (p->alive) return ASTOOLS_OK;
#ifdef _WIN32
  if (t->m->protocol == ASTOOLS_PROTOCOL_LSP) return ASTOOLS_ERR_UNSUPPORTED;
#endif

  for (;;) {
    int64_t now = astools_mono(c), wait;
    if (p->next_restart_mono <= now) break;
    if (astools_task_cancelled(cancel_task)) {
      astools_exec_result_set(r, "astools/cancelled", "invocation cancelled");
      return astools_seterr(c, ASTOOLS_ERR_CANCELLED, "invocation cancelled");
    }
    if (now >= deadline_mono) {
      astools_exec_result_set(r, "astools/timeout",
                              "deadline exceeded while tool was in restart backoff");
      return astools_seterr(c, ASTOOLS_ERR_TIMEOUT, "tool '%s' is in restart backoff", t->m->id);
    }
    wait = p->next_restart_mono - now;
    if (wait > deadline_mono - now) wait = deadline_mono - now;
    if (wait > 50) wait = 50;
    astools_exec_sleep(wait);
  }

  char **argv = NULL;
  char process_id[37];
  astools_uuid_v4(process_id);
  e = astools_entry_resolve_argv(t, &argv);
  if (e == ASTOOLS_OK) e = astools_sandbox_prepare(c, t, eff, process_id, argv, &p->setup);
  astools_argv_free(argv);
  if (e != ASTOOLS_OK) return e;
  const astools_sandbox_setup *setup = &p->setup;
  e = astools_pp_validate(c, t);
  if (e == ASTOOLS_OK && astools_task_cancelled(cancel_task)) e = ASTOOLS_ERR_CANCELLED;
  if (e == ASTOOLS_OK && astools_mono(c) >= deadline_mono) e = ASTOOLS_ERR_TIMEOUT;
  if (e != ASTOOLS_OK) {
    astools_sandbox_cleanup(c, &p->setup, false);
    return e;
  }

  memset(&o, 0, sizeof o);
  o.argv = (const char *const *)setup->argv;
  o.envp = (const char *const *)setup->envp;
  o.cwd = setup->scratch_dir;
  o.limit_cpu_seconds = setup->limit_cpu_seconds;
  o.limit_mem_bytes = setup->limit_mem_bytes;
  o.limit_nproc = setup->limit_nproc;
  e = os_proc_spawn(&o, &p->proc);
  if (e != ASTOOLS_OK) {
    astools_sandbox_cleanup(c, &p->setup, false);
    astools_pp_backoff(c, p);
    astools_exec_result_set(r, "astools/tool-crashed", "failed to spawn persistent tool process");
    return astools_seterr(c, ASTOOLS_ERR_TOOL, "cannot spawn persistent tool '%s' (%s)", t->m->id,
                          astools_err_name(e));
  }

  int64_t now = astools_mono(c);
  int64_t startup = t->m->startup_timeout_ms > 0 ? t->m->startup_timeout_ms : 10000;
  hello_deadline = startup < deadline_mono - now ? now + startup : deadline_mono;
  if (t->m->protocol != ASTOOLS_PROTOCOL_NATIVE) {
    e = t->m->protocol == ASTOOLS_PROTOCOL_MCP ?
        astools_mcp_initialize(c, p, hello_deadline, cancel_task) :
        astools_lsp_initialize(c, p, hello_deadline, cancel_task);
    if (e != ASTOOLS_OK) {
      astools_pp_stop(c, p, false);
      astools_pp_backoff(c, p);
      astools_exec_result_set(r, t->m->protocol == ASTOOLS_PROTOCOL_MCP ? "astools/mcp-startup" : "astools/lsp-startup",
          "%s", p->rpc_error[0] ? p->rpc_error : astools_err_name(e));
      return e;
    }
    p->alive = p->hello_done = true;
    p->backoff_ms = 0;
    p->next_restart_mono = 0;
    p->last_used_mono = astools_mono(c);
    return ASTOOLS_OK;
  }
  {
    char *line = NULL;
    size_t llen = 0;
    int why = 0;
    e = astools_pp_read_line(c, p, hello_deadline, cancel_task, &line, &llen, &why);
    if (e == ASTOOLS_OK) {
      char *emsg = NULL;
      if (astools_proto_parse_hello(line, llen, t, &emsg) != ASTOOLS_OK) {
        free(line);
        astools_pp_stop(c, p, false);
        astools_pp_backoff(c, p);
        astools_exec_result_set(r, "astools/protocol", "invalid #tool_hello: %s",
                                emsg ? emsg : "malformed");
        e = astools_seterr(c, ASTOOLS_ERR_PROTOCOL, "tool '%s': invalid #tool_hello: %s", t->m->id,
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
      astools_log(c, ASTOOLS_LOG_DEBUG, "proc", "persistent tool %s started", p->key);
      return ASTOOLS_OK;
    }
    astools_pp_stop(c, p, false);
    astools_pp_backoff(c, p);
    if (e == ASTOOLS_ERR_CANCELLED) {
      astools_exec_result_set(r, "astools/cancelled", "invocation cancelled");
      return astools_seterr(c, ASTOOLS_ERR_CANCELLED, "invocation cancelled");
    }
    if (e == ASTOOLS_ERR_TIMEOUT && astools_mono(c) >= deadline_mono) {
      astools_exec_result_set(r, "astools/timeout", "deadline exceeded during tool startup");
      return ASTOOLS_ERR_TIMEOUT;
    }
    if (e == ASTOOLS_ERR_NOMEM) return e;
    astools_exec_result_set(r, "astools/protocol", "no #tool_hello within startup_timeout");
    return astools_seterr(c, ASTOOLS_ERR_PROTOCOL,
                          "tool '%s': no #tool_hello within startup_timeout", t->m->id);
  }
}
