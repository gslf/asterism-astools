/*
 * policy.c — grants intersection and invocation pre-flight (§6.3–§6.4).
 *
 * SAFETY CRITICAL. Effective policy = manifest request ∩ host grants,
 * deny-by-default: a prefix that cannot be canonicalized is dropped (fewer
 * prefixes can only narrow the effective set), and a path argument that is
 * not covered by an effective prefix stops the invocation before anything
 * is spawned. Manifests, config grants and args are all untrusted input.
 */

#include "astools_internal.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Recursion cap for the params/args walk; the type tree comes from an
 * untrusted manifest, so depth must be bounded. */
#define POLICY_MAX_DEPTH 128

/* ---- small helpers ------------------------------------------------------- */

static char *vstr_printf(const char *fmt, va_list ap) {
  va_list ap2;
  int need;
  char *s;
  va_copy(ap2, ap);
  need = vsnprintf(NULL, 0, fmt, ap2);
  va_end(ap2);
  if (need < 0) return NULL;
  s = malloc((size_t)need + 1);
  if (!s) return NULL;
  vsnprintf(s, (size_t)need + 1, fmt, ap);
  return s;
}

static char *str_printf(const char *fmt, ...) {
  va_list ap;
  char *s;
  va_start(ap, fmt);
  s = vstr_printf(fmt, ap);
  va_end(ap);
  return s;
}

/* Record a policy failure: sets the context error, hands the malloc'd
 * message to *deny_msg (when provided), returns e. */
static astools_err fail_msg(astools_ctx *c, char **deny_msg, astools_err e,
                            const char *fmt, ...) {
  va_list ap;
  char *msg;
  va_start(ap, fmt);
  msg = vstr_printf(fmt, ap);
  va_end(ap);
  if (msg) {
    astools_seterr(c, e, "%s", msg);
    if (deny_msg)
      *deny_msg = msg;
    else
      free(msg);
  } else {
    astools_seterr(c, e, "policy: check failed (message unavailable)");
  }
  return e;
}

static const char *access_word(int access) {
  switch (access & ASTOOLS_ACCESS_RW) {
    case ASTOOLS_ACCESS_WRITE:
      return "write";
    case ASTOOLS_ACCESS_RW:
      return "read-write";
    default:
      return "read";
  }
}

/* ---- fs prefix lists ----------------------------------------------------- */

typedef struct {
  astools_fs_perm *v;
  size_t n, cap;
} perm_list;

/* Consumes path (frees it on failure). */
static astools_err perm_push(perm_list *l, char *path, int access) {
  if (!path) return ASTOOLS_ERR_NOMEM;
  if (l->n == l->cap) {
    size_t ncap = l->cap ? l->cap * 2 : 8;
    astools_fs_perm *nv;
    if (ncap > SIZE_MAX / sizeof(*nv)) {
      free(path);
      return ASTOOLS_ERR_NOMEM;
    }
    nv = realloc(l->v, ncap * sizeof(*nv));
    if (!nv) {
      free(path);
      return ASTOOLS_ERR_NOMEM;
    }
    l->v = nv;
    l->cap = ncap;
  }
  l->v[l->n].path = path;
  l->v[l->n].access = access;
  l->n++;
  return ASTOOLS_OK;
}

static void perm_list_free(perm_list *l) {
  size_t i;
  for (i = 0; i < l->n; i++) free(l->v[i].path);
  free(l->v);
  l->v = NULL;
  l->n = 0;
  l->cap = 0;
}

/* Canonicalize one requested/granted fs prefix (relative to the workspace,
 * missing_ok) and append it. A prefix that cannot be canonicalized is
 * skipped with a warning — dropping it only narrows the policy. NOMEM
 * propagates. */
static astools_err canon_push(astools_ctx *c, const char *what,
                              const char *path, int access, perm_list *l) {
  char *canon = NULL;
  astools_err e;
  if (!path || (access & ASTOOLS_ACCESS_RW) == 0) return ASTOOLS_OK;
  e = astools_path_canonical(c->workspace, path, true, &canon);
  if (e == ASTOOLS_ERR_NOMEM) return e;
  if (e != ASTOOLS_OK) {
    astools_log(c, ASTOOLS_LOG_WARN, "policy",
                "dropping %s fs prefix '%s': cannot canonicalize (%s)", what,
                path, astools_err_name(e));
    return ASTOOLS_OK;
  }
  return perm_push(l, canon, access & ASTOOLS_ACCESS_RW);
}

/* ---- effective policy (§6.3) --------------------------------------------- */

astools_err astools_policy_effective(astools_ctx *c, const astools_tool *t,
                                     astools_effective *out) {
  perm_list req, grant, effl;
  char **env = NULL;
  size_t env_n = 0, env_cap = 0;
  bool net_granted = false, proc_granted = false;
  const astools_manifest *m;
  size_t i, j;
  astools_err e = ASTOOLS_OK;

  if (!out) return ASTOOLS_ERR_INVALID;
  memset(out, 0, sizeof(*out));
  memset(&req, 0, sizeof(req));
  memset(&grant, 0, sizeof(grant));
  memset(&effl, 0, sizeof(effl));
  if (!c || !t || !t->m)
    return astools_seterr(c, ASTOOLS_ERR_INVALID, "policy: invalid arguments");
  m = t->m;

  /* requested fs prefixes (manifest) */
  for (i = 0; i < m->perms.fs_len; i++) {
    e = canon_push(c, "requested", m->perms.fs[i].path, m->perms.fs[i].access,
                   &req);
    if (e != ASTOOLS_OK) goto done;
  }

  /* granted fs prefixes: workspace auto-grant + matching grants.tools */
  if ((c->cfg.workspace_access & ASTOOLS_ACCESS_RW) != 0 && c->workspace) {
    char *ws = astools_strdup(c->workspace);
    if (!ws) {
      e = ASTOOLS_ERR_NOMEM;
      goto done;
    }
    e = perm_push(&grant, ws, c->cfg.workspace_access & ASTOOLS_ACCESS_RW);
    if (e != ASTOOLS_OK) goto done;
  }
  for (i = 0; i < c->cfg.tool_grants_len; i++) {
    const astools_grant *g = &c->cfg.tool_grants[i];
    if (!g->tool || !m->id || strcmp(g->tool, m->id) != 0) continue;
    net_granted = net_granted || g->net;
    proc_granted = proc_granted || g->proc;
    for (j = 0; j < g->fs_len; j++) {
      e = canon_push(c, "granted", g->fs[j].path, g->fs[j].access, &grant);
      if (e != ASTOOLS_OK) goto done;
    }
  }

  /* fs intersection: for every (request, grant) pair where one path
   * contains the other, keep the deeper path with the intersected access */
  for (i = 0; i < req.n; i++) {
    for (j = 0; j < grant.n; j++) {
      const char *deep;
      int access = req.v[i].access & grant.v[j].access;
      size_t k;
      bool dup = false;
      if (access == 0) continue;
      if (astools_path_under(req.v[i].path, grant.v[j].path))
        deep = req.v[i].path;
      else if (astools_path_under(grant.v[j].path, req.v[i].path))
        deep = grant.v[j].path;
      else
        continue;
      for (k = 0; k < effl.n; k++) {
        if (effl.v[k].access == access &&
            strcmp(effl.v[k].path, deep) == 0) {
          dup = true;
          break;
        }
      }
      if (dup) continue;
      e = perm_push(&effl, astools_strdup(deep), access);
      if (e != ASTOOLS_OK) goto done;
    }
  }

  /* env: requested names ∩ union of granted names. A tool whose manifest
   * requests env: [] cannot enumerate names up front (the env tool, §8.7:
   * "the grant list comes from the host"), so an EMPTY request means the
   * effective set is exactly what the operator granted to this tool —
   * still host-conceded, never manifest-widened. */
  {
    size_t req_n = m->perms.env_len;
    /* Build the candidate list: requested names, or every granted name
     * when the request is empty. */
    for (i = 0; ; i++) {
      const char *name = NULL;
      bool granted = false, dup = false;
      if (req_n > 0) {
        if (i >= req_n) break;
        name = m->perms.env[i];
      } else {
        /* enumerate grants: flatten (grant, k) pairs by linear index */
        size_t seen = 0, k;
        for (j = 0; j < c->cfg.tool_grants_len && !name; j++) {
          const astools_grant *g = &c->cfg.tool_grants[j];
          if (!g->tool || !m->id || strcmp(g->tool, m->id) != 0) continue;
          for (k = 0; k < g->env_len; k++) {
            if (seen++ == i) {
              name = g->env[k];
              break;
            }
          }
        }
        if (!name) break;
        granted = true;
      }
      if (!name) continue;
      for (j = 0; j < c->cfg.tool_grants_len && !granted; j++) {
        const astools_grant *g = &c->cfg.tool_grants[j];
        size_t k;
        if (!g->tool || !m->id || strcmp(g->tool, m->id) != 0) continue;
        for (k = 0; k < g->env_len; k++) {
          if (g->env[k] && strcmp(g->env[k], name) == 0) {
            granted = true;
            break;
          }
        }
      }
      if (!granted) continue;
      for (j = 0; j < env_n; j++) {
        if (strcmp(env[j], name) == 0) {
          dup = true;
          break;
        }
      }
      if (dup) continue;
    if (env_n == env_cap) {
      size_t ncap = env_cap ? env_cap * 2 : 4;
      char **ne;
      if (ncap > SIZE_MAX / sizeof(*ne)) {
        e = ASTOOLS_ERR_NOMEM;
        goto done;
      }
      ne = realloc(env, ncap * sizeof(*ne));
      if (!ne) {
        e = ASTOOLS_ERR_NOMEM;
        goto done;
      }
      env = ne;
      env_cap = ncap;
    }
      env[env_n] = astools_strdup(name);
      if (!env[env_n]) {
        e = ASTOOLS_ERR_NOMEM;
        goto done;
      }
      env_n++;
    }
  }

  out->fs = effl.v;
  out->fs_len = effl.n;
  effl.v = NULL;
  effl.n = 0;
  effl.cap = 0;
  out->net = m->perms.net && net_granted;
  out->proc = m->perms.proc && proc_granted;
  out->env = env;
  out->env_len = env_n;
  env = NULL;
  env_n = 0;
  env_cap = 0;
  e = ASTOOLS_OK;

done:
  perm_list_free(&req);
  perm_list_free(&grant);
  perm_list_free(&effl);
  for (i = 0; i < env_n; i++) free(env[i]);
  free(env);
  if (e == ASTOOLS_ERR_NOMEM)
    astools_seterr(c, e, "policy: out of memory");
  return e;
}

void astools_effective_free(astools_effective *eff) {
  size_t i;
  if (!eff) return;
  for (i = 0; i < eff->fs_len; i++) free(eff->fs[i].path);
  free(eff->fs);
  for (i = 0; i < eff->env_len; i++) free(eff->env[i]);
  free(eff->env);
  memset(eff, 0, sizeof(*eff));
}

/* ---- pre-flight (§5.1 step 3 + §6.4) ------------------------------------- */

/* Walk one typed value. Wrong node kinds and absent members are skipped:
 * schema validation already rejected them, and skipping cannot widen the
 * policy (a path that is never canonicalized never reaches a tool as a
 * grant-approved path — dispatch sends the args object as-is only after
 * this walk succeeded on every AT_PATH it could see, and validation
 * guarantees it saw them all). */
static astools_err walk_type(astools_ctx *c, const astools_tool *t,
                             const astools_type *ty, xcdn_node_t *node,
                             const char *name, const astools_effective *eff,
                             int depth, char **deny_msg) {
  xcdn_value_t *v;
  astools_err e;
  size_t i;

  if (!ty || !node) return ASTOOLS_OK;
  if (depth > POLICY_MAX_DEPTH)
    return astools_seterr(c, ASTOOLS_ERR_INVALID,
                          "param %s: nesting too deep", name);
  v = node->value;

  switch (ty->kind) {
    case AT_PATH: {
      char *canon = NULL;
      bool covered = false;
      if (!v || v->type != XCDN_VAL_STRING || !v->data.string)
        return ASTOOLS_OK;
      /* Always canonicalize through the deepest existing ancestor first:
       * the GRANT check must come before any existence disclosure, or the
       * error code would leak whether an ungranted path exists (existence
       * oracle). must_exist is enforced only for paths already inside the
       * grants. */
      e = astools_path_canonical(c->workspace, v->data.string, true, &canon);
      if (e == ASTOOLS_ERR_NOMEM)
        return astools_seterr(c, e, "policy: out of memory");
      if (e != ASTOOLS_OK)
        return astools_seterr(c, e, "param %s: cannot canonicalize path '%s'",
                              name, v->data.string);
      for (i = 0; i < eff->fs_len; i++) {
        if (eff->fs[i].path && (ty->access & ~eff->fs[i].access) == 0 &&
            astools_path_under(canon, eff->fs[i].path)) {
          covered = true;
          break;
        }
      }
      if (!covered) {
        e = fail_msg(c, deny_msg, ASTOOLS_ERR_DENIED,
                     "param %s: path '%s' is outside the effective "
                     "filesystem grants (missing %s grant); ask the "
                     "operator to extend grants.tools for tool '%s'",
                     name, canon, access_word(ty->access),
                     t->m->id ? t->m->id : "?");
        free(canon);
        return e;
      }
      if (ty->must_exist) {
        os_stat_info st;
        memset(&st, 0, sizeof st);
        if (os_stat(canon, &st) != ASTOOLS_OK || st.type == OS_FT_MISSING) {
          e = fail_msg(c, deny_msg, ASTOOLS_ERR_INVALID,
                       "param %s: path does not exist", name);
          free(canon);
          return e;
        }
      }
      /* The canonical path replaces the original: tools only ever see
       * grant-checked absolute paths. */
      free(v->data.string);
      v->data.string = canon;
      return ASTOOLS_OK;
    }
    case AT_ARRAY: {
      size_t len;
      if (!v || v->type != XCDN_VAL_ARRAY || !ty->item) return ASTOOLS_OK;
      len = xcdn_array_len(v);
      for (i = 0; i < len; i++) {
        xcdn_node_t *child = xcdn_array_get(v, i);
        char *cn;
        if (!child) continue;
        cn = str_printf("%s[%zu]", name, i);
        if (!cn)
          return astools_seterr(c, ASTOOLS_ERR_NOMEM,
                                "policy: out of memory");
        e = walk_type(c, t, ty->item, child, cn, eff, depth + 1, deny_msg);
        free(cn);
        if (e != ASTOOLS_OK) return e;
      }
      return ASTOOLS_OK;
    }
    case AT_MAP: {
      size_t len;
      if (!v || v->type != XCDN_VAL_OBJECT || !ty->value) return ASTOOLS_OK;
      len = xcdn_object_len(v);
      for (i = 0; i < len; i++) {
        xcdn_node_t *child = xcdn_object_node_at(v, i);
        const char *key = xcdn_object_key_at(v, i);
        char *cn;
        if (!child) continue;
        cn = str_printf("%s.%s", name, key ? key : "?");
        if (!cn)
          return astools_seterr(c, ASTOOLS_ERR_NOMEM,
                                "policy: out of memory");
        e = walk_type(c, t, ty->value, child, cn, eff, depth + 1, deny_msg);
        free(cn);
        if (e != ASTOOLS_OK) return e;
      }
      return ASTOOLS_OK;
    }
    case AT_OBJECT: {
      if (!v || v->type != XCDN_VAL_OBJECT) return ASTOOLS_OK;
      for (i = 0; i < ty->fields_len; i++) {
        const astools_param *p = &ty->fields[i];
        xcdn_node_t *child;
        char *cn;
        if (!p->name || !p->type) continue;
        child = xcdn_object_get(v, p->name);
        if (!child) continue;
        cn = str_printf("%s.%s", name, p->name);
        if (!cn)
          return astools_seterr(c, ASTOOLS_ERR_NOMEM,
                                "policy: out of memory");
        e = walk_type(c, t, p->type, child, cn, eff, depth + 1, deny_msg);
        free(cn);
        if (e != ASTOOLS_OK) return e;
      }
      return ASTOOLS_OK;
    }
    default:
      return ASTOOLS_OK;
  }
}

astools_err astools_policy_preflight(astools_ctx *c, const astools_tool *t,
                                     const astools_cmd *cmd,
                                     xcdn_node_t *args,
                                     const astools_effective *eff,
                                     char **deny_msg) {
  size_t i;
  astools_err e;

  if (deny_msg) *deny_msg = NULL;
  if (!c || !t || !t->m || !cmd || !eff)
    return astools_seterr(c, ASTOOLS_ERR_INVALID, "policy: invalid arguments");

  if (args && args->value && args->value->type == XCDN_VAL_OBJECT) {
    for (i = 0; i < cmd->params_len; i++) {
      const astools_param *p = &cmd->params[i];
      xcdn_node_t *v;
      if (!p->name || !p->type) continue;
      v = xcdn_object_get(args->value, p->name);
      if (!v) continue; /* absent optional (defaults already injected) */
      e = walk_type(c, t, p->type, v, p->name, eff, 0, deny_msg);
      if (e != ASTOOLS_OK) return e;
    }
  }

  if (t->m->perms.proc && !eff->proc)
    return fail_msg(c, deny_msg, ASTOOLS_ERR_DENIED,
                    "tool requests the proc grant which the host has not "
                    "granted (grants.tools: { tool: \"%s\", proc: true })",
                    t->m->id ? t->m->id : "?");
  if (t->m->perms.net && !eff->net)
    return fail_msg(c, deny_msg, ASTOOLS_ERR_DENIED,
                    "tool requests the net grant which the host has not "
                    "granted (grants.tools: { tool: \"%s\", net: true })",
                    t->m->id ? t->m->id : "?");
  /* env is never a denial: ungranted variables simply are not passed. */
  return ASTOOLS_OK;
}
