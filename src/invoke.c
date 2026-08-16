/*
 * invoke.c — the invocation pipeline.
 *
 * Engine failure and tool failure are never conflated: the returned
 * astools_err is the engine verdict; out->ok / out->error_* carry the
 * tool-level outcome. The exec layer (worker.c) fills reserved
 * "astools/..." codes into the result for engine-caused terminations so
 * audit and RESULT/ERROR lines stay self-describing.
 */

#include "astools_internal.h"

#include <stdlib.h>
#include <string.h>

/* ---- small helpers ------------------------------------------------------ */

static void result_clear(astools_result *r) {
  if (!r) return;
  free(r->result_xcdn);
  free(r->error_code);
  free(r->error_message);
  memset(r, 0, sizeof *r);
}

static const char *sb_level_name(int lvl) {
  switch (lvl) {
    case ASTOOLS_SB_NONE: return "none";
    case ASTOOLS_SB_BASIC: return "basic";
    case ASTOOLS_SB_STRICT: return "strict";
    default: return "-";
  }
}

static const char *mode_name(const astools_manifest *m) {
  if (m->kind == ASTOOLS_KIND_LIBRARY) return "library";
  return m->mode == ASTOOLS_MODE_PERSISTENT ? "persistent" : "oneshot";
}

/* Reserved code for engine verdicts that reached dispatch (audit trail). */
static const char *engine_code(astools_err e) {
  switch (e) {
    case ASTOOLS_ERR_DENIED: return "astools/denied";
    case ASTOOLS_ERR_TIMEOUT: return "astools/timeout";
    case ASTOOLS_ERR_CANCELLED: return "astools/cancelled";
    case ASTOOLS_ERR_PROTOCOL: return "astools/protocol";
    case ASTOOLS_ERR_TOOL: return "astools/tool-crashed";
    case ASTOOLS_ERR_INVALID: return "astools/invalid-args";
    default: return astools_err_name(e);
  }
}

/* args text -> first value; NULL/blank text means the empty object. The
 * document owns the tree; *out_node borrows. */
static astools_err parse_args(astools_ctx *c, const char *args_xcdn,
                              xcdn_document_t **out_doc,
                              xcdn_node_t **out_node) {
  const char *text = "{}";
  char *emsg = NULL;
  astools_err e;
  *out_doc = NULL;
  *out_node = NULL;
  if (args_xcdn && !astools_str_blank(args_xcdn)) text = args_xcdn;
  e = astools_xcdn_parse_first(text, strlen(text), out_doc, out_node, &emsg);
  if (e != ASTOOLS_OK) {
    e = astools_seterr(c, ASTOOLS_ERR_INVALID, "invalid args: %s",
                       emsg ? emsg : "parse error");
    free(emsg);
    return e;
  }
  if (!*out_node || !(*out_node)->value ||
      (*out_node)->value->type != XCDN_VAL_OBJECT) {
    xcdn_document_free(*out_doc);
    *out_doc = NULL;
    *out_node = NULL;
    return astools_seterr(c, ASTOOLS_ERR_INVALID, "args must be an object");
  }
  return ASTOOLS_OK;
}

static astools_err cmd_not_found(astools_ctx *c, const astools_tool *t,
                                 const char *command) {
  astools_buf b;
  astools_err e;
  size_t i;
  astools_buf_init(&b);
  for (i = 0; i < t->m->commands_len; i++) {
    if (i > 0) (void)astools_buf_appends(&b, ", ");
    (void)astools_buf_appends(&b, t->m->commands[i].name);
  }
  e = astools_seterr(c, ASTOOLS_ERR_NOT_FOUND,
                     "tool '%s' has no command '%s' (valid: %s)", t->m->id,
                     command, b.data ? b.data : "none");
  astools_buf_free(&b);
  return e;
}

/* ---- validation-only path (astools_validate_args) ------------------------ */

astools_err astools_validate_impl(astools_ctx *c, const char *ref,
                                  const char *command,
                                  const char *args_xcdn) {
  astools_tool *t = NULL;
  const astools_cmd *cmd;
  xcdn_document_t *doc = NULL;
  xcdn_node_t *args = NULL;
  char verr[512];
  astools_err e;

  if (!c || !ref || !command) return ASTOOLS_ERR_INVALID;
  e = astools_registry_resolve(c, ref, &t);
  if (e != ASTOOLS_OK) return e;
  cmd = astools_manifest_cmd(t->m, command);
  if (!cmd) {
    e = cmd_not_found(c, t, command);
    goto done;
  }
  e = parse_args(c, args_xcdn, &doc, &args);
  if (e != ASTOOLS_OK) goto done;
  e = astools_args_validate(cmd, args, verr, sizeof verr);
  if (e != ASTOOLS_OK)
    e = astools_seterr(c, ASTOOLS_ERR_INVALID, "%s", verr);
done:
  xcdn_document_free(doc);
  astools_tool_unref(t);
  return e;
}

/* ---- result validation (step 7) ------------------------------------- */

/* Returns ASTOOLS_OK when the successful result matches returns_type;
 * otherwise writes a message into err. */
static astools_err check_result(const astools_cmd *cmd,
                                const astools_result *r, char *err,
                                size_t err_cap) {
  xcdn_document_t *doc = NULL;
  xcdn_node_t *node = NULL;
  char *pmsg = NULL;
  astools_err e;
  if (!r->result_xcdn) {
    snprintf(err, err_cap, "result: missing value");
    return ASTOOLS_ERR_PROTOCOL;
  }
  e = astools_xcdn_parse_first(r->result_xcdn, strlen(r->result_xcdn), &doc,
                               &node, &pmsg);
  if (e != ASTOOLS_OK) {
    snprintf(err, err_cap, "result: %s", pmsg ? pmsg : "unparsable value");
    free(pmsg);
    return ASTOOLS_ERR_PROTOCOL;
  }
  e = astools_type_check(cmd->returns_type, node, "result", err, err_cap);
  xcdn_document_free(doc);
  free(pmsg);
  return e;
}

/* ---- the pipeline -------------------------------------------------- */

astools_err astools_invoke_impl(astools_ctx *c, const char *ref,
                                const char *command, const char *args_xcdn,
                                uint32_t deadline_ms,
                                astools_task *cancel_task,
                                astools_result *out) {
  astools_tool *t = NULL;
  const astools_cmd *cmd = NULL;
  xcdn_document_t *doc = NULL;
  xcdn_node_t *args = NULL;
  astools_effective eff;
  astools_sandbox_setup setup;
  bool eff_ok = false, setup_ok = false;
  bool keep_scratch = false;
  char **argv = NULL;
  char *req = NULL;
  char *lib_scratch = NULL;
  char *stderr_cap = NULL;
  char id[37];
  char verr[512];
  uint8_t args_sha[32];
  int have_slot = 0;
  int exit_code = 0;
  int sb_level = -1;
  int64_t t0, dl_ms, deadline_mono;
  astools_time deadline_wall;
  astools_err e;

  if (!out) return ASTOOLS_ERR_INVALID;
  memset(out, 0, sizeof *out);
  if (!c || !ref || !command) return ASTOOLS_ERR_INVALID;
  memset(&eff, 0, sizeof eff);
  memset(&setup, 0, sizeof setup);
  id[0] = '\0';
  t0 = astools_mono(c);

  /* 1. resolve ref, find command. */
  e = astools_registry_resolve(c, ref, &t);
  if (e != ASTOOLS_OK) goto cleanup_early;
  e = astools_registry_revalidate(c, t);
  if (e != ASTOOLS_OK) goto done;
  cmd = astools_manifest_cmd(t->m, command);
  if (!cmd) {
    e = cmd_not_found(c, t, command);
    goto done; /* a resolved tool: count + audit like any failed invocation */
  }

  /* 2. parse + validate args, inject defaults. */
  e = parse_args(c, args_xcdn, &doc, &args);
  if (e != ASTOOLS_OK) goto done;
  e = astools_args_validate(cmd, args, verr, sizeof verr);
  if (e != ASTOOLS_OK) {
    e = astools_seterr(c, ASTOOLS_ERR_INVALID, "%s", verr);
    goto done;
  }

  /* 3. policy pre-flight; canonicalizes path args in place. */
  e = astools_policy_effective(c, t, &eff);
  if (e != ASTOOLS_OK) goto done;
  eff_ok = true;
  {
    char *deny = NULL;
    e = astools_policy_preflight(c, t, cmd, args, &eff, &deny);
    if (e != ASTOOLS_OK) {
      if (e == ASTOOLS_ERR_DENIED) {
        e = astools_seterr(c, ASTOOLS_ERR_DENIED, "%s",
                           deny ? deny : "denied by policy");
        astools_log(c, ASTOOLS_LOG_WARN, "invoke", "deny %s.%s: %s",
                    t->m->id, command, deny ? deny : "denied by policy");
      }
      free(deny);
      goto done;
    }
  }

  /* 4. deadline precedence: arg > command timeout > invocation.timeout. */
  dl_ms = deadline_ms > 0 ? (int64_t)deadline_ms
                          : (cmd->timeout_ms > 0 ? cmd->timeout_ms
                                                 : c->cfg.timeout_ms);
  if (dl_ms <= 0) dl_ms = 30000;
  deadline_mono = astools_mono(c) + dl_ms;
  e = astools_slot_acquire(c, deadline_mono);
  if (e != ASTOOLS_OK) {
    if (e == ASTOOLS_ERR_TIMEOUT)
      e = astools_seterr(c, ASTOOLS_ERR_TIMEOUT,
                         "timed out waiting for an invocation slot");
    goto done;
  }
  have_slot = 1;

  /* 5. dispatch by kind/mode inside the sandbox. */
  astools_uuid_v4(id);
  deadline_wall = astools_clock_now(&c->clock) + (dl_ms + 999) / 1000;

  if (t->m->kind == ASTOOLS_KIND_LIBRARY) {
    if (!c->cfg.allow_library || t->trust != ASTOOLS_TRUST_FULL) {
      e = astools_seterr(
          c, ASTOOLS_ERR_DENIED,
          "library tool '%s' requires sandbox.allow_library and a "
          "full-trust registry root",
          t->m->id);
      goto done;
    }
    lib_scratch = os_path_join(c->scratch_base, id);
    if (!lib_scratch) {
      e = ASTOOLS_ERR_NOMEM;
      goto done;
    }
    (void)os_mkdir_p(lib_scratch); /* best effort; advisory for libraries */
    e = astools_proto_request(t, cmd, args, id, deadline_wall,
                              c->cfg.max_output_bytes, &eff, c->workspace,
                              lib_scratch, &req);
    if (e != ASTOOLS_OK) goto done;
    e = astools_exec_library(c, t, req, out);
  } else {
    e = astools_entry_resolve_argv(t, &argv);
    if (e != ASTOOLS_OK) goto done;
    e = astools_sandbox_prepare(c, t, &eff, id, argv, &setup);
    if (e != ASTOOLS_OK) goto done;
    setup_ok = true;
    sb_level = setup.level;
    e = astools_proto_request(t, cmd, args, id, deadline_wall,
                              c->cfg.max_output_bytes, &eff, c->workspace,
                              setup.scratch_dir, &req);
    if (e != ASTOOLS_OK) goto done;
    if (t->m->mode == ASTOOLS_MODE_PERSISTENT) {
#if defined(ASTOOLS_NO_THREADS)
      e = astools_seterr(c, ASTOOLS_ERR_UNSUPPORTED,
                         "persistent tools need threads "
                         "(ASTOOLS_NO_THREADS build)");
#else
      if (c->no_threads)
        e = astools_seterr(c, ASTOOLS_ERR_UNSUPPORTED,
                           "persistent tools are unavailable in "
                           "no-thread mode");
      else
        e = astools_exec_persistent(c, t, &setup, req, id, deadline_mono,
                                    cancel_task, out);
#endif
    } else {
      e = astools_exec_oneshot(c, &setup, req, id, deadline_mono,
                               c->cfg.max_output_bytes,
                               c->cfg.stderr_max_bytes, cancel_task, out,
                               &exit_code, &stderr_cap);
      out->exit_code = exit_code;
    }
  }

  /* 6. result validation (step 7). */
  if (e == ASTOOLS_OK && out->ok && cmd->returns_type &&
      c->cfg.result_validation != ASTOOLS_RESVAL_OFF) {
    char rerr[512];
    if (check_result(cmd, out, rerr, sizeof rerr) != ASTOOLS_OK) {
      if (c->cfg.result_validation == ASTOOLS_RESVAL_ENFORCE) {
        result_clear(out);
        e = astools_seterr(c, ASTOOLS_ERR_PROTOCOL,
                           "result validation failed for %s.%s: %s",
                           t->m->id, command, rerr);
      } else {
        astools_log(c, ASTOOLS_LOG_WARN, "invoke",
                    "result validation: %s.%s: %s", t->m->id, command, rerr);
      }
    }
  }

done:
  /* 7. counters, audit, log. Reached once the ref resolved to a tool `t`
   * (cmd/args may be absent on a post-resolve validation failure), so every
   * attempted invocation of a real tool is counted and audited —
   * symmetric with the policy-denied path. */
  out->duration_ms = (uint64_t)(astools_mono(c) - t0);

  os_rwlock_wrlock(&c->lock);
  c->stats.invocations++;
  if (e == ASTOOLS_OK) {
    if (out->ok)
      c->stats.ok++;
    else
      c->stats.failed++;
  } else if (e == ASTOOLS_ERR_DENIED) {
    c->stats.denied++;
  } else if (e == ASTOOLS_ERR_TIMEOUT) {
    c->stats.timeouts++;
  } else if (e == ASTOOLS_ERR_CANCELLED) {
    c->stats.cancelled++;
  } else {
    c->stats.failed++;
  }
  c->stats.last_invocation_unix = astools_clock_now(&c->clock);
  os_rwlock_wrunlock(&c->lock);

  {
    /* args_sha256 = SHA-256 of the canonical compact args text. */
    char *ctext = args ? astools_xcdn_write_node(args, false) : NULL;
    memset(args_sha, 0, sizeof args_sha);
    if (ctext) {
      astools_sha256(ctext, strlen(ctext), args_sha);
      free(ctext);
    }
  }
  {
    bool tool_ok = (e == ASTOOLS_OK && out->ok);
    const char *ecode = NULL;
    if (!tool_ok) {
      ecode = out->error_code;
      if (!ecode && e != ASTOOLS_OK) ecode = engine_code(e);
    }
    astools_audit_append(c, t->m->id, t->m->version, command, tool_ok, ecode,
                         out->duration_ms, args_sha);

    if (stderr_cap && stderr_cap[0] != '\0')
      astools_log(c, ASTOOLS_LOG_DEBUG, "invoke", "%s.%s stderr: %s",
                  t->m->id, command, stderr_cap);

    if (tool_ok)
      astools_log(c, ASTOOLS_LOG_INFO, "invoke",
                  "invoke %s.%s ok (%llums, %s, %s)", t->m->id, command,
                  (unsigned long long)out->duration_ms, mode_name(t->m),
                  sb_level_name(sb_level));
    else if (e == ASTOOLS_OK)
      astools_log(c, ASTOOLS_LOG_INFO, "invoke",
                  "invoke %s.%s error %s (%llums, %s, %s)", t->m->id,
                  command, out->error_code ? out->error_code : "?",
                  (unsigned long long)out->duration_ms, mode_name(t->m),
                  sb_level_name(sb_level));
    else
      astools_log(c, ASTOOLS_LOG_INFO, "invoke",
                  "invoke %s.%s failed %s (%llums, %s, %s)", t->m->id,
                  command, astools_err_name(e),
                  (unsigned long long)out->duration_ms, mode_name(t->m),
                  sb_level_name(sb_level));
  }
  goto cleanup;

cleanup_early:
  out->duration_ms = (uint64_t)(astools_mono(c) - t0);
cleanup:
  keep_scratch =
      (e != ASTOOLS_OK) && c->cfg.log_level >= ASTOOLS_LOG_DEBUG;
  if (have_slot) astools_slot_release(c);
  if (setup_ok) astools_sandbox_cleanup(c, &setup, keep_scratch);
  if (lib_scratch) {
    if (!keep_scratch) (void)os_rmtree(lib_scratch);
    free(lib_scratch);
  }
  astools_argv_free(argv);
  free(req);
  free(stderr_cap);
  if (eff_ok) astools_effective_free(&eff);
  xcdn_document_free(doc);
  astools_tool_unref(t);
  return e;
}
