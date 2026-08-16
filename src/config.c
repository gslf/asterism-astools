/*
 * config.c — configuration defaults + config.xcdn loading.
 *
 * astools_config_load merges a parsed #astools_config document over a cfg
 * that MUST already hold astools_config_defaults() output. Unknown keys,
 * wrong value types and unknown enum values are hard errors
 * (ASTOOLS_ERR_CONFIG) so a typo cannot silently weaken a safety-relevant
 * setting. A missing file is tolerated even for an explicit path
 * (builtin defaults, never create). After a load — successful,
 * defaulted, or file-missing — the ASTOOLS_PATH environment entries are
 * prepended to registry.paths as standard-trust roots, first in order.
 */

#include "astools_internal.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* ---- error helpers ------------------------------------------------------ */

/* First failure wins: an already-set *err_msg is never overwritten, so the
 * innermost (most precise) message survives up the call chain. */
static astools_err cfg_fail(char **err_msg, const char *fmt, ...) {
  va_list ap;
  int need;
  char *p;
  if (!err_msg || *err_msg || !fmt) return ASTOOLS_ERR_CONFIG;
  va_start(ap, fmt);
  need = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (need < 0) return ASTOOLS_ERR_CONFIG;
  p = malloc((size_t)need + 1);
  if (!p) return ASTOOLS_ERR_CONFIG;
  va_start(ap, fmt);
  vsnprintf(p, (size_t)need + 1, fmt, ap);
  va_end(ap);
  *err_msg = p;
  return ASTOOLS_ERR_CONFIG;
}

static astools_err cfg_oom(char **err_msg) {
  if (err_msg && !*err_msg) *err_msg = astools_strdup("config: out of memory");
  return ASTOOLS_ERR_NOMEM;
}

/* ---- deep-free helpers (shared with astools_config_free) ---------------- */

static void free_str_array(char **a, size_t n) {
  size_t i;
  if (!a) return;
  for (i = 0; i < n; i++) free(a[i]);
  free(a);
}

static void free_fs_perms(astools_fs_perm *f, size_t n) {
  size_t i;
  if (!f) return;
  for (i = 0; i < n; i++) free(f[i].path);
  free(f);
}

static void free_roots(astools_root *r, size_t n) {
  size_t i;
  if (!r) return;
  for (i = 0; i < n; i++) free(r[i].path);
  free(r);
}

static void free_pins(astools_pin *p, size_t n) {
  size_t i;
  if (!p) return;
  for (i = 0; i < n; i++) {
    free(p[i].tool);
    free(p[i].version);
  }
  free(p);
}

static void free_grants(astools_grant *g, size_t n) {
  size_t i;
  if (!g) return;
  for (i = 0; i < n; i++) {
    free(g[i].tool);
    free_fs_perms(g[i].fs, g[i].fs_len);
    free_str_array(g[i].env, g[i].env_len);
  }
  free(g);
}

/* ---- value extraction --------------------------------------------------- */

static astools_err replace_string(char **slot, const char *s) {
  char *d = astools_strdup(s);
  if (!d) return ASTOOLS_ERR_NOMEM;
  free(*slot);
  *slot = d;
  return ASTOOLS_OK;
}

static astools_err get_string(const xcdn_node_t *n, const char *key,
                              const char **out, char **err_msg) {
  const xcdn_value_t *v = n ? n->value : NULL;
  if (!v || v->type != XCDN_VAL_STRING || !v->data.string)
    return cfg_fail(err_msg, "config: %s: expected string", key);
  *out = v->data.string;
  return ASTOOLS_OK;
}

static astools_err get_bool(const xcdn_node_t *n, const char *key, bool *out,
                            char **err_msg) {
  const xcdn_value_t *v = n ? n->value : NULL;
  if (!v || v->type != XCDN_VAL_BOOL)
    return cfg_fail(err_msg, "config: %s: expected boolean", key);
  *out = v->data.boolean;
  return ASTOOLS_OK;
}

static astools_err get_int(const xcdn_node_t *n, const char *key, int64_t minv,
                           int64_t maxv, int64_t *out, char **err_msg) {
  const xcdn_value_t *v = n ? n->value : NULL;
  if (!v || v->type != XCDN_VAL_INT)
    return cfg_fail(err_msg, "config: %s: expected integer", key);
  if (v->data.integer < minv || v->data.integer > maxv)
    return cfg_fail(err_msg, "config: %s: value %lld out of range [%lld, %lld]",
                    key, (long long)v->data.integer, (long long)minv,
                    (long long)maxv);
  *out = v->data.integer;
  return ASTOOLS_OK;
}

/* Duration literal r"..." parsed to milliseconds; must be strictly
 * positive (every duration is a period, never zero). */
static astools_err get_duration_ms(const xcdn_node_t *n, const char *key,
                                   int64_t *out_ms, char **err_msg) {
  const xcdn_value_t *v = n ? n->value : NULL;
  int64_t secs = 0;
  if (!v || v->type != XCDN_VAL_DURATION || !v->data.string)
    return cfg_fail(err_msg, "config: %s: expected duration (r\"PT30S\")",
                    key);
  if (!astools_duration_parse(v->data.string, &secs))
    return cfg_fail(err_msg, "config: %s: invalid ISO 8601 duration '%s'",
                    key, v->data.string);
  if (secs <= 0)
    return cfg_fail(err_msg, "config: %s: duration must be positive", key);
  if (secs > INT64_MAX / 1000)
    return cfg_fail(err_msg, "config: %s: duration too large", key);
  *out_ms = secs * 1000;
  return ASTOOLS_OK;
}

typedef struct {
  const char *name;
  int value;
} cfg_enum;

static astools_err get_enum(const xcdn_node_t *n, const char *key,
                            const cfg_enum *choices, size_t n_choices,
                            int *out, char **err_msg) {
  const char *s = NULL;
  astools_err e = get_string(n, key, &s, err_msg);
  size_t i;
  if (e != ASTOOLS_OK) return e;
  for (i = 0; i < n_choices; i++) {
    if (strcmp(s, choices[i].name) == 0) {
      *out = choices[i].value;
      return ASTOOLS_OK;
    }
  }
  return cfg_fail(err_msg, "config: %s: unknown value '%s'", key, s);
}

/* ---- compound value parsers --------------------------------------------- */

/* [{path, access: "read"|"write"|"read-write"}, ...]; key is the dotted
 * location for messages. Fills *out and *out_len (caller pre-frees). */
static astools_err parse_fs_perms(const xcdn_value_t *arr, const char *key,
                                  astools_fs_perm **out, size_t *out_len,
                                  char **err_msg) {
  static const cfg_enum access_vals[] = {
      {"read", ASTOOLS_ACCESS_READ},
      {"write", ASTOOLS_ACCESS_WRITE},
      {"read-write", ASTOOLS_ACCESS_RW},
  };
  astools_fs_perm *f = NULL;
  size_t n, i;
  astools_err e;

  *out = NULL;
  *out_len = 0;
  if (!arr || arr->type != XCDN_VAL_ARRAY)
    return cfg_fail(err_msg, "config: %s: expected array", key);
  n = xcdn_array_len(arr);
  if (n == 0) return ASTOOLS_OK;
  f = calloc(n, sizeof *f);
  if (!f) return cfg_oom(err_msg);

  for (i = 0; i < n; i++) {
    const xcdn_node_t *el = xcdn_array_get(arr, i);
    const xcdn_value_t *ov = el ? el->value : NULL;
    char keybuf[160];
    size_t j, nk;
    bool has_access = false;

    if (!ov || ov->type != XCDN_VAL_OBJECT) {
      e = cfg_fail(err_msg, "config: %s[%zu]: expected object", key, i);
      goto fail;
    }
    nk = xcdn_object_len(ov);
    for (j = 0; j < nk; j++) {
      const char *k = xcdn_object_key_at(ov, j);
      const xcdn_node_t *kn = xcdn_object_node_at(ov, j);
      if (!k || !kn) {
        e = cfg_fail(err_msg, "config: %s[%zu]: malformed entry", key, i);
        goto fail;
      }
      if (strcmp(k, "path") == 0) {
        const char *s = NULL;
        snprintf(keybuf, sizeof keybuf, "%s[%zu].path", key, i);
        e = get_string(kn, keybuf, &s, err_msg);
        if (e != ASTOOLS_OK) goto fail;
        free(f[i].path);
        f[i].path = astools_strdup(s);
        if (!f[i].path) {
          e = cfg_oom(err_msg);
          goto fail;
        }
      } else if (strcmp(k, "access") == 0) {
        snprintf(keybuf, sizeof keybuf, "%s[%zu].access", key, i);
        e = get_enum(kn, keybuf, access_vals, 3, &f[i].access, err_msg);
        if (e != ASTOOLS_OK) goto fail;
        has_access = true;
      } else {
        e = cfg_fail(err_msg, "config: %s[%zu].%s: unknown key", key, i, k);
        goto fail;
      }
    }
    if (!f[i].path) {
      e = cfg_fail(err_msg, "config: %s[%zu]: missing 'path'", key, i);
      goto fail;
    }
    if (!has_access) {
      e = cfg_fail(err_msg, "config: %s[%zu]: missing 'access'", key, i);
      goto fail;
    }
  }
  *out = f;
  *out_len = n;
  return ASTOOLS_OK;

fail:
  free_fs_perms(f, n);
  return e;
}

static astools_err parse_str_array(const xcdn_value_t *arr, const char *key,
                                   char ***out, size_t *out_len,
                                   char **err_msg) {
  char **a = NULL;
  size_t n, i;
  astools_err e;

  *out = NULL;
  *out_len = 0;
  if (!arr || arr->type != XCDN_VAL_ARRAY)
    return cfg_fail(err_msg, "config: %s: expected array of strings", key);
  n = xcdn_array_len(arr);
  if (n == 0) return ASTOOLS_OK;
  a = calloc(n, sizeof *a);
  if (!a) return cfg_oom(err_msg);

  for (i = 0; i < n; i++) {
    const xcdn_node_t *el = xcdn_array_get(arr, i);
    const xcdn_value_t *v = el ? el->value : NULL;
    if (!v || v->type != XCDN_VAL_STRING || !v->data.string) {
      e = cfg_fail(err_msg, "config: %s[%zu]: expected string", key, i);
      goto fail;
    }
    a[i] = astools_strdup(v->data.string);
    if (!a[i]) {
      e = cfg_oom(err_msg);
      goto fail;
    }
  }
  *out = a;
  *out_len = n;
  return ASTOOLS_OK;

fail:
  free_str_array(a, n);
  return e;
}

/* registry.paths: [{path, trust: "standard"|"full"}]; trust defaults to
 * standard. Replaces the current root list on success. */
static astools_err parse_reg_paths(const xcdn_value_t *arr,
                                   astools_config *cfg, char **err_msg) {
  static const cfg_enum trust_vals[] = {
      {"standard", ASTOOLS_TRUST_STANDARD},
      {"full", ASTOOLS_TRUST_FULL},
  };
  astools_root *r = NULL;
  size_t n, i;
  astools_err e;

  if (!arr || arr->type != XCDN_VAL_ARRAY)
    return cfg_fail(err_msg, "config: registry.paths: expected array");
  n = xcdn_array_len(arr);
  if (n > 0) {
    r = calloc(n, sizeof *r);
    if (!r) return cfg_oom(err_msg);
  }

  for (i = 0; i < n; i++) {
    const xcdn_node_t *el = xcdn_array_get(arr, i);
    const xcdn_value_t *ov = el ? el->value : NULL;
    char keybuf[96];
    size_t j, nk;

    if (!ov || ov->type != XCDN_VAL_OBJECT) {
      e = cfg_fail(err_msg, "config: registry.paths[%zu]: expected object", i);
      goto fail;
    }
    r[i].trust = ASTOOLS_TRUST_STANDARD;
    nk = xcdn_object_len(ov);
    for (j = 0; j < nk; j++) {
      const char *k = xcdn_object_key_at(ov, j);
      const xcdn_node_t *kn = xcdn_object_node_at(ov, j);
      if (!k || !kn) {
        e = cfg_fail(err_msg, "config: registry.paths[%zu]: malformed entry",
                     i);
        goto fail;
      }
      if (strcmp(k, "path") == 0) {
        const char *s = NULL;
        snprintf(keybuf, sizeof keybuf, "registry.paths[%zu].path", i);
        e = get_string(kn, keybuf, &s, err_msg);
        if (e != ASTOOLS_OK) goto fail;
        free(r[i].path);
        r[i].path = astools_strdup(s);
        if (!r[i].path) {
          e = cfg_oom(err_msg);
          goto fail;
        }
      } else if (strcmp(k, "trust") == 0) {
        snprintf(keybuf, sizeof keybuf, "registry.paths[%zu].trust", i);
        e = get_enum(kn, keybuf, trust_vals, 2, &r[i].trust, err_msg);
        if (e != ASTOOLS_OK) goto fail;
      } else {
        e = cfg_fail(err_msg, "config: registry.paths[%zu].%s: unknown key",
                     i, k);
        goto fail;
      }
    }
    if (!r[i].path) {
      e = cfg_fail(err_msg, "config: registry.paths[%zu]: missing 'path'", i);
      goto fail;
    }
  }
  free_roots(cfg->reg_paths, cfg->reg_paths_len);
  cfg->reg_paths = r;
  cfg->reg_paths_len = n;
  return ASTOOLS_OK;

fail:
  free_roots(r, n);
  return e;
}

/* registry.pins: [{tool, version}] */
static astools_err parse_reg_pins(const xcdn_value_t *arr, astools_config *cfg,
                                  char **err_msg) {
  astools_pin *pn = NULL;
  size_t n, i;
  astools_err e;

  if (!arr || arr->type != XCDN_VAL_ARRAY)
    return cfg_fail(err_msg, "config: registry.pins: expected array");
  n = xcdn_array_len(arr);
  if (n > 0) {
    pn = calloc(n, sizeof *pn);
    if (!pn) return cfg_oom(err_msg);
  }

  for (i = 0; i < n; i++) {
    const xcdn_node_t *el = xcdn_array_get(arr, i);
    const xcdn_value_t *ov = el ? el->value : NULL;
    char keybuf[96];
    size_t j, nk;

    if (!ov || ov->type != XCDN_VAL_OBJECT) {
      e = cfg_fail(err_msg, "config: registry.pins[%zu]: expected object", i);
      goto fail;
    }
    nk = xcdn_object_len(ov);
    for (j = 0; j < nk; j++) {
      const char *k = xcdn_object_key_at(ov, j);
      const xcdn_node_t *kn = xcdn_object_node_at(ov, j);
      const char *s = NULL;
      char **slot;
      if (!k || !kn) {
        e = cfg_fail(err_msg, "config: registry.pins[%zu]: malformed entry",
                     i);
        goto fail;
      }
      if (strcmp(k, "tool") == 0) {
        slot = &pn[i].tool;
      } else if (strcmp(k, "version") == 0) {
        slot = &pn[i].version;
      } else {
        e = cfg_fail(err_msg, "config: registry.pins[%zu].%s: unknown key", i,
                     k);
        goto fail;
      }
      snprintf(keybuf, sizeof keybuf, "registry.pins[%zu].%s", i, k);
      e = get_string(kn, keybuf, &s, err_msg);
      if (e != ASTOOLS_OK) goto fail;
      free(*slot);
      *slot = astools_strdup(s);
      if (!*slot) {
        e = cfg_oom(err_msg);
        goto fail;
      }
    }
    if (!pn[i].tool || !pn[i].version) {
      e = cfg_fail(err_msg,
                   "config: registry.pins[%zu]: missing 'tool' or 'version'",
                   i);
      goto fail;
    }
  }
  free_pins(cfg->pins, cfg->pins_len);
  cfg->pins = pn;
  cfg->pins_len = n;
  return ASTOOLS_OK;

fail:
  free_pins(pn, n);
  return e;
}

/* grants.tools: [{tool, fs: [...], net, proc, env: [names]}] */
static astools_err parse_grant_list(const xcdn_value_t *arr,
                                    astools_config *cfg, char **err_msg) {
  astools_grant *g = NULL;
  size_t n, i;
  astools_err e;

  if (!arr || arr->type != XCDN_VAL_ARRAY)
    return cfg_fail(err_msg, "config: grants.tools: expected array");
  n = xcdn_array_len(arr);
  if (n > 0) {
    g = calloc(n, sizeof *g);
    if (!g) return cfg_oom(err_msg);
  }

  for (i = 0; i < n; i++) {
    const xcdn_node_t *el = xcdn_array_get(arr, i);
    const xcdn_value_t *ov = el ? el->value : NULL;
    char keybuf[96];
    size_t j, nk;

    if (!ov || ov->type != XCDN_VAL_OBJECT) {
      e = cfg_fail(err_msg, "config: grants.tools[%zu]: expected object", i);
      goto fail;
    }
    nk = xcdn_object_len(ov);
    for (j = 0; j < nk; j++) {
      const char *k = xcdn_object_key_at(ov, j);
      const xcdn_node_t *kn = xcdn_object_node_at(ov, j);
      if (!k || !kn) {
        e = cfg_fail(err_msg, "config: grants.tools[%zu]: malformed entry", i);
        goto fail;
      }
      if (strcmp(k, "tool") == 0) {
        const char *s = NULL;
        snprintf(keybuf, sizeof keybuf, "grants.tools[%zu].tool", i);
        e = get_string(kn, keybuf, &s, err_msg);
        if (e != ASTOOLS_OK) goto fail;
        free(g[i].tool);
        g[i].tool = astools_strdup(s);
        if (!g[i].tool) {
          e = cfg_oom(err_msg);
          goto fail;
        }
      } else if (strcmp(k, "fs") == 0) {
        snprintf(keybuf, sizeof keybuf, "grants.tools[%zu].fs", i);
        free_fs_perms(g[i].fs, g[i].fs_len);
        g[i].fs = NULL;
        g[i].fs_len = 0;
        e = parse_fs_perms(kn->value, keybuf, &g[i].fs, &g[i].fs_len,
                           err_msg);
        if (e != ASTOOLS_OK) goto fail;
      } else if (strcmp(k, "net") == 0) {
        snprintf(keybuf, sizeof keybuf, "grants.tools[%zu].net", i);
        e = get_bool(kn, keybuf, &g[i].net, err_msg);
        if (e != ASTOOLS_OK) goto fail;
      } else if (strcmp(k, "proc") == 0) {
        snprintf(keybuf, sizeof keybuf, "grants.tools[%zu].proc", i);
        e = get_bool(kn, keybuf, &g[i].proc, err_msg);
        if (e != ASTOOLS_OK) goto fail;
      } else if (strcmp(k, "env") == 0) {
        snprintf(keybuf, sizeof keybuf, "grants.tools[%zu].env", i);
        free_str_array(g[i].env, g[i].env_len);
        g[i].env = NULL;
        g[i].env_len = 0;
        e = parse_str_array(kn->value, keybuf, &g[i].env, &g[i].env_len,
                            err_msg);
        if (e != ASTOOLS_OK) goto fail;
      } else {
        e = cfg_fail(err_msg, "config: grants.tools[%zu].%s: unknown key", i,
                     k);
        goto fail;
      }
    }
    if (!g[i].tool) {
      e = cfg_fail(err_msg, "config: grants.tools[%zu]: missing 'tool'", i);
      goto fail;
    }
  }
  free_grants(cfg->tool_grants, cfg->tool_grants_len);
  cfg->tool_grants = g;
  cfg->tool_grants_len = n;
  return ASTOOLS_OK;

fail:
  free_grants(g, n);
  return e;
}

/* ---- sections ----------------------------------------------------------- */

static astools_err parse_sec_registry(const xcdn_value_t *v,
                                      astools_config *cfg, char **err_msg) {
  static const cfg_enum watch_vals[] = {{"poll", 1}, {"off", 0}};
  static const cfg_enum pin_vals[] = {{"off", ASTOOLS_PIN_OFF},
                                      {"warn", ASTOOLS_PIN_WARN},
                                      {"enforce", ASTOOLS_PIN_ENFORCE}};
  size_t i, n;
  astools_err e;

  if (!v || v->type != XCDN_VAL_OBJECT)
    return cfg_fail(err_msg, "config: registry: expected object");
  n = xcdn_object_len(v);
  for (i = 0; i < n; i++) {
    const char *k = xcdn_object_key_at(v, i);
    const xcdn_node_t *kn = xcdn_object_node_at(v, i);
    if (!k || !kn)
      return cfg_fail(err_msg, "config: registry: malformed entry");
    if (strcmp(k, "paths") == 0) {
      e = parse_reg_paths(kn->value, cfg, err_msg);
      if (e != ASTOOLS_OK) return e;
    } else if (strcmp(k, "watch") == 0) {
      int w = 1;
      e = get_enum(kn, "registry.watch", watch_vals, 2, &w, err_msg);
      if (e != ASTOOLS_OK) return e;
      cfg->watch_poll = w != 0;
    } else if (strcmp(k, "poll_interval") == 0) {
      e = get_duration_ms(kn, "registry.poll_interval",
                          &cfg->poll_interval_ms, err_msg);
      if (e != ASTOOLS_OK) return e;
    } else if (strcmp(k, "pinning") == 0) {
      e = get_enum(kn, "registry.pinning", pin_vals, 3, &cfg->pinning,
                   err_msg);
      if (e != ASTOOLS_OK) return e;
    } else if (strcmp(k, "lock_path") == 0) {
      const char *s = NULL;
      e = get_string(kn, "registry.lock_path", &s, err_msg);
      if (e != ASTOOLS_OK) return e;
      if (replace_string(&cfg->lock_path, s) != ASTOOLS_OK)
        return cfg_oom(err_msg);
    } else if (strcmp(k, "pins") == 0) {
      e = parse_reg_pins(kn->value, cfg, err_msg);
      if (e != ASTOOLS_OK) return e;
    } else {
      return cfg_fail(err_msg, "config: registry.%s: unknown key", k);
    }
  }
  return ASTOOLS_OK;
}

static astools_err parse_sec_workspace(const xcdn_value_t *v,
                                       astools_config *cfg, char **err_msg) {
  size_t i, n;
  astools_err e;

  if (!v || v->type != XCDN_VAL_OBJECT)
    return cfg_fail(err_msg, "config: workspace: expected object");
  n = xcdn_object_len(v);
  for (i = 0; i < n; i++) {
    const char *k = xcdn_object_key_at(v, i);
    const xcdn_node_t *kn = xcdn_object_node_at(v, i);
    if (!k || !kn)
      return cfg_fail(err_msg, "config: workspace: malformed entry");
    if (strcmp(k, "root") == 0) {
      const char *s = NULL;
      e = get_string(kn, "workspace.root", &s, err_msg);
      if (e != ASTOOLS_OK) return e;
      if (replace_string(&cfg->workspace_root, s) != ASTOOLS_OK)
        return cfg_oom(err_msg);
    } else if (strcmp(k, "scratch") == 0) {
      if (kn->value && kn->value->type == XCDN_VAL_NULL) {
        free(cfg->scratch);
        cfg->scratch = NULL;
      } else {
        const char *s = NULL;
        e = get_string(kn, "workspace.scratch", &s, err_msg);
        if (e != ASTOOLS_OK) return e;
        if (replace_string(&cfg->scratch, s) != ASTOOLS_OK)
          return cfg_oom(err_msg);
      }
    } else {
      return cfg_fail(err_msg, "config: workspace.%s: unknown key", k);
    }
  }
  return ASTOOLS_OK;
}

static astools_err parse_sec_sandbox(const xcdn_value_t *v,
                                     astools_config *cfg, char **err_msg) {
  static const cfg_enum level_vals[] = {{"none", ASTOOLS_SB_NONE},
                                        {"basic", ASTOOLS_SB_BASIC},
                                        {"strict", ASTOOLS_SB_STRICT}};
  static const cfg_enum fb_vals[] = {{"degrade", 0}, {"reject", 1}};
  size_t i, n;
  astools_err e;

  if (!v || v->type != XCDN_VAL_OBJECT)
    return cfg_fail(err_msg, "config: sandbox: expected object");
  n = xcdn_object_len(v);
  for (i = 0; i < n; i++) {
    const char *k = xcdn_object_key_at(v, i);
    const xcdn_node_t *kn = xcdn_object_node_at(v, i);
    if (!k || !kn)
      return cfg_fail(err_msg, "config: sandbox: malformed entry");
    if (strcmp(k, "default_level") == 0) {
      e = get_enum(kn, "sandbox.default_level", level_vals, 3,
                   &cfg->sandbox_level, err_msg);
      if (e != ASTOOLS_OK) return e;
    } else if (strcmp(k, "strict_fallback") == 0) {
      int r = 0;
      e = get_enum(kn, "sandbox.strict_fallback", fb_vals, 2, &r, err_msg);
      if (e != ASTOOLS_OK) return e;
      cfg->strict_fallback_reject = r != 0;
    } else if (strcmp(k, "allow_library") == 0) {
      e = get_bool(kn, "sandbox.allow_library", &cfg->allow_library, err_msg);
      if (e != ASTOOLS_OK) return e;
    } else {
      return cfg_fail(err_msg, "config: sandbox.%s: unknown key", k);
    }
  }
  return ASTOOLS_OK;
}

static astools_err parse_sec_grants(const xcdn_value_t *v,
                                    astools_config *cfg, char **err_msg) {
  static const cfg_enum wa_vals[] = {{"none", 0},
                                     {"read", ASTOOLS_ACCESS_READ},
                                     {"read-write", ASTOOLS_ACCESS_RW}};
  size_t i, n;
  astools_err e;

  if (!v || v->type != XCDN_VAL_OBJECT)
    return cfg_fail(err_msg, "config: grants: expected object");
  n = xcdn_object_len(v);
  for (i = 0; i < n; i++) {
    const char *k = xcdn_object_key_at(v, i);
    const xcdn_node_t *kn = xcdn_object_node_at(v, i);
    if (!k || !kn)
      return cfg_fail(err_msg, "config: grants: malformed entry");
    if (strcmp(k, "workspace_access") == 0) {
      e = get_enum(kn, "grants.workspace_access", wa_vals, 3,
                   &cfg->workspace_access, err_msg);
      if (e != ASTOOLS_OK) return e;
    } else if (strcmp(k, "tools") == 0) {
      e = parse_grant_list(kn->value, cfg, err_msg);
      if (e != ASTOOLS_OK) return e;
    } else {
      return cfg_fail(err_msg, "config: grants.%s: unknown key", k);
    }
  }
  return ASTOOLS_OK;
}

static astools_err parse_sec_invocation(const xcdn_value_t *v,
                                        astools_config *cfg, char **err_msg) {
  static const cfg_enum rv_vals[] = {{"off", ASTOOLS_RESVAL_OFF},
                                     {"warn", ASTOOLS_RESVAL_WARN},
                                     {"enforce", ASTOOLS_RESVAL_ENFORCE}};
  size_t i, n;
  astools_err e;
  /* get_int always writes on success, but gcc 13 loses that correlation
   * across inlining and reports -Wmaybe-uninitialized under -Werror. */
  int64_t iv = 0;

  if (!v || v->type != XCDN_VAL_OBJECT)
    return cfg_fail(err_msg, "config: invocation: expected object");
  n = xcdn_object_len(v);
  for (i = 0; i < n; i++) {
    const char *k = xcdn_object_key_at(v, i);
    const xcdn_node_t *kn = xcdn_object_node_at(v, i);
    if (!k || !kn)
      return cfg_fail(err_msg, "config: invocation: malformed entry");
    if (strcmp(k, "timeout") == 0) {
      e = get_duration_ms(kn, "invocation.timeout", &cfg->timeout_ms,
                          err_msg);
      if (e != ASTOOLS_OK) return e;
    } else if (strcmp(k, "max_concurrent") == 0) {
      e = get_int(kn, "invocation.max_concurrent", 1, INT_MAX, &iv, err_msg);
      if (e != ASTOOLS_OK) return e;
      cfg->max_concurrent = (int)iv;
    } else if (strcmp(k, "max_output_bytes") == 0) {
      e = get_int(kn, "invocation.max_output_bytes", 1, INT64_MAX,
                  &cfg->max_output_bytes, err_msg);
      if (e != ASTOOLS_OK) return e;
    } else if (strcmp(k, "stderr_max_bytes") == 0) {
      e = get_int(kn, "invocation.stderr_max_bytes", 0, INT64_MAX,
                  &cfg->stderr_max_bytes, err_msg);
      if (e != ASTOOLS_OK) return e;
    } else if (strcmp(k, "result_validation") == 0) {
      e = get_enum(kn, "invocation.result_validation", rv_vals, 3,
                   &cfg->result_validation, err_msg);
      if (e != ASTOOLS_OK) return e;
    } else {
      return cfg_fail(err_msg, "config: invocation.%s: unknown key", k);
    }
  }
  return ASTOOLS_OK;
}

static astools_err parse_sec_catalog(const xcdn_value_t *v,
                                     astools_config *cfg, char **err_msg) {
  static const cfg_enum lvl_vals[] = {{"index", ASTOOLS_CATALOG_INDEX},
                                      {"summary", ASTOOLS_CATALOG_SUMMARY},
                                      {"full", ASTOOLS_CATALOG_FULL}};
  size_t i, n;
  astools_err e;
  int64_t iv = 0;

  if (!v || v->type != XCDN_VAL_OBJECT)
    return cfg_fail(err_msg, "config: catalog: expected object");
  n = xcdn_object_len(v);
  for (i = 0; i < n; i++) {
    const char *k = xcdn_object_key_at(v, i);
    const xcdn_node_t *kn = xcdn_object_node_at(v, i);
    if (!k || !kn)
      return cfg_fail(err_msg, "config: catalog: malformed entry");
    if (strcmp(k, "level") == 0) {
      e = get_enum(kn, "catalog.level", lvl_vals, 3, &cfg->catalog_level,
                   err_msg);
      if (e != ASTOOLS_OK) return e;
    } else if (strcmp(k, "char_budget") == 0) {
      e = get_int(kn, "catalog.char_budget", 0, INT32_MAX, &iv, err_msg);
      if (e != ASTOOLS_OK) return e;
      cfg->char_budget = (size_t)iv;
    } else if (strcmp(k, "priority") == 0) {
      char **arr = NULL;
      size_t alen = 0;
      e = parse_str_array(kn->value, "catalog.priority", &arr, &alen,
                          err_msg);
      if (e != ASTOOLS_OK) return e;
      free_str_array(cfg->priority, cfg->priority_len);
      cfg->priority = arr;
      cfg->priority_len = alen;
    } else {
      return cfg_fail(err_msg, "config: catalog.%s: unknown key", k);
    }
  }
  return ASTOOLS_OK;
}

static astools_err parse_sec_mcp(const xcdn_value_t *v, astools_config *cfg,
                                 char **err_msg) {
  size_t i, n;
  astools_err e;

  if (!v || v->type != XCDN_VAL_OBJECT)
    return cfg_fail(err_msg, "config: mcp: expected object");
  n = xcdn_object_len(v);
  for (i = 0; i < n; i++) {
    const char *k = xcdn_object_key_at(v, i);
    const xcdn_node_t *kn = xcdn_object_node_at(v, i);
    if (!k || !kn) return cfg_fail(err_msg, "config: mcp: malformed entry");
    if (strcmp(k, "management") == 0) {
      e = get_bool(kn, "mcp.management", &cfg->mcp_management, err_msg);
      if (e != ASTOOLS_OK) return e;
    } else {
      return cfg_fail(err_msg, "config: mcp.%s: unknown key", k);
    }
  }
  return ASTOOLS_OK;
}

static astools_err parse_sec_logging(const xcdn_value_t *v,
                                     astools_config *cfg, char **err_msg) {
  static const cfg_enum lvl_vals[] = {{"error", ASTOOLS_LOG_ERROR},
                                      {"warn", ASTOOLS_LOG_WARN},
                                      {"info", ASTOOLS_LOG_INFO},
                                      {"debug", ASTOOLS_LOG_DEBUG}};
  size_t i, n;
  astools_err e;
  int64_t iv = 0;

  if (!v || v->type != XCDN_VAL_OBJECT)
    return cfg_fail(err_msg, "config: logging: expected object");
  n = xcdn_object_len(v);
  for (i = 0; i < n; i++) {
    const char *k = xcdn_object_key_at(v, i);
    const xcdn_node_t *kn = xcdn_object_node_at(v, i);
    if (!k || !kn)
      return cfg_fail(err_msg, "config: logging: malformed entry");
    if (strcmp(k, "path") == 0) {
      if (kn->value && kn->value->type == XCDN_VAL_NULL) {
        free(cfg->log_path);
        cfg->log_path = NULL;
      } else {
        const char *s = NULL;
        e = get_string(kn, "logging.path", &s, err_msg);
        if (e != ASTOOLS_OK) return e;
        if (replace_string(&cfg->log_path, s) != ASTOOLS_OK)
          return cfg_oom(err_msg);
      }
    } else if (strcmp(k, "level") == 0) {
      e = get_enum(kn, "logging.level", lvl_vals, 4, &cfg->log_level,
                   err_msg);
      if (e != ASTOOLS_OK) return e;
    } else if (strcmp(k, "max_size_kb") == 0) {
      /* bounded so log.c's "kb * 1024" comparison cannot overflow */
      e = get_int(kn, "logging.max_size_kb", 0, INT64_MAX / 1024,
                  &cfg->log_max_size_kb, err_msg);
      if (e != ASTOOLS_OK) return e;
    } else if (strcmp(k, "max_files") == 0) {
      e = get_int(kn, "logging.max_files", 1, INT_MAX, &iv, err_msg);
      if (e != ASTOOLS_OK) return e;
      cfg->log_max_files = (int)iv;
    } else if (strcmp(k, "sync") == 0) {
      e = get_bool(kn, "logging.sync", &cfg->log_sync, err_msg);
      if (e != ASTOOLS_OK) return e;
    } else if (strcmp(k, "audit") == 0) {
      e = get_bool(kn, "logging.audit", &cfg->audit, err_msg);
      if (e != ASTOOLS_OK) return e;
    } else {
      return cfg_fail(err_msg, "config: logging.%s: unknown key", k);
    }
  }
  return ASTOOLS_OK;
}

/* ---- ASTOOLS_PATH prepend ---------------------------------------- */

/* Env roots go first, in list order, at trust "standard". Empty segments
 * are skipped. On OOM the existing root list is left untouched. */
static astools_err prepend_env_paths(astools_config *cfg, char **err_msg) {
#ifdef _WIN32
  const char sep = ';';
#else
  const char sep = ':';
#endif
  char *env;
  const char *p;
  astools_root *nr;
  size_t count = 0, idx = 0, i;

  env = os_env_dup("ASTOOLS_PATH");
  if (!env) return ASTOOLS_OK;
  for (p = env; *p;) {
    const char *start = p;
    while (*p && *p != sep) p++;
    if (p > start) count++;
    if (*p == sep) p++;
  }
  if (count == 0) {
    free(env);
    return ASTOOLS_OK;
  }
  nr = calloc(count + cfg->reg_paths_len, sizeof *nr);
  if (!nr) {
    free(env);
    return cfg_oom(err_msg);
  }
  for (p = env; *p;) {
    const char *start = p;
    while (*p && *p != sep) p++;
    if (p > start) {
      nr[idx].path = astools_strndup(start, (size_t)(p - start));
      nr[idx].trust = ASTOOLS_TRUST_STANDARD;
      if (!nr[idx].path) {
        free_roots(nr, idx);
        free(env);
        return cfg_oom(err_msg);
      }
      idx++;
    }
    if (*p == sep) p++;
  }
  for (i = 0; i < cfg->reg_paths_len; i++) nr[idx++] = cfg->reg_paths[i];
  free(cfg->reg_paths);
  cfg->reg_paths = nr;
  cfg->reg_paths_len = idx;
  free(env);
  return ASTOOLS_OK;
}

/* Directory part of a config path; "." when no separator is present. */
static char *dir_of(const char *path) {
  const char *last = NULL;
  const char *p;
  for (p = path; *p; p++) {
    if (*p == '/') last = p;
#ifdef _WIN32
    if (*p == '\\') last = p;
#endif
  }
  if (!last) return astools_strdup(".");
  if (last == path) return astools_strndup(path, 1); /* filesystem root */
  return astools_strndup(path, (size_t)(last - path));
}

/* ---- public entry points ------------------------------------------------ */

void astools_config_defaults(astools_config *cfg) {
  if (!cfg) return;
  memset(cfg, 0, sizeof *cfg);
  /* On allocation failure fields stay NULL/empty; astools_config_free and
   * astools_config_load tolerate that degenerate (OOM) state. */
  cfg->reg_paths = calloc(1, sizeof *cfg->reg_paths);
  if (cfg->reg_paths) {
    cfg->reg_paths[0].path = astools_strdup("tools");
    cfg->reg_paths[0].trust = ASTOOLS_TRUST_STANDARD;
    if (cfg->reg_paths[0].path) {
      cfg->reg_paths_len = 1;
    } else {
      free(cfg->reg_paths);
      cfg->reg_paths = NULL;
    }
  }
  cfg->watch_poll = true;
  cfg->poll_interval_ms = 5000;
  cfg->pinning = ASTOOLS_PIN_WARN;
  cfg->lock_path = astools_strdup("astools.lock.xcdn");
  cfg->pins = NULL;
  cfg->pins_len = 0;
  cfg->workspace_root = astools_strdup(".");
  cfg->scratch = NULL;
  cfg->sandbox_level = ASTOOLS_SB_BASIC;
  cfg->strict_fallback_reject = false;
  cfg->allow_library = false;
  cfg->workspace_access = ASTOOLS_ACCESS_RW;
  cfg->tool_grants = NULL;
  cfg->tool_grants_len = 0;
  cfg->timeout_ms = 30000;
  cfg->max_concurrent = 4;
  cfg->max_output_bytes = 1048576;
  cfg->stderr_max_bytes = 262144;
  cfg->result_validation = ASTOOLS_RESVAL_WARN;
  cfg->catalog_level = ASTOOLS_CATALOG_SUMMARY;
  cfg->char_budget = 8000;
  cfg->priority = NULL;
  cfg->priority_len = 0;
  cfg->mcp_management = false;
  cfg->log_path = NULL;
  cfg->log_level = ASTOOLS_LOG_INFO;
  cfg->log_max_size_kb = 5120;
  cfg->log_max_files = 3;
  cfg->log_sync = false;
  cfg->audit = false;
  cfg->config_dir = astools_strdup(".");
}

/* cfg must already hold astools_config_defaults() output. NULL path and
 * missing files load nothing (defaults stand); both still receive the
 * ASTOOLS_PATH prepend. explicit_path does not change tolerance. */
astools_err astools_config_load(const char *path, bool explicit_path,
                                astools_config *cfg, char **err_msg) {
  char *text = NULL;
  size_t len = 0;
  xcdn_document_t *doc = NULL;
  xcdn_error_t perr;
  const xcdn_node_t *top;
  const xcdn_value_t *obj;
  astools_err e;
  size_t i, n;

  if (err_msg) *err_msg = NULL;
  if (!cfg) return ASTOOLS_ERR_INVALID;
  (void)explicit_path; /* missing files tolerated in both modes */

  if (!path || path[0] == '\0') return prepend_env_paths(cfg, err_msg);

  e = os_read_file(path, &text, &len);
  if (e == ASTOOLS_ERR_NOT_FOUND) return prepend_env_paths(cfg, err_msg);
  if (e == ASTOOLS_ERR_NOMEM) return cfg_oom(err_msg);
  if (e != ASTOOLS_OK)
    return cfg_fail(err_msg, "config: %s: cannot read file", path);

  perr = xcdn_error_none();
  doc = xcdn_parse_str(text, len, &perr);
  free(text);
  if (!doc)
    return cfg_fail(err_msg, "config: %s: parse error at line %zu, "
                    "column %zu: %s", path, perr.span.line, perr.span.column,
                    perr.message);

  if (doc->values_len != 1) {
    e = cfg_fail(err_msg, "config: %s: expected exactly one top-level value",
                 path);
    goto out;
  }
  top = xcdn_document_get(doc, 0);
  if (!top || !top->value) {
    e = cfg_fail(err_msg, "config: %s: empty document", path);
    goto out;
  }
  if (top->tags_len > 0 && !xcdn_node_has_tag(top, "astools_config")) {
    e = cfg_fail(err_msg, "config: %s: unexpected tag (want #astools_config)",
                 path);
    goto out;
  }
  obj = top->value;
  if (obj->type != XCDN_VAL_OBJECT) {
    e = cfg_fail(err_msg, "config: %s: top-level value must be an object",
                 path);
    goto out;
  }

  n = xcdn_object_len(obj);
  for (i = 0; i < n; i++) {
    const char *k = xcdn_object_key_at(obj, i);
    const xcdn_node_t *kn = xcdn_object_node_at(obj, i);
    const xcdn_value_t *kv = kn ? kn->value : NULL;
    if (!k || !kn) {
      e = cfg_fail(err_msg, "config: %s: malformed top-level entry", path);
      goto out;
    }
    if (strcmp(k, "registry") == 0) {
      e = parse_sec_registry(kv, cfg, err_msg);
    } else if (strcmp(k, "workspace") == 0) {
      e = parse_sec_workspace(kv, cfg, err_msg);
    } else if (strcmp(k, "sandbox") == 0) {
      e = parse_sec_sandbox(kv, cfg, err_msg);
    } else if (strcmp(k, "grants") == 0) {
      e = parse_sec_grants(kv, cfg, err_msg);
    } else if (strcmp(k, "invocation") == 0) {
      e = parse_sec_invocation(kv, cfg, err_msg);
    } else if (strcmp(k, "catalog") == 0) {
      e = parse_sec_catalog(kv, cfg, err_msg);
    } else if (strcmp(k, "mcp") == 0) {
      e = parse_sec_mcp(kv, cfg, err_msg);
    } else if (strcmp(k, "logging") == 0) {
      e = parse_sec_logging(kv, cfg, err_msg);
    } else {
      e = cfg_fail(err_msg, "config: %s: unknown key", k);
    }
    if (e != ASTOOLS_OK) goto out;
  }

  {
    char *dir = dir_of(path);
    if (!dir) {
      e = cfg_oom(err_msg);
      goto out;
    }
    free(cfg->config_dir);
    cfg->config_dir = dir;
  }
  xcdn_document_free(doc);
  return prepend_env_paths(cfg, err_msg);

out:
  xcdn_document_free(doc);
  return e;
}

void astools_config_free(astools_config *cfg) {
  if (!cfg) return;
  free_roots(cfg->reg_paths, cfg->reg_paths_len);
  free(cfg->lock_path);
  free_pins(cfg->pins, cfg->pins_len);
  free(cfg->workspace_root);
  free(cfg->scratch);
  free_grants(cfg->tool_grants, cfg->tool_grants_len);
  free_str_array(cfg->priority, cfg->priority_len);
  free(cfg->log_path);
  free(cfg->config_dir);
  memset(cfg, 0, sizeof *cfg);
}
