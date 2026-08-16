/*
 * api.c — public API funnel and context lifecycle.
 *
 * Every astools.h entry point validates its arguments, funnels into the
 * owning module (registry.c, invoke.c, catalog.c, ...) and reports engine
 * failures through astools_seterr. astools_open/astools_close wire the
 * whole context together; they are NOT thread-safe with respect to the
 * same context (public contract), so teardown runs lock-free after the
 * supervisor is joined.
 */

#include "astools_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- error names / reporting -------------------------------------------- */

static const char *const err_names[] = {
  "ASTOOLS_OK",
  "ASTOOLS_ERR_IO",
  "ASTOOLS_ERR_PARSE",
  "ASTOOLS_ERR_CONFIG",
  "ASTOOLS_ERR_NOT_FOUND",
  "ASTOOLS_ERR_INVALID",
  "ASTOOLS_ERR_DENIED",
  "ASTOOLS_ERR_TIMEOUT",
  "ASTOOLS_ERR_CANCELLED",
  "ASTOOLS_ERR_TOOL",
  "ASTOOLS_ERR_PROTOCOL",
  "ASTOOLS_ERR_UNSUPPORTED",
  "ASTOOLS_ERR_BUSY",
  "ASTOOLS_ERR_NOMEM"
};

#if defined(_MSC_VER)
#define ASTOOLS_TLS __declspec(thread)
#elif defined(__GNUC__) || defined(__clang__)
#define ASTOOLS_TLS __thread
#else
#define ASTOOLS_TLS _Thread_local
#endif

typedef struct {
  const astools_ctx *ctx;
  unsigned age;
  char text[512];
} err_tls_slot;

/* A small per-thread context map keeps astools_last_error both race-free and
 * context-specific without adding allocation or process-global state. */
static ASTOOLS_TLS err_tls_slot g_err_tls[8];
static ASTOOLS_TLS unsigned g_err_age;

static err_tls_slot *err_tls_get(const astools_ctx *c, int create) {
  size_t i, victim = 0;
  for (i = 0; i < sizeof g_err_tls / sizeof g_err_tls[0]; i++) {
    if (g_err_tls[i].ctx == c) return &g_err_tls[i];
    if (!g_err_tls[i].ctx) victim = i;
    else if (g_err_tls[i].age < g_err_tls[victim].age) victim = i;
  }
  if (!create) return NULL;
  g_err_tls[victim].ctx = c;
  g_err_tls[victim].text[0] = '\0';
  return &g_err_tls[victim];
}

const char *astools_err_name(astools_err e) {
  if ((int)e < 0 ||
      (size_t)e >= sizeof err_names / sizeof err_names[0])
    return "ASTOOLS_ERR_UNKNOWN";
  return err_names[e];
}

astools_err astools_seterr(astools_ctx *c, astools_err e, const char *fmt,
                           ...) {
  va_list ap;
  char text[512] = {0};
  err_tls_slot *slot;
  if (!c || !fmt) return e;
  va_start(ap, fmt);
  vsnprintf(text, sizeof text, fmt, ap);
  va_end(ap);
  os_mutex_lock(&c->err_mu);
  memcpy(c->err_buf, text, sizeof text);
  os_mutex_unlock(&c->err_mu);
  slot = err_tls_get(c, 1);
  if (slot) {
    memcpy(slot->text, text, sizeof text);
    slot->age = ++g_err_age;
  }
  return e;
}

const char *astools_last_error(const astools_ctx *c) {
  err_tls_slot *slot;
  if (!c) return "";
  slot = err_tls_get(c, 1);
  if (!slot) return "";
  if (slot->text[0] == '\0') {
    os_mutex_lock((os_mutex *)&c->err_mu);
    memcpy(slot->text, c->err_buf, sizeof slot->text);
    os_mutex_unlock((os_mutex *)&c->err_mu);
  }
  slot->age = ++g_err_age;
  return slot->text;
}

int64_t astools_mono(astools_ctx *c) {
  if (c && c->mono_ms) return c->mono_ms(c->mono_ud);
  return os_monotonic_ms();
}

/* ---- open / close -------------------------------------------------------- */

/* Default host callback (log.c contract): WARN and worse to stderr until
 * the host installs its own via astools_set_logger (NULL disables). */
static void default_log_cb(int level, const char *msg, void *ud) {
  (void)ud;
  if (level <= ASTOOLS_LOG_WARN) fprintf(stderr, "%s\n", msg);
}

/* cfg.lock_path resolved against the config dir when relative; malloc'd. */
static char *resolve_lock_path(const astools_ctx *c) {
  const char *lp = c->cfg.lock_path;
  if (!lp || lp[0] == '\0') lp = "astools.lock.xcdn";
  if (os_path_is_abs(lp)) return astools_strdup(lp);
  if (c->cfg.config_dir && c->cfg.config_dir[0] != '\0')
    return os_path_join(c->cfg.config_dir, lp);
  return astools_strdup(lp);
}

/* Full teardown. Requires: locks initialized, supervisor not running, no
 * other thread using c (astools_close contract). */
static void ctx_free(astools_ctx *c) {
  size_t i;
  for (i = 0; i < c->tools_n; i++) astools_tool_unref(c->tools[i]);
  free(c->tools);
  c->tools = NULL;
  c->tools_n = 0;
  astools_lockfile_free(&c->lockfile);
  astools_config_free(&c->cfg);
  free(c->workspace);
  free(c->scratch_base);
  astools_log_close(c);
  os_rwlock_destroy(&c->lock);
  os_mutex_destroy(&c->slot_mu);
  os_cond_destroy(&c->slot_cv);
  os_mutex_destroy(&c->pp_mu);
  os_mutex_destroy(&c->ev_mu);
  os_cond_destroy(&c->ev_cv);
  os_mutex_destroy(&c->err_mu);
  os_mutex_destroy(&c->log_mu);
  os_mutex_destroy(&c->audit_mu);
  free(c);
}

astools_err astools_open(const astools_open_params *p, astools_ctx **out) {
  astools_ctx *c;
  astools_err e;
  char *msg = NULL;
  const char *cfg_path;
  const char *ws;
  int changed = 0;

  if (!out) return ASTOOLS_ERR_INVALID;
  *out = NULL;

  c = calloc(1, sizeof *c);
  if (!c) return ASTOOLS_ERR_NOMEM;
  os_rwlock_init(&c->lock);
  os_mutex_init(&c->slot_mu);
  os_cond_init(&c->slot_cv);
  os_mutex_init(&c->pp_mu);
  os_mutex_init(&c->ev_mu);
  os_cond_init(&c->ev_cv);
  os_mutex_init(&c->err_mu);
  os_mutex_init(&c->log_mu);
  os_mutex_init(&c->audit_mu);
  c->clock = astools_clock_system();
  c->mono_ms = NULL; /* astools_mono falls back to os_monotonic_ms */
  c->mono_ud = NULL;
  c->log_cb = default_log_cb;
  c->log_ud = NULL;

  /* 2. configuration */
  astools_config_defaults(&c->cfg);
  cfg_path = p ? p->config_path : NULL;
  e = astools_config_load(cfg_path, cfg_path != NULL, &c->cfg, &msg);
  if (e != ASTOOLS_OK) {
    astools_log(c, ASTOOLS_LOG_ERROR, "config", "%s",
                msg ? msg : "configuration load failed");
    astools_seterr(c, e, "%s", msg ? msg : "configuration load failed");
    free(msg);
    goto fail;
  }

  /* 3. parameter overrides. Param roots are ADDITIVE: they are searched
   * first (standard trust), ahead of the ASTOOLS_PATH and config roots —
   * precedence, and the semantics astools-mcp relies on. */
  if (p && p->registry_paths) {
    size_t n = 0, i;
    astools_root *roots;
    while (p->registry_paths[n]) n++;
    roots = calloc(n + c->cfg.reg_paths_len + 1, sizeof *roots);
    if (!roots) {
      e = astools_seterr(c, ASTOOLS_ERR_NOMEM, "out of memory");
      goto fail;
    }
    for (i = 0; i < n; i++) {
      roots[i].path = astools_strdup(p->registry_paths[i]);
      if (!roots[i].path) {
        size_t k;
        for (k = 0; k < i; k++) free(roots[k].path);
        free(roots);
        e = astools_seterr(c, ASTOOLS_ERR_NOMEM, "out of memory");
        goto fail;
      }
      roots[i].trust = ASTOOLS_TRUST_STANDARD;
    }
    for (i = 0; i < c->cfg.reg_paths_len; i++)
      roots[n + i] = c->cfg.reg_paths[i]; /* ownership moves */
    free(c->cfg.reg_paths);
    c->cfg.reg_paths = roots;
    c->cfg.reg_paths_len = n + c->cfg.reg_paths_len;
  }
  if (p && p->workspace_root) {
    char *dup = astools_strdup(p->workspace_root);
    if (!dup) {
      e = astools_seterr(c, ASTOOLS_ERR_NOMEM, "out of memory");
      goto fail;
    }
    free(c->cfg.workspace_root);
    c->cfg.workspace_root = dup;
  }

  /* 4. workspace + scratch base (not created here; sandbox mkdir_p's) */
  ws = (c->cfg.workspace_root && c->cfg.workspace_root[0] != '\0')
           ? c->cfg.workspace_root
           : ".";
  e = os_realpath(ws, &c->workspace);
  if (e != ASTOOLS_OK) {
    astools_log(c, ASTOOLS_LOG_ERROR, "config",
                "workspace does not exist: %s", ws);
    e = astools_seterr(c, ASTOOLS_ERR_CONFIG, "workspace does not exist: %s",
                       ws);
    goto fail;
  }
  if (c->cfg.scratch && c->cfg.scratch[0] != '\0') {
    c->scratch_base = os_path_is_abs(c->cfg.scratch)
                          ? astools_strdup(c->cfg.scratch)
                          : os_path_join(c->workspace, c->cfg.scratch);
  } else {
    c->scratch_base = os_path_join(c->workspace, ".astools/tmp");
  }
  if (!c->scratch_base) {
    e = astools_seterr(c, ASTOOLS_ERR_NOMEM, "out of memory");
    goto fail;
  }

  /* 5. file log sink — IO failure degrades to callback-only */
  if (astools_log_open(c) != ASTOOLS_OK)
    astools_log(c, ASTOOLS_LOG_WARN, "config",
                "cannot open log file '%s'; file sink disabled",
                c->cfg.log_path ? c->cfg.log_path : "");

  /* 6. lockfile (missing => empty) */
  {
    char *lp = resolve_lock_path(c);
    if (lp) {
      if (astools_lockfile_load(lp, &c->lockfile) != ASTOOLS_OK) {
        astools_lockfile_free(&c->lockfile);
        c->lockfile.entries = NULL;
        c->lockfile.len = 0;
        astools_log(c, ASTOOLS_LOG_WARN, "config",
                    "lockfile '%s' unreadable; treating as empty", lp);
      }
      free(lp);
    }
  }

  /* 7. initial scan — per-package rejects are logged inside; even a total
   * failure only means an empty registry */
  e = astools_registry_scan(c, &changed);
  if (e != ASTOOLS_OK)
    astools_log(c, ASTOOLS_LOG_WARN, "registry", "initial scan failed: %s",
                astools_err_name(e));

  /* 8. supervisor */
#ifndef ASTOOLS_NO_THREADS
  c->no_threads = false;
  e = astools_worker_start(c);
  if (e != ASTOOLS_OK)
    astools_log(c, ASTOOLS_LOG_WARN, "worker",
                "supervisor failed to start: %s", astools_err_name(e));
#else
  c->no_threads = true;
#endif

  /* 9. open summary */
  {
    astools_stats st;
    os_rwlock_rdlock(&c->lock);
    st = c->stats;
    os_rwlock_rdunlock(&c->lock);
    astools_log(c, ASTOOLS_LOG_INFO, "config",
                "open: workspace=%s tools=%zu enabled=%zu unavailable=%zu",
                c->workspace, st.tools_total, st.tools_enabled,
                st.tools_unavailable);
  }

  *out = c;
  return ASTOOLS_OK;

fail:
  /* The supervisor is never running on a failure path (steps after
   * worker_start cannot fail), so plain teardown suffices. */
  ctx_free(c);
  return e;
}

void astools_close(astools_ctx *c) {
  if (!c) return;
  astools_worker_stop(c); /* joins supervisor, reaps persistent tools */
  ctx_free(c);
}

/* ---- registry ------------------------------------------------------------ */

astools_err astools_registry_refresh(astools_ctx *c) {
  int changed = 0;
  if (!c) return ASTOOLS_ERR_INVALID;
  return astools_registry_scan(c, &changed);
}

static int cmp_str_ptr(const void *a, const void *b) {
  const char *const *pa = (const char *const *)a;
  const char *const *pb = (const char *const *)b;
  return strcmp(*pa, *pb);
}

astools_err astools_tool_list(astools_ctx *c, char ***out_ids, size_t *n) {
  char **ids;
  size_t total, i, uniq;
  if (!c || !out_ids || !n)
    return astools_seterr(c, ASTOOLS_ERR_INVALID, "tool_list: bad arguments");
  *out_ids = NULL;
  *n = 0;
  os_rwlock_rdlock(&c->lock);
  total = c->tools_n;
  ids = malloc((total ? total : 1) * sizeof *ids);
  if (!ids) {
    os_rwlock_rdunlock(&c->lock);
    return astools_seterr(c, ASTOOLS_ERR_NOMEM, "out of memory");
  }
  for (i = 0; i < total; i++) {
    ids[i] = astools_strdup(c->tools[i]->m->id);
    if (!ids[i]) {
      size_t j;
      os_rwlock_rdunlock(&c->lock);
      for (j = 0; j < i; j++) free(ids[j]);
      free(ids);
      return astools_seterr(c, ASTOOLS_ERR_NOMEM, "out of memory");
    }
  }
  os_rwlock_rdunlock(&c->lock);
  qsort(ids, total, sizeof *ids, cmp_str_ptr);
  uniq = 0;
  for (i = 0; i < total; i++) {
    if (uniq > 0 && strcmp(ids[uniq - 1], ids[i]) == 0)
      free(ids[i]); /* another version of the same id */
    else
      ids[uniq++] = ids[i];
  }
  *out_ids = ids;
  *n = uniq;
  return ASTOOLS_OK;
}

astools_err astools_tool_manifest(astools_ctx *c, const char *ref,
                                  char **out_xcdn) {
  astools_tool *t = NULL;
  astools_err e;
  if (!c || !ref || !out_xcdn)
    return astools_seterr(c, ASTOOLS_ERR_INVALID,
                          "tool_manifest: bad arguments");
  *out_xcdn = NULL;
  e = astools_registry_resolve(c, ref, &t);
  if (e != ASTOOLS_OK) return e; /* resolve already set the message */
  e = astools_manifest_render(t->m, out_xcdn);
  astools_tool_unref(t);
  if (e != ASTOOLS_OK)
    return astools_seterr(c, e, "manifest render failed for '%s'", ref);
  return ASTOOLS_OK;
}

/* Split "id" / "id@version": malloc'd id, version borrowed from ref. */
static astools_err ref_split(const char *ref, char **out_id,
                             const char **out_ver) {
  const char *at;
  *out_id = NULL;
  *out_ver = NULL;
  if (!ref || ref[0] == '\0') return ASTOOLS_ERR_INVALID;
  at = strchr(ref, '@');
  if (at) {
    if (at == ref || at[1] == '\0') return ASTOOLS_ERR_INVALID;
    *out_id = astools_strndup(ref, (size_t)(at - ref));
    *out_ver = at + 1;
  } else {
    *out_id = astools_strdup(ref);
  }
  return *out_id ? ASTOOLS_OK : ASTOOLS_ERR_NOMEM;
}

astools_err astools_tool_enable(astools_ctx *c, const char *ref, int on) {
  char *id = NULL;
  const char *ver = NULL;
  size_t i, matched = 0;
  astools_err e;
  if (!c || !ref)
    return astools_seterr(c, ASTOOLS_ERR_INVALID,
                          "tool_enable: bad arguments");
  e = ref_split(ref, &id, &ver);
  if (e != ASTOOLS_OK)
    return astools_seterr(c, e, "tool_enable: invalid ref '%s'", ref);
  os_rwlock_wrlock(&c->lock);
  for (i = 0; i < c->tools_n; i++) {
    astools_tool *t = c->tools[i];
    if (strcmp(t->m->id, id) != 0) continue;
    if (ver && strcmp(t->m->version, ver) != 0) continue;
    t->host_disabled = (on == 0);
    /* Mirror registry.c: enabled = host toggle AND pinning gate; under
     * "enforce" only a hash-verified lockfile entry passes. */
    t->enabled = !t->host_disabled &&
                 !(c->cfg.pinning == ASTOOLS_PIN_ENFORCE &&
                   t->lock_state != ASTOOLS_LOCK_OK);
    matched++;
  }
  if (matched > 0) {
    size_t en = 0;
    for (i = 0; i < c->tools_n; i++)
      if (c->tools[i]->enabled && c->tools[i]->available) en++;
    c->stats.tools_enabled = en;
  }
  os_rwlock_wrunlock(&c->lock);
  free(id);
  if (matched == 0)
    return astools_seterr(c, ASTOOLS_ERR_NOT_FOUND, "tool '%s' not found",
                          ref);
  return ASTOOLS_OK;
}

static const char *pin_version(const astools_config *cfg, const char *id) {
  size_t i;
  for (i = 0; i < cfg->pins_len; i++)
    if (cfg->pins[i].tool && strcmp(cfg->pins[i].tool, id) == 0)
      return cfg->pins[i].version;
  return NULL;
}

/* Administrative lookup for approve: same ref resolution as the registry
 * (pins applied, highest release for bare refs) but WITHOUT the enabled /
 * pinning gates — a pin-blocked tool must remain approvable, otherwise
 * "enforce" mode could never be satisfied. Adds a ref; caller unrefs. */
static astools_err find_tool_admin(astools_ctx *c, const char *ref,
                                   astools_tool **out) {
  char *id = NULL;
  const char *ver = NULL;
  astools_tool *best = NULL;
  size_t i;
  astools_err e = ref_split(ref, &id, &ver);
  if (e != ASTOOLS_OK) return e;
  os_rwlock_rdlock(&c->lock);
  if (!ver) {
    const char *pv = pin_version(&c->cfg, id);
    if (pv && pv[0] != '\0') ver = pv;
  }
  for (i = 0; i < c->tools_n; i++) {
    astools_tool *t = c->tools[i];
    if (strcmp(t->m->id, id) != 0) continue;
    if (ver) {
      if (strcmp(t->m->version, ver) == 0) {
        best = t; /* table order preserves "first root wins" */
        break;
      }
    } else if (!best) {
      best = t;
    } else {
      /* highest version, releases preferred over pre-releases */
      bool best_pre = best->ver.prerelease != NULL;
      bool cand_pre = t->ver.prerelease != NULL;
      if ((best_pre && !cand_pre) ||
          (best_pre == cand_pre &&
           astools_semver_cmp(&t->ver, &best->ver) > 0))
        best = t;
    }
  }
  if (best) astools_tool_ref(best);
  os_rwlock_rdunlock(&c->lock);
  free(id);
  if (!best) return ASTOOLS_ERR_NOT_FOUND;
  *out = best;
  return ASTOOLS_OK;
}

astools_err astools_tool_approve(astools_ctx *c, const char *ref) {
  astools_tool *t = NULL;
  char *lp = NULL;
  astools_err e;
  int changed = 0;
  if (!c || !ref)
    return astools_seterr(c, ASTOOLS_ERR_INVALID,
                          "tool_approve: bad arguments");
  e = find_tool_admin(c, ref, &t);
  if (e == ASTOOLS_ERR_NOT_FOUND)
    return astools_seterr(c, e, "tool '%s' not found", ref);
  if (e != ASTOOLS_OK)
    return astools_seterr(c, e, "tool_approve: invalid ref '%s'", ref);
  lp = resolve_lock_path(c);
  if (!lp) {
    astools_tool_unref(t);
    return astools_seterr(c, ASTOOLS_ERR_NOMEM, "out of memory");
  }
  e = astools_lockfile_approve(lp, t->m->id, t->m->version, t->pkg_dir, t->m,
                               astools_clock_now(&c->clock));
  if (e != ASTOOLS_OK) {
    astools_tool_unref(t);
    free(lp);
    return astools_seterr(c, e, "approve of '%s' failed (%s)", ref,
                          astools_err_name(e));
  }
  /* Reload the lockfile so future checks see the fresh hashes, then
   * rescan so lock_state / enabled refresh across the table. */
  {
    astools_lockfile fresh;
    memset(&fresh, 0, sizeof fresh);
    if (astools_lockfile_load(lp, &fresh) == ASTOOLS_OK) {
      os_rwlock_wrlock(&c->lock);
      astools_lockfile_free(&c->lockfile);
      c->lockfile = fresh;
      os_rwlock_wrunlock(&c->lock);
    } else {
      astools_lockfile_free(&fresh);
    }
  }
  free(lp);
  astools_tool_unref(t);
  (void)astools_registry_scan(c, &changed);
  return ASTOOLS_OK;
}

/* ---- conversational interface -------------------------------------------- */

astools_err astools_catalog(astools_ctx *c, astools_catalog_level level,
                            size_t char_budget, char **out_text) {
  if (!c || !out_text)
    return astools_seterr(c, ASTOOLS_ERR_INVALID, "catalog: bad arguments");
  *out_text = NULL;
  return astools_catalog_render(c, level, char_budget, out_text);
}

astools_err astools_grammar_export(astools_ctx *c, char **out_gbnf) {
  if (!c || !out_gbnf)
    return astools_seterr(c, ASTOOLS_ERR_INVALID,
                          "grammar_export: bad arguments");
  *out_gbnf = NULL;
  return astools_gbnf_render(c, out_gbnf);
}

astools_err astools_call_parse(astools_ctx *c, const char *model_output,
                               char **out_ref, char **out_command,
                               char **out_args_xcdn) {
  if (!c || !model_output || !out_ref || !out_command || !out_args_xcdn)
    return ASTOOLS_ERR_INVALID;
  *out_ref = NULL;
  *out_command = NULL;
  *out_args_xcdn = NULL;
  /* ASTOOLS_ERR_NOT_FOUND (no call line in the text) passes through
   * without touching err_buf: scanning free-form model output is not an
   * engine failure. */
  return astools_callline_parse(model_output, out_ref, out_command,
                                out_args_xcdn);
}

astools_err astools_call_format(astools_ctx *c, const char *ref,
                                const char *command,
                                const struct astools_result_s *r,
                                char **out_line) {
  if (!c || !ref || !command || !r || !out_line)
    return astools_seterr(c, ASTOOLS_ERR_INVALID,
                          "call_format: bad arguments");
  *out_line = NULL;
  return astools_callline_format(ref, command, r, out_line);
}

/* ---- invocation ----------------------------------------------------------- */

astools_err astools_validate_args(astools_ctx *c, const char *ref,
                                  const char *command,
                                  const char *args_xcdn) {
  if (!c || !ref || !command)
    return astools_seterr(c, ASTOOLS_ERR_INVALID,
                          "validate_args: bad arguments");
  return astools_validate_impl(c, ref, command, args_xcdn);
}

astools_err astools_invoke(astools_ctx *c, const char *ref,
                           const char *command, const char *args_xcdn,
                           uint32_t deadline_ms, astools_result *out) {
  if (!c || !ref || !command || !out)
    return astools_seterr(c, ASTOOLS_ERR_INVALID, "invoke: bad arguments");
  memset(out, 0, sizeof *out);
  return astools_invoke_impl(c, ref, command, args_xcdn, deadline_ms, NULL,
                             out);
}

/* ---- sandbox / observability ---------------------------------------------- */

astools_err astools_get_sandbox_caps(astools_ctx *c, int strict,
                                     astools_sandbox_caps *out) {
  if (!c || !out)
    return astools_seterr(c, ASTOOLS_ERR_INVALID,
                          "sandbox_caps: bad arguments");
  memset(out, 0, sizeof *out);
  return astools_sandbox_caps_impl(strict, out);
}

astools_err astools_get_stats(astools_ctx *c, astools_stats *out) {
  if (!c || !out)
    return astools_seterr(c, ASTOOLS_ERR_INVALID, "get_stats: bad arguments");
  os_rwlock_rdlock(&c->lock);
  *out = c->stats;
  /* c->last_refresh_unix is the authoritative scan timestamp; merge in
   * case registry.c only updated the context field. */
  if (c->last_refresh_unix > out->last_refresh_unix)
    out->last_refresh_unix = c->last_refresh_unix;
  os_rwlock_rdunlock(&c->lock);
  return ASTOOLS_OK;
}

void astools_set_logger(astools_ctx *c, astools_log_fn fn, void *ud) {
  if (!c) return;
  os_mutex_lock(&c->log_mu);
  c->log_cb = fn;
  c->log_ud = ud;
  os_mutex_unlock(&c->log_mu);
}

astools_err astools_tick(astools_ctx *c) {
  if (!c) return ASTOOLS_ERR_INVALID;
  /* Threaded builds: the supervisor already runs this work; an extra
   * tick is harmless (worker_tick is idempotent for due work). */
  return astools_worker_tick(c);
}

void astools_result_free(astools_result *r) {
  if (!r) return;
  free(r->result_xcdn);
  free(r->error_code);
  free(r->error_message);
  memset(r, 0, sizeof *r);
}

void astools_free(void *p) { free(p); }
